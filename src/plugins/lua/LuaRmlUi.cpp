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

#include "LuaRmlUi.h"
#include "LuaThread.h"

#include "mq/api/RmlUi.h"
#include "mq/base/Config.h"
#include "mq/Plugin.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mq::lua {

class LuaRmlOwner
{
public:
	explicit LuaRmlOwner(mq::RmlOwnerHandle handle) : m_handle(handle) {}
	~LuaRmlOwner() { mq::DestroyRmlOwner(m_handle); }

	mq::RmlOwnerHandle GetHandle() const { return m_handle; }

private:
	mq::RmlOwnerHandle m_handle = 0;
};

namespace {

// Bounds the recursive conversion on the Lua side. The core enforces its own
// depth limit on the stored value as well; this one exists so a pathological or
// accidentally cyclic table is reported here rather than deep in a stack overflow.
constexpr size_t kMaxLuaConversionDepth = 16;

// A record key that is not a string is stringified rather than dropped, so a table
// with numeric keys still converts to something an author can address by name.
std::string ModelKeyName(sol::object key)
{
	if (key.is<std::string>())
		return key.as<std::string>();
	if (key.is<double>())
	{
		const double number = key.as<double>();
		// Integral keys print without a fractional part, which is what a data
		// expression would use to address the field.
		if (number == static_cast<double>(static_cast<long long>(number)))
			return std::to_string(static_cast<long long>(number));
		return std::to_string(number);
	}
	if (key.is<bool>())
		return key.as<bool>() ? "true" : "false";
	return key.as<std::string>();
}

// Converts a Lua value into a model value.
//
// A Lua table can be a record or an array, and the two are told apart the same
// way Lua itself does: a table with contiguous integer keys from 1 through its
// length is an array, anything else is a record. So `{ 'a', 'b' }` becomes an
// array while `{ name = 'a' }` becomes a record, and a mixed table becomes a
// record whose keys are stringified - which is what an author would expect to
// address in RmlUi.
//
// A table can also be self-referential, and a value tree cannot, so the tables
// currently being visited are tracked and a cycle is reported rather than
// recursed into. Repeated references to the same table that are not a cycle
// are still copied, once per appearance.
bool LuaToModelValue(sol::object object, size_t depth, std::vector<const sol::table*>& visiting,
	mq::RmlUiValue& out, std::string& error)
{
	switch (object.get_type())
	{
	case sol::type::nil:
	case sol::type::none:
		out = mq::RmlUiValue();
		return true;
	case sol::type::boolean:
		out = mq::RmlUiValue(object.as<bool>());
		return true;
	case sol::type::number:
		out = mq::RmlUiValue(object.as<double>());
		return true;
	case sol::type::string:
		out = mq::RmlUiValue(object.as<std::string>());
		return true;
	case sol::type::table:
		break;
	default:
		error = "unsupported value type";
		return false;
	}

	if (depth > kMaxLuaConversionDepth)
	{
		error = "value nests deeper than the Lua conversion limit of "
			+ std::to_string(kMaxLuaConversionDepth);
		return false;
	}

	sol::table table = object.as<sol::table>();
	if (std::find(visiting.begin(), visiting.end(), &table) != visiting.end())
	{
		error = "table contains a cycle";
		return false;
	}

	// An array is a table whose integer keys are contiguous from 1.
	const size_t count = table.size();
	bool isArray = count > 0;
	for (size_t index = 1; index <= count && isArray; ++index)
	{
		if (table.get<sol::object>(index).get_type() == sol::type::nil)
			isArray = false;
	}

	visiting.push_back(&table);
	bool succeeded = true;

	if (isArray)
	{
		mq::RmlUiValue array = mq::RmlUiValue::MakeArray();
		for (size_t index = 1; index <= count; ++index)
		{
			mq::RmlUiValue element;
			if (!LuaToModelValue(table.get<sol::object>(index), depth + 1, visiting, element, error))
			{
				succeeded = false;
				break;
			}
			array.AddElement(std::move(element));
		}
		out = std::move(array);
	}
	else
	{
		mq::RmlUiValue record = mq::RmlUiValue::MakeRecord();
		for (const auto& entry : table)
		{
			const auto& key = entry.first;
			mq::RmlUiValue field;
			if (!LuaToModelValue(entry.second, depth + 1, visiting, field, error))
			{
				succeeded = false;
				break;
			}
			record.SetField(ModelKeyName(key), std::move(field));
		}
		out = std::move(record);
	}

	visiting.pop_back();
	return succeeded;
}

// Converts a stored model value back into a Lua value, so a script can observe
// what RmlUi wrote through a two-way binding. Depth is bounded for the same
// reason the Lua-to-value direction is: a value that somehow exceeded the
// stored limits must not turn into an unbounded walk here.
sol::object ModelValueToLua(sol::this_state s, const mq::RmlUiValue& value, size_t depth)
{
	sol::state_view state(s);
	switch (value.GetType())
	{
	case mq::RmlUiValue::Type::Nil:
		return sol::make_object(s, sol::lua_nil);
	case mq::RmlUiValue::Type::Bool:
		return sol::make_object(s, value.AsBool());
	case mq::RmlUiValue::Type::Number:
		return sol::make_object(s, value.AsNumber());
	case mq::RmlUiValue::Type::String:
		return sol::make_object(s, value.AsString());
	default:
		break;
	}

	if (depth >= kMaxLuaConversionDepth)
		return sol::make_object(s, sol::lua_nil);

	const size_t count = value.Size();
	if (value.IsArray())
	{
		sol::table result = state.create_table();
		for (size_t index = 0; index < count; ++index)
		{
			if (const mq::RmlUiValue* element = value.ElementAt(index))
				result[index + 1] = ModelValueToLua(s, *element, depth + 1);
		}
		return sol::make_object(s, result);
	}

	sol::table result = state.create_table();
	value.ForEachField([&](const std::string& key, const mq::RmlUiValue& field)
	{
		result[key] = ModelValueToLua(s, field, depth + 1);
		return true;
	});
	return sol::make_object(s, result);
}

}

