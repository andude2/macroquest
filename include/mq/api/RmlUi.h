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

#include "mq/base/Common.h"
#include "mq/base/PluginHandle.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace mq {

// Opaque handle to an RmlUi document owned by MQ. Handles are unique for the
// lifetime of the process and never expose the underlying Rml::ElementDocument*.
using RmlDocumentHandle = uint64_t;
using RmlListenerHandle = uint64_t;
using RmlOwnerHandle = uint64_t;

struct RmlEvent
{
	std::string type;
	std::string targetId;
	std::string currentId;
	std::string targetTag;
	std::string targetClasses;
	// Optional opaque payload copied from the target element's data-mq-value
	// attribute. This supports delegated events for repeated/data-bound elements.
	std::string targetValue;
	std::string currentValue;
	// Nearest event-target ancestor (up to the listener element) carrying a
	// data-mq-value attribute. This makes complex delegated rows/buttons behave
	// as one action even when the click lands on a nested icon or label.
	std::string actionId;
	std::string actionValue;
	std::string value;
	bool checked = false;
	int mouseX = 0;
	int mouseY = 0;
	float handleX = 0.0f;
	float handleY = 0.0f;
	int button = -1;
	int keyIdentifier = 0;
	float wheelDelta = 0.0f;
	bool ctrlKey = false;
	bool shiftKey = false;
	bool altKey = false;
	bool metaKey = false;
};

struct RmlElementGeometry
{
	float x = 0.0f;
	float y = 0.0f;
	float width = 0.0f;
	float height = 0.0f;
};

struct RmlScrollMetrics
{
	float offset = 0.0f;
	float extent = 0.0f;
	float viewport = 0.0f;
};

using RmlEventCallback = void(*)(const RmlEvent& event, void* userData);

// Present as a value type so callers never hold onto RmlUi types.
struct RmlDocumentOptions
{
	enum class Presentation { Direct, Hosted } presentation = Presentation::Direct;
	enum class InputMode { Interactive, ClickThrough } input = InputMode::Interactive;

	int layer = 0;
	bool visible = true;

	// Data models to declare on the document's context before it parses.
	// RmlUi binds data-model attributes at parse time, so a model that is
	// created after the document loads will not be seen by that document.
	// Declaring here means the first update of a model needs no reload.
	std::vector<std::string> models;

	// Variable names to bind before document parsing. RmlUi resolves not only
	// the model but each referenced variable while parsing {{ expressions }} and
	// data-* attributes. Values can be supplied immediately after loading, but
	// the names must exist first or the source expression remains literal text.
	std::map<std::string, std::vector<std::string>> modelVariables;
};

// Load a new document instance rooted at the RmlUi resource root. Multiple calls
// for the same path return independent handles. The requested
// path must be a relative .rml path; absolute paths, traversal, and device paths
// are rejected. Documents are owned by MQ and destroyed when unloaded, when their
// owning plugin unloads, or when MQ shuts down.
//
// Returns a non-zero handle on success, or 0 on failure. Calls from a worker
// thread reserve a handle and queue loading onto MacroQuest's main thread, so
// RmlDocumentIsLoaded may remain false until a later pulse.
MQLIB_API RmlDocumentHandle LoadRmlDocument(
	std::string_view virtualPath,
	const RmlDocumentOptions& options = {},
	MQPluginHandle owner = MQPluginHandle{});

// Scoped owners allow a host plugin to isolate resources belonging to a
// shorter-lived consumer such as an individual Lua script.
MQLIB_API RmlOwnerHandle CreateRmlOwner(MQPluginHandle parent);
MQLIB_API void DestroyRmlOwner(RmlOwnerHandle owner);
MQLIB_API RmlDocumentHandle LoadRmlDocumentForOwner(std::string_view virtualPath,
	const RmlDocumentOptions& options, RmlOwnerHandle owner);

// Unload a previously loaded document. Safe to call with an invalid handle.
MQLIB_API void UnloadRmlDocument(RmlDocumentHandle handle);

// Reload a document from its source. If parsing fails, the previous document is
// retained. Returns true if the document was successfully reloaded.
MQLIB_API bool ReloadRmlDocument(RmlDocumentHandle handle);

