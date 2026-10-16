#include "pch.h"
#include "RmlUiResizeEdge.h"

#include "Logging.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Factory.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace mq::rmlui {
namespace {

// RmlUi drag lifecycle for an element with `drag: drag`:
//   dragstart -> drag (per mouse move) -> dragend
// All three carry integer mouse_x/mouse_y parameters in context coordinates.
// NOTE: verify against the pinned RmlUi headers if drag behavior ever changes:
//   grep for "dragend" in the RmlUi include tree.
constexpr const char* kDragStartEvent = "dragstart";
constexpr const char* kDragEvent = "drag";
constexpr const char* kDragEndEvent = "dragend";

constexpr float kDefaultMinSize = 64.0f;

enum class Edge
{
	None = 0,
	Left = 1 << 0,
	Right = 1 << 1,
	Top = 1 << 2,
	Bottom = 1 << 3,
};

inline Edge ParseEdge(const Rml::String& value)
{
	Rml::String lower;
	lower.reserve(value.size());
	for (char ch : value)
		lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));

	if (lower == "left") return Edge::Left;
	if (lower == "right") return Edge::Right;
	if (lower == "top") return Edge::Top;
	if (lower == "bottom") return Edge::Bottom;
	if (lower == "top-left") return static_cast<Edge>(static_cast<int>(Edge::Top) | static_cast<int>(Edge::Left));
	if (lower == "top-right") return static_cast<Edge>(static_cast<int>(Edge::Top) | static_cast<int>(Edge::Right));
	if (lower == "bottom-left") return static_cast<Edge>(static_cast<int>(Edge::Bottom) | static_cast<int>(Edge::Left));
	if (lower == "bottom-right") return static_cast<Edge>(static_cast<int>(Edge::Bottom) | static_cast<int>(Edge::Right));
	return Edge::None;
}

inline bool HasEdge(Edge edges, Edge flag)
{
	return (static_cast<int>(edges) & static_cast<int>(flag)) != 0;
}

bool GetMousePosition(const Rml::Event& event, int& x, int& y)
{
	const Rml::Dictionary& params = event.GetParameters();
	const auto itX = params.find("mouse_x");
	const auto itY = params.find("mouse_y");
	if (itX == params.end() || itY == params.end())
		return false;
	if (!itX->second.GetInto(x) || !itY->second.GetInto(y))
		return false;
	return true;
}

class ResizeEdgeElement : public Rml::Element, public Rml::EventListener
{
public:
	ResizeEdgeElement(const Rml::String& tag)
		: Rml::Element(tag)
	{
		// RmlUi owns the drag lifecycle (including mouse capture when the
		// pointer leaves the element). We only consume the deltas.
		SetProperty("drag", "drag");
		AddEventListener(kDragStartEvent, this);
		AddEventListener(kDragEvent, this);
		AddEventListener(kDragEndEvent, this);
	}

	~ResizeEdgeElement() override
	{
		RemoveEventListener(kDragStartEvent, this);
		RemoveEventListener(kDragEvent, this);
		RemoveEventListener(kDragEndEvent, this);
	}

	void OnAttributeChange(const Rml::ElementAttributes& changed_attributes) override
	{
		Rml::Element::OnAttributeChange(changed_attributes);
		if (changed_attributes.count("edge") > 0)
			m_edge = ParseEdge(GetAttribute<Rml::String>("edge", "right"));
		if (changed_attributes.count("target") > 0)
			m_target = GetAttribute<Rml::String>("target", "");
		if (changed_attributes.count("min-width") > 0)
			m_minWidth = GetAttribute<float>("min-width", kDefaultMinSize);
		if (changed_attributes.count("min-height") > 0)
			m_minHeight = GetAttribute<float>("min-height", kDefaultMinSize);
		if (changed_attributes.count("max-width") > 0)
			m_maxWidth = GetAttribute<float>("max-width", 0.0f);
		if (changed_attributes.count("max-height") > 0)
			m_maxHeight = GetAttribute<float>("max-height", 0.0f);
	}

	void ProcessEvent(Rml::Event& event) override
	{
		const Rml::String& type = event.GetType();
		if (type == kDragStartEvent)
			BeginDrag(event);
		else if (type == kDragEvent)
			UpdateDrag(event);
		else if (type == kDragEndEvent)
			EndDrag();
	}

private:
	Rml::Element* FindTarget() const
	{
		Rml::ElementDocument* document = GetOwnerDocument();
		if (!document || m_target.empty())
			return nullptr;
		return document->GetElementById(m_target);
	}

	void BeginDrag(Rml::Event& event)
	{
		m_dragging = false;
		if (m_edge == Edge::None)
		{
			LOG_WARN("RmlUi: <resize-edge> ignored: unknown edge attribute");
			return;
		}

		Rml::Element* target = FindTarget();
		if (!target || target->GetNumBoxes() == 0)
			return;

		int mouseX = 0, mouseY = 0;
		if (!GetMousePosition(event, mouseX, mouseY))
			return;

		// Border box: matches SetDocumentGeometry's left/top convention, and
		// sizing math stays exact regardless of the target's box-sizing.
		const Rml::Vector2f position = target->GetAbsoluteOffset(Rml::BoxArea::Border);
		const Rml::Vector2f size = target->GetBox().GetSize(Rml::BoxArea::Border);

		m_startMouseX = mouseX;
		m_startMouseY = mouseY;
		m_startX = position.x;
		m_startY = position.y;
		m_startWidth = size.x;
		m_startHeight = size.y;
		m_dragging = true;
	}

