#include "pch.h"
#include "RmlUiManager.h"
#include "RmlUiModel.h"
#include "RmlUiRendererDX9.h"
#include "RmlUiResizeEdge.h"

#include "GraphicsEngine.h"
#include "Logging.h"
#include "MQ2Globals.h"
#include "OverlayInput.h"

#include "mq/api/CommandAPI.h"
#include "mq/base/Threading.h"
#include "mq/base/WString.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <type_traits>
#include <utility>
#include <windowsx.h>

namespace mq::rmlui {
namespace {

bool IsValidAuthorityComponent(std::string_view authority)
{
	if (authority.empty())
		return false;

	for (char c : authority)
	{
		const bool valid = (c >= 'a' && c <= 'z')
			|| (c >= 'A' && c <= 'Z')
			|| (c >= '0' && c <= '9')
			|| c == '_' || c == '-' || c == '.';
		if (!valid)
			return false;
	}
	return authority != "." && authority != "..";
}

bool ContainsForbiddenWindowsPathSyntax(std::wstring_view path)
{
	if (path.find(L'\0') != std::wstring_view::npos || path.find(L':') != std::wstring_view::npos)
		return true;
	return path.starts_with(LR"(\\)") || path.starts_with(L"//")
		|| path.starts_with(LR"(\\?\)") || path.starts_with(LR"(\\.\)");
}

bool IsPathWithinRoot(const std::filesystem::path& root, const std::filesystem::path& candidate)
{
	auto rootPart = root.begin();
	auto candidatePart = candidate.begin();
	for (; rootPart != root.end(); ++rootPart, ++candidatePart)
	{
		if (candidatePart == candidate.end()
			|| _wcsicmp(rootPart->c_str(), candidatePart->c_str()) != 0)
		{
			return false;
		}
	}
	return true;
}

bool TraversesReparsePoint(const std::filesystem::path& root,
	const std::filesystem::path& relative)
{
	std::filesystem::path current = root;
	for (const auto& component : relative)
	{
		current /= component;
		const DWORD attributes = GetFileAttributesW(current.c_str());
		if (attributes != INVALID_FILE_ATTRIBUTES
			&& (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
		{
			return true;
		}
	}
	return false;
}

std::optional<std::filesystem::path> JoinSafeRoot(
	const std::filesystem::path& root, std::wstring_view subpath)
{
	if (subpath.empty())
	{
		std::error_code error;
		const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(root, error);
		return error ? std::nullopt : std::optional<std::filesystem::path>(canonicalRoot);
	}

	if (ContainsForbiddenWindowsPathSyntax(subpath))
		return std::nullopt;

	std::filesystem::path relative(subpath);
	if (relative.is_absolute() || relative.has_root_name())
		return std::nullopt;

	for (const auto& component : relative)
	{
		if (component == L"..")
			return std::nullopt;
	}
	relative = relative.lexically_normal();

	std::error_code error;
	const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(root, error);
	if (error)
		return std::nullopt;
	const std::filesystem::path candidate = std::filesystem::weakly_canonical(root / relative, error);
	if (error || !IsPathWithinRoot(canonicalRoot, candidate))
		return std::nullopt;
	if (TraversesReparsePoint(root, relative))
		return std::nullopt;
	return candidate;
}

} // namespace

std::optional<std::filesystem::path> ResolveRmlUiPath(
	const std::filesystem::path& coreRoot,
	const std::filesystem::path& pluginsRoot,
	const std::filesystem::path& luaRoot,
	std::string_view path)
{
	if (path.empty())
		return std::nullopt;
	if (path.find('\0') != std::string_view::npos || path.starts_with("\\\\")
		|| path.starts_with("//") || path.starts_with("\\\\?\\") || path.starts_with("\\\\.\\"))
	{
		return std::nullopt;
	}

	// RmlUi canonicalizes the scheme separator to "|//" before linked
	// templates, stylesheets, and images reach the file interface. Accept that
	// internal representation as well as the spelling authors use in RML.
	size_t schemeEnd = path.find("://");
	if (schemeEnd == std::string_view::npos)
		schemeEnd = path.find("|//");
	if (schemeEnd == std::string_view::npos)
		return JoinSafeRoot(coreRoot, utf8_to_wstring(path));

	const std::string_view scheme = path.substr(0, schemeEnd);
	const std::string_view rest = path.substr(schemeEnd + 3);
	const size_t slash = rest.find('/');
	const std::string_view authority = slash == std::string_view::npos ? rest : rest.substr(0, slash);
	const std::string_view subpath = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);

	if (scheme == "mq")
	{
		if (authority != "rmlui")
			return std::nullopt;
		return JoinSafeRoot(coreRoot, utf8_to_wstring(subpath));
	}

	if (scheme == "plugin")
	{
		if (!IsValidAuthorityComponent(authority))
			return std::nullopt;
		const std::filesystem::path pluginRoot =
			pluginsRoot / std::filesystem::path(utf8_to_wstring(authority)) / "resources";
		return JoinSafeRoot(pluginRoot, utf8_to_wstring(subpath));
	}

	if (scheme == "lua")
	{
		if (!IsValidAuthorityComponent(authority))
			return std::nullopt;
		const std::filesystem::path scriptRoot =
			luaRoot / std::filesystem::path(utf8_to_wstring(authority));
		return JoinSafeRoot(scriptRoot, utf8_to_wstring(subpath));
	}

	return std::nullopt;
}

namespace {

class SystemInterface : public Rml::SystemInterface
{
public:
	double GetElapsedTime() override
	{
		static const auto start = std::chrono::steady_clock::now();
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	}

	bool LogMessage(Rml::Log::Type type, const Rml::String& message) override
	{
		if (type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT || type == Rml::Log::LT_WARNING)
		{
			const auto now = std::chrono::steady_clock::now();
			auto iter = m_recentMessages.find(message);
			if (iter != m_recentMessages.end() && now - iter->second < std::chrono::seconds(5))
				return true;
			if (m_recentMessages.size() >= 256)
				m_recentMessages.clear();
			m_recentMessages[message] = now;
		}
		switch (type)
		{
		case Rml::Log::LT_ERROR:
		case Rml::Log::LT_ASSERT:
			LOG_ERROR("RmlUi: {}", message);
			break;
		case Rml::Log::LT_WARNING:
			LOG_WARN("RmlUi: {}", message);
			break;
		default:
			LOG_DEBUG("RmlUi: {}", message);
			break;
		}
		return true;
	}

	void SetClipboardText(const Rml::String& text) override
	{
		if (!OpenClipboard(nullptr))
			return;

		EmptyClipboard();
		const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
		if (length > 0)
		{
			HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(length) * sizeof(wchar_t));
			if (memory)
			{
				auto* output = static_cast<wchar_t*>(GlobalLock(memory));
				if (output)
				{
					MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, output, length);
					GlobalUnlock(memory);
					if (!SetClipboardData(CF_UNICODETEXT, memory))
						GlobalFree(memory);
				}
				else
				{
					GlobalFree(memory);
				}
			}
		}
		CloseClipboard();
	}

	void GetClipboardText(Rml::String& text) override
	{
		text.clear();
		if (!OpenClipboard(nullptr))
			return;

		if (HANDLE data = GetClipboardData(CF_UNICODETEXT))
		{
			if (const auto* input = static_cast<const wchar_t*>(GlobalLock(data)))
			{
				const int length = WideCharToMultiByte(CP_UTF8, 0, input, -1, nullptr, 0, nullptr, nullptr);
				if (length > 1)
				{
					std::string buffer(static_cast<size_t>(length), '\0');
					WideCharToMultiByte(CP_UTF8, 0, input, -1, buffer.data(), length, nullptr, nullptr);
					buffer.resize(static_cast<size_t>(length - 1));
					text = std::move(buffer);
				}
				GlobalUnlock(data);
			}
		}
		CloseClipboard();
	}

private:
	std::map<std::string, std::chrono::steady_clock::time_point> m_recentMessages;
};

class FileInterface : public Rml::FileInterface
{
public:
	void SetRoot(std::filesystem::path root)
	{
		m_root = std::filesystem::absolute(std::move(root)).lexically_normal();
		std::error_code error;
		std::filesystem::create_directories(m_root, error);
	}

	void SetPluginsRoot(std::filesystem::path root)
	{
		m_pluginsRoot = std::filesystem::absolute(std::move(root)).lexically_normal();
	}

	void SetLuaRoot(std::filesystem::path root)
	{
		m_luaRoot = std::filesystem::absolute(std::move(root)).lexically_normal();
	}

	const std::filesystem::path& GetRoot() const { return m_root; }

	std::optional<std::filesystem::path> Resolve(const Rml::String& path) const
	{
		return ResolveRmlUiPath(m_root, m_pluginsRoot, m_luaRoot, path);
	}

	Rml::FileHandle Open(const Rml::String& path) override
	{
		const auto resolved = Resolve(path);
		if (!resolved)
		{
			LOG_WARN("RmlUi rejected file path '{}'", path);
			return {};
		}

		FILE* file = nullptr;
		const errno_t result = _wfopen_s(&file, resolved->c_str(), L"rb");
		if (result != 0 || !file)
		{
			LOG_WARN("RmlUi failed to open resource '{}' as '{}' (error {})",
				path, resolved->string(), result);
		}
		return reinterpret_cast<Rml::FileHandle>(file);
	}

	void Close(Rml::FileHandle file) override
	{
		if (file)
			std::fclose(reinterpret_cast<FILE*>(file));
	}

	size_t Read(void* buffer, size_t size, Rml::FileHandle file) override
	{
		return std::fread(buffer, 1, size, reinterpret_cast<FILE*>(file));
	}

	bool Seek(Rml::FileHandle file, long offset, int origin) override
	{
		return std::fseek(reinterpret_cast<FILE*>(file), offset, origin) == 0;
	}

	size_t Tell(Rml::FileHandle file) override
	{
		const long position = std::ftell(reinterpret_cast<FILE*>(file));
		return position >= 0 ? static_cast<size_t>(position) : 0;
	}

private:
	std::filesystem::path m_root;
	std::filesystem::path m_pluginsRoot;
	std::filesystem::path m_luaRoot;
};

struct DocumentEntry
{
	struct ScrollAnchor
	{
		float threshold = 2.0f;
		bool wasAtEnd = false;
		float scrollTopBefore = 0.0f;
	};

	RmlDocumentHandle handle = 0;
	std::string path;
	Rml::ElementDocument* document = nullptr;
	RmlDocumentOptions options;
	MQPluginHandle owner;
	RmlOwnerHandle scopedOwner = 0;
	uint64_t focusOrder = 0;
	std::map<std::string, ScrollAnchor> scrollAnchors;
};

class EventListener final : public Rml::EventListener
{
public:
	EventListener(RmlEventCallback callback, void* userData)
		: m_callback(callback), m_userData(userData) {}

	void ProcessEvent(Rml::Event& source) override
	{
		if (!m_callback)
			return;

		RmlEvent event;
		event.type = source.GetType();
		if (const Rml::Element* target = source.GetTargetElement())
		{
			event.targetId = target->GetId();
			event.targetTag = target->GetTagName();
			event.targetClasses = target->GetClassNames();
			const Rml::String value = target->GetAttribute<Rml::String>("data-mq-value", "");
			event.targetValue.assign(value.begin(), value.end());
			const Rml::ElementFormControl* formControl = rmlui_dynamic_cast<const Rml::ElementFormControl*>(target);
			const Rml::String formValue = formControl
				? formControl->GetValue()
				: target->GetAttribute<Rml::String>("value", "");
			event.value.assign(formValue.begin(), formValue.end());
			event.checked = target->HasAttribute("checked");
		}
		if (const Rml::Element* current = source.GetCurrentElement())
		{
			event.currentId = current->GetId();
			const Rml::String value = current->GetAttribute<Rml::String>("data-mq-value", "");
			event.currentValue.assign(value.begin(), value.end());
		}
		const Rml::Element* current = source.GetCurrentElement();
		for (const Rml::Element* element = source.GetTargetElement(); element;
			element = element == current ? nullptr : element->GetParentNode())
		{
			if (element->HasAttribute("data-mq-value"))
			{
				event.actionId = element->GetId();
				const Rml::String value = element->GetAttribute<Rml::String>("data-mq-value", "");
				event.actionValue.assign(value.begin(), value.end());
				break;
			}
		}
		event.mouseX = source.GetParameter<int>("mouse_x", 0);
		event.mouseY = source.GetParameter<int>("mouse_y", 0);
		event.handleX = source.GetParameter<float>("handle_x", 0.0f);
		event.handleY = source.GetParameter<float>("handle_y", 0.0f);
		event.button = source.GetParameter<int>("button", -1);
		event.keyIdentifier = source.GetParameter<int>("key_identifier", 0);
		event.wheelDelta = source.GetParameter<float>("wheel_delta", 0.0f);
		event.ctrlKey = source.GetParameter<bool>("ctrl_key", false);
		event.shiftKey = source.GetParameter<bool>("shift_key", false);
		event.altKey = source.GetParameter<bool>("alt_key", false);
		event.metaKey = source.GetParameter<bool>("meta_key", false);
		m_callback(event, m_userData);
	}

private:
	RmlEventCallback m_callback = nullptr;
	void* m_userData = nullptr;
};

struct ListenerEntry
{
	RmlListenerHandle handle = 0;
	RmlDocumentHandle document = 0;
	std::string eventType;
	std::string elementId;
	std::unique_ptr<EventListener> listener;
	Rml::Element* attachedElement = nullptr;
};

struct ModelVariable
{
	// Values are held in a node-based map so the address RmlUi binds stays valid
	// as other variables are added or replaced.
	RmlUiValue value;