// Returns true if the handle refers to a currently loaded document.
MQLIB_API bool RmlDocumentIsLoaded(RmlDocumentHandle handle);

// Show or hide a document. Returns true if the handle is valid.
MQLIB_API bool SetRmlDocumentVisible(RmlDocumentHandle handle, bool visible);

// Bring a document in front of other documents on the same explicit layer.
// Layer remains authoritative: this never promotes a document across layers.
MQLIB_API bool BringRmlDocumentToFront(RmlDocumentHandle handle);

// DOM mutations. These operate on a named element (by id) within the document.
// Worker-thread calls copy their arguments and queue the mutation onto
// MacroQuest's main thread. In that case, true means the mutation was accepted
// for delivery; main-thread calls return false if the handle or element is invalid.
//
// Set the text content of an element.
MQLIB_API bool RmlDocumentSetText(RmlDocumentHandle handle, std::string_view elementId, std::string_view text);

// Set the value of an input/textarea/select element (its "value" attribute).
MQLIB_API bool RmlDocumentSetValue(RmlDocumentHandle handle, std::string_view elementId, std::string_view value);

// Add or remove a class on an element.
MQLIB_API bool RmlDocumentSetClass(RmlDocumentHandle handle, std::string_view elementId, std::string_view className, bool activate);

// Set an inline style property (e.g. property="color", value="#ff0000").
MQLIB_API bool RmlDocumentSetStyle(RmlDocumentHandle handle, std::string_view elementId, std::string_view property, std::string_view value);

// Attribute and focus control for stateful components. Attribute reads are
// main-thread only; writes and focus changes use the normal queued mutation
// behavior when called from a worker thread.
MQLIB_API bool RmlDocumentSetAttribute(RmlDocumentHandle handle, std::string_view elementId,
	std::string_view attribute, std::string_view value);
MQLIB_API bool RmlDocumentRemoveAttribute(RmlDocumentHandle handle, std::string_view elementId,
	std::string_view attribute);
MQLIB_API bool RmlDocumentGetAttribute(RmlDocumentHandle handle, std::string_view elementId,
	std::string_view attribute, std::string& value);
MQLIB_API bool RmlDocumentFocusElement(RmlDocumentHandle handle, std::string_view elementId);
MQLIB_API bool RmlDocumentBlurElement(RmlDocumentHandle handle, std::string_view elementId);

// Read or restore an element's border-box position and content-box size. Writes
// clamp the rectangle to a reachable portion of the current viewport. Reads are
// main-thread only; writes follow the normal queued mutation behavior.
MQLIB_API bool RmlDocumentGetGeometry(RmlDocumentHandle handle, std::string_view elementId,
	RmlElementGeometry& geometry);
MQLIB_API bool RmlDocumentSetGeometry(RmlDocumentHandle handle, std::string_view elementId,
	const RmlElementGeometry& geometry);

// Vertical scrolling for overflow elements. A scroll anchor keeps an element
// pinned to its end across a layout/model update only when it was already
// within threshold pixels of the end; scrolling upward disables the pin until
// the user returns to the bottom.
MQLIB_API bool RmlDocumentGetScrollMetrics(RmlDocumentHandle handle,
	std::string_view elementId, RmlScrollMetrics& metrics);
MQLIB_API bool RmlDocumentSetScrollOffset(RmlDocumentHandle handle,
	std::string_view elementId, float offset);
MQLIB_API bool RmlDocumentScrollToEnd(RmlDocumentHandle handle,
	std::string_view elementId);
MQLIB_API bool RmlDocumentSetScrollAnchor(RmlDocumentHandle handle,
	std::string_view elementId, bool enabled, float threshold = 2.0f);

