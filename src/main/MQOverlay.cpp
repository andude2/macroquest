/*
 * MacroQuest: The extension platform for EverQuest
 * Copyright (C) 2002-present MacroQuest Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "pch.h"

#include "MQ2Main.h"
#include "mq/api/Overlay.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace mq {

namespace {

struct OverlayCallbackEntry
{
	OverlayCallback callback = nullptr;
	void* userData = nullptr;
	MQPluginHandle owner;
	OverlayHandle handle = 0;
};

std::mutex s_overlayMutex;
std::vector<OverlayCallbackEntry> s_overlayCallbacks;
OverlayHandle s_nextOverlayHandle = 1;
std::atomic<float> s_overlayHealthPercent{ -1.0f };
std::atomic<uint64_t> s_overlayHealthLastUpdate{ 0 };
constexpr uint64_t kOverlayHealthResetDelayMs = 5000; // fallback to full after 5 seconds of silence
bool s_overlayHookInstalled = false;

bool IsEZServer()
{
	const char* shortName = GetServerShortName();
	return shortName && _stricmp(shortName, "EZ (Linux) x4 Exp") == 0;
}

void UpdateEZPercent(std::string_view text)
{
	if (!IsEZServer())
	{
		ResetOverlayHealthPercent();
		return;
	}

	constexpr std::string_view kPrefix = "Health:";
	const size_t pos = text.find(kPrefix);
	if (pos == std::string_view::npos)
		return;

	size_t cursor = pos + kPrefix.size();
	while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])))
		++cursor;

	size_t start = cursor;
	while (cursor < text.size() && (std::isdigit(static_cast<unsigned char>(text[cursor])) || text[cursor] == '.'))
		++cursor;

	if (cursor == start)
		return;

	std::string number(text.substr(start, cursor - start));
	char* endPtr = nullptr;
	const float parsed = std::strtof(number.c_str(), &endPtr);
	if (endPtr == number.c_str())
		return;

	s_overlayHealthPercent.store(std::clamp(parsed, 0.0f, 100.0f), std::memory_order_relaxed);
	s_overlayHealthLastUpdate.store(MQGetTickCount64(), std::memory_order_relaxed);
}

void MaybeResetOverlayHealthToFull()
{
	const float current = s_overlayHealthPercent.load(std::memory_order_relaxed);
	if (current < 0.0f || current >= 100.0f)
		return;

	const uint64_t lastUpdate = s_overlayHealthLastUpdate.load(std::memory_order_relaxed);
	if (lastUpdate == 0)
		return;

	const uint64_t now = MQGetTickCount64();
	if (now < lastUpdate)
	{
		// Tick counter rolled over, treat now as the new baseline.
		s_overlayHealthLastUpdate.store(now, std::memory_order_relaxed);
		return;
	}

	if (now - lastUpdate >= kOverlayHealthResetDelayMs)
	{
		s_overlayHealthLastUpdate.store(now, std::memory_order_relaxed);
		s_overlayHealthPercent.store(100.0f, std::memory_order_relaxed);
	}
}

bool DispatchOverlayCallbacks(const OverlayMessage& message)
{
	std::vector<OverlayCallbackEntry> callbacks;
	{
		std::scoped_lock lock(s_overlayMutex);
		callbacks = s_overlayCallbacks;
	}

	bool suppress = false;
	for (const OverlayCallbackEntry& entry : callbacks)
	{
		if (entry.callback)
		{
			if (entry.callback(message, entry.userData))
				suppress = true;
		}
	}

	return suppress;
}

#ifdef CTextOverlay__DisplayText_x
class OverlayTextHook
{
public:
	DETOUR_TRAMPOLINE_DEF(void, DisplayText_Trampoline,
		(const char*, int, int, int, uint32_t, uint32_t, uint32_t))

	void DisplayText_Detour(const char* Str, int TextColor, int Priority, int MaxAlpha,
		uint32_t FadeInTime, uint32_t FadeOutTime, uint32_t DisplayTime)
	{
		OverlayMessage message;
		message.text = Str ? std::string_view(Str) : std::string_view{};
		message.color = TextColor;
		message.priority = Priority;
		message.maxAlpha = MaxAlpha;
		message.fadeIn = FadeInTime;
		message.fadeOut = FadeOutTime;
		message.displayTime = DisplayTime;

		UpdateEZPercent(message.text);

		const bool suppress = DispatchOverlayCallbacks(message);
		if (!suppress)
		{
			DisplayText_Trampoline(Str, TextColor, Priority, MaxAlpha, FadeInTime, FadeOutTime, DisplayTime);
		}
	}
};
#endif // CTextOverlay__DisplayText_x

} // namespace

OverlayHandle RegisterOverlayCallback(OverlayCallback callback, void* userData, MQPluginHandle owner)
{
	if (!callback)
		return 0;

	OverlayHandle handle = 0;
	{
		std::scoped_lock lock(s_overlayMutex);
		handle = s_nextOverlayHandle++;
		s_overlayCallbacks.push_back({ callback, userData, owner, handle });
	}
	return handle;
}

void UnregisterOverlayCallback(OverlayHandle handle)
{
	if (handle == 0)
		return;

	std::scoped_lock lock(s_overlayMutex);
	s_overlayCallbacks.erase(std::remove_if(s_overlayCallbacks.begin(), s_overlayCallbacks.end(),
		[handle](const OverlayCallbackEntry& entry) { return entry.handle == handle; }), s_overlayCallbacks.end());
}

void RemoveOverlayCallbacksForPlugin(MQPluginHandle owner)
{
	if (owner.pluginID == 0)
		return;

	std::scoped_lock lock(s_overlayMutex);
	s_overlayCallbacks.erase(std::remove_if(s_overlayCallbacks.begin(), s_overlayCallbacks.end(),
		[owner](const OverlayCallbackEntry& entry) { return entry.owner == owner; }), s_overlayCallbacks.end());
}

float GetOverlayHealthPercent()
{
	if (!IsEZServer())
		return -1.0f;

	MaybeResetOverlayHealthToFull();

	float value = s_overlayHealthPercent.load(std::memory_order_relaxed);
	if (value < 0.0f)
		return -1.0f;

	if (value > 100.0f)
		return 100.0f;

	return value;
}

void ResetOverlayHealthPercent()
{
	s_overlayHealthPercent.store(-1.0f, std::memory_order_relaxed);
	s_overlayHealthLastUpdate.store(0, std::memory_order_relaxed);
}

void InitializeOverlayService()
{
#ifdef CTextOverlay__DisplayText_x
	if (!s_overlayHookInstalled)
	{
		if (EzDetour(CTextOverlay__DisplayText, &OverlayTextHook::DisplayText_Detour, &OverlayTextHook::DisplayText_Trampoline))
		{
			s_overlayHookInstalled = true;
			ResetOverlayHealthPercent();
		}
		else
		{
			WriteChatf("[MQ] \arFailed to hook overlay text.\ax");
		}
	}
#endif // CTextOverlay__DisplayText_x
}

void ShutdownOverlayService()
{
#ifdef CTextOverlay__DisplayText_x
	if (s_overlayHookInstalled)
	{
		RemoveDetour(CTextOverlay__DisplayText);
		s_overlayHookInstalled = false;
	}
#endif // CTextOverlay__DisplayText_x

	ResetOverlayHealthPercent();
	std::scoped_lock lock(s_overlayMutex);
	s_overlayCallbacks.clear();
}

} // namespace mq