	// Usage of the value currently stored, kept so the owner's retained-byte
	// total can be adjusted when a variable is replaced.
	ModelValueUsage usage;
};

struct ModelState
{
	Rml::DataModelConstructor constructor;
	Rml::DataModelHandle handle;
	std::map<std::string, ModelVariable> variables;
};

SystemInterface s_systemInterface;
FileInterface s_fileInterface;
#if HAS_DIRECTX_9
RendererDX9 s_renderer;
#endif

// One RmlUi context per scoped owner.
//
// Data models live in a context, so this is what isolates namespaces: two Lua
// scripts that each declare a model named "hud" get separate contexts and
// cannot observe or mutate one another. Documents opened without a scoped
// owner (native plugins) share the context for owner 0.
//
// Contexts are cheap. Everything expensive stays global and shared - the DX9
// render interface, the system and file interfaces, font handles, and the
// texture cache - so this does not reintroduce a per-document render target.
std::map<RmlOwnerHandle, Rml::Context*> s_contexts;
// A context must complete Update() after document creation/reload before input
// is dispatched. In particular, text widgets build their line table there and
// assume it exists during mouse hit testing.
std::set<Rml::Context*> s_inputReadyContexts;

// Data models, grouped by owner because the model namespace is the owner's.
std::map<RmlOwnerHandle, std::map<std::string, ModelState>> s_dataModels;

bool s_initialized = false;
std::atomic_bool s_acceptingCommands = false;
bool s_visible = false;
bool s_supported = false;
uint32_t s_highSurrogate = 0;
bool s_pendingKeyboardConsume = false;
bool s_mouseOverInteractive = false;
uint32_t s_mouseCaptureButtons = 0;
OverlayInputHandle s_inputConsumer = 0;
std::map<RmlDocumentHandle, DocumentEntry> s_documents;
std::atomic<RmlDocumentHandle> s_nextDocumentHandle = 1;
uint64_t s_nextFocusOrder = 1;
std::map<RmlOwnerHandle, MQPluginHandle> s_owners;
std::atomic<RmlOwnerHandle> s_nextOwnerHandle = 1;
std::map<RmlListenerHandle, ListenerEntry> s_listeners;
RmlListenerHandle s_nextListenerHandle = 1;
std::vector<RmlDocumentHandle> s_pendingHandles;
std::vector<std::vector<Rml::byte>> s_fontData;

#if HAS_DIRECTX_9
class ScopedResourceOwner
{
public:
	explicit ScopedResourceOwner(RmlOwnerHandle owner) { s_renderer.SetResourceOwner(owner); }
	~ScopedResourceOwner() { s_renderer.SetResourceOwner(0); }
};
#endif

size_t s_maxDocumentsPerScopedOwner = 16;
size_t s_maxListenersPerScopedOwner = 128;
size_t s_maxModelVariablesPerScopedOwner = 256;
size_t s_maxModelArrayEntriesPerScopedOwner = 4096;
size_t s_maxModelFieldsPerRecord = 128;
size_t s_maxModelFieldsPerScopedOwner = 4096;
size_t s_maxModelValueDepth = 8;
size_t s_maxModelBytesPerScopedOwner = 8 * 1024 * 1024;
size_t s_maxContexts = 32;
int s_maxTextureDimension = 4096;
size_t s_maxTextureBytesPerScopedOwner = 64 * 1024 * 1024;

bool ShouldLogOwnerLimit(RmlOwnerHandle owner, std::string_view category)
{
	static std::map<std::pair<RmlOwnerHandle, std::string>, std::chrono::steady_clock::time_point> lastMessages;
	const auto now = std::chrono::steady_clock::now();
	auto& last = lastMessages[{ owner, std::string(category) }];
	if (last != std::chrono::steady_clock::time_point{} && now - last < std::chrono::seconds(5))
		return false;
	last = now;
	return true;
}

size_t GetTextureBytesForOwner(RmlOwnerHandle owner)
{
#if HAS_DIRECTX_9
	return s_renderer.GetTextureBytes(owner);
#else
	return 0;
#endif
}

RmlOwnerHandle GetScopedOwner(RmlDocumentHandle document)
{
	const auto iter = s_documents.find(document);
	return iter != s_documents.end() ? iter->second.scopedOwner : 0;
}

RmlOwnerHandle ResolveResourceOwner(std::string_view source)
{
	const size_t schemeEnd = source.find("://");
	if (schemeEnd == std::string_view::npos)
		return 0;
	const size_t authorityEnd = source.find('/', schemeEnd + 3);
	const std::string_view resourceRoot = authorityEnd == std::string_view::npos
		? source : source.substr(0, authorityEnd);

	RmlOwnerHandle matchedOwner = 0;
	for (const auto& [handle, document] : s_documents)
	{
		if (document.scopedOwner == 0 || !document.path.starts_with(resourceRoot)
			|| (document.path.size() > resourceRoot.size() && document.path[resourceRoot.size()] != '/'))
		{
			continue;
		}
		if (matchedOwner != 0 && matchedOwner != document.scopedOwner)
			return 0;
		matchedOwner = document.scopedOwner;
	}
	return matchedOwner;
}

size_t CountDocumentsForOwner(RmlOwnerHandle owner)
{
	return static_cast<size_t>(std::count_if(s_documents.begin(), s_documents.end(),
		[owner](const auto& item) { return item.second.scopedOwner == owner; }));
}

size_t CountListenersForOwner(RmlOwnerHandle owner)
{
	return static_cast<size_t>(std::count_if(s_listeners.begin(), s_listeners.end(),
		[owner](const auto& item) { return GetScopedOwner(item.second.document) == owner; }));
}

// Model namespaces belong to an owner, so these look only at that owner's
// models. Usage is tracked per variable as it is stored rather than recounted by
// walking values, which keeps quota checks O(models) instead of O(entries).
size_t CountModelVariablesForOwner(RmlOwnerHandle owner)
{
	auto models = s_dataModels.find(owner);
	if (models == s_dataModels.end())
		return 0;

	size_t count = 0;
	for (const auto& [name, model] : models->second)
		count += model.variables.size();
	return count;
}

size_t CountModelArrayEntriesForOwner(RmlOwnerHandle owner)
{
	auto models = s_dataModels.find(owner);
	if (models == s_dataModels.end())
		return 0;

	size_t count = 0;
	for (const auto& [name, model] : models->second)
	{
		for (const auto& [variableName, variable] : model.variables)
			count += variable.usage.arrayEntries;
	}
	return count;
}

size_t CountModelFieldsForOwner(RmlOwnerHandle owner)
{
	auto models = s_dataModels.find(owner);
	if (models == s_dataModels.end())
		return 0;

	size_t count = 0;
	for (const auto& [name, model] : models->second)
	{
		for (const auto& [variableName, variable] : model.variables)
			count += variable.usage.fields;
	}
	return count;
}

size_t CountModelBytesForOwner(RmlOwnerHandle owner)
{
	auto models = s_dataModels.find(owner);
	if (models == s_dataModels.end())
		return 0;

	size_t total = 0;
	for (const auto& [name, model] : models->second)
	{
		for (const auto& [variableName, variable] : model.variables)
			total += variable.usage.bytes;
	}
	return total;
}

// On-demand update scheduling. Update() is skipped when nothing is dirty and no
// animation is running, throttled by the foreground/background update rates.
int s_updateRateForeground = 60;
int s_updateRateBackground = 10;
double s_nextUpdateDelay = 0.0;
double s_lastUpdateTime = 0.0;
bool s_inputDirty = false;
bool s_firstRender = true;
double s_lastUpdateMicroseconds = 0.0;
double s_lastRenderMicroseconds = 0.0;
uint64_t s_updateCount = 0;
uint64_t s_renderCount = 0;

int GetKeyModifiers()
{
	int modifiers = 0;
	if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= Rml::Input::KM_CTRL;
	if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= Rml::Input::KM_SHIFT;
	if (GetKeyState(VK_MENU) & 0x8000) modifiers |= Rml::Input::KM_ALT;
	if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) modifiers |= Rml::Input::KM_META;
	if (GetKeyState(VK_CAPITAL) & 1) modifiers |= Rml::Input::KM_CAPSLOCK;
	if (GetKeyState(VK_NUMLOCK) & 1) modifiers |= Rml::Input::KM_NUMLOCK;
	if (GetKeyState(VK_SCROLL) & 1) modifiers |= Rml::Input::KM_SCROLLLOCK;
	return modifiers;
}

Rml::Input::KeyIdentifier TranslateKey(WPARAM key)
{
	using namespace Rml::Input;
	if (key >= '0' && key <= '9') return static_cast<KeyIdentifier>(KI_0 + key - '0');
	if (key >= 'A' && key <= 'Z') return static_cast<KeyIdentifier>(KI_A + key - 'A');
	if (key >= VK_F1 && key <= VK_F24) return static_cast<KeyIdentifier>(KI_F1 + key - VK_F1);
	if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) return static_cast<KeyIdentifier>(KI_NUMPAD0 + key - VK_NUMPAD0);

	switch (key)
	{
	case VK_SPACE: return KI_SPACE;
	case VK_BACK: return KI_BACK;
	case VK_TAB: return KI_TAB;
	case VK_RETURN: return KI_RETURN;
	case VK_PAUSE: return KI_PAUSE;
	case VK_CAPITAL: return KI_CAPITAL;
	case VK_ESCAPE: return KI_ESCAPE;
	case VK_PRIOR: return KI_PRIOR;
	case VK_NEXT: return KI_NEXT;
	case VK_END: return KI_END;
	case VK_HOME: return KI_HOME;
	case VK_LEFT: return KI_LEFT;
	case VK_UP: return KI_UP;
	case VK_RIGHT: return KI_RIGHT;
	case VK_DOWN: return KI_DOWN;
	case VK_SNAPSHOT: return KI_SNAPSHOT;
	case VK_INSERT: return KI_INSERT;
	case VK_DELETE: return KI_DELETE;
	case VK_LWIN: return KI_LWIN;
	case VK_RWIN: return KI_RWIN;
	case VK_APPS: return KI_APPS;
	case VK_MULTIPLY: return KI_MULTIPLY;
	case VK_ADD: return KI_ADD;
	case VK_SEPARATOR: return KI_SEPARATOR;
	case VK_SUBTRACT: return KI_SUBTRACT;
	case VK_DECIMAL: return KI_DECIMAL;
	case VK_DIVIDE: return KI_DIVIDE;
	case VK_NUMLOCK: return KI_NUMLOCK;
	case VK_SCROLL: return KI_SCROLL;
	case VK_SHIFT: return KI_LSHIFT;
	case VK_LSHIFT: return KI_LSHIFT;
	case VK_RSHIFT: return KI_RSHIFT;
	case VK_CONTROL: return KI_LCONTROL;
	case VK_LCONTROL: return KI_LCONTROL;
	case VK_RCONTROL: return KI_RCONTROL;
	case VK_MENU: return KI_LMENU;
	case VK_LMENU: return KI_LMENU;
	case VK_RMENU: return KI_RMENU;
	case VK_OEM_1: return KI_OEM_1;
	case VK_OEM_PLUS: return KI_OEM_PLUS;
	case VK_OEM_COMMA: return KI_OEM_COMMA;
	case VK_OEM_MINUS: return KI_OEM_MINUS;
	case VK_OEM_PERIOD: return KI_OEM_PERIOD;
	case VK_OEM_2: return KI_OEM_2;
	case VK_OEM_3: return KI_OEM_3;
	case VK_OEM_4: return KI_OEM_4;
	case VK_OEM_5: return KI_OEM_5;
	case VK_OEM_6: return KI_OEM_6;
	case VK_OEM_7: return KI_OEM_7;
	case VK_OEM_102: return KI_OEM_102;
	default: return KI_UNKNOWN;
	}
}

int TranslateMouseButton(UINT message, WPARAM wParam)
{
	switch (message)
	{
	case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: return 0;
	case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: return 1;
	case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: return 2;
	case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
		return GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 3 : 4;
	default: return -1;
	}
}

bool LoadFontFile(const std::filesystem::path& font, const char* family, Rml::Style::FontWeight weight, bool fallback)
{
	std::ifstream stream(font, std::ios::binary | std::ios::ate);
	if (!stream)
	{
		LOG_WARN("RmlUi: could not open font file '{}'", font.string());
		return false;
	}

	const std::streamsize size = stream.tellg();
	if (size <= 0)
	{
		LOG_WARN("RmlUi: font file '{}' is empty", font.string());
		return false;
	}
	stream.seekg(0);
	auto& data = s_fontData.emplace_back(static_cast<size_t>(size));
	if (!stream.read(reinterpret_cast<char*>(data.data()), size))
	{
		s_fontData.pop_back();
		LOG_WARN("RmlUi: failed to read font file '{}'", font.string());
		return false;
	}

	if (!Rml::LoadFontFace(data, family, Rml::Style::FontStyle::Normal, weight, fallback))
	{
		s_fontData.pop_back();
		LOG_WARN("RmlUi: LoadFontFace failed for '{}'", font.string());
		return false;
	}
	return true;
}

bool LoadDefaultFonts()
{
	char windowsDirectory[MAX_PATH] = {};
	if (!GetWindowsDirectoryA(windowsDirectory, MAX_PATH))
	{
		LOG_WARN("RmlUi: could not determine the Windows directory for font loading");
		return false;
	}

	const std::filesystem::path fonts = std::filesystem::path(windowsDirectory) / "Fonts";
	const bool regular = LoadFontFile(fonts / "segoeui.ttf", "Segoe UI", Rml::Style::FontWeight::Normal, true);
	LoadFontFile(fonts / "seguisb.ttf", "Segoe UI", static_cast<Rml::Style::FontWeight>(600), false);
	LoadFontFile(fonts / "segoeuib.ttf", "Segoe UI", Rml::Style::FontWeight::Bold, false);
	// Segoe UI has no icon glyphs. These faces cover the missing code points
	// and can be selected directly with font-family.
	LoadFontFile(fonts / "seguisym.ttf", "Segoe UI Symbol", Rml::Style::FontWeight::Normal, true);
	LoadFontFile(fonts / "segmdl2.ttf", "Segoe MDL2 Assets", Rml::Style::FontWeight::Normal, true);
	return regular;
}