	void UpdateDrag(Rml::Event& event)
	{
		if (!m_dragging)
			return;

		Rml::Element* target = FindTarget();
		if (!target)
		{
			m_dragging = false;
			return;
		}

		int mouseX = 0, mouseY = 0;
		if (!GetMousePosition(event, mouseX, mouseY))
			return;

		const float dx = static_cast<float>(mouseX - m_startMouseX);
		const float dy = static_cast<float>(mouseY - m_startMouseY);

		float x = m_startX;
		float y = m_startY;
		float width = m_startWidth;
		float height = m_startHeight;

		if (HasEdge(m_edge, Edge::Right))
			width = m_startWidth + dx;
		if (HasEdge(m_edge, Edge::Left))
		{
			width = m_startWidth - dx;
			x = m_startX + dx;
		}
		if (HasEdge(m_edge, Edge::Bottom))
			height = m_startHeight + dy;
		if (HasEdge(m_edge, Edge::Top))
		{
			height = m_startHeight - dy;
			y = m_startY + dy;
		}

		// Clamp to the context viewport, mirroring SetDocumentGeometry so a
		// window can neither collapse nor strand itself off-screen.
		float minWidth = std::max(1.0f, m_minWidth);
		float minHeight = std::max(1.0f, m_minHeight);
		float maxWidth = m_maxWidth > 0.0f ? m_maxWidth : std::numeric_limits<float>::max();
		float maxHeight = m_maxHeight > 0.0f ? m_maxHeight : std::numeric_limits<float>::max();
		if (Rml::ElementDocument* document = GetOwnerDocument())
		{
			if (Rml::Context* context = document->GetContext())
			{
				const Rml::Vector2i viewport = context->GetDimensions();
				if (viewport.x > 0 && viewport.y > 0)
				{
					maxWidth = std::min(maxWidth, static_cast<float>(viewport.x));
					maxHeight = std::min(maxHeight, static_cast<float>(viewport.y));
				}
			}
		}
		maxWidth = std::max(maxWidth, minWidth);
		maxHeight = std::max(maxHeight, minHeight);

		width = std::clamp(width, minWidth, maxWidth);
		height = std::clamp(height, minHeight, maxHeight);
		if (HasEdge(m_edge, Edge::Left))
			x = m_startX + (m_startWidth - width);
		if (HasEdge(m_edge, Edge::Top))
			y = m_startY + (m_startHeight - height);
		x = std::max(0.0f, x);
		y = std::max(0.0f, y);

		target->SetProperty("left", fmt::format("{}px", static_cast<int>(std::round(x))));
		target->SetProperty("top", fmt::format("{}px", static_cast<int>(std::round(y))));
		target->SetProperty("width", fmt::format("{}px", static_cast<int>(std::round(width))));
		target->SetProperty("height", fmt::format("{}px", static_cast<int>(std::round(height))));
	}

	void EndDrag()
	{
		m_dragging = false;
		// If RmlUi moved this strip while dragging (`drag: drag` follows the
		// pointer), snap it back to its RCSS position. The strip is
		// transparent, so this is only about keeping the hit area correct.
		RemoveProperty("left");
		RemoveProperty("top");
	}

	Edge m_edge = Edge::Right;
	Rml::String m_target;
	float m_minWidth = kDefaultMinSize;
	float m_minHeight = kDefaultMinSize;
	float m_maxWidth = 0.0f;
	float m_maxHeight = 0.0f;

	bool m_dragging = false;
	int m_startMouseX = 0;
	int m_startMouseY = 0;
	float m_startX = 0.0f;
	float m_startY = 0.0f;
	float m_startWidth = 0.0f;
	float m_startHeight = 0.0f;
};

class ResizeEdgeInstancer : public Rml::ElementInstancer
{
public:
	Rml::ElementPtr InstanceElement(Rml::Element* /*parent*/, const Rml::String& tag,
		const Rml::XMLAttributes& /*attributes*/) override
	{
		return Rml::ElementPtr(new ResizeEdgeElement(tag));
	}

	void ReleaseElement(Rml::Element* element) override
	{
		delete element;
	}
};

} // namespace

void RegisterResizeEdgeElement()
{
	// NOTE (RmlUi 4.x, per Feb-2022 vcpkg baseline): Factory takes ownership
	// of the raw instancer pointer and returns void. If the pinned RmlUi ever
	// moves to 5.x+, this call may become:
	//   Rml::Factory::RegisterElementInstancer("resize-edge",
	//       Rml::ElementInstancerPtr(new ResizeEdgeInstancer()));
	// Verify with: grep "RegisterElementInstancer" <rmlui include>/Core/Factory.h
	Rml::Factory::RegisterElementInstancer("resize-edge", new ResizeEdgeInstancer());
}

} // namespace mq::rmlui
