# MacroQuest RmlUi resources

Core RmlUi documents are loaded from this directory. Paths supplied to the
`/rmlui` command are relative to this directory; absolute paths and parent
directory traversal are rejected.

Commands:

    /rmlui load example.rml
    /rmlui reload example.rml
    /rmlui close example.rml
    /rmlui list

RML documents may reference relative RCSS stylesheets and raster images. The
DX9 renderer uses D3DX image decoding and supports the formats accepted by
`D3DXCreateTextureFromFileEx`, including PNG, JPEG, BMP, DDS, and TGA.

EverQuest's shared icon atlases are available through virtual image URLs:

```xml
<img src="eq://spell/42" />
<img src="eq://item/650" />
```

`eq://spell/<icon>` takes the spell's `SpellIcon` value. `eq://item/<icon>`
takes the item's ordinary icon ID (the same value exposed by MacroQuest, whose
atlas cells begin at 500). These resources are available only while the EQ UI
and texture loader are initialized. They borrow the game's shared atlas memory
and do not count against the document owner's image-byte quota.

## Standard direct window

`mq-window.rml` and `mq-window.rcss` provide the standard direct-overlay window.
It has a click-through full-screen viewport, draggable title bar, clipped and
scrollable content, bottom-right resize handle, and the `mq-locked` state used
by `document:set_locked()`.

```xml
<rml>
<head>
  <title>My HUD</title>
  <link type="text/template" href="mq://rmlui/mq-window.rml" />
  <link type="text/rcss" href="my-hud.rcss" />
</head>
<body template="mq-window">
  <p>HUD content is injected into the scrolling content area.</p>
</body>
</rml>
```

The template intentionally uses stable IDs so it works with the Lua defaults:

- `window` — geometry, persistence, and lock element
- `window-title` — set with `document:set_text()`
- `window-content` — clipped/scrollable content host
- `window-handle` and `resize-handle` — native RmlUi handles

Keep comments out of `mq-window.rml`: RmlUi 6.2's template tag scanner does not
ignore comments and can mistake a `text/template` example for the actual
`<template>` opening tag.

One standard window is intended per document. Override `.mq-window` in the
document's own RCSS for a different default size or appearance. Open it with a
stable `id` and `persist = true` to retain geometry and lock state.

## Component primitives

`mq-components.rcss` is the shared component vocabulary for application-like
RmlUi documents. It includes cards and statistic cards, toolbars, buttons,
icon buttons, segmented controls, switches, badges, inputs, progress bars,
tables, notices, tooltips, layout helpers, and empty states.

`mq-theme-default.rcss` defines the shared RCSS custom-property tokens. The
standard `mq-window` template loads it automatically; documents that use the
component sheet without that template should load the default theme first.
Append `mq-theme-compact.rcss` or `mq-theme-high-contrast.rcss` after the
component stylesheet to override density or contrast tokens without changing
markup. Applications can add their own later RCSS overrides as well. This
requires RmlUi 6.3 or newer for RCSS variables.

The versioned inventory is [mq-components.manifest.json](mq-components.manifest.json).
It records required model fields (none are implicit), conditional binding types,
minimum sizing rules, event payload fields, states, and keyboard guarantees.
Entries marked `experimental` or `planned` are not part of the supported
contract. Load the component sheet before application overrides. Applications
remain responsible for labels, model updates, and adding explicit
keyboard-operable controls to otherwise presentational rows.

Run `python tools/build_scripts/Validate_RmlUiComponents.py` from the repository
root to check manifest selectors, gallery samples and IDs, linked resources,
Lua listener/model references, and shared theme-token references. This is a
source-contract check; it does not replace in-client layout, input, scale, or
golden-image testing.

The `components.rml` catalog uses the shared `mq-window` template, so it also
serves as a practical example of composing an application without copying the
window shell.

```xml
<link type="text/rcss" href="mq://rmlui/mq-components.rcss" />

<div class="mq-stat-card">
  <div class="mq-stat-icon">★</div>
  <div>
    <div class="mq-stat-value">{{ named_count }}</div>
    <div class="mq-stat-label">Named</div>
  </div>
</div>

<button class="mq-switch" data-class-checked="alerts">
  <span class="mq-switch-thumb"></span>
</button>
```

Run `lua/examples/rmlui_components.lua` through MQ2Lua for the interactive
catalog. The catalog intentionally uses data models and delegated events, so it
also acts as a smoke test for the application layer rather than only showing
static styles.

