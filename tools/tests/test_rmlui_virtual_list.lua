package.path = 'src/plugins/lua/lua/?.lua;src/plugins/lua/lua/?/init.lua;' .. package.path

local VirtualList = require('mq.rmlui.virtual_list')

local function items(count, first_id)
    local result = {}
    for index = 1, count do
        local id = (first_id or 0) + index
        result[index] = { id = id, name = 'row ' .. id }
    end
    return result
end

local list = VirtualList.new({ row_height = 20, overscan = 3, items = items(10000) })
local view = list:window(100000, 200)
assert(view.total == 10000)
assert(view.first == 4998 and view.last == 5013)
assert(view.realized == 16, 'realized rows must depend on viewport, not total count')
assert(view.top == (view.first - 1) * 20)
assert(view.bottom == (view.total - view.last) * 20)
assert(view.rows[1].item.id == view.first)

assert(list:set_selected(5000))
assert(list:set_focused(5000))
local next_key = list:move_focus(1, true)
assert(next_key == 5001 and list:selected() == 5001)
assert(not list:set_selected(20000))
list:set_focused(9000)
assert(list:ensure_focused_visible(200) == (9000 * 20) - 200)
list:window(list:get_scroll_offset(), 200)

-- Insert before the visible anchor; retain the same stable row at the same
-- intra-row pixel offset even though its numeric index changes.
list:window(100009, 200)
local anchor_before = list:key_at(math.floor(list:get_scroll_offset() / 20) + 1)
list:splice(1, 0, { { id = 0, name = 'inserted' } })
local anchor_after = list:key_at(math.floor(list:get_scroll_offset() / 20) + 1)
assert(anchor_before == anchor_after)
assert(list:get_scroll_offset() == 100029)
assert(list:count() == 10001 and list.splice_count == 1)
assert(list:selected() == 5001 and list:focused() == 9000)

local before_count = list:count()
local ok = pcall(function()
    list:splice(1, 0, { { id = 0 }, { id = 0 } })
end)
assert(not ok and list:count() == before_count, 'invalid splice must be atomic')

list:splice(5002, 1, {})
assert(list:selected() == nil and list:focused() == 9000,
    'selection clears for a deleted row while focus survives by key')
list:splice(9000, 1, {})
assert(list:focused() == nil, 'focus must clear when its stable row is removed')

-- Exercise the document adapter with the same API shape exposed by MQ2Lua.
local fake_document = {
    valid = true,
    metrics = { offset = 0, viewport = 200 },
    models = {},
    splices = {},
}
function fake_document:is_valid() return self.valid end
function fake_document:get_scroll() return self.metrics end
function fake_document:set_scroll(_, offset)
    self.metrics.offset = offset
    return true
end
function fake_document:model(name, values)
    self.models[name] = self.models[name] or {}
    for key, value in pairs(values) do
        self.models[name][key] = value
    end
    return true
end
function fake_document:splice_model(name, variable, start, delete_count, values)
    local rows = self.models[name][variable]
    for _ = 1, delete_count do
        table.remove(rows, start)
    end
    for index, value in ipairs(values) do
        table.insert(rows, start + index - 1, value)
    end
    self.splices[#self.splices + 1] = {
        start = start,
        delete_count = delete_count,
        insert_count = #values,
    }
    return true
end

local bound = VirtualList.new({ row_height = 20, overscan = 3, items = items(10000) })
local binding = bound:bind(fake_document, { viewport = 'list-viewport', model = 'list' })
local bound_ok, bound_window = binding:refresh()
assert(bound_ok and bound_window.realized == 13)
assert(#fake_document.models.list.rows == 13)
assert(fake_document.models.list.top_padding_css == '0.00px')
fake_document.metrics.offset = 20
bound_ok, bound_window = binding:refresh()
assert(bound_ok and bound_window.realized == 14)
assert(#fake_document.models.list.rows == 14)
assert(fake_document.splices[1].delete_count == 0
    and fake_document.splices[1].insert_count == 1,
    'scrolling by one row should splice only the newly realized row')

fake_document.metrics.offset = 100000
assert(binding:refresh())
local anchor_before = bound:key_at(math.floor(fake_document.metrics.offset / 20) + 1)
bound_ok = binding:splice(1, 0, { { id = 0 } })
assert(bound_ok and fake_document.metrics.offset == 100020,
    'bound splice should move scroll offset with the stable anchor row')
local anchor_after = bound:key_at(math.floor(fake_document.metrics.offset / 20) + 1)
assert(anchor_before == anchor_after)

fake_document.metrics.viewport = 20000
local rows_before_budget_failure = #fake_document.models.list.rows
local over_budget, reason = binding:refresh()
assert(not over_budget and reason == 'visible window exceeds max_realized',
    'adapter must refuse unbounded realized windows')
assert(#fake_document.models.list.rows == rows_before_budget_failure,
    'budget rejection must not mutate rows')

print(string.format('RmlUi virtual list: PASS (%d source rows, %d realized)',
    view.total, view.realized))
