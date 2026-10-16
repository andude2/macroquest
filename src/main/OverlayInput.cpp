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
#include "OverlayInput.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <utility>

namespace mq {
namespace {

struct ConsumerEntry
{
	OverlayInputHandle handle = 0;
	OverlayInputLayer layer = OverlayInputLayer::RmlUi;
	MQPluginHandle owner;
	OverlayInputConsumer consumer;
};

std::mutex s_mutex;
using ConsumerEntryPtr = std::shared_ptr<ConsumerEntry>;
std::vector<ConsumerEntryPtr> s_consumers;
OverlayInputHandle s_nextHandle = 1;

// Mouse capture: a consumer that handled a button-down keeps the mouse until the
// matching button-up is observed.
OverlayInputHandle s_mouseCaptureHandle = 0;
uint32_t s_mouseCaptureButtons = 0;

bool IsMouseButtonDownMessage(UINT msg)
{
	switch (msg)
	{
	case WM_LBUTTONDOWN:
	case WM_RBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_XBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
	case WM_RBUTTONDBLCLK:
	case WM_MBUTTONDBLCLK:
	case WM_XBUTTONDBLCLK:
		return true;
	default:
		return false;
	}
}

bool IsMouseButtonUpMessage(UINT msg)
{
	switch (msg)
	{
	case WM_LBUTTONUP:
	case WM_RBUTTONUP:
	case WM_MBUTTONUP:
	case WM_XBUTTONUP:
		return true;
	default:
		return false;
	}
}

bool IsMouseMessage(UINT msg)
{
	if (IsMouseButtonDownMessage(msg) || IsMouseButtonUpMessage(msg))
		return true;

	switch (msg)
	{
	case WM_MOUSEMOVE:
	case WM_MOUSELEAVE:
	case WM_MOUSEWHEEL:
	case WM_MOUSEHWHEEL:
		return true;
	default:
		return false;
	}
}

// Returns the button index for a mouse button message, or -1 if not a button.
int MouseButtonFromMessage(UINT msg, WPARAM wParam)
{
	switch (msg)
	{
	case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: return 0;
	case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: return 1;
	case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: return 2;
	case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
		return GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 3 : 4;
	default: return -1;
	}
}

std::vector<ConsumerEntryPtr> ConsumersByPriority()
{
	std::vector<ConsumerEntryPtr> result = s_consumers;

	std::stable_sort(result.begin(), result.end(),
		[](const ConsumerEntryPtr& a, const ConsumerEntryPtr& b)
		{
			return static_cast<int>(a->layer) < static_cast<int>(b->layer);
		});
	return result;
}

} // namespace

OverlayInputHandle RegisterOverlayInputConsumer(OverlayInputConsumer consumer)
{
	if (!consumer.handleWndProc)
		return 0;

	OverlayInputHandle handle = 0;
	{
		std::scoped_lock lock(s_mutex);
		handle = s_nextHandle++;
		s_consumers.push_back(std::make_shared<ConsumerEntry>(
			ConsumerEntry{ handle, consumer.layer, consumer.owner, std::move(consumer) }));
	}
	return handle;
}

void UnregisterOverlayInputConsumer(OverlayInputHandle handle)
{
	std::scoped_lock lock(s_mutex);
	s_consumers.erase(std::remove_if(s_consumers.begin(), s_consumers.end(),
		[handle](const ConsumerEntryPtr& entry) { return entry->handle == handle; }), s_consumers.end());

	if (s_mouseCaptureHandle == handle)
	{
		s_mouseCaptureHandle = 0;
		s_mouseCaptureButtons = 0;
	}
}

void RemoveOverlayInputConsumersForPlugin(MQPluginHandle owner)
{
	if (owner.pluginID == 0)
		return;

	std::scoped_lock lock(s_mutex);
	s_consumers.erase(std::remove_if(s_consumers.begin(), s_consumers.end(),
		[owner](const ConsumerEntryPtr& entry) { return entry->owner == owner; }), s_consumers.end());

	const bool capturedOwnerRemoved = std::none_of(s_consumers.begin(), s_consumers.end(),
		[](const ConsumerEntryPtr& entry) { return entry->handle == s_mouseCaptureHandle; });
	if (capturedOwnerRemoved)
	{
		s_mouseCaptureHandle = 0;
		s_mouseCaptureButtons = 0;
	}
}

bool OverlayWndProcDispatch(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	const bool isButtonDown = IsMouseButtonDownMessage(msg);
	const bool isButtonUp = IsMouseButtonUpMessage(msg);
	const int button = (isButtonDown || isButtonUp) ? MouseButtonFromMessage(msg, wParam) : -1;
	bool capturedMouseMessage = false;

	// Snapshot the dispatch order (and clear capture on focus loss) under the lock,
	// then invoke callbacks WITHOUT holding it. Consumer callbacks can synchronously
	// re-enter the WndProc (e.g. ImGui's SetCapture/SetFocus), which would otherwise
	// deadlock on a non-recursive mutex.
	std::vector<ConsumerEntryPtr> order;
	{
		std::scoped_lock lock(s_mutex);

		if (msg == WM_KILLFOCUS || (msg == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE))
		{
			s_mouseCaptureHandle = 0;
			s_mouseCaptureButtons = 0;
		}

		order = ConsumersByPriority();
		if (IsMouseMessage(msg) && s_mouseCaptureHandle != 0)
		{
			capturedMouseMessage = true;
			auto it = std::find_if(order.begin(), order.end(),
				[](const ConsumerEntryPtr& entry) { return entry->handle == s_mouseCaptureHandle; });
			if (it != order.end())
			{
				ConsumerEntryPtr owner = *it;
				// Mouse capture is exclusive. Do not let another consumer take a
				// second button or hover event in the middle of an active drag.
				order.clear();
				order.push_back(std::move(owner));
			}
			else
			{
				s_mouseCaptureHandle = 0;
				s_mouseCaptureButtons = 0;
				capturedMouseMessage = false;
			}
		}
	}

	for (const ConsumerEntryPtr& entry : order)
	{
		if (entry->consumer.handleWndProc(hWnd, msg, wParam, lParam))
		{
			std::scoped_lock lock(s_mutex);
			if (isButtonDown && button >= 0)
			{
				s_mouseCaptureHandle = entry->handle;
				s_mouseCaptureButtons |= (1u << button);
			}
			else if (isButtonUp && button >= 0)
			{
				s_mouseCaptureButtons &= ~(1u << button);
				if (s_mouseCaptureButtons == 0)
					s_mouseCaptureHandle = 0;
			}
			return true;
		}
	}

	// Once a button-down is captured, its matching release must not leak to EQ
	// even if the consumer no longer reports the cursor as being over its UI.
	if (capturedMouseMessage)
	{
		std::scoped_lock lock(s_mutex);
		if (isButtonDown && button >= 0)
			s_mouseCaptureButtons |= (1u << button);
		else if (isButtonUp && button >= 0)
			s_mouseCaptureButtons &= ~(1u << button);
		if (s_mouseCaptureButtons == 0)
			s_mouseCaptureHandle = 0;
		return true;
	}

	return false;
}

bool OverlayWantsMouseInput()
{
	std::vector<ConsumerEntryPtr> consumers;
	{
		std::scoped_lock lock(s_mutex);
		consumers = s_consumers;
	}
	for (const ConsumerEntryPtr& entry : consumers)
	{
		if (entry->consumer.wantsMouseInput && entry->consumer.wantsMouseInput())
			return true;
	}
	return false;
}

bool OverlayWantsKeyboardInput()
{
	std::vector<ConsumerEntryPtr> consumers;
	{
		std::scoped_lock lock(s_mutex);
		consumers = s_consumers;
	}
	for (const ConsumerEntryPtr& entry : consumers)
	{
		if (entry->consumer.wantsKeyboardInput && entry->consumer.wantsKeyboardInput())
			return true;
	}
	return false;
}

bool OverlayConsumeKeyboardInput()
{
	std::vector<ConsumerEntryPtr> consumers;
	{
		std::scoped_lock lock(s_mutex);
		consumers = s_consumers;
	}
	for (const ConsumerEntryPtr& entry : consumers)
	{
		if (entry->consumer.consumeKeyboardInput && entry->consumer.consumeKeyboardInput())
			return true;
	}
	return false;
}

void OverlayClearCapture()
{
	std::scoped_lock lock(s_mutex);
	s_mouseCaptureHandle = 0;
	s_mouseCaptureButtons = 0;
}

} // namespace mq