For a broader, animation-forward tour, run
`lua/examples/rmlui_showcase.lua`. The showcase covers native RCSS transitions,
keyframes, easing, transforms, staggered animation, gradients, typography,
selectors and pseudo-classes, clipping, flex layout, form controls, reactive
models, conditional markup, array iteration and splicing, scrolling, draggable
and edge-resizable persistent windows, and the Lua document/event API. Use
`/rmlshowcase` to show it, `/rmlshowcase lock` to toggle the persisted lock,
`/rmlshowcase reload` to reload its document, or `/rmlshowcase exit` to stop it.

For delegated controls with nested markup, put `data-mq-value` on the logical
row or button and read `event.action_value`. It walks from the actual click
target up to the listener element, avoiding special cases for clicks on child
icons and labels.

Lua documents can also manipulate component state directly with
`set_attribute`, `remove_attribute`, `get_attribute`, `focus`, and `blur`.
Event snapshots expose target tag/classes, target and delegated values, form
value and checked state, modifier keys, and wheel delta.

## Large collections (M10 foundation)

`require('mq.rmlui.virtual_list')` provides a Lua-side data window for lists,
tables, trees, and logs. Keep the full source collection in Lua and publish only
the returned `rows` to an RML model. Each item needs a unique, stable `id` or
`key`, or pass `key = function(item) ... end`. The source must be a dense Lua
array. `row_height` is fixed in this first iteration; `overscan` is the number
of extra rows realized on each side of the viewport.

```lua
local rmlui = require('mq.rmlui')
local VirtualList = require('mq.rmlui.virtual_list')
local source = VirtualList.new({ row_height = 22, overscan = 4, items = source_rows })

-- Declare the model and its fields before the RML document parses.
local list_model = {
    rows = {}, top_padding = 0, bottom_padding = 0,
    top_padding_css = '0px', bottom_padding_css = '0px', total_rows = 0,
}
local document = rmlui.open({
    document = 'lua://myplugin/list.rml',
    model = 'list',
    initial = { list = list_model },
})

local binding = source:bind(document, {
    viewport = 'list-viewport',
    model = 'list',
    max_realized = 512, -- defaults to 512; hard limit is 4096
})
local ok, window = binding:refresh() -- call again after scroll/range changes
```

The engine supports stable-key selection/focus, keyboard focus movement, and
focused-row visibility. For a mounted document, use `binding:splice(start,
delete_count, inserted)` or `binding:set_items(items)` so the adapter reads the
current scroll position before the change and keeps the same anchor row at its
new pixel position. The document needs top/bottom spacers whose dynamic heights
use `top_padding_css` and `bottom_padding_css` (numeric `top_padding` and
`bottom_padding` are also published); call `binding:refresh()` when scrolling or
source data changes. The binding publishes `rows` initially, then uses the
smallest contiguous `splice_model` update when the realized window changes. Call
`binding:refresh(true)` after mutating an existing row in place so the visible
model records are republished. Run
`lua tools/tests/test_rmlui_virtual_list.lua` for the 10,000-row bounded-window,
one-row splice, scroll-anchor preservation, and over-budget rejection checks.
Tables/trees, variable row heights, automatic refresh scheduling, and native
plot/drawing elements remain future M10 work.

`lua/examples/rmlui_virtual_list.lua` and `virtual-list.rml` provide the first
in-client reference: 10,000 records, a fixed-height independently scrolling
viewport, delegated row selection, append-through-splice, and realized/total
counts. Run `/lua run rmlui_virtual_list`; use `/rmlvirtual top`, `/rmlvirtual
bottom`, and `/rmlvirtual exit` to drive it.

## Edge resize without a grip icon

The `<resize-edge>` custom element (registered by MQ core, alongside the
native `<handle>` element) resizes a target element from an invisible strip,
so windows can be grabbed by their borders instead of a corner icon:

```xml
<resize-edge target="window" edge="right" />
<resize-edge target="window" edge="bottom-right" />
```

Attributes: `target` (element id, required), `edge` — `left`, `right`,
`top`, `bottom`, `top-left`, `top-right`, `bottom-left`, `bottom-right`
(required) — plus optional `min-width`, `min-height`, `max-width`,
`max-height` in px. Position the strip over the window border with RCSS,
e.g. `position: absolute; right: 0; top: 0; bottom: 0; width: 8px;` for a
right-edge strip. Resizing flows through the normal geometry pipeline, so
`persist = true` windows keep saving their size with no extra work.