std::optional<std::string> NormalizeDocumentPath(const char* path)
{
	if (!path || !path[0])
		return std::nullopt;

	Rml::String requested(path);
	const auto resolved = s_fileInterface.Resolve(requested);
	if (!resolved || !ci_equals(resolved->extension().string(), ".rml"))
		return std::nullopt;

	// Normalize separators without touching a virtual scheme ("plugin://").
	std::string normalized(requested);
	for (char& c : normalized)
	{
		if (c == '\\')
			c = '/';
	}
	return normalized;
}

// Context names must be unique across the process, so every owner but the
// default gets its own suffix.
Rml::String MakeContextName(RmlOwnerHandle owner)
{
	std::string name = "macroquest_core";
	if (owner != 0)
		name += "_owner" + std::to_string(static_cast<unsigned long long>(owner));

	return Rml::String(name.begin(), name.end());
}

Rml::Context* GetContext(RmlOwnerHandle owner)
{
	auto existing = s_contexts.find(owner);
	return existing == s_contexts.end() ? nullptr : existing->second;
}

Rml::Context* EnsureContextForOwner(RmlOwnerHandle owner)
{
#if HAS_DIRECTX_9
	if (Rml::Context* existing = GetContext(owner))
		return existing;
	if (!s_initialized || !gpD3D9Device)
		return nullptr;

	if (s_contexts.size() >= s_maxContexts)
	{
		if (ShouldLogOwnerLimit(owner, "contexts"))
			LOG_WARN("RmlUi: refusing to create a context for owner {}; the context limit ({}) is reached",
				owner, s_maxContexts);
		return nullptr;
	}

	D3DVIEWPORT9 viewport = {};
	if (FAILED(gpD3D9Device->GetViewport(&viewport)) || viewport.Width == 0 || viewport.Height == 0)
		return nullptr;

	Rml::Context* context = Rml::CreateContext(MakeContextName(owner),
		Rml::Vector2i(static_cast<int>(viewport.Width), static_cast<int>(viewport.Height)), &s_renderer);
	if (!context)
	{
		LOG_ERROR("RmlUi failed to create a context for owner {}", owner);
		return nullptr;
	}

	s_contexts.emplace(owner, context);
	return context;
#else
	(void)owner;
	return nullptr;
#endif
}

// Destroys an owner's context. Callers must have unloaded its documents and
// dropped its models first: RmlUi defers document destruction until the next
// Context::Update, and a DataVariable must never outlive its context.
void RemoveContextForOwner(RmlOwnerHandle owner)
{
	Rml::Context* context = GetContext(owner);
	if (!context)
		return;

	s_inputReadyContexts.erase(context);
	Rml::RemoveContext(MakeContextName(owner));
	s_contexts.erase(owner);
}

DocumentEntry* FindDocumentByPath(const std::string& path)
{
	for (auto& [handle, entry] : s_documents)
	{
		if (_stricmp(entry.path.c_str(), path.c_str()) == 0)
			return &entry;
	}
	return nullptr;
}

void DisablePointerEventsRecursive(Rml::Element* element)
{
	if (!element)
		return;
	element->SetProperty("pointer-events", "none");
	for (int index = 0; index < element->GetNumChildren(); ++index)
		DisablePointerEventsRecursive(element->GetChild(index));
}

void ApplyDocumentOptions(DocumentEntry& entry)
{
	if (!entry.document)
		return;

	// A direct document fills the viewport, so its root must never become a
	// full-screen input target. Interactive content opts in with
	// `pointer-events: auto` on its visible window/root element. Click-through
	// documents are additionally excluded from input dispatch by the manager.
	entry.document->SetProperty("pointer-events", "none");
	if (entry.options.input == RmlDocumentOptions::InputMode::ClickThrough)
		DisablePointerEventsRecursive(entry.document);
}

void RefreshDocumentStacking()
{
	std::vector<DocumentEntry*> ordered;
	ordered.reserve(s_documents.size());
	for (auto& [handle, entry] : s_documents)
	{
		if (entry.document)
			ordered.push_back(&entry);
	}

	std::stable_sort(ordered.begin(), ordered.end(), [](const DocumentEntry* lhs, const DocumentEntry* rhs)
	{
		if (lhs->options.layer != rhs->options.layer)
			return lhs->options.layer < rhs->options.layer;
		if (lhs->focusOrder != rhs->focusOrder)
			return lhs->focusOrder < rhs->focusOrder;
		return lhs->handle < rhs->handle;
	});

	// Pulling documents forward in ascending order leaves the highest layer at
	// the front of the document's own context. Ordering is only meaningful within
	// one context; ordering across owners is handled by GetContextsInRenderOrder.
	for (DocumentEntry* entry : ordered)
		entry->document->PullToFront();
}

std::string EscapeRmlText(std::string_view text)
{
	std::string escaped;
	escaped.reserve(text.size());
	for (char c : text)
	{
		switch (c)
		{
		case '&': escaped += "&amp;"; break;
		case '<': escaped += "&lt;"; break;
		case '>': escaped += "&gt;"; break;
		default: escaped += c; break;
		}
	}
	return escaped;
}

bool HasVisibleDocuments()
{
	return std::any_of(s_documents.begin(), s_documents.end(), [](const auto& item)
	{
		const DocumentEntry& entry = item.second;
		return entry.document && entry.options.visible;
	});
}

bool HasInteractiveDocuments()
{
	return std::any_of(s_documents.begin(), s_documents.end(), [](const auto& item)
	{
		const DocumentEntry& entry = item.second;
		return entry.document && entry.options.visible
			&& entry.options.input == RmlDocumentOptions::InputMode::Interactive;
	});
}

bool AttachListener(ListenerEntry& entry)
{
	if (entry.attachedElement)
		return true;
	auto documentIter = s_documents.find(entry.document);
	if (documentIter == s_documents.end() || !documentIter->second.document)
		return false;

	Rml::Element* element = documentIter->second.document->GetElementById(entry.elementId);
	if (!element)
		return false;

	element->AddEventListener(entry.eventType, entry.listener.get());
	entry.attachedElement = element;
	return true;
}

void DetachListenersForDocument(RmlDocumentHandle document)
{
	for (auto& [handle, entry] : s_listeners)
	{
		if (entry.document == document && entry.attachedElement)
		{
			entry.attachedElement->RemoveEventListener(entry.eventType, entry.listener.get());
			entry.attachedElement = nullptr;
		}
	}
}

void AttachListenersForDocument(RmlDocumentHandle document)
{
	for (auto& [handle, entry] : s_listeners)
	{
		if (entry.document == document)
			AttachListener(entry);
	}
}

void RmlUiCommand(eqlib::PlayerClient*, const char* line)
{
	char argument[MAX_STRING] = {};
	char documentArgument[MAX_STRING] = {};
	GetArg(argument, line, 1);
	GetArg(documentArgument, line, 2);

	if (!_stricmp(argument, "show"))
	{
		SetVisible(true);
	}
	else if (!_stricmp(argument, "hide"))
	{
		SetVisible(false);
	}
	else if (!_stricmp(argument, "reload"))
	{
		if (documentArgument[0])
		{
			const auto path = NormalizeDocumentPath(documentArgument);
			bool succeeded = false;
			if (path)
			{
				if (DocumentEntry* entry = FindDocumentByPath(*path))
					succeeded = ReloadDocument(entry->handle);
			}
			WriteChatf("\at[RmlUi]\ax reload '%s' %s.", documentArgument,
				succeeded ? "succeeded" : "failed");
		}
		else
		{
			int loaded = 0;
			for (const auto& [handle, entry] : s_documents)
				loaded += ReloadDocument(handle) ? 1 : 0;
			WriteChatf("\at[RmlUi]\ax reloaded %d/%d document(s).",
				loaded, static_cast<int>(s_documents.size()));
		}
	}
	else if (!_stricmp(argument, "load"))
	{
		const auto path = NormalizeDocumentPath(documentArgument);
		if (!path)
		{
			WriteChatf("\ar[RmlUi]\ax invalid document path. Use a relative .rml path under resources/rmlui.");
			return;
		}
		SetVisible(true);
		const RmlDocumentHandle handle = LoadDocument(*path, RmlDocumentOptions{}, MQPluginHandle{});
		WriteChatf("\at[RmlUi]\ax load '%s' %s.", path->c_str(),
			handle != 0 ? "succeeded" : "failed");
	}
	else if (!_stricmp(argument, "close"))
	{
		const auto path = NormalizeDocumentPath(documentArgument);
		bool closed = false;
		if (path)
		{
			if (DocumentEntry* entry = FindDocumentByPath(*path))
			{
				UnloadDocument(entry->handle);
				closed = true;
			}
		}
		WriteChatf("\at[RmlUi]\ax close '%s' %s.", documentArgument,
			closed ? "succeeded" : "failed");
	}
	else if (!_stricmp(argument, "list"))
	{
		WriteChatf("\at[RmlUi]\ax %d document(s), root: %s",
			static_cast<int>(s_documents.size()), s_fileInterface.GetRoot().string().c_str());
		for (const auto& [handle, entry] : s_documents)
			WriteChatf("  \ay%s\ax", entry.path.c_str());
	}
	else if (!_stricmp(argument, "stats"))
	{
		int loaded = 0;
		int visible = 0;
		int interactive = 0;
		for (const auto& [handle, entry] : s_documents)
		{
			loaded += entry.document != nullptr ? 1 : 0;
			visible += entry.document && entry.options.visible ? 1 : 0;
			interactive += entry.document && entry.options.visible
				&& entry.options.input == RmlDocumentOptions::InputMode::Interactive ? 1 : 0;
		}

		const int attachedListeners = static_cast<int>(std::count_if(s_listeners.begin(), s_listeners.end(),
			[](const auto& item) { return item.second.attachedElement != nullptr; }));
		size_t totalModels = 0;
		for (const auto& [owner, models] : s_dataModels)
			totalModels += models.size();
		WriteChatf("\at[RmlUi]\ax documents=%d loaded=%d visible=%d interactive=%d contexts=%zu/%zu models=%zu owners=%zu listeners=%d/%d",
			static_cast<int>(s_documents.size()), loaded, visible, interactive,
			s_contexts.size(), s_maxContexts, totalModels, s_owners.size(),
			attachedListeners, static_cast<int>(s_listeners.size()));
		for (const auto& ownerEntry : s_owners)
		{
			const RmlOwnerHandle owner = ownerEntry.first;
			auto models = s_dataModels.find(owner);
			const size_t modelCount = models == s_dataModels.end() ? 0 : models->second.size();
			WriteChatf("  owner=%llu context=%s documents=%zu/%zu listeners=%zu/%zu models=%zu model-vars=%zu/%zu array-entries=%zu/%zu fields=%zu/%zu model=%.1f/%.1f KiB texture=%.1f/%.1f KiB",
				static_cast<unsigned long long>(owner), GetContext(owner) ? "yes" : "no",
				CountDocumentsForOwner(owner), s_maxDocumentsPerScopedOwner,
				CountListenersForOwner(owner), s_maxListenersPerScopedOwner,
				modelCount,
				CountModelVariablesForOwner(owner), s_maxModelVariablesPerScopedOwner,
				CountModelArrayEntriesForOwner(owner), s_maxModelArrayEntriesPerScopedOwner,
				CountModelFieldsForOwner(owner), s_maxModelFieldsPerScopedOwner,
				static_cast<double>(CountModelBytesForOwner(owner)) / 1024.0,
				static_cast<double>(s_maxModelBytesPerScopedOwner) / 1024.0,
				static_cast<double>(GetTextureBytesForOwner(owner)) / 1024.0,
				static_cast<double>(s_maxTextureBytesPerScopedOwner) / 1024.0);
		}
		const std::string nextUpdate = !HasVisibleDocuments() ? "idle" : std::isfinite(s_nextUpdateDelay)
			? fmt::format("{:.3f}s", s_nextUpdateDelay) : "idle";
		WriteChatf("  update=%.1f us (#%llu), render=%.1f us (#%llu), next-update=%s",
			s_lastUpdateMicroseconds, static_cast<unsigned long long>(s_updateCount),
			s_lastRenderMicroseconds, static_cast<unsigned long long>(s_renderCount),
			nextUpdate.c_str());
	#if HAS_DIRECTX_9
		const RendererDX9::Statistics& stats = s_renderer.GetStatistics();
		WriteChatf("  draws=%llu vertices=%llu triangles=%llu geometry=%.1f KiB textures=%.1f KiB",
			static_cast<unsigned long long>(stats.drawCalls),
			static_cast<unsigned long long>(stats.vertices),
			static_cast<unsigned long long>(stats.triangles),
			static_cast<double>(stats.geometryBytes) / 1024.0,
			static_cast<double>(stats.textureBytes) / 1024.0);
	#endif
		for (const auto& [handle, entry] : s_documents)
		{
			const auto resolved = s_fileInterface.Resolve(entry.path);
			WriteChatf("  #%llu %s layer=%d focus=%llu input=%s state=%s owner=%llu source=%s",
				static_cast<unsigned long long>(handle), entry.path.c_str(), entry.options.layer,
				static_cast<unsigned long long>(entry.focusOrder),
				entry.options.input == RmlDocumentOptions::InputMode::Interactive ? "interactive" : "click-through",
				!entry.document ? "pending" : entry.options.visible ? "visible" : "hidden",
				static_cast<unsigned long long>(entry.scopedOwner),
				resolved ? resolved->string().c_str() : "unresolved");
		}
	}
	else
	{
		WriteChatf("\at[RmlUi]\ax initialized=%s supported=%s visible=%s version=%s",
			s_initialized ? "yes" : "no", s_supported ? "yes" : "no",
			s_visible ? "yes" : "no", s_initialized ? Rml::GetVersion().c_str() : "n/a");
		WriteChatf("Usage: \ay/rmlui show|hide|load <file>|close <file>|reload [file]|list|stats|status\ax");
	}
}

} // namespace

