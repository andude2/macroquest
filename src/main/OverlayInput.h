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
#include <functional>
#include <vector>

namespace mq {

// Overlay input is arbitrated by a neutral broker so that neither ImGui nor
// RmlUi (nor EverQuest) independently decides what receives an event. Consumers
// are dispatched in ascending layer order; a lower layer number wins.
enum class OverlayInputLayer
{
	// ImGui is rendered after direct RmlUi documents, so it must receive input
	// first wherever the two presentation layers overlap.
	ImGui = 0,
	RmlUi = 1,
};

// A participant in overlay input arbitration. All callbacks are invoked from the
// UI thread. Consumers are responsible for their own internal capture state; the
// broker only guarantees deterministic ordering and mouse capture across the
// duration of a button press.
struct OverlayInputConsumer
{
	OverlayInputLayer layer = OverlayInputLayer::RmlUi;
	MQPluginHandle owner;

	// Win32 message handler. Returns true if the message was consumed.
	std::function<bool(HWND, UINT, WPARAM, LPARAM)> handleWndProc;

	// Returns true if this consumer currently wants the mouse (hovering or
	// interacting). Used by the DirectInput mouse hook to decide whether to
	// suppress the EQ mouse.
	std::function<bool()> wantsMouseInput;

	// Returns true if this consumer currently wants the keyboard/text input.
	std::function<bool()> wantsKeyboardInput;

	// Returns true if this consumer should consume buffered DirectInput keyboard
	// state for the current frame.
	std::function<bool()> consumeKeyboardInput;
};

using OverlayInputHandle = uint64_t;

// Register an overlay input consumer. Returns a non-zero handle on success.
OverlayInputHandle RegisterOverlayInputConsumer(OverlayInputConsumer consumer);

// Remove a previously registered consumer. Safe to call with an invalid handle.
void UnregisterOverlayInputConsumer(OverlayInputHandle handle);

// Remove all consumers owned by a plugin. Called automatically during plugin
// unload; owners with pluginID == 0 are never removed here.
void RemoveOverlayInputConsumersForPlugin(MQPluginHandle owner);

// Central WndProc dispatch. Routes the message to the topmost eligible consumer
// (honoring mouse capture), returning true if a consumer consumed it.
bool OverlayWndProcDispatch(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Queries used by the DirectInput hooks to decide whether to suppress EQ input.
// These are OR-combinations across all registered consumers, matching the
// pre-broker behavior.
bool OverlayWantsMouseInput();
bool OverlayWantsKeyboardInput();
bool OverlayConsumeKeyboardInput();

// Clears mouse capture and keyboard focus. Invoked on focus loss, UI disable,
// and device reset.
void OverlayClearCapture();

} // namespace mq