// A dynamically typed data model value.
//
// RmlUi's data binding is keyed by C++ type, so schema-free records cannot be
// expressed with its ordinary RegisterStruct/RegisterArray helpers. MQ stores
// model state as this recursive value instead and projects it into RmlUi
// through custom variable definitions that resolve members and indices at
// runtime. That keeps the public API free of RmlUi types, which is what lets
// MQ2Lua build model trees without linking against RmlUi.
//
// Addresses: a variable's RmlUiValue lives in a std::map node, so its address is
// stable for as long as the model holds it - that is what RmlUi's binding relies
// on. Array elements live in a vector, so an element's address is only borrowed
// for the duration of one data expression. That is sufficient because a model
// value is replaced wholesale rather than grown while RmlUi is mid-expression;
// the incremental append path is used to build a tree before it is handed over.
//
// The array container is a vector rather than a deque because std::deque may not
// be instantiated with an incomplete element type, and RmlUiValue is incomplete
// while it declares its own members.
// MQLIB_OBJECT, not MQLIB_API: MQLIB_API expands to `extern "C"`, which is a
// linkage specification and cannot be applied to a class.
//
// C4251 is suppressed for the type below. Exporting a value type that owns STL
// containers is deliberate here - R5 requires MQ-owned value types rather than
// RmlUi types crossing the boundary - and every consumer in the tree is built
// against the same runtime, so there is no version-skew risk to warn about.
#pragma warning(push)
#pragma warning(disable : 4251)
class MQLIB_OBJECT RmlUiValue
{
public:
	enum class Type { Nil, Bool, Number, String, Array, Record };

	RmlUiValue() = default;
	explicit RmlUiValue(bool value) : m_type(Type::Bool), m_bool(value) {}
	explicit RmlUiValue(double value) : m_type(Type::Number), m_number(value) {}
	explicit RmlUiValue(std::string value) : m_type(Type::String), m_string(std::move(value)) {}

	static RmlUiValue MakeArray() { RmlUiValue value; value.m_type = Type::Array; return value; }
	static RmlUiValue MakeRecord() { RmlUiValue value; value.m_type = Type::Record; return value; }

	Type GetType() const { return m_type; }
	bool IsNil() const { return m_type == Type::Nil; }
	bool IsArray() const { return m_type == Type::Array; }
	bool IsRecord() const { return m_type == Type::Record; }
	bool IsScalar() const { return m_type == Type::Bool || m_type == Type::Number || m_type == Type::String; }

	bool AsBool(bool fallback = false) const { return m_type == Type::Bool ? m_bool : fallback; }
	double AsNumber(double fallback = 0.0) const { return m_type == Type::Number ? m_number : fallback; }
	const std::string& AsString() const { return m_string; }

	// Number of elements in an array or fields in a record; zero for scalars.
	size_t Size() const;

	// Mutators coerce the receiver to the required type, so a Nil value can be
	// grown in place: values.AddElement().GetOrAddField("name") = ...;
	RmlUiValue& AddElement(RmlUiValue value);
	RmlUiValue& AddElement();
	// Replace deleteCount array entries at index with the supplied values.
	// Returns false when the receiver is not an array or index is out of range.
	// deleteCount is clamped to the entries available after index.
	bool SpliceArray(size_t index, size_t deleteCount, const std::vector<RmlUiValue>& values);
	RmlUiValue& SetField(std::string key, RmlUiValue value);
	RmlUiValue& GetOrAddField(std::string key);

	// Read-only lookup. Returns nullptr when the value is not a record or has
	// no such field. A field that exists but holds nil is returned, not null.
	RmlUiValue* Find(std::string_view key);
	const RmlUiValue* Find(std::string_view key) const;

	// Indexed access into an array. Returns nullptr when the value is not an
	// array or the index is out of range.
	RmlUiValue* ElementAt(size_t index);
	const RmlUiValue* ElementAt(size_t index) const;

	// Visits each field of a record in key order. The visitor returns false to
	// stop, which makes this return false. Returns false immediately when the
	// value is not a record.
	//
	// Records are exposed through iteration rather than a keyed cursor because
	// the only consumer that needs to walk one is the quota measurement in
	// core, and this keeps the caller-side walk linear.
	template <typename Visitor>
	bool ForEachField(Visitor&& visit)
	{
		if (m_type != Type::Record)
			return false;

		for (auto& field : m_record)
		{
			if (!visit(field.first, field.second))
				return false;
		}
		return true;
	}