// Defined alongside the rest of the model subsystem below, but declared here
// because the document load path has to declare models before the document
// parses.
ModelState* GetOrCreateModel(RmlOwnerHandle owner, Rml::Context* context, const std::string& name);
bool DeclareModelVariable(RmlOwnerHandle owner, Rml::Context* context,
	const std::string& model, const std::string& variable);

void ClearDocumentResourceCaches()
{
	// RmlUi caches linked RCSS and templates globally, even after every document
	// using them has been unloaded. Clear before parsing so script restart and
	// reload observe edited resources instead of silently reusing stale styles.
	Rml::Factory::ClearStyleSheetCache();
	Rml::Factory::ClearTemplateCache();
}

RmlDocumentHandle LoadDocumentWithHandle(RmlDocumentHandle handle, std::string_view virtualPath,
	const RmlDocumentOptions& options, MQPluginHandle owner, RmlOwnerHandle scopedOwner = 0)
{
	if (!s_initialized || handle == 0)
		return 0;
	if (scopedOwner != 0 && CountDocumentsForOwner(scopedOwner) >= s_maxDocumentsPerScopedOwner)
	{
		if (ShouldLogOwnerLimit(scopedOwner, "documents"))
			LOG_WARN("RmlUi: owner {} reached the document quota ({})",
				scopedOwner, s_maxDocumentsPerScopedOwner);
		return 0;
	}

	if (options.presentation != RmlDocumentOptions::Presentation::Direct)
	{
		LOG_WARN("RmlUi: hosted presentation is not implemented; rejecting document '{}'", virtualPath);
		return 0;
	}

	const auto normalized = NormalizeDocumentPath(std::string(virtualPath).c_str());
	if (!normalized)
		return 0;

	const std::string path = *normalized;

	// Every load creates an independent instance. Different consumers commonly use
	// the same template and must retain distinct handles, state, options, and owners.
	// Register it immediately so the caller owns a handle even
	// if the actual Rml document can't be loaded until the graphics device is ready.
	s_documents.emplace(handle, DocumentEntry{ handle, path, nullptr, options, owner, scopedOwner,
		s_nextFocusOrder++ });

	if (Rml::Context* context = EnsureContextForOwner(scopedOwner))
	{
		s_inputReadyContexts.erase(context);
		DocumentEntry& entry = s_documents.at(handle);

		// Declare this document's models before it parses. RmlUi resolves a
		// document's data-model attributes while parsing, so a model created
		// afterwards is invisible to the document; declaring up front is what
		// lets a model's first update work without reloading the document.
		for (const std::string& modelName : options.models)
			GetOrCreateModel(scopedOwner, context, modelName);
		for (const auto& [modelName, variables] : options.modelVariables)
		{
			for (const std::string& variable : variables)
				DeclareModelVariable(scopedOwner, context, modelName, variable);
		}

	#if HAS_DIRECTX_9
		ScopedResourceOwner resourceOwner(entry.scopedOwner);
	#endif
		ClearDocumentResourceCaches();
		Rml::ElementDocument* document = context->LoadDocument(path);
		if (!document)
		{
			s_documents.erase(handle);
			return 0;
		}

		entry.document = document;
		ApplyDocumentOptions(entry);
		if (options.visible)
			document->Show();
		RefreshDocumentStacking();
		s_inputDirty = true;
	}
	else
	{
		s_pendingHandles.push_back(handle);
	}

	return handle;
}

RmlDocumentHandle LoadDocument(std::string_view virtualPath, const RmlDocumentOptions& options, MQPluginHandle owner)
{
	return LoadDocumentWithHandle(s_nextDocumentHandle.fetch_add(1, std::memory_order_relaxed),
		virtualPath, options, owner, 0);
}

RmlOwnerHandle CreateOwner(MQPluginHandle parent)
{
	if (!s_initialized || parent.pluginID == 0)
		return 0;
	const RmlOwnerHandle owner = s_nextOwnerHandle.fetch_add(1, std::memory_order_relaxed);
	s_owners.emplace(owner, parent);
	return owner;
}

void DestroyOwner(RmlOwnerHandle owner)
{
	if (owner == 0)
		return;

	std::vector<RmlDocumentHandle> documents;
	for (const auto& [handle, entry] : s_documents)
	{
		if (entry.scopedOwner == owner)
			documents.push_back(handle);
	}
	for (RmlDocumentHandle document : documents)
		UnloadDocument(document);

	// The owner's models live in its context and are meaningless once it is gone.
	// They must be dropped before the context so no DataVariable outlives it.
	if (Rml::Context* context = GetContext(owner))
	{
		auto models = s_dataModels.find(owner);
		if (models != s_dataModels.end())
		{
			for (const auto& [name, model] : models->second)
				context->RemoveDataModel(Rml::String(name.begin(), name.end()));
		}
	}
	s_dataModels.erase(owner);
	RemoveContextForOwner(owner);
	s_owners.erase(owner);
}

RmlDocumentHandle LoadDocumentForOwner(std::string_view virtualPath, const RmlDocumentOptions& options,
	RmlOwnerHandle owner)
{
	auto iter = s_owners.find(owner);
	if (iter == s_owners.end())
		return 0;
	return LoadDocumentWithHandle(s_nextDocumentHandle.fetch_add(1, std::memory_order_relaxed),
		virtualPath, options, iter->second, owner);
}

void FlushPendingDocuments()
{
	if (s_pendingHandles.empty())
		return;

	std::vector<RmlDocumentHandle> pending = std::move(s_pendingHandles);
	s_pendingHandles.clear();

	for (RmlDocumentHandle handle : pending)
	{
		auto iter = s_documents.find(handle);
		if (iter == s_documents.end() || iter->second.document != nullptr)
			continue;

		Rml::Context* context = EnsureContextForOwner(iter->second.scopedOwner);
		if (!context)
			continue;

		// Pending documents get the same pre-parse model declaration as a
		// document loaded straight from the device-ready path.
		for (const std::string& modelName : iter->second.options.models)
			GetOrCreateModel(iter->second.scopedOwner, context, modelName);
		for (const auto& [modelName, variables] : iter->second.options.modelVariables)
		{
			for (const std::string& variable : variables)
				DeclareModelVariable(iter->second.scopedOwner, context, modelName, variable);
		}

	#if HAS_DIRECTX_9
		ScopedResourceOwner resourceOwner(iter->second.scopedOwner);
	#endif
		ClearDocumentResourceCaches();
		Rml::ElementDocument* document = context->LoadDocument(iter->second.path);
		if (document)
		{
			iter->second.document = document;
			ApplyDocumentOptions(iter->second);
			if (iter->second.options.visible)
				document->Show();
			RefreshDocumentStacking();

			// A newly loaded document needs an update pass.
			s_inputDirty = true;
		}
	}
}

void UnloadDocument(RmlDocumentHandle handle)
{
	auto iter = s_documents.find(handle);
	if (iter == s_documents.end())
		return;

	DetachListenersForDocument(handle);
	if (Rml::Context* context = GetContext(iter->second.scopedOwner); context && iter->second.document)
		context->UnloadDocument(iter->second.document);
	s_documents.erase(iter);
	for (auto listenerIter = s_listeners.begin(); listenerIter != s_listeners.end();)
	{
		if (listenerIter->second.document == handle)
			listenerIter = s_listeners.erase(listenerIter);
		else
			++listenerIter;
	}

	// Models deliberately outlive the document that first declared them. A model
	// belongs to the owner's namespace and is shared by every document that
	// owner loads, so it is dropped in DestroyOwner instead. Keeping it here
	// would break a second document of the same owner that binds the same model.

	if (!HasInteractiveDocuments())
	{
		OverlayClearCapture();
		s_mouseOverInteractive = false;
		s_mouseCaptureButtons = 0;
	}
}

bool ReloadDocument(RmlDocumentHandle handle)
{
	auto iter = s_documents.find(handle);
	if (iter == s_documents.end())
		return false;

	Rml::Context* context = EnsureContextForOwner(iter->second.scopedOwner);
	if (!context)
		return false;
	s_inputReadyContexts.erase(context);

	#if HAS_DIRECTX_9
	ScopedResourceOwner resourceOwner(iter->second.scopedOwner);
	#endif
	ClearDocumentResourceCaches();
	Rml::ElementDocument* replacement = context->LoadDocument(iter->second.path);
	if (!replacement)
		return false;

	DetachListenersForDocument(handle);
	if (iter->second.document)
		context->UnloadDocument(iter->second.document);
	iter->second.document = replacement;
	ApplyDocumentOptions(iter->second);
	if (iter->second.options.visible)
		replacement->Show();
	RefreshDocumentStacking();
	AttachListenersForDocument(handle);
	s_inputDirty = true;
	return true;
}

bool IsDocumentLoaded(RmlDocumentHandle handle)
{
	auto iter = s_documents.find(handle);
	return iter != s_documents.end() && iter->second.document != nullptr;
}

bool SetDocumentVisible(RmlDocumentHandle handle, bool visible)
{
	auto iter = s_documents.find(handle);
	if (iter == s_documents.end())
		return false;

	iter->second.options.visible = visible;
	if (!iter->second.document)
		return false;

	if (visible)
		iter->second.document->Show();
	else
		iter->second.document->Hide();
	s_inputDirty = true;
	if (!HasInteractiveDocuments())
	{
		OverlayClearCapture();
		s_mouseOverInteractive = false;
		s_mouseCaptureButtons = 0;
	}
	return true;
}

Rml::Element* FindDocumentElement(RmlDocumentHandle handle, std::string_view elementId)
{
	auto iter = s_documents.find(handle);
	if (iter == s_documents.end() || !iter->second.document)
		return nullptr;

	return iter->second.document->GetElementById(Rml::String(elementId));
}

bool SetDocumentText(RmlDocumentHandle handle, std::string_view elementId, std::string_view text)
{
	Rml::Element* element = FindDocumentElement(handle, elementId);
	if (!element)
		return false;

	element->SetInnerRML(EscapeRmlText(text));
	s_inputDirty = true;
	return true;
}

bool SetDocumentValue(RmlDocumentHandle handle, std::string_view elementId, std::string_view value)
{
	Rml::Element* element = FindDocumentElement(handle, elementId);
	if (!element)
		return false;

	element->SetAttribute("value", Rml::String(value));
	s_inputDirty = true;
	return true;
}

bool SetDocumentClass(RmlDocumentHandle handle, std::string_view elementId, std::string_view className, bool activate)
{
	Rml::Element* element = FindDocumentElement(handle, elementId);
	if (!element)
		return false;

	element->SetClass(Rml::String(className), activate);
	s_inputDirty = true;
	return true;
}

bool SetDocumentStyle(RmlDocumentHandle handle, std::string_view elementId, std::string_view property, std::string_view value)
{
	Rml::Element* element = FindDocumentElement(handle, elementId);
	if (!element)
		return false;

	element->SetProperty(Rml::String(property), Rml::String(value));
	s_inputDirty = true;
	return true;
}

bool SetDocumentAttribute(RmlDocumentHandle handle, std::string_view elementId,
	std::string_view attribute, std::string_view value)
{
	Rml::Element* element = FindDocumentElement(handle, elementId);
	if (!element)
		return false;
	element->SetAttribute(Rml::String(attribute), Rml::String(value));
	s_inputDirty = true;
	return true;
}

bool RemoveDocumentAttribute(RmlDocumentHandle handle, std::string_view elementId,
	std::string_view attribute)
{
	Rml::Element* element = FindDocumentElement(handle, elementId);
	if (!element)
		return false;
	element->RemoveAttribute(Rml::String(attribute));
	s_inputDirty = true;
	return true;
}

bool GetDocumentAttribute(RmlDocumentHandle handle, std::string_view elementId,
	std::string_view attribute, std::string& value)
{
	Rml::Element* element = FindDocumentElement(handle, elementId);
	if (!element || !element->HasAttribute(Rml::String(attribute)))
		return false;
	const Rml::String result = element->GetAttribute<Rml::String>(Rml::String(attribute), "");
	value.assign(result.begin(), result.end());
	return true;
}

bool FocusDocumentElement(RmlDocumentHandle handle, std::string_view elementId)
{
	Rml::Element* element = FindDocumentElement(handle, elementId);
	return element && element->Focus();
}

bool BlurDocumentElement(RmlDocumentHandle handle, std::string_view elementId)
{
	Rml::Element* element = FindDocumentElement(handle, elementId);
	if (!element)
		return false;
	element->Blur();
	return true;
}

// Model namespaces belong to a scoped owner, and every owner has its own
// context, so two scripts may both declare a model named "hud" without
// observing or mutating one another. There is deliberately no cross-owner
// ownership check here any more; isolation comes from the context.
ModelState* GetOrCreateModel(RmlOwnerHandle owner, Rml::Context* context, const std::string& name)
{
	auto& models = s_dataModels[owner];
	auto [it, inserted] = models.try_emplace(name);
	if (!inserted)
		return it->second.constructor ? &it->second : nullptr;

	it->second.constructor = context->CreateDataModel(Rml::String(name.begin(), name.end()));
	it->second.handle = it->second.constructor.GetModelHandle();
	return it->second.constructor ? &it->second : nullptr;
}

