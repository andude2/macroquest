#pragma once

// <resize-edge> custom element: edge/corner resize without a visible grip.
//
// Usage in RML:
//     <resize-edge target="window" edge="right" />
//     <resize-edge target="window" edge="bottom" min-height="120" />
//     <resize-edge target="window" edge="bottom-right" />
//
// Attributes:
//     target      (required) id of the element to resize, resolved in the
//                 owner document at drag start.
//     edge        (required) left | right | top | bottom |
//                 top-left | top-right | bottom-left | bottom-right
//     min-width / min-height   minimum target size in px (default 64)
//     max-width / max-height   maximum target size in px (default: viewport)
//
// The element itself is a transparent drag strip: position it over the window
// border with RCSS (e.g. position:absolute; right:0; top:0; bottom:0;
// width:8px) and it resizes the target as the pointer moves. Geometry flows
// through the normal persisted-geometry pipeline, so resized windows keep
// saving to RmlUiWindows.ini with no extra work.
//
// Must be registered after Rml::Initialise() via RegisterResizeEdgeElement().

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementInstancer.h>

namespace mq::rmlui {

void RegisterResizeEdgeElement();

} // namespace mq::rmlui