// Wraps an opaque RmlDocumentHandle for Lua. The document is unloaded when this
// object is collected, providing script-scoped cleanup.
class LuaRmlDocument : public std::enable_shared_from_this<LuaRmlDocument>
{
public:
	LuaRmlDocument(mq::RmlDocumentHandle handle, std::shared_ptr<LuaRmlOwner> owner,
		LuaThread* thread)
		: m_handle(handle), m_owner(std::move(owner)), m_thread(thread)
	{
	}

	~LuaRmlDocument()
	{
		FlushPersistentGeometry();
		ClearListeners();
		if (m_handle != 0)
			mq::UnloadRmlDocument(m_handle);
	}

	void EnablePersistence(std::string key, std::string elementId)
	{
		m_persistenceKey = std::move(key);
		m_geometryElement = std::move(elementId);

		const std::string value = mq::GetPrivateProfileString("LuaWindows",
			m_persistenceKey, "", PersistenceFile());
		mq::RmlElementGeometry geometry;
		int locked = -1;
		const int fields = ::sscanf_s(value.c_str(), "%f,%f,%f,%f,%d", &geometry.x,
			&geometry.y, &geometry.width, &geometry.height, &locked);
		if (fields >= 4)
		{
			mq::RmlDocumentSetGeometry(m_handle, m_geometryElement, geometry);
		}
		if (fields == 5)
			SetLocked(locked != 0);

		if (mq::RmlDocumentGetGeometry(m_handle, m_geometryElement, m_lastGeometry))
			m_haveLastGeometry = true;
	}

	void PollPersistence()
	{
		// Template-owned elements may not exist until RmlUi has completed its
		// first update. Lock application is therefore retried independently of
		// geometry persistence instead of making rml.open() fail synchronously.
		ApplyPendingLock();
		if (m_persistenceKey.empty() || m_handle == 0)
			return;

		mq::RmlElementGeometry geometry;
		if (!mq::RmlDocumentGetGeometry(m_handle, m_geometryElement, geometry))
			return;
		if (!m_haveLastGeometry)
		{
			m_lastGeometry = geometry;
			m_haveLastGeometry = true;
			return;
		}

		if (!SameGeometry(geometry, m_lastGeometry))
		{
			m_lastGeometry = geometry;
			m_geometryDirty = true;
			m_lastGeometryChange = std::chrono::steady_clock::now();
		}
		else if (m_geometryDirty
			&& std::chrono::steady_clock::now() - m_lastGeometryChange >= PersistenceDelay)
		{
			FlushPersistentGeometry();
		}
	}

	bool ConfigureLocking(std::string elementId, bool locked)
	{
		if (m_handle == 0 || elementId.empty())
			return false;
		m_lockElement = std::move(elementId);
		m_locked = locked;
		m_lockPending = true;
		ApplyPendingLock();
		return true;
	}