ModelState* FindModel(RmlOwnerHandle owner, std::string_view name)
{
	auto models = s_dataModels.find(owner);
	if (models == s_dataModels.end())
		return nullptr;

	auto found = models->second.find(std::string(name));
	return found == models->second.end() ? nullptr : &found->second;
}

bool DeclareModelVariable(RmlOwnerHandle owner, Rml::Context* context,
	const std::string& model, const std::string& variable)
{
	ModelState* state = GetOrCreateModel(owner, context, model);
	if (!state || !state->constructor)
		return false;

	auto [iter, inserted] = state->variables.try_emplace(variable);
	if (!inserted)
		return true;
	if (CountModelVariablesForOwner(owner) > s_maxModelVariablesPerScopedOwner)
	{
		state->variables.erase(iter);
		if (ShouldLogOwnerLimit(owner, "model-variables"))
			LOG_WARN("RmlUi: owner {} reached the model-variable quota ({})",
				owner, s_maxModelVariablesPerScopedOwner);
		return false;
	}

	if (!state->constructor.BindCustomDataVariable(
			Rml::String(variable.begin(), variable.end()), MakeModelVariable(iter->second.value)))
	{
		state->variables.erase(iter);
		return false;
	}
	return true;
}

bool BringDocumentToFront(RmlDocumentHandle handle)
{
	auto iter = s_documents.find(handle);
	if (iter == s_documents.end() || !iter->second.document)
		return false;

	iter->second.focusOrder = s_nextFocusOrder++;
	RefreshDocumentStacking();
	s_inputDirty = true;
	return true;
}

bool GetDocumentGeometry(RmlDocumentHandle handle, std::string_view elementId, RmlElementGeometry& geometry)
{
	Rml::Element* element = FindDocumentElement(handle, elementId);
	if (!element || element->GetNumBoxes() == 0)
		return false;

	const Rml::Vector2f position = element->GetAbsoluteOffset(Rml::BoxArea::Border);
	const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Content);
	geometry = { position.x, position.y, size.x, size.y };
	return true;
}

bool SetDocumentGeometry(RmlDocumentHandle handle, std::string_view elementId,
	const RmlElementGeometry& geometry)
{
	if (!std::isfinite(geometry.x) || !std::isfinite(geometry.y)
		|| !std::isfinite(geometry.width) || !std::isfinite(geometry.height)
		|| geometry.width <= 0.0f || geometry.height <= 0.0f)
	{
		return false;
	}

	auto document = s_documents.find(handle);
	Rml::Element* element = FindDocumentElement(handle, elementId);
	if (document == s_documents.end() || !element)
		return false;

	RmlElementGeometry clamped = geometry;
	if (Rml::Context* context = GetContext(document->second.scopedOwner))
	{
		const Rml::Vector2i viewport = context->GetDimensions();
		if (viewport.x > 0 && viewport.y > 0)
		{
			const float viewportWidth = static_cast<float>(viewport.x);
			const float viewportHeight = static_cast<float>(viewport.y);
			clamped.width = std::clamp(clamped.width,
				std::min(64.0f, viewportWidth), viewportWidth);
			clamped.height = std::clamp(clamped.height,
				std::min(64.0f, viewportHeight), viewportHeight);
			// Restored/API-supplied rectangles are fully recovered into the current
			// viewport. Native dragging may still place a window partly off-screen
			// during the session, but it cannot become stranded after restart.
			clamped.x = std::clamp(clamped.x, 0.0f, viewportWidth - clamped.width);
			clamped.y = std::clamp(clamped.y, 0.0f, viewportHeight - clamped.height);
		}
	}

	element->SetProperty("left", fmt::format("{}px", clamped.x));
	element->SetProperty("top", fmt::format("{}px", clamped.y));
	element->SetProperty("width", fmt::format("{}px", clamped.width));
	element->SetProperty("height", fmt::format("{}px", clamped.height));
	s_inputDirty = true;
	return true;
}

bool GetDocumentScrollMetrics(RmlDocumentHandle handle, std::string_view elementId,
	RmlScrollMetrics& metrics)
{
	Rml::Element* element = FindDocumentElement(handle, elementId);
	if (!element)
		return false;

	metrics.offset = element->GetScrollTop();
	metrics.extent = element->GetScrollHeight();
	metrics.viewport = element->GetClientHeight();
	return true;
}

bool SetDocumentScrollOffset(RmlDocumentHandle handle, std::string_view elementId, float offset)
{
	Rml::Element* element = FindDocumentElement(handle, elementId);
	if (!element || !std::isfinite(offset))
		return false;

	element->SetScrollTop(std::max(0.0f, offset));
	s_inputDirty = true;
	return true;
}

bool ScrollDocumentToEnd(RmlDocumentHandle handle, std::string_view elementId)
{
	Rml::Element* element = FindDocumentElement(handle, elementId);
	if (!element)
		return false;

	element->SetScrollTop(std::max(0.0f,
		element->GetScrollHeight() - element->GetClientHeight()));
	s_inputDirty = true;
	return true;
}

bool SetDocumentScrollAnchor(RmlDocumentHandle handle, std::string_view elementId,
	bool enabled, float threshold)
{
	auto document = s_documents.find(handle);
	if (document == s_documents.end() || !document->second.document || elementId.empty()
		|| !std::isfinite(threshold) || threshold < 0.0f)
	{
		return false;
	}

	const std::string id(elementId);
	if (!enabled)
	{
		document->second.scrollAnchors.erase(id);
		return true;
	}
	if (!FindDocumentElement(handle, elementId))
		return false;

	document->second.scrollAnchors[id].threshold = threshold;
	return true;
}

// The single entry point for every model write. The typed helpers and the
// structured SetModelValue all funnel through here so that quota accounting and
// dirtying behave identically no matter how a value was produced.
//
// A variable's value may change between scalar, array, and record over its
// lifetime. That is free here because one dynamic definition covers every kind
// and RmlUi refuses to rebind a name, so the binding is made once and the stored
// value is simply replaced.
bool SetModelValue(RmlDocumentHandle handle, std::string_view model, std::string_view variable, const RmlUiValue& value)
{
	auto docIt = s_documents.find(handle);
	if (docIt == s_documents.end() || !docIt->second.document)
		return false;

	const RmlOwnerHandle owner = docIt->second.scopedOwner;
	Rml::Context* context = GetContext(owner);
	if (!context)
		return false;

	// Measure before storing so an oversized tree is never briefly resident, and
	// so the walk is bounded even for a hostile value.
	ModelValueUsage usage;
	std::string error;
	if (!MeasureModelValue(value, s_maxModelValueDepth, s_maxModelFieldsPerRecord,
		s_maxModelArrayEntriesPerScopedOwner, usage, error))
	{
		if (ShouldLogOwnerLimit(owner, "model-shape"))
			LOG_WARN("RmlUi: owner {} rejected model variable '{}': {}", owner, variable, error);
		return false;
	}

	ModelState* state = FindModel(owner, model);
	if (!state)
		state = GetOrCreateModel(owner, context, std::string(model));
	if (!state || !state->constructor)
		return false;

	const std::string name(variable);
	auto existing = state->variables.find(name);
	const bool variableCreated = existing == state->variables.end();

	if (variableCreated && CountModelVariablesForOwner(owner) >= s_maxModelVariablesPerScopedOwner)
	{
		if (ShouldLogOwnerLimit(owner, "model-variables"))
			LOG_WARN("RmlUi: owner {} reached the model-variable quota ({})",
				owner, s_maxModelVariablesPerScopedOwner);
		return false;
	}

	// Quotas are cumulative per owner, so replacing a value is admitted only if
	// the owner's totals stay under budget once the old value is discounted.
	const size_t oldBytes = variableCreated ? 0 : existing->second.usage.bytes;
	const size_t oldEntries = variableCreated ? 0 : existing->second.usage.arrayEntries;
	const size_t oldFields = variableCreated ? 0 : existing->second.usage.fields;

	if (CountModelArrayEntriesForOwner(owner) - oldEntries
		> s_maxModelArrayEntriesPerScopedOwner - usage.arrayEntries)
	{
		if (ShouldLogOwnerLimit(owner, "model-array-entries"))
			LOG_WARN("RmlUi: owner {} reached the model-array entry quota ({})",
				owner, s_maxModelArrayEntriesPerScopedOwner);
		return false;
	}

	if (CountModelFieldsForOwner(owner) - oldFields
		> s_maxModelFieldsPerScopedOwner - usage.fields)
	{
		if (ShouldLogOwnerLimit(owner, "model-fields"))
			LOG_WARN("RmlUi: owner {} reached the model field quota ({})",
				owner, s_maxModelFieldsPerScopedOwner);
		return false;
	}

	if (CountModelBytesForOwner(owner) - oldBytes > s_maxModelBytesPerScopedOwner - usage.bytes)
	{
		if (ShouldLogOwnerLimit(owner, "model-bytes"))
			LOG_WARN("RmlUi: owner {} reached the model byte quota ({} bytes)",
				owner, s_maxModelBytesPerScopedOwner);
		return false;
	}

	auto [varIt, inserted] = state->variables.try_emplace(name);
	varIt->second.value = value;
	varIt->second.usage = usage;

	// Bind once, on creation. RmlUi will not rebind the name later, and the
	// definition handed out here already handles every value kind.
	if (inserted && !state->constructor.BindCustomDataVariable(
			Rml::String(name.begin(), name.end()), MakeModelVariable(varIt->second.value)))
	{
		state->variables.erase(varIt);
		return false;
	}

	if (state->handle)
		state->handle.DirtyVariable(Rml::String(name.begin(), name.end()));

	s_inputDirty = true;
	return true;
}

bool GetModelValue(RmlDocumentHandle handle, std::string_view model, std::string_view variable, RmlUiValue& value)
{
	auto docIt = s_documents.find(handle);
	if (docIt == s_documents.end())
		return false;

	ModelState* state = FindModel(docIt->second.scopedOwner, model);
	if (!state)
		return false;

	auto found = state->variables.find(std::string(variable));
	if (found == state->variables.end())
		return false;

	value = found->second.value;
	return true;
}

bool SpliceModelArray(RmlDocumentHandle handle, std::string_view model, std::string_view variable,
	size_t index, size_t deleteCount, const std::vector<RmlUiValue>& values)
{
	auto docIt = s_documents.find(handle);
	if (docIt == s_documents.end() || !docIt->second.document)
		return false;

	ModelState* state = FindModel(docIt->second.scopedOwner, model);
	if (!state)
		return false;

	auto found = state->variables.find(std::string(variable));
	if (found == state->variables.end() || !found->second.value.IsArray())
		return false;

	// Work on a snapshot so validation and quota rejection leave the live model
	// untouched. SetModelValue remains the single quota/accounting path.
	RmlUiValue replacement = found->second.value;
	if (!replacement.SpliceArray(index, deleteCount, values))
		return false;

	return SetModelValue(handle, model, variable, replacement);
}

bool DeclareModel(RmlDocumentHandle handle, std::string_view model)
{
	auto docIt = s_documents.find(handle);
	if (docIt == s_documents.end())
		return false;

	Rml::Context* context = GetContext(docIt->second.scopedOwner);
	if (!context)
		return false;

	return GetOrCreateModel(docIt->second.scopedOwner, context, std::string(model)) != nullptr;
}

bool SetModelString(RmlDocumentHandle handle, std::string_view model, std::string_view variable, std::string_view value)
{
	return SetModelValue(handle, model, variable, RmlUiValue(std::string(value)));
}

bool SetModelNumber(RmlDocumentHandle handle, std::string_view model, std::string_view variable, double value)
{
	return SetModelValue(handle, model, variable, RmlUiValue(value));
}

bool SetModelBoolean(RmlDocumentHandle handle, std::string_view model, std::string_view variable, bool value)
{
	return SetModelValue(handle, model, variable, RmlUiValue(value));
}

bool SetModelStringArray(RmlDocumentHandle handle, std::string_view model, std::string_view variable,
	const std::vector<std::string>& values)
{
	RmlUiValue array = RmlUiValue::MakeArray();
	for (const std::string& value : values)
		array.AddElement(RmlUiValue(value));
	return SetModelValue(handle, model, variable, array);
}

bool SetModelNumberArray(RmlDocumentHandle handle, std::string_view model, std::string_view variable,
	const std::vector<double>& values)
{
	RmlUiValue array = RmlUiValue::MakeArray();
	for (double value : values)
		array.AddElement(RmlUiValue(value));
	return SetModelValue(handle, model, variable, array);
}

RmlListenerHandle AddDocumentEventListener(RmlDocumentHandle document, std::string_view eventType,
	std::string_view elementId, RmlEventCallback callback, void* userData)
{
	if (!callback || eventType.empty() || elementId.empty())
		return 0;

	auto documentIter = s_documents.find(document);
	if (documentIter == s_documents.end() || !documentIter->second.document)
		return 0;
	const RmlOwnerHandle owner = documentIter->second.scopedOwner;
	if (owner != 0 && CountListenersForOwner(owner) >= s_maxListenersPerScopedOwner)
	{
		if (ShouldLogOwnerLimit(owner, "listeners"))
			LOG_WARN("RmlUi: owner {} reached the listener quota ({})",
				owner, s_maxListenersPerScopedOwner);
		return 0;
	}

	const RmlListenerHandle handle = s_nextListenerHandle++;
	ListenerEntry entry;
	entry.handle = handle;
	entry.document = document;
	entry.eventType = eventType;
	entry.elementId = elementId;
	entry.listener = std::make_unique<EventListener>(callback, userData);

	auto [iter, inserted] = s_listeners.emplace(handle, std::move(entry));
	if (!inserted)
	{
		s_listeners.erase(handle);
		return 0;
	}

	// Data-bound subtrees may not exist until their model is created and the
	// document is reloaded. Keep the listener registration pending; reload calls
	// AttachListenersForDocument and resolves the element then.
	if (!AttachListener(iter->second))
	{
		LOG_DEBUG("RmlUi: listener {} for event '{}' is pending element '{}' in document {}",
			handle, eventType, elementId, document);
	}
	return handle;
}