	template <typename Visitor>
	bool ForEachField(Visitor&& visit) const
	{
		if (m_type != Type::Record)
			return false;

		for (const auto& field : m_record)
		{
			if (!visit(field.first, field.second))
				return false;
		}
		return true;
	}

private:
	void EnsureType(Type type);

	Type m_type = Type::Nil;
	bool m_bool = false;
	double m_number = 0.0;
	std::string m_string;
	std::vector<RmlUiValue> m_array;
	std::map<std::string, RmlUiValue> m_record;
};
#pragma warning(pop)

// Data models.
//
// A model belongs to the context of the owner that declared it, so two scripts
// may each use a model named "hud" without observing or mutating each other.
// Models are created by declaring them on a document before it parses, via the
// "models" option on LoadRmlDocument / LoadRmlDocumentForOwner; RmlUi resolves
// a document's data-model attributes while parsing, so a model created later is
// invisible to an already-loaded document. Declaring up front is what allows
// first use without reloading.
//
// Variables are set with a typed or structured value and marked dirty, so the
// next update reflects them. Writes originating inside RmlUi (for example a
// two-way "value" binding on an input) are applied to the stored value and
// reported through the model change callback.
MQLIB_API bool RmlDocumentSetModelString(RmlDocumentHandle handle, std::string_view model, std::string_view variable, std::string_view value);
MQLIB_API bool RmlDocumentSetModelNumber(RmlDocumentHandle handle, std::string_view model, std::string_view variable, double value);
MQLIB_API bool RmlDocumentSetModelBoolean(RmlDocumentHandle handle, std::string_view model, std::string_view variable, bool value);
MQLIB_API bool RmlDocumentSetModelStringArray(RmlDocumentHandle handle, std::string_view model,
	std::string_view variable, const std::vector<std::string>& values);
MQLIB_API bool RmlDocumentSetModelNumberArray(RmlDocumentHandle handle, std::string_view model,
	std::string_view variable, const std::vector<double>& values);

// Set a variable to an arbitrary value, including nested records and arrays of
// records. Replaces any previous value for the variable. Returns false if the
// handle, model, or variable name is invalid, or if the value exceeds the
// owner's configured nesting, field-count, array-length, or retained-size
// limits.
MQLIB_API bool RmlDocumentSetModelValue(RmlDocumentHandle handle, std::string_view model,
	std::string_view variable, const RmlUiValue& value);

// Atomically replace part of an existing array variable. The index is
// zero-based. This avoids transferring the unchanged portion of a large model
// across a language/plugin boundary for append-and-trim workloads such as chat.
// The mutation is rejected without changing the stored value if the target is
// not an array, index is out of range, or the resulting value exceeds a quota.
MQLIB_API bool RmlDocumentSpliceModelArray(RmlDocumentHandle handle, std::string_view model,
	std::string_view variable, size_t index, size_t deleteCount,
	const std::vector<RmlUiValue>& values);

// Read the current value of a model variable back out to the caller. Returns
// false if the handle, model, or variable is not found. Use this to observe
// values RmlUi wrote through a two-way binding.
MQLIB_API bool RmlDocumentGetModelValue(RmlDocumentHandle handle, std::string_view model,
	std::string_view variable, RmlUiValue& value);

// Declares a data model on the document's context without setting a variable.
// Call before the document parses; declaring a model that already exists is a
// no-op. Returns false if the handle or model name is invalid.
MQLIB_API bool RmlDocumentDeclareModel(RmlDocumentHandle handle, std::string_view model);

// Register a listener on an element id. Events are delivered on MacroQuest's
// main thread as value snapshots; no RmlUi object escapes through this API.
// Listeners survive document reload and are removed with their document.
MQLIB_API RmlListenerHandle AddRmlDocumentEventListener(RmlDocumentHandle document,
	std::string_view eventType, std::string_view elementId, RmlEventCallback callback, void* userData);
MQLIB_API void RemoveRmlDocumentEventListener(RmlListenerHandle listener);

// Sets the root used by lua:// paths. MQ2Lua calls this with its configured
// script directory so script discovery and RmlUi resource loading agree.
MQLIB_API void SetRmlUiLuaDirectory(std::string_view directory);

} // namespace mq