	bool SetLocked(bool locked)
	{
		if (m_handle == 0 || m_lockElement.empty())
			return false;
		if (m_locked != locked && !m_persistenceKey.empty())
		{
			m_geometryDirty = true;
			m_lastGeometryChange = std::chrono::steady_clock::now();
		}
		m_locked = locked;
		m_lockPending = true;
		return ApplyPendingLock();
	}

	bool IsLocked() const { return m_locked; }

	sol::object On(std::string_view eventType, std::string_view elementId, sol::function callback, sol::this_state s)
	{
		if (m_handle == 0 || !callback.valid())
			return sol::lua_nil;
		const std::shared_ptr<LuaThread> thread = LuaThread::get_from(s);
		if (!thread)
			return sol::lua_nil;

		auto registration = std::make_unique<EventRegistration>();
		registration->owner = this;
		// A plain sol::function retains the lua_State pointer on which it was
		// received. That may be a short-lived coroutine even though the registry
		// entry belongs to the script's VM. Anchor both the function and protected
		// call error handler to the main state so teardown can safely unref them.
		registration->callback = sol::main_protected_function(
			thread->GetState().lua_state(), callback);
		const mq::RmlListenerHandle listener = mq::AddRmlDocumentEventListener(m_handle,
			eventType, elementId, &LuaRmlDocument::HandleEvent, registration.get());
		if (listener == 0)
			return sol::lua_nil;

		registration->listener = listener;
		m_listeners.emplace(listener, std::move(registration));
		return sol::make_object(s, listener);
	}

	bool Off(mq::RmlListenerHandle listener)
	{
		auto iter = m_listeners.find(listener);
		if (iter == m_listeners.end())
			return false;

		iter->second->owner = nullptr;
		mq::RemoveRmlDocumentEventListener(listener);
		m_listeners.erase(iter);
		m_pendingEvents.erase(std::remove_if(m_pendingEvents.begin(), m_pendingEvents.end(),
			[listener](const PendingEvent& item) { return item.listener == listener; }), m_pendingEvents.end());
		return true;
	}

	sol::table EventStats(sol::this_state s) const
	{
		sol::state_view state(s);
		sol::table result = state.create_table();
		result["listeners"] = m_listeners.size();
		result["pending"] = m_pendingEvents.size();
		result["dropped"] = m_droppedEvents;
		result["coalesced"] = m_coalescedEvents;
		result["callback_errors"] = m_callbackErrors;
		result["capacity"] = MaxPendingEvents;
		return result;
	}

	void ProcessEvents()
	{
		std::vector<PendingEvent> pending;
		pending.swap(m_pendingEvents);
		for (const PendingEvent& item : pending)
		{
			auto iter = m_listeners.find(item.listener);
			if (iter == m_listeners.end())
				continue;

			EventRegistration& registration = *iter->second;
			sol::state_view state(registration.callback.lua_state());
			sol::table event = state.create_table();
			event["type"] = item.event.type;
			event["target_id"] = item.event.targetId;
			event["current_id"] = item.event.currentId;
			event["target_tag"] = item.event.targetTag;
			event["target_classes"] = item.event.targetClasses;
			event["target_value"] = item.event.targetValue;
			event["current_value"] = item.event.currentValue;
			event["action_id"] = item.event.actionId;
			event["action_value"] = item.event.actionValue;
			event["value"] = item.event.value;
			event["checked"] = item.event.checked;
			event["mouse_x"] = item.event.mouseX;
			event["mouse_y"] = item.event.mouseY;
			event["handle_x"] = item.event.handleX;
			event["handle_y"] = item.event.handleY;
			event["button"] = item.event.button;
			event["key_identifier"] = item.event.keyIdentifier;
			event["wheel_delta"] = item.event.wheelDelta;
			event["ctrl_key"] = item.event.ctrlKey;
			event["shift_key"] = item.event.shiftKey;
			event["alt_key"] = item.event.altKey;
			event["meta_key"] = item.event.metaKey;

			bool failed = false;
			try
			{
				std::shared_ptr<LuaThread> thread = LuaThread::get_from(state);
				ScopedYieldDisabler disableYield(thread);
				sol::main_protected_function callback = registration.callback;
				sol::protected_function_result result = callback(event);
				if (!result.valid())
				{
					sol::error error = result;
					LuaError("RmlUi callback failed (script=%s pid=%d document=%llu listener=%llu event=%s target=%s): %s",
						thread ? thread->GetName().c_str() : "unknown", thread ? thread->GetPID() : 0,
						static_cast<unsigned long long>(m_handle), static_cast<unsigned long long>(item.listener),
						item.event.type.c_str(), item.event.targetId.c_str(), error.what());
					result.abandon();
					failed = true;
				}
			}
			catch (const std::exception& error)
			{
				LuaError("RmlUi callback threw (document=%llu listener=%llu event=%s target=%s): %s",
					static_cast<unsigned long long>(m_handle), static_cast<unsigned long long>(item.listener),
					item.event.type.c_str(), item.event.targetId.c_str(), error.what());
				failed = true;
			}
			catch (...)
			{
				LuaError("RmlUi callback threw an unknown exception (document=%llu listener=%llu event=%s target=%s)",
					static_cast<unsigned long long>(m_handle), static_cast<unsigned long long>(item.listener),
					item.event.type.c_str(), item.event.targetId.c_str());
				failed = true;
			}

			if (failed)
			{
				++m_callbackErrors;
				// Disable a failing subscription so a high-frequency event cannot flood chat
				// or repeatedly execute a callback that has already demonstrated failure.
				Off(item.listener);
			}
		}
	}

