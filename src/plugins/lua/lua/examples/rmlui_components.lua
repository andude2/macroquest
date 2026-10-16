local mq = require('mq')
local rmlui = require('mq.rmlui')

local running = true
local alerts = true
local scope = 'local'
local selected = 'cards'

local rows = {
    { id = 'cards', name = 'Cards and statistics', state = 'Ready', ok = true, selected = true },
    { id = 'controls', name = 'Buttons and switches', state = 'Ready', ok = true, selected = false },
    { id = 'tables', name = 'Tables and progress', state = 'Ready', ok = true, selected = false },
}

local function model()
    return {
        count = 403,
        status = 'Ready',
        query = '',
        scope = scope,
        alerts = alerts,
        progress = '68%',
        row_count = #rows,
        rows = rows,
    }
end

local window = rmlui.open({
    document = 'mq://rmlui/components.rml',
    id = 'components',
    persist = true,
    models = { 'components' },
    initial = { components = model() },
})

if not window then return end
window:set_text('window-title', 'RmlUi Components')

window:on('click', 'alerts', function()
    alerts = not alerts
    window:model('components', { alerts = alerts })
end)

window:on('click', 'scope', function(event)
    if event.action_value ~= '' then
        scope = event.action_value
        window:model('components', { scope = scope })
    end
end)

window:on('click', 'rows', function(event)
    if event.action_value == '' then return end
    selected = event.action_value
    for _, row in ipairs(rows) do row.selected = row.id == selected end
    window:model('components', { rows = rows })
end)

window:on('click', 'refresh', function()
    window:model('components', { status = 'Updated', progress = '100%' })
end)

mq.bind('/rmlcomponents', function(command)
    if command == 'exit' then running = false
    elseif command == 'focus' then window:focus('search-input')
    else window:show() end
end)

while running do mq.delay(50) end

mq.unbind('/rmlcomponents')
window:close()
