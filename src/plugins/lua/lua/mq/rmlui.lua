---@meta

---@class MQRmlUiEvent
---@field type string
---@field target_id string
---@field current_id string
---@field target_tag string
---@field target_classes string
---@field target_value string Opaque value from the event target's `data-mq-value` attribute.
---@field current_value string Opaque value from the listener element's `data-mq-value` attribute.
---@field action_id string Nearest target ancestor carrying `data-mq-value`, up to the listener element.
---@field action_value string Delegated action value from `action_id`; use this for rows containing nested markup.
---@field value string Current target form value, when present.
---@field checked boolean Whether the target has the `checked` attribute.
---@field mouse_x integer
---@field mouse_y integer
---@field handle_x number Position reported by a handledrag event.
---@field handle_y number Position reported by a handledrag event.
---@field button integer
---@field key_identifier integer
---@field wheel_delta number
---@field ctrl_key boolean
---@field shift_key boolean
---@field alt_key boolean
---@field meta_key boolean

---@class MQRmlUiValue
---@field [string|integer] any A model value: nil, boolean, number, string, array, or nested record.

---@class MQRmlUiOpenOptions
---@field document string Virtual path using mq://rmlui/, plugin://, or lua://.
---@field presentation? 'direct' Hosted presentation is not implemented yet and is rejected.
---@field input? 'interactive'|'clickthrough'
---@field layer? integer Higher values render in front of lower values.
---@field visible? boolean
---@field id? string Stable window id, required when persistence is enabled.
---@field persist? boolean Restore and debounce-save this window's geometry and lock state.
---@field geometry_element? string Element whose rectangle is persisted; defaults to `window`.
---@field lock_element? string Element receiving the `mq-locked` class; defaults to `geometry_element`.
---@field locked? boolean Initial window lock state.
---@field model? string Data model to declare before the document parses.
---@field models? string[] Data models to declare before the document parses.
---@field initial? table<string, table<string, MQRmlUiValue>> Declares variable names before parsing, then applies their values immediately after load. Include every variable referenced by RML expressions.

---@class MQRmlUiEventStats
---@field listeners integer
---@field pending integer
---@field dropped integer
---@field coalesced integer
---@field callback_errors integer
---@field capacity integer

---@class MQRmlUiGeometry
---@field x number
---@field y number
---@field width number
---@field height number

---@class MQRmlUiScrollMetrics
---@field offset number
---@field extent number
---@field viewport number
---@field max_offset number
---@field at_end boolean

---@class MQRmlUiDocument
local document = {}

function document:show() end
function document:hide() end
---@return boolean
function document:bring_to_front() end
---@return boolean
function document:reload() end
function document:close() end
---@return boolean
function document:is_valid() end
---@param element_id string
---@param text string
---@return boolean
function document:set_text(element_id, text) end
---@param element_id string
---@param value string
---@return boolean
function document:set_value(element_id, value) end
---@param element_id string
---@param class_name string
---@param active boolean
---@return boolean
function document:set_class(element_id, class_name, active) end
---@param element_id string
---@param property string
---@param value string
---@return boolean
function document:set_style(element_id, property, value) end
---@param element_id string
---@param attribute string
---@param value string
---@return boolean
function document:set_attribute(element_id, attribute, value) end
---@param element_id string
---@param attribute string
---@return boolean
function document:remove_attribute(element_id, attribute) end
---@param element_id string
---@param attribute string
---@return string|nil
function document:get_attribute(element_id, attribute) end
---@param element_id string
---@return boolean
function document:focus(element_id) end
---@param element_id string
---@return boolean
function document:blur(element_id) end
---@param element_id string
---@return MQRmlUiGeometry|nil
function document:get_geometry(element_id) end
---@param element_id string
---@param geometry MQRmlUiGeometry
---@return boolean
function document:set_geometry(element_id, geometry) end
---@param element_id string
---@return MQRmlUiScrollMetrics|nil
function document:get_scroll(element_id) end
---@param element_id string
---@param offset number
---@return boolean
function document:set_scroll(element_id, offset) end
---@param element_id string
---@return boolean
function document:scroll_to_end(element_id) end
---Keeps the element pinned to its bottom across model/layout updates while it
---was already within `threshold` pixels of the bottom before the update.
---@param element_id string
---@param enabled boolean
---@param threshold? number
---@return boolean
function document:set_scroll_anchor(element_id, enabled, threshold) end
---@param locked boolean Adds or removes `mq-locked` on the configured lock element.
---@return boolean
function document:set_locked(locked) end
---@return boolean
function document:is_locked() end

---Sets variables on a data model. A Lua table becomes an array when its integer
---keys are contiguous from 1, and a record otherwise, so `{ 'a', 'b' }` and
---`{ name = 'a' }` need no extra call. Records may nest to any depth the
---configured limits allow, and a variable may change between a scalar, an array,
---and a record over its lifetime.
---
---The model must be declared through the `model` or `models` option on
---`rmlui.open`, because RmlUi binds a document's data-model attributes while the
---document parses. A model that was not declared is created on demand but will
---not be visible to an already-parsed document.
---@param model_name string
---@param values table<string, MQRmlUiValue>
---@return boolean
function document:model(model_name, values) end

---Atomically replaces part of an existing array model variable. `start` is
---one-based and may be `#array + 1` to append. `delete_count` is clamped to the
---available entries. The existing value remains unchanged if validation or a
---model quota rejects the result.
---@param model_name string
---@param variable string
---@param start integer
---@param delete_count integer
---@param values MQRmlUiValue[]
---@return boolean
function document:splice_model(model_name, variable, start, delete_count, values) end

---Reads a variable back out. This is how a value written by RmlUi through a
---two-way binding (an input's `value` attribute, for example) is observed.
---Returns nil if the model or variable is not found, or when called off the
---main thread.
---@param model_name string
---@param variable string
---@return MQRmlUiValue|nil
function document:get_model(model_name, variable) end
---@param event_type string
---@param element_id string
---@param callback fun(event: MQRmlUiEvent)
---@return integer? subscription
function document:on(event_type, element_id, callback) end
---@param subscription integer
---@return boolean
function document:off(subscription) end
---@return MQRmlUiEventStats
function document:event_stats() end

local rmlui = {}

---@param options MQRmlUiOpenOptions
---@return MQRmlUiDocument?
function rmlui.open(options) end

return rmlui