	void Show() { mq::SetRmlDocumentVisible(m_handle, true); }
	void Hide() { mq::SetRmlDocumentVisible(m_handle, false); }
	bool BringToFront() { return m_handle != 0 && mq::BringRmlDocumentToFront(m_handle); }

	bool Reload()
	{
		if (m_handle == 0)
			return false;
		mq::RmlElementGeometry geometry;
		const bool preserveGeometry = !m_persistenceKey.empty()
			&& mq::RmlDocumentGetGeometry(m_handle, m_geometryElement, geometry);
		if (!mq::ReloadRmlDocument(m_handle))
			return false;
		if (preserveGeometry)
		{
			mq::RmlDocumentSetGeometry(m_handle, m_geometryElement, geometry);
			m_lastGeometry = geometry;
			m_haveLastGeometry = true;
		}
		m_lockPending = true;
		ApplyPendingLock();
		return true;
	}

	void Close()
	{
		if (m_handle != 0)
		{
			FlushPersistentGeometry();
			ClearListeners();
			mq::UnloadRmlDocument(m_handle);
			m_handle = 0;
		}
	}

	bool IsValid() const { return m_handle != 0 && mq::RmlDocumentIsLoaded(m_handle); }
	bool IsOwnedBy(const LuaThread* thread) const { return m_thread == thread; }

	bool SetText(std::string_view elementId, std::string_view text)
	{
		return m_handle != 0 && mq::RmlDocumentSetText(m_handle, elementId, text);
	}

	bool SetValue(std::string_view elementId, std::string_view value)
	{
		return m_handle != 0 && mq::RmlDocumentSetValue(m_handle, elementId, value);
	}

	bool SetClass(std::string_view elementId, std::string_view className, bool activate)
	{
		return m_handle != 0 && mq::RmlDocumentSetClass(m_handle, elementId, className, activate);
	}

	bool SetStyle(std::string_view elementId, std::string_view property, std::string_view value)
	{
		return m_handle != 0 && mq::RmlDocumentSetStyle(m_handle, elementId, property, value);
	}

	bool SetAttribute(std::string_view elementId, std::string_view attribute, std::string_view value)
	{
		return m_handle != 0 && mq::RmlDocumentSetAttribute(m_handle, elementId, attribute, value);
	}

	bool RemoveAttribute(std::string_view elementId, std::string_view attribute)
	{
		return m_handle != 0 && mq::RmlDocumentRemoveAttribute(m_handle, elementId, attribute);
	}

	sol::object GetAttribute(std::string_view elementId, std::string_view attribute, sol::this_state s)
	{
		std::string value;
		if (m_handle == 0 || !mq::RmlDocumentGetAttribute(m_handle, elementId, attribute, value))
			return sol::make_object(s, sol::lua_nil);
		return sol::make_object(s, std::move(value));
	}

	bool Focus(std::string_view elementId)
	{
		return m_handle != 0 && mq::RmlDocumentFocusElement(m_handle, elementId);
	}

	bool Blur(std::string_view elementId)
	{
		return m_handle != 0 && mq::RmlDocumentBlurElement(m_handle, elementId);
	}

	sol::object GetGeometry(std::string_view elementId, sol::this_state s)
	{
		mq::RmlElementGeometry geometry;
		if (m_handle == 0 || !mq::RmlDocumentGetGeometry(m_handle, elementId, geometry))
			return sol::make_object(s, sol::lua_nil);

		sol::state_view state(s);
		sol::table result = state.create_table();
		result["x"] = geometry.x;
		result["y"] = geometry.y;
		result["width"] = geometry.width;
		result["height"] = geometry.height;
		return sol::make_object(s, result);
	}

