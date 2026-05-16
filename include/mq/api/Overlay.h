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

#pragma once

#include "mq/base/PluginHandle.h"

#include <cstdint>
#include <string_view>

namespace mq {

struct OverlayMessage
{
	std::string_view text;
	int color = 0;
	int priority = 0;
	int maxAlpha = 0;
	uint32_t fadeIn = 0;
	uint32_t fadeOut = 0;
	uint32_t displayTime = 0;
};

using OverlayCallback = bool(*)(const OverlayMessage& message, void* userData);
using OverlayHandle = uint64_t;

MQLIB_API OverlayHandle RegisterOverlayCallback(OverlayCallback callback, void* userData = nullptr, MQPluginHandle owner = MQPluginHandle{});
MQLIB_API void UnregisterOverlayCallback(OverlayHandle handle);

// Returns the last "Health: ##%%" overlay percent observed on EZ servers, or -1.f if unknown.
MQLIB_API float GetOverlayHealthPercent();
MQLIB_API void ResetOverlayHealthPercent();

} // namespace mq
