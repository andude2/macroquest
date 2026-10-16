---RMInventory spike entry point — RML card-grid port of EZInventory.
---Read-only Equipped view. Run with: /lua run rminventory
---Commands: /rminv toggle|show|hide|refresh|diag|exit
local mq = require('mq')

local Shell = require('rminventory.ui.shell')

local open = true
local scriptName = 'rminventory'

local function printHelp()
    printf('\ag[%s] Commands:', scriptName)
    printf('\ay  /rminv toggle \ax- show/hide the window')
    printf('\ay  /rminv show|hide \ax- explicit visibility')
    printf('\ay  /rminv refresh \ax- rescan equipment now')
    printf('\ay  /rminv diag \ax- report per-character snapshot and index counts')
    printf('\ay  /rminv mini on|off \ax- floating toggle pill')
    printf('\ay  /rminv startmin on|off \ax- start minimized next load')
    printf('\ay  /rminv exit \ax- unload %s', scriptName)
end

mq.bind('/rminv', function(cmd, arg)
    cmd = (cmd or ''):lower()
    arg = (arg or ''):lower()
    if cmd == 'toggle' or cmd == '' then
        Shell.Toggle()
    elseif cmd == 'show' then
        Shell.Open()
    elseif cmd == 'hide' then
        Shell.Close()
    elseif cmd == 'refresh' then
        Shell.Pulse()
    elseif cmd == 'diag' then
        Shell.Diagnostics()
    elseif cmd == 'mini' then
        if arg == 'on' then Shell.SetShowToggle(true)
        elseif arg == 'off' then Shell.SetShowToggle(false) end
        local s = Shell.GetSettings()
        printf('\ag[%s] toggle pill: %s', scriptName, s.showToggle and 'on' or 'off')
    elseif cmd == 'startmin' then
        if arg == 'on' then Shell.SetStartMinimized(true)
        elseif arg == 'off' then Shell.SetStartMinimized(false) end
        local s = Shell.GetSettings()
        printf('\ag[%s] start minimized: %s', scriptName, s.startMinimized and 'on' or 'off')
    elseif cmd == 'exit' then
        open = false
    else
        printHelp()
    end
end)

if mq.onunload then
    mq.onunload(function() pcall(Shell.Shutdown) end)
end

printf('\ag[%s] Loaded. Use /rminv toggle.', scriptName)
printHelp()
Shell.StartNetwork()

while open do
    local ok, err = pcall(Shell.Pulse)
    if not ok then
        printf('\ar[%s] pulse error: %s', scriptName, tostring(err))
    end
    mq.doevents()
    mq.delay(250)
end

pcall(Shell.Shutdown)
pcall(mq.unbind, '/rminv')
printf('\ay[%s] Unloaded.', scriptName)
