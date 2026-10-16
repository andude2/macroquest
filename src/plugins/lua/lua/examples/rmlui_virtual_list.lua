-- M10 reference: 10,000 Lua-side rows, bounded RML realization.
-- Usage: /lua run rmlui_virtual_list
--        /rmlvirtual [show|top|bottom|exit]

local mq = require('mq')
local rmlui = require('mq.rmlui')
local VirtualList = require('mq.rmlui.virtual_list')

local running = true
local total = 10000
local rows = {}
for id = 1, total do
    rows[id] = {
        id = id,
        name = string.format('Record %05d', id),
        zone = string.format('Test Zone %02d', (id - 1) % 37 + 1),
        level = (id - 1) % 125 + 1,
    }
end

local source = VirtualList.new({
    row_height = 26,
    overscan = 4,
    key = function(item) return item.id end,
    items = rows,
})

local selected_label = 'none'
local last_realized = -1
local document = rmlui.open({
    document = 'mq://rmlui/virtual-list.rml',
    id = 'rmlui-virtual-list',
    persist = true,
    model = 'list',
    initial = {
        list = {
            rows = {},
            top_padding = 0,
            bottom_padding = 0,
            top_padding_css = '0px',
            bottom_padding_css = '0px',
            total_rows = total,
            realized_count = 0,
            selected_label = selected_label,
        },
    },
})

if not document then
    print('[rmlui_virtual_list] failed to open mq://rmlui/virtual-list.rml')
    return
end

local binding = source:bind(document, {
    viewport = 'list-viewport',
    model = 'list',
    max_realized = 256,
    map_row = function(row)
        return {
            key = row.key,
            name = row.item.name,
            zone = row.item.zone,
            level = row.item.level,
            selected = row.selected,
            focused = row.focused,
        }
    end,
})

document:on('click', 'list-rows', function(event)
    local key = tonumber(event.action_value)
    if not key or not source:set_selected(key) then
        return
    end
    selected_label = string.format('#%05d - %s', key, rows[key] and rows[key].name or 'new row')
    document:model('list', { selected_label = selected_label })
    binding:refresh()
end)

document:on('click', 'scroll-top', function()
    document:set_scroll('list-viewport', 0)
    binding:refresh()
end)

document:on('click', 'scroll-bottom', function()
    document:scroll_to_end('list-viewport')
    binding:refresh()
end)

document:on('click', 'append-row', function()
    total = total + 1
    local item = {
        id = total,
        name = string.format('Record %05d (appended)', total),
        zone = 'Live update',
        level = total % 125 + 1,
    }
    rows[#rows + 1] = item
    local ok, window = binding:splice(source:count() + 1, 0, { item })
    if ok then
        print(string.format('[rmlui_virtual_list] appended row %d; realized=%d total=%d',
            total, window.realized, window.total))
    else
        print('[rmlui_virtual_list] append failed: ' .. tostring(window))
        binding:refresh(true)
    end
end)

mq.bind('/rmlvirtual', function(command)
    if command == 'exit' then
        running = false
    elseif command == 'top' then
        document:set_scroll('list-viewport', 0)
        document:show()
    elseif command == 'bottom' then
        document:scroll_to_end('list-viewport')
        document:show()
    else
        document:show()
        document:bring_to_front()
    end
end)

local last_report = 0
while running and document:is_valid() do
    mq.delay(100)
    local ok, window = binding:refresh()
    if ok then
        if window.realized ~= last_realized then
            last_realized = window.realized
            document:model('list', { realized_count = last_realized })
        end
        if mq.gettime() - last_report >= 5000 then
            last_report = mq.gettime()
            print(string.format('[rmlui_virtual_list] realized=%d total=%d range=%d-%d',
                window.realized, window.total, window.first, window.last))
        end
    end
end

mq.unbind('/rmlvirtual')
document:close()
