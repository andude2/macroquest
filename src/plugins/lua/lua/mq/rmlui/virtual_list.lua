-- Bounded data window for RmlUi list/table/log components.
--
-- The source collection stays in Lua. Call window(offset, viewport_height) to
-- obtain only the visible rows plus overscan for the RML data model. Items need
-- stable IDs (`id` or `key` by default), or callers can supply `key(item)`.

local VirtualList = {}
VirtualList.__index = VirtualList

local function default_key(item)
    if type(item) ~= 'table' then
        return nil
    end
    return item.id ~= nil and item.id or item.key
end

local function validate_items(items, key_of)
    assert(type(items) == 'table', 'items must be an array')

    local copy = {}
    local indices = {}
    local count = #items
    for index = 1, count do
        local item = items[index]
        local key = key_of(item, index)
        assert(key ~= nil, 'each item must have a stable key')
        assert(indices[key] == nil, 'item keys must be unique')
        copy[index] = item
        indices[key] = index
    end
    return copy, indices
end

function VirtualList.new(options)
    options = options or {}
    local row_height = assert(tonumber(options.row_height), 'row_height is required')
    assert(row_height > 0, 'row_height must be greater than zero')
    local overscan = math.max(0, math.floor(tonumber(options.overscan) or 2))
    local key_of = options.key or default_key
    assert(type(key_of) == 'function', 'key must be a function')

    local self = setmetatable({
        row_height = row_height,
        overscan = overscan,
        key_of = key_of,
        items = {},
        indices = {},
        scroll_offset = 0,
        selected_key = nil,
        focused_key = nil,
        splice_count = 0,
    }, VirtualList)

    if options.items then
        self:set_items(options.items, false)
    end
    return self
end

function VirtualList:count()
    return #self.items
end

function VirtualList:get(key)
    local index = self.indices[key]
    if not index then
        return nil
    end
    return self.items[index], index
end

function VirtualList:key_at(index)
    local item = self.items[index]
    if item == nil then
        return nil
    end
    return self.key_of(item, index)
end

function VirtualList:get_scroll_offset()
    return self.scroll_offset
end

function VirtualList:selected()
    return self.selected_key
end

function VirtualList:focused()
    return self.focused_key
end

function VirtualList:set_selected(key)
    if key ~= nil and self.indices[key] == nil then
        return false
    end
    self.selected_key = key
    return true
end

function VirtualList:set_focused(key)
    if key ~= nil and self.indices[key] == nil then
        return false
    end
    self.focused_key = key
    return true
end