	bool SetGeometry(std::string_view elementId, sol::table value)
	{
		if (m_handle == 0)
			return false;
		mq::RmlElementGeometry geometry;
		geometry.x = value.get_or("x", 0.0f);
		geometry.y = value.get_or("y", 0.0f);
		geometry.width = value.get_or("width", 0.0f);
		geometry.height = value.get_or("height", 0.0f);
		return mq::RmlDocumentSetGeometry(m_handle, elementId, geometry);
	}

	sol::object GetScroll(std::string_view elementId, sol::this_state s)
	{
		mq::RmlScrollMetrics metrics;
		if (m_handle == 0 || !mq::RmlDocumentGetScrollMetrics(m_handle, elementId, metrics))
			return sol::make_object(s, sol::lua_nil);

		sol::state_view state(s);
		sol::table result = state.create_table();
		result["offset"] = metrics.offset;
		result["extent"] = metrics.extent;
		result["viewport"] = metrics.viewport;
		result["max_offset"] = std::max(0.0f, metrics.extent - metrics.viewport);
		result["at_end"] = metrics.extent - metrics.viewport - metrics.offset <= 2.0f;
		return sol::make_object(s, result);
	}

	bool SetScroll(std::string_view elementId, float offset)
	{
		return m_handle != 0 && mq::RmlDocumentSetScrollOffset(m_handle, elementId, offset);
	}

	bool ScrollToEnd(std::string_view elementId)
	{
		return m_handle != 0 && mq::RmlDocumentScrollToEnd(m_handle, elementId);
	}

	bool SetScrollAnchor(std::string_view elementId, bool enabled, sol::optional<float> threshold)
	{
		return m_handle != 0 && mq::RmlDocumentSetScrollAnchor(m_handle, elementId,
			enabled, threshold.value_or(2.0f));
	}

	// Sets model variables from a Lua table. Values may be scalars, arrays, or
	// nested records in any combination; the table's shape decides the model
	// value's type, so no separate call is needed per type.
	//
	// The model must already have been declared, either through the "models"
	// option on rmlui.open or implicitly by an earlier call. A model that has
	// not been declared by the time the document parses will not be visible to
	// that document.
	bool SetModel(std::string_view model, sol::table data)
	{
		if (m_handle == 0)
			return false;

		bool succeeded = true;
		for (const auto& entry : data)
		{
			const std::string name = ModelKeyName(entry.first);
			mq::RmlUiValue converted;
			std::string error;
			std::vector<const sol::table*> visiting;
			if (!LuaToModelValue(entry.second, 1, visiting, converted, error))
			{
				LuaError("RmlUi model '%.*s.%s' could not be converted: %s",
					static_cast<int>(model.size()), model.data(), name.c_str(), error.c_str());
				succeeded = false;
				continue;
			}

			succeeded &= mq::RmlDocumentSetModelValue(m_handle, model, name, converted);
		}
		return succeeded;
	}

	// Lua uses one-based indices. start may be #array + 1 to append.
	bool SpliceModel(std::string_view model, std::string_view variable, size_t start,
		size_t deleteCount, sol::table values)
	{
		if (m_handle == 0 || start == 0)
			return false;

		std::vector<mq::RmlUiValue> convertedValues;
		convertedValues.reserve(values.size());
		for (size_t index = 1; index <= values.size(); ++index)
		{
			mq::RmlUiValue converted;
			std::string error;
			std::vector<const sol::table*> visiting;
			if (!LuaToModelValue(values.get<sol::object>(index), 1, visiting, converted, error))
			{
				LuaError("RmlUi splice '%.*s.%.*s' value %zu could not be converted: %s",
					static_cast<int>(model.size()), model.data(),
					static_cast<int>(variable.size()), variable.data(), index, error.c_str());
				return false;
			}
			convertedValues.push_back(std::move(converted));
		}

		return mq::RmlDocumentSpliceModelArray(m_handle, model, variable,
			start - 1, deleteCount, convertedValues);
	}

	// Reads a variable back out, which is how a value written by RmlUi through a
	// two-way binding (an input's "value" attribute, for example) is observed.
	// Must be called from the main thread; the core refuses reads from anywhere
	// else rather than queueing a read with no completion path.
	sol::object GetModel(std::string_view model, std::string_view variable, sol::this_state s)
	{
		mq::RmlUiValue value;
		if (m_handle == 0 || !mq::RmlDocumentGetModelValue(m_handle, model, variable, value))
			return sol::make_object(s, sol::lua_nil);

		return ModelValueToLua(s, value, 1);
	}

private:
	static constexpr auto PersistenceDelay = std::chrono::milliseconds(500);