void RemoveDocumentEventListener(RmlListenerHandle listener)
{
	auto iter = s_listeners.find(listener);
	if (iter == s_listeners.end())
		return;

	ListenerEntry& entry = iter->second;
	if (entry.attachedElement)
		entry.attachedElement->RemoveEventListener(entry.eventType, entry.listener.get());
	s_listeners.erase(iter);
}

void SetLuaDirectory(std::string_view directory)
{
	if (directory.empty())
		return;

	const std::filesystem::path luaRoot(directory);
	s_fileInterface.SetLuaRoot(luaRoot);
#if HAS_DIRECTX_9
	const std::filesystem::path resourceRoot = std::filesystem::path(internal_paths::Resources) / "rmlui";
	const std::filesystem::path pluginsRoot = std::filesystem::path(internal_paths::Plugins);
	s_renderer.SetResourceRoot(resourceRoot, pluginsRoot, luaRoot);
	s_renderer.SetResourceOwnerResolver(ResolveResourceOwner);
#endif
	LOG_INFO("RmlUi: lua:// root set to '{}'", luaRoot.string());
}

void RemoveDocumentsForPlugin(MQPluginHandle owner)
{
	if (owner.pluginID == 0)
		return;

	std::vector<RmlDocumentHandle> handles;
	handles.reserve(s_documents.size());
	for (const auto& [handle, entry] : s_documents)
	{
		if (entry.owner == owner)
			handles.push_back(handle);
	}

	for (RmlDocumentHandle handle : handles)
		UnloadDocument(handle);

	for (auto iter = s_owners.begin(); iter != s_owners.end();)
	{
		if (iter->second == owner)
			iter = s_owners.erase(iter);
		else
			++iter;
	}
}

