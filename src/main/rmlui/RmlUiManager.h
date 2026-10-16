#pragma once

#include "mq/api/RmlUi.h"

#include <filesystem>
#include <optional>
#include <string_view>

namespace mq::rmlui {

// Resolve a virtual RmlUi path against the core, plugin, and Lua resource roots.
//
// Supported forms:
//   - a relative path (no scheme) resolves under coreRoot;
//   - "mq://rmlui/<path>" resolves under coreRoot;
//   - "plugin://<name>/<path>" resolves under <pluginsRoot>/<name>/resources/;
//   - "lua://<name>/<path>" resolves under <luaRoot>/<name>/.
// RmlUi may canonicalize the scheme separator for linked resources to "|//";
// those otherwise-identical forms are accepted as well.
//
// Absolute paths, traversal (".."), unsupported schemes, and invalid authorities
// return nullopt.
std::optional<std::filesystem::path> ResolveRmlUiPath(
	const std::filesystem::path& coreRoot,
	const std::filesystem::path& pluginsRoot,
	const std::filesystem::path& luaRoot,
	std::string_view path);

void Initialize();
void Shutdown();

void CreateDeviceObjects();
void InvalidateDeviceObjects();
// Drop image handles before EQ destroys or replaces its UI texture atlases.
// RmlUi reloads them lazily after the EQ UI has been rebuilt.
void InvalidateEqUiTextures();
void Render();

bool HandleWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
bool WantsMouseInput();
bool WantsKeyboardInput();
bool ConsumePendingKeyboardInput();

bool IsInitialized();
bool IsVisible();
void SetVisible(bool visible);

// Internal document service backing the opaque public API in mq/api/RmlUi.h and
// the /rmlui console command.
RmlDocumentHandle LoadDocument(std::string_view virtualPath, const RmlDocumentOptions& options, MQPluginHandle owner);
RmlOwnerHandle CreateOwner(MQPluginHandle parent);
void DestroyOwner(RmlOwnerHandle owner);
RmlDocumentHandle LoadDocumentForOwner(std::string_view virtualPath, const RmlDocumentOptions& options,
	RmlOwnerHandle owner);
void UnloadDocument(RmlDocumentHandle handle);
bool ReloadDocument(RmlDocumentHandle handle);
bool IsDocumentLoaded(RmlDocumentHandle handle);
bool SetDocumentVisible(RmlDocumentHandle handle, bool visible);
bool BringDocumentToFront(RmlDocumentHandle handle);
void RemoveDocumentsForPlugin(MQPluginHandle owner);

// DOM mutations scoped to a document handle. Return false on an invalid handle
// or unknown element id.
bool SetDocumentText(RmlDocumentHandle handle, std::string_view elementId, std::string_view text);
bool SetDocumentValue(RmlDocumentHandle handle, std::string_view elementId, std::string_view value);
bool SetDocumentClass(RmlDocumentHandle handle, std::string_view elementId, std::string_view className, bool activate);
bool SetDocumentStyle(RmlDocumentHandle handle, std::string_view elementId, std::string_view property, std::string_view value);
bool SetDocumentAttribute(RmlDocumentHandle handle, std::string_view elementId, std::string_view attribute, std::string_view value);
bool RemoveDocumentAttribute(RmlDocumentHandle handle, std::string_view elementId, std::string_view attribute);
bool GetDocumentAttribute(RmlDocumentHandle handle, std::string_view elementId, std::string_view attribute, std::string& value);
bool FocusDocumentElement(RmlDocumentHandle handle, std::string_view elementId);
bool BlurDocumentElement(RmlDocumentHandle handle, std::string_view elementId);
bool GetDocumentGeometry(RmlDocumentHandle handle, std::string_view elementId, RmlElementGeometry& geometry);
bool SetDocumentGeometry(RmlDocumentHandle handle, std::string_view elementId, const RmlElementGeometry& geometry);
bool GetDocumentScrollMetrics(RmlDocumentHandle handle, std::string_view elementId, RmlScrollMetrics& metrics);
bool SetDocumentScrollOffset(RmlDocumentHandle handle, std::string_view elementId, float offset);
bool ScrollDocumentToEnd(RmlDocumentHandle handle, std::string_view elementId);
bool SetDocumentScrollAnchor(RmlDocumentHandle handle, std::string_view elementId, bool enabled, float threshold);

// Data model variables.
//
// A model belongs to the context of the document's scoped owner, so two scripts
// may each use a model named "hud" without observing or mutating one another.
// Models are declared through the "models" option on the load options, before the
// document parses, because RmlUi resolves data-model attributes at parse time.
bool SetModelString(RmlDocumentHandle handle, std::string_view model, std::string_view variable, std::string_view value);
bool SetModelNumber(RmlDocumentHandle handle, std::string_view model, std::string_view variable, double value);
bool SetModelBoolean(RmlDocumentHandle handle, std::string_view model, std::string_view variable, bool value);
bool SetModelStringArray(RmlDocumentHandle handle, std::string_view model, std::string_view variable,
	const std::vector<std::string>& values);
bool SetModelNumberArray(RmlDocumentHandle handle, std::string_view model, std::string_view variable,
	const std::vector<double>& values);

// Set an arbitrary value, including nested records and arrays of records. A
// variable's value may change between scalar, array, and record over its life.
bool SetModelValue(RmlDocumentHandle handle, std::string_view model, std::string_view variable,
	const RmlUiValue& value);
bool SpliceModelArray(RmlDocumentHandle handle, std::string_view model, std::string_view variable,
	size_t index, size_t deleteCount, const std::vector<RmlUiValue>& values);

// Read a variable back out, which is how a value RmlUi wrote through a two-way
// binding is observed.
bool GetModelValue(RmlDocumentHandle handle, std::string_view model, std::string_view variable,
	RmlUiValue& value);

// Declare a model on the document's context without setting a variable.
bool DeclareModel(RmlDocumentHandle handle, std::string_view model);

RmlListenerHandle AddDocumentEventListener(RmlDocumentHandle document, std::string_view eventType,
	std::string_view elementId, RmlEventCallback callback, void* userData);
void RemoveDocumentEventListener(RmlListenerHandle listener);
void SetLuaDirectory(std::string_view directory);

} // namespace mq::rmlui