	static std::string PersistenceFile()
	{
		return (std::filesystem::path(gPathConfig) / "RmlUiWindows.ini").string();
	}

	static bool SameGeometry(const mq::RmlElementGeometry& lhs,
		const mq::RmlElementGeometry& rhs)
	{
		constexpr float epsilon = 0.25f;
		return std::abs(lhs.x - rhs.x) < epsilon && std::abs(lhs.y - rhs.y) < epsilon
			&& std::abs(lhs.width - rhs.width) < epsilon
			&& std::abs(lhs.height - rhs.height) < epsilon;
	}

	bool ApplyPendingLock()
	{
		if (!m_lockPending)
			return true;
		if (m_handle == 0 || m_lockElement.empty()
			|| !mq::RmlDocumentSetClass(m_handle, m_lockElement, "mq-locked", m_locked))
		{
			return false;
		}
		m_lockPending = false;
		return true;
	}

	void FlushPersistentGeometry()
	{
		if (m_persistenceKey.empty())
			return;
		mq::RmlElementGeometry geometry;
		if (m_handle != 0 && mq::RmlDocumentGetGeometry(m_handle, m_geometryElement, geometry)
			&& (!m_haveLastGeometry || !SameGeometry(geometry, m_lastGeometry)))
		{
			m_lastGeometry = geometry;
			m_haveLastGeometry = true;
			m_geometryDirty = true;
		}
		if (!m_geometryDirty || !m_haveLastGeometry)
			return;
		if (mq::WritePrivateProfileString("LuaWindows", m_persistenceKey,
			fmt::format("{:.1f},{:.1f},{:.1f},{:.1f},{}", m_lastGeometry.x,
				m_lastGeometry.y, m_lastGeometry.width, m_lastGeometry.height, m_locked ? 1 : 0),
			PersistenceFile()))
		{
			m_geometryDirty = false;
		}
	}

	struct EventRegistration
	{
		LuaRmlDocument* owner = nullptr;
		mq::RmlListenerHandle listener = 0;
		sol::main_protected_function callback;
	};

	struct PendingEvent
	{
		mq::RmlListenerHandle listener = 0;
		mq::RmlEvent event;
	};

	static void HandleEvent(const mq::RmlEvent& event, void* userData)
	{
		auto* registration = static_cast<EventRegistration*>(userData);
		if (registration && registration->owner)
			registration->owner->QueueEvent(registration->listener, event);
	}

	void QueueEvent(mq::RmlListenerHandle listener, const mq::RmlEvent& event)
	{
		const bool coalescible = event.type == "mousemove" || event.type == "drag"
			|| event.type == "dragmove" || event.type == "handledrag";
		if (coalescible)
		{
			for (auto iter = m_pendingEvents.rbegin(); iter != m_pendingEvents.rend(); ++iter)
			{
				if (iter->listener == listener && iter->event.type == event.type)
				{
					iter->event = event;
					++m_coalescedEvents;
					return;
				}
			}
		}

		if (m_pendingEvents.size() >= MaxPendingEvents)
		{
			m_pendingEvents.erase(m_pendingEvents.begin());
			++m_droppedEvents;
		}
		m_pendingEvents.push_back({ listener, event });
	}

	void ClearListeners()
	{
		for (const auto& [handle, registration] : m_listeners)
		{
			registration->owner = nullptr;
			mq::RemoveRmlDocumentEventListener(handle);
		}
		m_listeners.clear();
		m_pendingEvents.clear();
	}

	mq::RmlDocumentHandle m_handle = 0;
	std::shared_ptr<LuaRmlOwner> m_owner;
	LuaThread* m_thread = nullptr;
	static constexpr size_t MaxPendingEvents = 256;
	std::unordered_map<mq::RmlListenerHandle, std::unique_ptr<EventRegistration>> m_listeners;
	std::vector<PendingEvent> m_pendingEvents;
	uint64_t m_droppedEvents = 0;
	uint64_t m_coalescedEvents = 0;
	uint64_t m_callbackErrors = 0;
	std::string m_persistenceKey;
	std::string m_geometryElement = "window";
	mq::RmlElementGeometry m_lastGeometry;
	std::chrono::steady_clock::time_point m_lastGeometryChange;
	bool m_haveLastGeometry = false;
	bool m_geometryDirty = false;
	std::string m_lockElement = "window";
	bool m_locked = false;
	bool m_lockPending = false;
};