void Initialize()
{
	AddCommand("/rmlui", RmlUiCommand, false, false, false);

	if (s_inputConsumer == 0)
	{
		OverlayInputConsumer consumer;
		consumer.layer = OverlayInputLayer::RmlUi;
		consumer.handleWndProc = [](HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
		{
			return HandleWndProc(hWnd, msg, wParam, lParam);
		};
		consumer.wantsMouseInput = []() { return WantsMouseInput(); };
		consumer.wantsKeyboardInput = []() { return WantsKeyboardInput(); };
		consumer.consumeKeyboardInput = []() { return ConsumePendingKeyboardInput(); };
		s_inputConsumer = RegisterOverlayInputConsumer(std::move(consumer));
	}

	const bool enabled = GetPrivateProfileBool("RmlUi", "Enabled", false, internal_paths::MQini);
	if (!enabled)
	{
		LOG_INFO("RmlUi core support is disabled. Set [RmlUi] Enabled=1 to enable it.");
		return;
	}

#if HAS_DIRECTX_9
	const std::filesystem::path resourceRoot = std::filesystem::path(internal_paths::Resources) / "rmlui";
	const std::filesystem::path pluginsRoot = std::filesystem::path(internal_paths::Plugins);
	// Lua resources live next to the Lua scripts. Default to <MQRoot>/lua, but allow
	// an explicit override for installations that relocate the Lua directory.
	const std::string luaDirectoryOverride = GetPrivateProfileString("RmlUi", "LuaDirectory", "", internal_paths::MQini);
	const std::filesystem::path luaRoot = luaDirectoryOverride.empty()
		? std::filesystem::path(internal_paths::MQRoot) / "lua"
		: std::filesystem::path(luaDirectoryOverride);
	s_maxDocumentsPerScopedOwner = static_cast<size_t>(std::clamp(
		GetPrivateProfileInt("RmlUi", "MaxDocumentsPerOwner", 16, internal_paths::MQini), 1, 64));
	s_maxListenersPerScopedOwner = static_cast<size_t>(std::clamp(
		GetPrivateProfileInt("RmlUi", "MaxListenersPerOwner", 128, internal_paths::MQini), 1, 1024));
	s_maxModelVariablesPerScopedOwner = static_cast<size_t>(std::clamp(
		GetPrivateProfileInt("RmlUi", "MaxModelVariablesPerOwner", 256, internal_paths::MQini), 1, 4096));
	s_maxModelArrayEntriesPerScopedOwner = static_cast<size_t>(std::clamp(
		GetPrivateProfileInt("RmlUi", "MaxModelArrayEntriesPerOwner", 4096, internal_paths::MQini), 1, 65536));
	s_maxModelFieldsPerRecord = static_cast<size_t>(std::clamp(
		GetPrivateProfileInt("RmlUi", "MaxModelFieldsPerRecord", 128, internal_paths::MQini), 1, 4096));
	s_maxModelFieldsPerScopedOwner = static_cast<size_t>(std::clamp(
		GetPrivateProfileInt("RmlUi", "MaxModelFieldsPerOwner", 4096, internal_paths::MQini), 1, 65536));
	s_maxModelValueDepth = static_cast<size_t>(std::clamp(
		GetPrivateProfileInt("RmlUi", "MaxModelValueDepth", 8, internal_paths::MQini), 1, 32));
	s_maxModelBytesPerScopedOwner = static_cast<size_t>(std::clamp(
		GetPrivateProfileInt("RmlUi", "MaxModelMiBPerOwner", 8, internal_paths::MQini), 1, 256)) * 1024 * 1024;
	s_maxContexts = static_cast<size_t>(std::clamp(
		GetPrivateProfileInt("RmlUi", "MaxContexts", 32, internal_paths::MQini), 1, 256));
	s_maxTextureDimension = std::clamp(
		GetPrivateProfileInt("RmlUi", "MaxTextureDimension", 4096, internal_paths::MQini), 256, 8192);
	const int maxTextureMiB = std::clamp(
		GetPrivateProfileInt("RmlUi", "MaxTextureMiBPerOwner", 64, internal_paths::MQini), 1, 256);
	s_maxTextureBytesPerScopedOwner = static_cast<size_t>(maxTextureMiB) * 1024 * 1024;
	s_fileInterface.SetRoot(resourceRoot);
	s_fileInterface.SetPluginsRoot(pluginsRoot);
	s_fileInterface.SetLuaRoot(luaRoot);
	s_renderer.SetResourceRoot(resourceRoot, pluginsRoot, luaRoot);
	s_renderer.SetResourceOwnerResolver(ResolveResourceOwner);
	s_renderer.SetTextureLimits(s_maxTextureDimension, s_maxTextureBytesPerScopedOwner);
	Rml::SetSystemInterface(&s_systemInterface);
	Rml::SetFileInterface(&s_fileInterface);
	Rml::SetRenderInterface(&s_renderer);
	if (!Rml::Initialise())
	{
		LOG_ERROR("RmlUi core initialization failed");
		return;
	}

	// Custom MQ elements (<resize-edge>, ...). Registered once, before any
	// document parses, so all subsequently loaded documents can use them.
	RegisterResizeEdgeElement();

	s_initialized = true;
	s_acceptingCommands.store(true, std::memory_order_release);
	s_supported = true;
	s_visible = GetPrivateProfileBool("RmlUi", "Visible", true, internal_paths::MQini);
	s_updateRateForeground = GetPrivateProfileInt("RmlUi", "UpdateRateForeground", 60, internal_paths::MQini);
	s_updateRateBackground = GetPrivateProfileInt("RmlUi", "UpdateRateBackground", 10, internal_paths::MQini);
	if (s_updateRateForeground <= 0) s_updateRateForeground = 60;
	if (s_updateRateBackground <= 0) s_updateRateBackground = 10;
	if (!LoadDefaultFonts())
		LOG_WARN("RmlUi could not load the default Segoe UI font");

	LOG_INFO("RmlUi {} core initialized (DX9 direct renderer)", Rml::GetVersion());
#else
	LOG_WARN("RmlUi is enabled, but this client does not yet have an RmlUi renderer backend");
#endif
}

void Shutdown()
{
	s_acceptingCommands.store(false, std::memory_order_release);
	RemoveCommand("/rmlui");

	if (s_inputConsumer != 0)
	{
		UnregisterOverlayInputConsumer(s_inputConsumer);
		s_inputConsumer = 0;
	}

	if (!s_initialized)
		return;

	for (auto& [handle, entry] : s_listeners)
	{
		if (entry.attachedElement)
			entry.attachedElement->RemoveEventListener(entry.eventType, entry.listener.get());
	}
	s_listeners.clear();
	for (auto& [owner, context] : s_contexts)
	{
		for (auto& [handle, entry] : s_documents)
		{
			if (entry.scopedOwner == owner && entry.document)
				context->UnloadDocument(entry.document);
		}
	}
	s_documents.clear();
	s_owners.clear();
	s_pendingHandles.clear();
	s_dataModels.clear();
	for (const auto& [owner, context] : s_contexts)
		Rml::RemoveContext(context->GetName());
	s_contexts.clear();
	s_inputReadyContexts.clear();
	Rml::Shutdown();
	s_fontData.clear();
	s_initialized = false;
	s_supported = false;
	s_visible = false;
	s_highSurrogate = 0;
	s_pendingKeyboardConsume = false;
	s_mouseOverInteractive = false;
	s_mouseCaptureButtons = 0;
	s_firstRender = true;
	s_nextUpdateDelay = 0.0;
	s_lastUpdateTime = 0.0;
	s_lastUpdateMicroseconds = 0.0;
	s_lastRenderMicroseconds = 0.0;
	s_updateCount = 0;
	s_renderCount = 0;
}

void CreateDeviceObjects()
{
#if HAS_DIRECTX_9
	s_renderer.SetDevice(gpD3D9Device);
#endif
}

void InvalidateDeviceObjects()
{
	s_mouseOverInteractive = false;
	s_mouseCaptureButtons = 0;
#if HAS_DIRECTX_9
	s_renderer.SetDevice(nullptr);
	if (s_initialized)
	{
		// Managed-pool textures (including the font atlas) are lost on device
		// invalidation; release them so RmlUi regenerates them lazily on restore.
		Rml::ReleaseTextures(&s_renderer);
		Rml::ReleaseFontResources();
	}
#endif
}

void InvalidateEqUiTextures()
{
#if HAS_DIRECTX_9
	if (s_initialized)
		Rml::ReleaseTextures(&s_renderer);
#endif
}

// Returns each context that has at least one visible document, ordered so that
// a context whose documents sit at a lower layer renders first.
//
// A document can only be stacked against others in its own context, so this is
// what preserves the caller's layer order across owners. Within a context the
// order is already settled by RefreshDocumentStacking. Contexts on the same
// layer use their most recently focused document, then owner id as a stable tie.
std::vector<Rml::Context*> GetContextsInRenderOrder()
{
	struct ContextOrder
	{
		int layer;
		uint64_t focusOrder;
		RmlOwnerHandle owner;
		Rml::Context* context;
	};
	std::vector<ContextOrder> ordered;
	ordered.reserve(s_contexts.size());

	for (const auto& [owner, context] : s_contexts)
	{
		int lowestLayer = std::numeric_limits<int>::max();
		uint64_t focusOrder = 0;
		bool hasVisible = false;
		for (const auto& [handle, entry] : s_documents)
		{
			if (entry.scopedOwner != owner || !entry.document || !entry.options.visible)
				continue;

			if (entry.options.layer < lowestLayer)
			{
				lowestLayer = entry.options.layer;
				focusOrder = entry.focusOrder;
			}
			else if (entry.options.layer == lowestLayer)
			{
				focusOrder = std::max(focusOrder, entry.focusOrder);
			}
			hasVisible = true;
		}

		if (hasVisible)
			ordered.push_back({ lowestLayer, focusOrder, owner, context });
	}

	std::stable_sort(ordered.begin(), ordered.end(),
		[](const ContextOrder& lhs, const ContextOrder& rhs)
		{
			if (lhs.layer != rhs.layer)
				return lhs.layer < rhs.layer;
			if (lhs.focusOrder != rhs.focusOrder)
				return lhs.focusOrder < rhs.focusOrder;
			return lhs.owner < rhs.owner;
		});

	std::vector<Rml::Context*> result;
	result.reserve(ordered.size());
	for (const ContextOrder& item : ordered)
		result.push_back(item.context);
	return result;
}

void Render()
{
#if HAS_DIRECTX_9
	if (!s_initialized || !s_supported || !s_visible || !gpD3D9Device)
		return;

	D3DVIEWPORT9 viewport = {};
	if (FAILED(gpD3D9Device->GetViewport(&viewport)) || viewport.Width == 0 || viewport.Height == 0)
		return;

	FlushPendingDocuments();
	if (!HasVisibleDocuments())
		return;

	if (s_contexts.empty())
	{
		s_visible = false;
		return;
	}

	// On-demand update scheduling. Run Update() only when input was observed, an
	// animation/transition is due, or enough time has passed for a rate-limited
	// refresh. Render() still runs every displayed frame so direct documents keep
	// submitting their retained geometry to the EQ backbuffer.
	//
	// The schedule is global rather than per context. Every owner's documents
	// share one rate so that a 24-box multibox does not multiply the update cost
	// by the number of scripts that happen to be open.
	const double now = s_systemInterface.GetElapsedTime();
	const bool inputDirty = std::exchange(s_inputDirty, false);
	const bool firstRender = std::exchange(s_firstRender, false);

	const int updateRate = gbInForeground ? s_updateRateForeground : s_updateRateBackground;
	const double minUpdateInterval = updateRate > 0 ? 1.0 / static_cast<double>(updateRate) : 0.0;
	const bool rateElapsed = (now - s_lastUpdateTime) >= minUpdateInterval;
	const bool rmluiDue = (now - s_lastUpdateTime) >= s_nextUpdateDelay;

	if (firstRender || inputDirty || (rateElapsed && rmluiDue))
	{
		const auto updateStart = std::chrono::steady_clock::now();
		// Capture end proximity before layout changes. Chat-style views should
		// follow new content only while the user is already at the bottom.
		for (auto& [handle, document] : s_documents)
		{
			for (auto& [elementId, anchor] : document.scrollAnchors)
			{
				Rml::Element* element = FindDocumentElement(handle, elementId);
				if (!element)
				{
					anchor.wasAtEnd = false;
					continue;
				}
				const float end = std::max(0.0f,
					element->GetScrollHeight() - element->GetClientHeight());
				anchor.scrollTopBefore = element->GetScrollTop();
				anchor.wasAtEnd = end - anchor.scrollTopBefore <= anchor.threshold;
			}
		}
		for (const auto& [owner, context] : s_contexts)
		{
			context->SetDimensions({ static_cast<int>(viewport.Width), static_cast<int>(viewport.Height) });
			context->Update();
			s_inputReadyContexts.insert(context);
		}
		// Templates and data views can materialize listener targets during
		// Update(). Resolve registrations that were intentionally left pending.
		for (auto& [listenerHandle, listener] : s_listeners)
			AttachListener(listener);
		for (auto& [handle, document] : s_documents)
		{
			for (auto& [elementId, anchor] : document.scrollAnchors)
			{
				if (!anchor.wasAtEnd)
					continue;
				if (Rml::Element* element = FindDocumentElement(handle, elementId))
				{
					// RmlUi 6 applies the mouse wheel as a smooth scroll inside
					// Context::Update. Pinning afterwards would undo that wheel
					// on the same frame the user is still at the bottom.
					if (std::abs(element->GetScrollTop() - anchor.scrollTopBefore) > 0.5f)
						continue;
					element->SetScrollTop(std::max(0.0f,
						element->GetScrollHeight() - element->GetClientHeight()));
				}
			}
		}
		s_lastUpdateMicroseconds = std::chrono::duration<double, std::micro>(
			std::chrono::steady_clock::now() - updateStart).count();
		++s_updateCount;

		// The soonest deadline across contexts governs the next update, so a
		// context with a pending transition is not starved by a longer one.
		double nextDelay = s_nextUpdateDelay;
		for (const auto& [owner, context] : s_contexts)
		{
			const double delay = context->GetNextUpdateDelay();
			if (std::isfinite(delay) && (nextDelay < 0.0 || delay < nextDelay))
				nextDelay = delay;
		}
		s_nextUpdateDelay = nextDelay < 0.0 ? 0.0 : nextDelay;
		s_lastUpdateTime = now;
	}
	else
	{
		for (const auto& [owner, context] : s_contexts)
			context->SetDimensions({ static_cast<int>(viewport.Width), static_cast<int>(viewport.Height) });
	}

	IDirect3DStateBlock9* stateBlock = nullptr;
	if (FAILED(gpD3D9Device->CreateStateBlock(D3DSBT_ALL, &stateBlock)))
		return;

	s_renderer.SetDevice(gpD3D9Device);
	s_renderer.BeginFrame(static_cast<int>(viewport.Width), static_cast<int>(viewport.Height));
	const auto renderStart = std::chrono::steady_clock::now();
	// All contexts render inside one BeginFrame/EndFrame pair, so the renderer
	// captures and restores device state once for the whole overlay rather than
	// once per owner.
	for (Rml::Context* context : GetContextsInRenderOrder())
		context->Render();
	s_lastRenderMicroseconds = std::chrono::duration<double, std::micro>(
		std::chrono::steady_clock::now() - renderStart).count();
	++s_renderCount;
	s_renderer.EndFrame();
	stateBlock->Apply();
	stateBlock->Release();
#endif
}

// Offers an event to each context in turn and reports whether one consumed it.
//
// `process` returns RmlUi's own convention: true means the event was not
// handled and would propagate to the game. This helper inverts that into "an
// overlay took the event", which is what the input broker wants.
template <typename Fn>
bool DispatchToFirstContext(Fn&& process)
{
	auto contexts = GetContextsInRenderOrder();
	for (auto iter = contexts.rbegin(); iter != contexts.rend(); ++iter)
	{
		if (!s_inputReadyContexts.contains(*iter))
			continue;
		if (!process(**iter))
			return true;
	}
	return false;
}

// Offers an event to every context so each maintains its own state, and reports
// whether any consumed it. Used for pure motion, where two overlays may both
// legitimately be under the cursor and each still needs its hover updated.
template <typename Fn>
bool DispatchToAllContexts(Fn&& process)
{
	bool handled = false;
	for (const auto& [owner, context] : s_contexts)
	{
		if (!s_inputReadyContexts.contains(context))
			continue;
		// The call is the left operand so that it always runs; only a
		// short-circuit on the right would skip a context.
		handled = !process(*context) || handled;
	}
	return handled;
}

void BlurAllContextFocus()
{
	for (const auto& [owner, context] : s_contexts)
	{
		if (Rml::Element* focus = context->GetFocusElement())
			focus->Blur();
	}
}

void PromoteFocusedDocument(Rml::Context& context)
{
	Rml::Element* focus = context.GetFocusElement();
	Rml::ElementDocument* focusedDocument = focus ? focus->GetOwnerDocument() : nullptr;
	if (!focusedDocument)
		return;

	for (auto& [handle, entry] : s_documents)
	{
		if (entry.document == focusedDocument)
		{
			BringDocumentToFront(handle);
			break;
		}
	}

	// Owner contexts isolate models, but keyboard focus is global to the user.
	for (const auto& [owner, other] : s_contexts)
	{
		if (other != &context)
		{
			if (Rml::Element* otherFocus = other->GetFocusElement())
				otherFocus->Blur();
		}
	}
}

bool HandleWndProc(HWND, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (!s_initialized || !s_visible || s_contexts.empty() || !HasInteractiveDocuments())
		return false;

	// Any input observation means the contexts should be updated to reflect it.
	s_inputDirty = true;

	const int modifiers = GetKeyModifiers();
	switch (message)
	{
	case WM_MOUSEMOVE:
	{
		const int x = GET_X_LPARAM(lParam);
		const int y = GET_Y_LPARAM(lParam);
		const bool handled = DispatchToAllContexts(
			[&](Rml::Context& context) { return context.ProcessMouseMove(x, y, modifiers); });

		s_mouseOverInteractive = false;
		for (const auto& [owner, context] : s_contexts)
			s_mouseOverInteractive = s_mouseOverInteractive || context->IsMouseInteracting();
		return handled;
	}
	case WM_MOUSELEAVE:
		s_mouseOverInteractive = false;
		return DispatchToAllContexts([](Rml::Context& context) { return context.ProcessMouseLeave(); });
	case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
	case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: case WM_XBUTTONDBLCLK:
	{
		const int button = TranslateMouseButton(message, wParam);
		// A press must reach exactly one owner, so the first context to consume
		// it wins and no later script sees the click. Walk backward through render
		// order so the visually top owner receives the press first.
		bool handled = false;
		Rml::Context* handledContext = nullptr;
		auto contexts = GetContextsInRenderOrder();
		for (auto iter = contexts.rbegin(); iter != contexts.rend(); ++iter)
		{
			if (!s_inputReadyContexts.contains(*iter))
				continue;
			if (!(*iter)->ProcessMouseButtonDown(button, modifiers))
			{
				handled = true;
				handledContext = *iter;
				break;
			}
		}
		if (handledContext)
			PromoteFocusedDocument(*handledContext);
		if (handled && button >= 0)
			s_mouseCaptureButtons |= (1u << button);
		if (!handled && WantsKeyboardInput())
			BlurAllContextFocus();
		return handled;
	}
	case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
	{
		const int button = TranslateMouseButton(message, wParam);
		// The release goes to the first context that consumes it. In practice that
		// is the context that took the press, because a context only consumes a
		// release for an element the pointer is still over.
		const bool handled = DispatchToFirstContext(
			[&](Rml::Context& context) { return context.ProcessMouseButtonUp(button, modifiers); });
		if (button >= 0)
			s_mouseCaptureButtons &= ~(1u << button);
		return handled;
	}
	case WM_MOUSEWHEEL:
		return DispatchToFirstContext([&](Rml::Context& context) {
			return context.ProcessMouseWheel(
				Rml::Vector2f(0.0f, -static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA), modifiers);
		});
	case WM_MOUSEHWHEEL:
		return DispatchToFirstContext([&](Rml::Context& context) {
			return context.ProcessMouseWheel(
				Rml::Vector2f(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA, 0.0f), modifiers);
		});
	case WM_KEYDOWN: case WM_SYSKEYDOWN:
	{
		const bool hadKeyboardFocus = WantsKeyboardInput();
		const auto key = TranslateKey(wParam);
		if (key == Rml::Input::KI_UNKNOWN)
			return false;
		// Keys go only to the context that holds focus, so two scripts with
		// inputs on screen never both receive the same keystroke.
		const bool handled = DispatchToFirstContext(
			[&](Rml::Context& context) { return context.ProcessKeyDown(key, modifiers); });
		if (hadKeyboardFocus && key == Rml::Input::KI_ESCAPE)
		{
			BlurAllContextFocus();
			s_pendingKeyboardConsume = true;
		}
		return hadKeyboardFocus || handled;
	}
	case WM_KEYUP: case WM_SYSKEYUP:
	{
		const bool hadKeyboardFocus = WantsKeyboardInput();
		const auto key = TranslateKey(wParam);
		if (key == Rml::Input::KI_UNKNOWN)
			return false;
		const bool handled = DispatchToFirstContext(
			[&](Rml::Context& context) { return context.ProcessKeyUp(key, modifiers); });
		return hadKeyboardFocus || handled;
	}
	case WM_CHAR:
	{
		const uint32_t character = static_cast<uint32_t>(wParam);
		if (character < 32 || character == 127)
			return WantsKeyboardInput();

		if (character >= 0xd800 && character <= 0xdbff)
		{
			s_highSurrogate = character;
			return WantsKeyboardInput();
		}

		uint32_t codepoint = character;
		if (character >= 0xdc00 && character <= 0xdfff && s_highSurrogate)
			codepoint = 0x10000 + ((s_highSurrogate - 0xd800) << 10) + (character - 0xdc00);
		s_highSurrogate = 0;
		const bool hadKeyboardFocus = WantsKeyboardInput();
		const bool handled = DispatchToFirstContext([&](Rml::Context& context) {
			return context.ProcessTextInput(static_cast<Rml::Character>(codepoint));
		});
		return hadKeyboardFocus || handled;
	}
	case WM_KILLFOCUS:
		s_highSurrogate = 0;
		s_mouseOverInteractive = false;
		s_mouseCaptureButtons = 0;
		return false;
	default:
		return false;
	}
}

bool WantsMouseInput()
{
	return s_initialized && s_visible && !s_contexts.empty() && HasInteractiveDocuments()
		&& (s_mouseOverInteractive || s_mouseCaptureButtons != 0);
}

bool WantsKeyboardInput()
{
	if (!s_initialized || !s_visible || s_contexts.empty() || !HasInteractiveDocuments())
		return false;

	// Any context holding focus on a text control claims the keyboard.
	for (const auto& [owner, context] : s_contexts)
	{
		if (!s_inputReadyContexts.contains(context))
			continue;
		const Rml::Element* focus = context->GetFocusElement();
		if (!focus)
			continue;

		const Rml::String& tag = focus->GetTagName();
		if (tag == "input" || tag == "textarea" || tag == "select")
			return true;
	}
	return false;
}

bool ConsumePendingKeyboardInput()
{
	if (WantsKeyboardInput())
		return true;

	return std::exchange(s_pendingKeyboardConsume, false);
}

bool IsInitialized() { return s_initialized; }
bool IsVisible() { return s_visible; }

void SetVisible(bool visible)
{
	if (!s_initialized)
	{
		WriteChatf("\ar[RmlUi]\ax is disabled. Set [RmlUi] Enabled=1 and restart MacroQuest.");
		return;
	}

	if (!visible && !s_contexts.empty())
	{
		BlurAllContextFocus();
		OverlayClearCapture();
		s_mouseOverInteractive = false;
		s_mouseCaptureButtons = 0;
	}
	s_visible = visible;
	WritePrivateProfileBool("RmlUi", "Visible", visible, internal_paths::MQini);
}

} // namespace mq::rmlui