function VirtualList:move_focus(delta, select_focused)
    delta = math.floor(tonumber(delta) or 0)
    if #self.items == 0 or delta == 0 then
        return self.focused_key
    end

    local index = self.indices[self.focused_key] or 1
    index = math.max(1, math.min(#self.items, index + delta))
    self.focused_key = self:key_at(index)
    if select_focused then
        self.selected_key = self.focused_key
    end
    return self.focused_key, index
end

function VirtualList:ensure_focused_visible(viewport_height)
    local index = self.indices[self.focused_key]
    if not index then
        return self.scroll_offset
    end

    viewport_height = math.max(0, tonumber(viewport_height) or 0)
    if viewport_height == 0 then
        return self.scroll_offset
    end

    local row_top = (index - 1) * self.row_height
    local row_bottom = row_top + self.row_height
    if row_top < self.scroll_offset then
        self.scroll_offset = row_top
    elseif row_bottom > self.scroll_offset + viewport_height then
        self.scroll_offset = row_bottom - viewport_height
    end

    self.scroll_offset = math.max(0, math.min(self.scroll_offset,
        math.max(0, (#self.items * self.row_height) - viewport_height)))
    return self.scroll_offset
end

-- Replace the source snapshot. If the current scroll anchor still exists, keep
-- it at the same pixel offset; selection and focus survive by stable key.
function VirtualList:set_items(items, preserve_anchor)
    local next_items, next_indices = validate_items(items, self.key_of)
    local next_offset = self.scroll_offset

    if preserve_anchor ~= false and #self.items > 0 and #next_items > 0 then
        local old_index = math.floor(self.scroll_offset / self.row_height) + 1
        old_index = math.min(old_index, #self.items)
        local anchor_key = self:key_at(old_index)
        local new_index = next_indices[anchor_key]
        if new_index then
            local intra_row = self.scroll_offset - ((old_index - 1) * self.row_height)
            next_offset = ((new_index - 1) * self.row_height) + intra_row
        end
    end

    self.items = next_items
    self.indices = next_indices
    self.scroll_offset = math.max(0, next_offset)
    if self.selected_key ~= nil and not next_indices[self.selected_key] then
        self.selected_key = nil
    end
    if self.focused_key ~= nil and not next_indices[self.focused_key] then
        self.focused_key = nil
    end
    return true
end

-- One-based splice, matching document:splice_model. Validation is performed on
-- a candidate snapshot first, so duplicate/missing IDs leave the source intact.
function VirtualList:splice(start, delete_count, inserted)
    start = math.floor(tonumber(start) or 0)
    delete_count = math.floor(tonumber(delete_count) or 0)
    assert(start >= 1 and start <= #self.items + 1, 'start is out of range')
    assert(delete_count >= 0, 'delete_count must not be negative')
    inserted = inserted or {}
    assert(type(inserted) == 'table', 'inserted must be an array')

    local removed = math.min(delete_count, #self.items - start + 1)
    local candidate = {}
    for index = 1, start - 1 do
        candidate[#candidate + 1] = self.items[index]
    end
    for index = 1, #inserted do
        candidate[#candidate + 1] = inserted[index]
    end
    for index = start + removed, #self.items do
        candidate[#candidate + 1] = self.items[index]
    end

    self:set_items(candidate, true)
    self.splice_count = self.splice_count + 1
    return true
end

-- Returns [first,last], total, and pixel spacers. The returned row array is
-- bounded by viewport height / row_height plus 2*overscan, independent of the
-- total source size.
function VirtualList:window(offset, viewport_height)
    offset = math.max(0, tonumber(offset) or self.scroll_offset)
    viewport_height = math.max(0, tonumber(viewport_height) or 0)
    offset = math.min(offset, math.max(0, (#self.items * self.row_height) - viewport_height))
    self.scroll_offset = offset

    local total = #self.items
    local first, last = 1, 0
    if total > 0 and viewport_height > 0 then
        local visible_first = math.floor(offset / self.row_height) + 1
        local visible_last = math.ceil((offset + viewport_height) / self.row_height)
        first = math.max(1, visible_first - self.overscan)
        last = math.min(total, visible_last + self.overscan)
    end

    local rows = {}
    for index = first, last do
        rows[#rows + 1] = {
            key = self:key_at(index),
            index = index,
            item = self.items[index],
            selected = self:key_at(index) == self.selected_key,
            focused = self:key_at(index) == self.focused_key,
        }
    end

    return {
        first = first,
        last = last,
        total = total,
        rows = rows,
        top = (first - 1) * self.row_height,
        bottom = math.max(0, (total - last) * self.row_height),
        content_height = total * self.row_height,
        scroll_offset = offset,
        realized = #rows,
    }
end

local function same_row(left, right)
    for key, value in pairs(left) do
        if right[key] ~= value then
            return false
        end
    end
    for key, value in pairs(right) do
        if left[key] ~= value then
            return false
        end
    end
    return true
end

local function publish_row(map_row, row)
    local value = map_row(row)
    assert(type(value) == 'table', 'map_row must return a table')
    return value
end

-- Bind this source to an RML model. The model must be declared before parsing,
-- and its initial table should include the configured rows/padding/count fields.
function VirtualList:bind(document, options)
    options = options or {}
    assert(type(document) == 'table' or type(document) == 'userdata',
        'document is required')
    local viewport_id = assert(options.viewport, 'viewport element id is required')
    local model_name = assert(options.model, 'model name is required')
    local rows_name = options.rows or 'rows'
    local top_name = options.top or 'top_padding'
    local bottom_name = options.bottom or 'bottom_padding'
    local top_css_name = options.top_css or 'top_padding_css'
    local bottom_css_name = options.bottom_css or 'bottom_padding_css'
    local total_name = options.total or 'total_rows'
    local max_realized = math.floor(tonumber(options.max_realized) or 512)
    assert(max_realized >= 1 and max_realized <= 4096,
        'max_realized must be between 1 and 4096')
    local map_row = options.map_row or function(row)
        return {
            key = row.key,
            index = row.index,
            selected = row.selected,
            focused = row.focused,
            item = row.item,
        }
    end

    local binding = {
        list = self,
        document = document,
        viewport_id = viewport_id,
        model_name = model_name,
        rows_name = rows_name,
        top_name = top_name,
        bottom_name = bottom_name,
        top_css_name = top_css_name,
        bottom_css_name = bottom_css_name,
        total_name = total_name,
        max_realized = max_realized,
        map_row = map_row,
        published = nil,
        metadata = nil,
        last_window = nil,
    }

    function binding:refresh(force)
        if not self.document:is_valid() then
            return false, 'document is not valid'
        end

        local metrics = self.document:get_scroll(self.viewport_id)
        if not metrics then
            return false, 'scroll metrics unavailable'
        end

        local window = self.list:window(metrics.offset, metrics.viewport)
        if window.realized > self.max_realized then
            return false, 'visible window exceeds max_realized'
        end

        local rows = {}
        for index, row in ipairs(window.rows) do
            rows[index] = publish_row(self.map_row, row)
        end

        local metadata = {
            [self.top_name] = window.top,
            [self.bottom_name] = window.bottom,
            [self.top_css_name] = string.format('%.2fpx', window.top),
            [self.bottom_css_name] = string.format('%.2fpx', window.bottom),
            [self.total_name] = window.total,
        }

        if not self.published then
            local initial = {
                [self.rows_name] = rows,
            }
            for name, value in pairs(metadata) do
                initial[name] = value
            end
            if not self.document:model(self.model_name, initial) then
                return false, 'initial model publication failed'
            end
            self.published = rows
            self.metadata = metadata
            self.last_window = window
            return true, window
        end

        local old_rows = self.published
        local prefix = 0
        if not force then
            while prefix < #old_rows and prefix < #rows
                and same_row(old_rows[prefix + 1], rows[prefix + 1]) do
                prefix = prefix + 1
            end
        end

        local suffix = 0
        if not force then
            while suffix < (#old_rows - prefix) and suffix < (#rows - prefix)
                and same_row(old_rows[#old_rows - suffix], rows[#rows - suffix]) do
                suffix = suffix + 1
            end
        end

        local delete_count = #old_rows - prefix - suffix
        local inserted = {}
        for index = prefix + 1, #rows - suffix do
            inserted[#inserted + 1] = rows[index]
        end

        if delete_count > 0 or #inserted > 0 then
            if not self.document:splice_model(self.model_name, self.rows_name,
                prefix + 1, delete_count, inserted) then
                return false, 'row splice failed'
            end
            self.published = rows
        end

        local metadata_changed = false
        for name, value in pairs(metadata) do
            if self.metadata[name] ~= value then
                metadata_changed = true
                break
            end
        end
        if metadata_changed then
            if not self.document:model(self.model_name, metadata) then
                self.last_window = window
                return false, 'window metadata publication failed'
            end
            self.metadata = metadata
        end

        self.last_window = window
        return true, window
    end

    local function sync_anchor(binding_self, mutate)
        if not binding_self.document:is_valid() then
            return false, 'document is not valid'
        end
        local metrics = binding_self.document:get_scroll(binding_self.viewport_id)
        if not metrics then
            return false, 'scroll metrics unavailable'
        end

        binding_self.list:window(metrics.offset, metrics.viewport)
        local old_offset = metrics.offset
        mutate()
        local new_offset = binding_self.list:get_scroll_offset()
        if new_offset ~= old_offset
            and not binding_self.document:set_scroll(binding_self.viewport_id, new_offset) then
            return false, 'scroll anchor update failed'
        end
        return binding_self:refresh()
    end

    -- Mutate through the binding when a mounted document must preserve its
    -- visible stable row. The scroll position is read immediately beforehand,
    -- then restored to the row's new pixel position.
    function binding:splice(start, delete_count, inserted)
        return sync_anchor(self, function()
            self.list:splice(start, delete_count, inserted)
        end)
    end

    function binding:set_items(items, preserve_anchor)
        return sync_anchor(self, function()
            self.list:set_items(items, preserve_anchor)
        end)
    end

    return binding
end

return VirtualList