namespace {

std::vector<std::weak_ptr<LuaRmlDocument>> s_documents;

std::string PersistenceKey(std::string_view script, std::string_view id)
{
	std::string key;
	key.reserve(script.size() + id.size() + 1);
	for (char ch : fmt::format("{}.{}", script, id))
	{
		const unsigned char value = static_cast<unsigned char>(ch);
		key.push_back(std::isalnum(value) || ch == '_' || ch == '-' || ch == '.' ? ch : '_');
	}
	return key;
}

sol::object RmlOpen(const std::shared_ptr<LuaRmlOwner>& owner, sol::table options, sol::this_state s)
{
	if (!owner || owner->GetHandle() == 0)
		return sol::lua_nil;
	const std::string document = options.get_or("document", std::string{});
	if (document.empty())
		return sol::lua_nil;
	const bool persist = options.get_or("persist", false);
	const std::string persistenceId = options.get_or("id", std::string{});
	const std::string geometryElement = options.get_or("geometry_element", std::string("window"));
	const std::string lockElement = options.get_or("lock_element", geometryElement);
	if (persist && (persistenceId.empty() || persistenceId.size() > 128
		|| geometryElement.empty() || geometryElement.size() > 128))
	{
		LuaError("RmlUi: persist=true requires non-empty id and geometry_element values of at most 128 characters");
		return sol::lua_nil;
	}
	if (lockElement.empty() || lockElement.size() > 128)
	{
		LuaError("RmlUi: lock_element must be non-empty and at most 128 characters");
		return sol::lua_nil;
	}

	mq::RmlDocumentOptions rmlOptions;

	if (auto presentation = options.get<sol::optional<std::string>>("presentation"); presentation && *presentation == "hosted")
	{
		// The core rejects a hosted document, so say so here rather than letting
		// open() return nil with no explanation.
		LuaError("RmlUi: presentation 'hosted' is not implemented yet; use the default direct presentation");
		return sol::lua_nil;
	}

	if (auto input = options.get<sol::optional<std::string>>("input"); input && *input == "clickthrough")
		rmlOptions.input = mq::RmlDocumentOptions::InputMode::ClickThrough;

	rmlOptions.layer = options.get_or("layer", 0);
	rmlOptions.visible = options.get_or("visible", true);

	// Models must be declared before the document parses, because RmlUi resolves
	// a document's data-model attributes while parsing. Declaring them here is what
	// lets a model's first update work without reloading the document.
	//
	// Accepts either a single name or an array of names, so a document with one
	// model does not need a wrapper table.
	if (auto single = options.get<sol::optional<std::string>>("model"); single && !single->empty())
		rmlOptions.models.push_back(*single);
	if (auto many = options.get<sol::optional<sol::table>>("models"); many)
	{
		sol::table models = *many;
		const size_t count = models.size();
		for (size_t index = 1; index <= count; ++index)
		{
			if (auto name = models.get<sol::optional<std::string>>(index); name && !name->empty())
				rmlOptions.models.push_back(*name);
		}
	}

	// RmlUi resolves variable names while parsing, not merely the model name.
	// Derive the pre-parse schema from the initial-value tables so expressions
	// such as {{ name }} are bound before LoadDocument reads the markup.
	if (auto initial = options.get<sol::optional<sol::table>>("initial"); initial)
	{
		for (const auto& modelEntry : *initial)
		{
			if (!modelEntry.first.is<std::string>() || modelEntry.second.get_type() != sol::type::table)
				continue;
			const std::string modelName = modelEntry.first.as<std::string>();
			auto& variables = rmlOptions.modelVariables[modelName];
			for (const auto& variableEntry : modelEntry.second.as<sol::table>())
			{
				if (variableEntry.first.is<std::string>())
					variables.push_back(variableEntry.first.as<std::string>());
			}
		}
	}

	const mq::RmlDocumentHandle handle = mq::LoadRmlDocumentForOwner(
		document, rmlOptions, owner->GetHandle());
	if (handle == 0)
		return sol::lua_nil;

	const std::shared_ptr<LuaThread> thread = LuaThread::get_from(s);
	auto result = std::make_shared<LuaRmlDocument>(handle, owner, thread.get());
	s_documents.emplace_back(result);
	if (!result->ConfigureLocking(lockElement, options.get_or("locked", false)))
	{
		LuaError("RmlUi: lock_element '%s' could not be configured", lockElement.c_str());
		return sol::lua_nil;
	}

	if (persist)
	{
		const std::string script = thread ? thread->GetName() : "unknown";
		result->EnablePersistence(PersistenceKey(script, persistenceId), geometryElement);
	}

	// Apply any initial values now, after the models were declared and the
	// documents parsed. Doing this here rather than from the script's next line
	// means the document's first update already sees them, so nothing has to
	// reload or rebuild in order to pick up the initial state.
	//
	// `initial` maps a model name to that model's variables, so a document with
	// several models can seed all of them in one call.
	if (auto initial = options.get<sol::optional<sol::table>>("initial"); initial)
	{
		for (const auto& entry : *initial)
		{
			if (!entry.first.is<std::string>())
				continue;
			if (entry.second.get_type() != sol::type::table)
				continue;

			result->SetModel(entry.first.as<std::string>(), entry.second.as<sol::table>());
		}
	}

	return sol::make_object(s, std::move(result));
}

} // namespace