namespace mq {

RmlDocumentHandle LoadRmlDocument(
	std::string_view virtualPath,
	const RmlDocumentOptions& options,
	MQPluginHandle owner)
{
	if (!IsMainThread())
	{
		if (!rmlui::s_acceptingCommands.load(std::memory_order_acquire))
			return 0;
		const RmlDocumentHandle handle = rmlui::s_nextDocumentHandle.fetch_add(1, std::memory_order_relaxed);
		const std::string path(virtualPath);
		PostToMainThread([handle, path, options, owner]()
		{
			rmlui::LoadDocumentWithHandle(handle, path, options, owner);
		});
		return handle;
	}
	return rmlui::LoadDocument(virtualPath, options, owner);
}

RmlOwnerHandle CreateRmlOwner(MQPluginHandle parent)
{
	if (!IsMainThread())
		return 0;
	return rmlui::CreateOwner(parent);
}

void DestroyRmlOwner(RmlOwnerHandle owner)
{
	if (!IsMainThread())
	{
		PostToMainThread([owner]() { rmlui::DestroyOwner(owner); });
		return;
	}
	rmlui::DestroyOwner(owner);
}

RmlDocumentHandle LoadRmlDocumentForOwner(std::string_view virtualPath,
	const RmlDocumentOptions& options, RmlOwnerHandle owner)
{
	if (!IsMainThread())
		return 0;
	return rmlui::LoadDocumentForOwner(virtualPath, options, owner);
}

void UnloadRmlDocument(RmlDocumentHandle handle)
{
	if (!IsMainThread())
	{
		PostToMainThread([handle]() { rmlui::UnloadDocument(handle); });
		return;
	}
	rmlui::UnloadDocument(handle);
}

bool ReloadRmlDocument(RmlDocumentHandle handle)
{
	if (!IsMainThread())
	{
		if (handle == 0)
			return false;
		PostToMainThread([handle]() { rmlui::ReloadDocument(handle); });
		return true;
	}
	return rmlui::ReloadDocument(handle);
}

bool RmlDocumentIsLoaded(RmlDocumentHandle handle)
{
	if (!IsMainThread())
		return false;
	return rmlui::IsDocumentLoaded(handle);
}

bool SetRmlDocumentVisible(RmlDocumentHandle handle, bool visible)
{
	if (!IsMainThread())
	{
		if (handle == 0)
			return false;
		PostToMainThread([handle, visible]() { rmlui::SetDocumentVisible(handle, visible); });
		return true;
	}
	return rmlui::SetDocumentVisible(handle, visible);
}

bool BringRmlDocumentToFront(RmlDocumentHandle handle)
{
	if (!IsMainThread())
	{
		if (handle == 0)
			return false;
		PostToMainThread([handle]() { rmlui::BringDocumentToFront(handle); });
		return true;
	}
	return rmlui::BringDocumentToFront(handle);
}

bool RmlDocumentSetText(RmlDocumentHandle handle, std::string_view elementId, std::string_view text)
{
	if (!IsMainThread())
	{
		if (handle == 0)
			return false;
		const std::string id(elementId), value(text);
		PostToMainThread([handle, id, value]() { rmlui::SetDocumentText(handle, id, value); });
		return true;
	}
	return rmlui::SetDocumentText(handle, elementId, text);
}

bool RmlDocumentSetValue(RmlDocumentHandle handle, std::string_view elementId, std::string_view value)
{
	if (!IsMainThread())
	{
		if (handle == 0)
			return false;
		const std::string id(elementId), copiedValue(value);
		PostToMainThread([handle, id, copiedValue]() { rmlui::SetDocumentValue(handle, id, copiedValue); });
		return true;
	}
	return rmlui::SetDocumentValue(handle, elementId, value);
}

bool RmlDocumentSetClass(RmlDocumentHandle handle, std::string_view elementId, std::string_view className, bool activate)
{
	if (!IsMainThread())
	{
		if (handle == 0)
			return false;
		const std::string id(elementId), name(className);
		PostToMainThread([handle, id, name, activate]() { rmlui::SetDocumentClass(handle, id, name, activate); });
		return true;
	}
	return rmlui::SetDocumentClass(handle, elementId, className, activate);
}

bool RmlDocumentSetStyle(RmlDocumentHandle handle, std::string_view elementId, std::string_view property, std::string_view value)
{
	if (!IsMainThread())
	{
		if (handle == 0)
			return false;
		const std::string id(elementId), copiedProperty(property), copiedValue(value);
		PostToMainThread([handle, id, copiedProperty, copiedValue]()
		{
			rmlui::SetDocumentStyle(handle, id, copiedProperty, copiedValue);
		});
		return true;
	}
	return rmlui::SetDocumentStyle(handle, elementId, property, value);
}

bool RmlDocumentSetAttribute(RmlDocumentHandle handle, std::string_view elementId,
	std::string_view attribute, std::string_view value)
{
	if (!IsMainThread())
	{
		if (handle == 0)
			return false;
		const std::string id(elementId), name(attribute), copiedValue(value);
		PostToMainThread([handle, id, name, copiedValue]()
		{
			rmlui::SetDocumentAttribute(handle, id, name, copiedValue);
		});
		return true;
	}
	return rmlui::SetDocumentAttribute(handle, elementId, attribute, value);
}

bool RmlDocumentRemoveAttribute(RmlDocumentHandle handle, std::string_view elementId,
	std::string_view attribute)
{
	if (!IsMainThread())
	{
		if (handle == 0)
			return false;
		const std::string id(elementId), name(attribute);
		PostToMainThread([handle, id, name]() { rmlui::RemoveDocumentAttribute(handle, id, name); });
		return true;
	}
	return rmlui::RemoveDocumentAttribute(handle, elementId, attribute);
}

bool RmlDocumentGetAttribute(RmlDocumentHandle handle, std::string_view elementId,
	std::string_view attribute, std::string& value)
{
	if (!IsMainThread())
		return false;
	return rmlui::GetDocumentAttribute(handle, elementId, attribute, value);
}

bool RmlDocumentFocusElement(RmlDocumentHandle handle, std::string_view elementId)
{
	if (!IsMainThread())
	{
		if (handle == 0)
			return false;
		const std::string id(elementId);
		PostToMainThread([handle, id]() { rmlui::FocusDocumentElement(handle, id); });
		return true;
	}
	return rmlui::FocusDocumentElement(handle, elementId);
}

bool RmlDocumentBlurElement(RmlDocumentHandle handle, std::string_view elementId)
{
	if (!IsMainThread())
	{
		if (handle == 0)
			return false;
		const std::string id(elementId);
		PostToMainThread([handle, id]() { rmlui::BlurDocumentElement(handle, id); });
		return true;
	}
	return rmlui::BlurDocumentElement(handle, elementId);
}

bool RmlDocumentGetGeometry(RmlDocumentHandle handle, std::string_view elementId,
	RmlElementGeometry& geometry)
{
	if (!IsMainThread())
		return false;
	return rmlui::GetDocumentGeometry(handle, elementId, geometry);
}

bool RmlDocumentSetGeometry(RmlDocumentHandle handle, std::string_view elementId,
	const RmlElementGeometry& geometry)
{
	if (!IsMainThread())
	{
		if (handle == 0)
			return false;
		const std::string id(elementId);
		PostToMainThread([handle, id, geometry]() { rmlui::SetDocumentGeometry(handle, id, geometry); });
		return true;
	}
	return rmlui::SetDocumentGeometry(handle, elementId, geometry);
}

bool RmlDocumentGetScrollMetrics(RmlDocumentHandle handle, std::string_view elementId,
	RmlScrollMetrics& metrics)
{
	if (!IsMainThread())
		return false;
	return rmlui::GetDocumentScrollMetrics(handle, elementId, metrics);
}

bool RmlDocumentSetScrollOffset(RmlDocumentHandle handle, std::string_view elementId,
	float offset)
{
	if (!IsMainThread())
	{
		if (handle == 0)
			return false;
		const std::string id(elementId);
		PostToMainThread([handle, id, offset]()
		{
			rmlui::SetDocumentScrollOffset(handle, id, offset);
		});
		return true;
	}
	return rmlui::SetDocumentScrollOffset(handle, elementId, offset);
}

bool RmlDocumentScrollToEnd(RmlDocumentHandle handle, std::string_view elementId)
{
	if (!IsMainThread())
	{
		if (handle == 0)
			return false;
		const std::string id(elementId);
		PostToMainThread([handle, id]() { rmlui::ScrollDocumentToEnd(handle, id); });
		return true;
	}
	return rmlui::ScrollDocumentToEnd(handle, elementId);
}

bool RmlDocumentSetScrollAnchor(RmlDocumentHandle handle, std::string_view elementId,
	bool enabled, float threshold)
{
	if (!IsMainThread())
	{
		if (handle == 0)
			return false;
		const std::string id(elementId);
		PostToMainThread([handle, id, enabled, threshold]()
		{
			rmlui::SetDocumentScrollAnchor(handle, id, enabled, threshold);
		});
		return true;
	}
	return rmlui::SetDocumentScrollAnchor(handle, elementId, enabled, threshold);
}

bool RmlDocumentSetModelString(RmlDocumentHandle handle, std::string_view model, std::string_view variable, std::string_view value)
{
	if (!IsMainThread())
	{
		if (handle == 0)
			return false;
		const std::string copiedModel(model), copiedVariable(variable), copiedValue(value);
		PostToMainThread([handle, copiedModel, copiedVariable, copiedValue]()
		{
			rmlui::SetModelString(handle, copiedModel, copiedVariable, copiedValue);
		});
		return true;
	}
	return rmlui::SetModelString(handle, model, variable, value);
}

bool RmlDocumentSetModelNumber(RmlDocumentHandle handle, std::string_view model, std::string_view variable, double value)
{
	if (!IsMainThread())
	{
		if (handle == 0)
			return false;
		const std::string copiedModel(model), copiedVariable(variable);
		PostToMainThread([handle, copiedModel, copiedVariable, value]()
		{
			rmlui::SetModelNumber(handle, copiedModel, copiedVariable, value);
		});
		return true;
	}
	return rmlui::SetModelNumber(handle, model, variable, value);
}

bool RmlDocumentSetModelBoolean(RmlDocumentHandle handle, std::string_view model, std::string_view variable, bool value)
{
	if (!IsMainThread())
	{
		if (handle == 0)
			return false;
		const std::string copiedModel(model), copiedVariable(variable);
		PostToMainThread([handle, copiedModel, copiedVariable, value]()
		{
			rmlui::SetModelBoolean(handle, copiedModel, copiedVariable, value);
		});
		return true;
	}
	return rmlui::SetModelBoolean(handle, model, variable, value);
}

bool RmlDocumentSetModelValue(RmlDocumentHandle handle, std::string_view model,
	std::string_view variable, const RmlUiValue& value)
{
	if (!IsMainThread())
	{
		if (handle == 0)
			return false;
		// The value tree is copied onto the queue so the caller's storage does not
		// need to outlive the call.
		const std::string copiedModel(model), copiedVariable(variable);
		const RmlUiValue copiedValue(value);
		PostToMainThread([handle, copiedModel, copiedVariable, copiedValue]()
		{
			rmlui::SetModelValue(handle, copiedModel, copiedVariable, copiedValue);
		});
		return true;
	}
	return rmlui::SetModelValue(handle, model, variable, value);
}

bool RmlDocumentSpliceModelArray(RmlDocumentHandle handle, std::string_view model,
	std::string_view variable, size_t index, size_t deleteCount,
	const std::vector<RmlUiValue>& values)
{
	if (!IsMainThread())
	{
		if (handle == 0)
			return false;
		const std::string copiedModel(model), copiedVariable(variable);
		const std::vector<RmlUiValue> copiedValues(values);
		PostToMainThread([handle, copiedModel, copiedVariable, index, deleteCount, copiedValues]()
		{
			rmlui::SpliceModelArray(handle, copiedModel, copiedVariable,
				index, deleteCount, copiedValues);
		});
		return true;
	}

	return rmlui::SpliceModelArray(handle, model, variable, index, deleteCount, values);
}

bool RmlDocumentGetModelValue(RmlDocumentHandle handle, std::string_view model,
	std::string_view variable, RmlUiValue& value)
{
	// Reads must stay on the main thread: model storage is only stable there, and
	// queueing a read would need a completion path the caller does not have.
	if (!IsMainThread())
		return false;
	return rmlui::GetModelValue(handle, model, variable, value);
}

bool RmlDocumentDeclareModel(RmlDocumentHandle handle, std::string_view model)
{
	if (!IsMainThread())
	{
		if (handle == 0)
			return false;
		const std::string copiedModel(model);
		PostToMainThread([handle, copiedModel]()
		{
			rmlui::DeclareModel(handle, copiedModel);
		});
		return true;
	}
	return rmlui::DeclareModel(handle, model);
}

bool RmlDocumentSetModelStringArray(RmlDocumentHandle handle, std::string_view model,
	std::string_view variable, const std::vector<std::string>& values)
{
	if (!IsMainThread())
	{
		if (handle == 0)
			return false;
		const std::string copiedModel(model), copiedVariable(variable);
		PostToMainThread([handle, copiedModel, copiedVariable, values]()
		{
			rmlui::SetModelStringArray(handle, copiedModel, copiedVariable, values);
		});
		return true;
	}
	return rmlui::SetModelStringArray(handle, model, variable, values);
}

bool RmlDocumentSetModelNumberArray(RmlDocumentHandle handle, std::string_view model,
	std::string_view variable, const std::vector<double>& values)
{
	if (!IsMainThread())
	{
		if (handle == 0)
			return false;
		const std::string copiedModel(model), copiedVariable(variable);
		PostToMainThread([handle, copiedModel, copiedVariable, values]()
		{
			rmlui::SetModelNumberArray(handle, copiedModel, copiedVariable, values);
		});
		return true;
	}
	return rmlui::SetModelNumberArray(handle, model, variable, values);
}

RmlListenerHandle AddRmlDocumentEventListener(RmlDocumentHandle document,
	std::string_view eventType, std::string_view elementId, RmlEventCallback callback, void* userData)
{
	if (!IsMainThread())
		return 0;
	return rmlui::AddDocumentEventListener(document, eventType, elementId, callback, userData);
}

void RemoveRmlDocumentEventListener(RmlListenerHandle listener)
{
	if (!IsMainThread())
	{
		PostToMainThread([listener]() { rmlui::RemoveDocumentEventListener(listener); });
		return;
	}
	rmlui::RemoveDocumentEventListener(listener);
}

void SetRmlUiLuaDirectory(std::string_view directory)
{
	if (!IsMainThread())
	{
		const std::string copiedDirectory(directory);
		PostToMainThread([copiedDirectory]() { rmlui::SetLuaDirectory(copiedDirectory); });
		return;
	}
	rmlui::SetLuaDirectory(directory);
}

void RemoveRmlDocumentsForPlugin(MQPluginHandle owner)
{
	rmlui::RemoveDocumentsForPlugin(owner);
}

} // namespace mq
