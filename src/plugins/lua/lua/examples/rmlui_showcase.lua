local mq = require('mq')
local rmlui = require('mq.rmlui')

local running = true
local tab = 'motion'
local paused = false
local timeline_running = true
local progress = 0
local counter = 42
local enabled = true
local choice = 'two'
local events = 0
local ticks = 0
local replay = false
local next_row = 5
local player_name = 'Adventurer'
local message = 'Hello from RmlUi'
local rows = {
    { id = 'scalar', name = 'Player health', kind = 'scalar', value = '87%', live = true },
    { id = 'record', name = 'Target', kind = 'nested record', value = 'Goblin', live = true },
    { id = 'array', name = 'Inventory', kind = 'array', value = '4 items', live = true },
    { id = 'form', name = 'Search query', kind = 'two-way', value = 'RmlUi', live = true },
}
local feed = {
    { title = 'Block flow', body = 'Natural vertical document layout.' },
    { title = 'Flexbox', body = 'Rows, columns, wrapping and alignment.' },
    { title = 'Positioning', body = 'Relative, absolute and fixed surfaces.' },
    { title = 'Overflow', body = 'Clipping and independently scrolling regions.' },
    { title = 'Units', body = 'Pixels, percentages, em, rem, vw and vh.' },
}

local function state()
    return {
        tab = tab, paused = paused, pause_label = paused and 'Resume' or 'Pause',
        timeline_label = timeline_running and 'Pause timeline' or 'Play timeline',
        progress = math.floor(progress), progress_css = string.format('%.1f%%', progress),
        counter = counter, counter_css = string.format('%d%%', math.max(0, math.min(100, counter))),
        enabled = enabled, choice = choice, name = player_name, message = message,
        status = paused and 'Paused' or 'Live', ticks = ticks, event_count = events,
        rows = rows, row_count = #rows, feed = feed, replay = replay,
    }
end

local window = rmlui.open({
    -- Keep this example self-contained below the configured Lua directory.
    -- lua://rmlui_showcase/... resolves to <LuaDir>/rmlui_showcase/...
    -- and does not depend on MacroQuest's core resources root.
    document = 'lua://rmlui_showcase/showcase.rml', id = 'rmlui-showcase-v2', persist = true,
    models = { 'showcase' }, initial = { showcase = state() },
})
assert(window, 'RmlUi showcase failed to open lua://rmlui_showcase/showcase.rml; check that its assets are installed below the Lua directory')

local function update(values)
    window:model('showcase', values or state())
end
local function handled()
    events = events + 1
    update({ event_count = events })
end

window:on('click', 'tabs', function(event)
    if event.action_value == '' then return end
    tab = event.action_value
    events = events + 1
    update({ tab = tab, event_count = events })
end)
window:on('click', 'pause', function()
    paused = not paused
    events = events + 1
    update({ paused = paused, pause_label = paused and 'Resume' or 'Pause', status = paused and 'Paused' or 'Live', event_count = events })
end)
window:on('click', 'replay', function()
    replay = not replay
    events = events + 1
    update({ replay = replay, event_count = events })
end)
window:on('click', 'timeline-toggle', function()
    timeline_running = not timeline_running
    events = events + 1
    update({ timeline_label = timeline_running and 'Pause timeline' or 'Play timeline', event_count = events })
end)
window:on('click', 'timeline-reset', function()
    progress = 0
    handled()
end)
window:on('click', 'counter-up', function()
    counter = math.min(100, counter + 5)
    events = events + 1
    update({ counter = counter, counter_css = counter .. '%', event_count = events })
end)
window:on('click', 'counter-down', function()
    counter = math.max(0, counter - 5)
    events = events + 1
    update({ counter = counter, counter_css = counter .. '%', event_count = events })
end)
window:on('click', 'enabled-toggle', function()
    enabled = not enabled
    events = events + 1
    update({ enabled = enabled, event_count = events })
end)
window:on('click', 'choice', function(event)
    if event.action_value == '' then return end
    choice = event.action_value
    events = events + 1
    update({ choice = choice, event_count = events })
end)
window:on('change', 'name-input', function(event)
    player_name = event.value
    events = events + 1
    update({ name = player_name, event_count = events })
end)
window:on('change', 'message-input', function(event)
    message = event.value
    events = events + 1
    update({ message = message, event_count = events })
end)
window:on('click', 'add-row', function()
    local row = { id = 'dynamic-' .. next_row, name = 'Dynamic item ' .. next_row, kind = 'splice', value = os.date('%H:%M:%S'), live = true }
    next_row = next_row + 1
    table.insert(rows, row)
    window:splice_model('showcase', 'rows', #rows, 0, { row })
    events = events + 1
    update({ row_count = #rows, event_count = events })
end)

mq.bind('/rmlshowcase', function(command)
    if command == 'exit' then running = false
    elseif command == 'reload' then window:reload()
    elseif command == 'lock' then window:set_locked(not window:is_locked())
    else window:show(); window:bring_to_front() end
end)

-- Keep this Lua owner alive. Closing the document externally also ends the
-- script cleanly; `/rmlshowcase exit` remains the normal shutdown path.
local last_tick = mq.gettime()
while running and window:is_valid() do
    mq.delay(16)
    if timeline_running and not paused then
        progress = (progress + 0.35) % 100
        update({ progress = math.floor(progress), progress_css = string.format('%.1f%%', progress) })
    end
    if mq.gettime() - last_tick >= 1000 then
        ticks = ticks + 1
        last_tick = mq.gettime()
        update({ ticks = ticks })
    end
end

mq.unbind('/rmlshowcase')
window:close()