sol::table LuaRmlUi::RegisterLua(sol::state_view s)
{
	if (std::shared_ptr<LuaThread> thread = LuaThread::get_from(s))
		mq::SetRmlUiLuaDirectory(thread->GetLuaDir());

	auto rml = s.create_table();
	auto owner = std::make_shared<LuaRmlOwner>(mq::CreateRmlOwner(mqplugin::ThisPluginHandle));

	rml.new_usertype<LuaRmlDocument>("document", sol::no_constructor,
		"show", &LuaRmlDocument::Show,
		"hide", &LuaRmlDocument::Hide,
		"bring_to_front", &LuaRmlDocument::BringToFront,
		"reload", &LuaRmlDocument::Reload,
		"close", &LuaRmlDocument::Close,
		"is_valid", &LuaRmlDocument::IsValid,
		"set_text", &LuaRmlDocument::SetText,
		"set_value", &LuaRmlDocument::SetValue,
		"set_class", &LuaRmlDocument::SetClass,
			"set_style", &LuaRmlDocument::SetStyle,
			"set_attribute", &LuaRmlDocument::SetAttribute,
			"remove_attribute", &LuaRmlDocument::RemoveAttribute,
			"get_attribute", &LuaRmlDocument::GetAttribute,
			"focus", &LuaRmlDocument::Focus,
			"blur", &LuaRmlDocument::Blur,
			"get_geometry", &LuaRmlDocument::GetGeometry,
			"set_geometry", &LuaRmlDocument::SetGeometry,
			"get_scroll", &LuaRmlDocument::GetScroll,
			"set_scroll", &LuaRmlDocument::SetScroll,
			"scroll_to_end", &LuaRmlDocument::ScrollToEnd,
			"set_scroll_anchor", &LuaRmlDocument::SetScrollAnchor,
			"set_locked", &LuaRmlDocument::SetLocked,
			"is_locked", &LuaRmlDocument::IsLocked,
			"on", &LuaRmlDocument::On,
			"off", &LuaRmlDocument::Off,
			"event_stats", &LuaRmlDocument::EventStats,
			"model", &LuaRmlDocument::SetModel,
			"splice_model", &LuaRmlDocument::SpliceModel,
		"get_model", &LuaRmlDocument::GetModel);

	rml.set_function("open", [owner](sol::table options, sol::this_state state)
	{
		return RmlOpen(owner, std::move(options), state);
	});

	return rml;
}

void LuaRmlUi::Pulse()
{
	for (auto iter = s_documents.begin(); iter != s_documents.end();)
	{
		if (std::shared_ptr<LuaRmlDocument> document = iter->lock())
		{
			document->ProcessEvents();
			document->PollPersistence();
			++iter;
		}
		else
		{
			iter = s_documents.erase(iter);
		}
	}
}

void LuaRmlUi::Shutdown(LuaThread* thread)
{
	// Release every registry-backed callback while the owning Lua state is still
	// alive. Leaving these references attached until (or beyond) lua_close makes
	// a later Pulse() copy a sol::protected_function from a dead registry.
	for (auto iter = s_documents.begin(); iter != s_documents.end();)
	{
		if (std::shared_ptr<LuaRmlDocument> document = iter->lock())
		{
			if (document->IsOwnedBy(thread))
			{
				document->Close();
				iter = s_documents.erase(iter);
			}
			else
			{
				++iter;
			}
		}
		else
		{
			iter = s_documents.erase(iter);
		}
	}
}

} // namespace mq::lua
