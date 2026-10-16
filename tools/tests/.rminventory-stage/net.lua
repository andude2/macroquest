---RMInventory networking — lean peer sync on top of the EZInventory actor.
---Own mailboxes (rminventory_exchange/_command) so it can run without
---colliding with /ezinventory transports. Run ONE inventory script per box:
---peers key off character name, so two publishers for the same char will
---flap peer tables everywhere.
local mq = require('mq')

local M = {
    _version = '0.1',
    _name = 'RMInventoryNet',
}

local actor = nil
local started = false
local lastPublishMs = 0
local lastRequestMs = 0
-- Inventory changes publish immediately; these are just recovery heartbeats.
-- Frequent forced full publishes/peer requests made every client rescan and
-- rebroadcast the entire inventory continuously.
local PUBLISH_INTERVAL_MS = 120000
local REQUEST_INTERVAL_MS = 300000

local function nowMs()
    local ok, t = pcall(function() return mq.gettime() end)
    if ok and tonumber(t) then return tonumber(t) end
    return math.floor((os.clock() or 0) * 1000)
end

function M.setup(opts)
    actor = opts and opts.actor or nil
end

function M.actor() return actor end

---Register mailboxes + publish self + request peers. Idempotent.
function M.start()
    if started then return true end
    if not actor then
        local ok, mod = pcall(require, 'EZInventory.modules.inventory_actor')
        if not ok then
            printf('\ar[RMInventory] network unavailable (actor failed to load)')
            return false
        end
        actor = mod
    end
    _G.EZINV_MODULE = 'rminventory'
    if actor.update_config then
        pcall(actor.update_config, { loadBasicStats = true })
    end
    local ok = actor.init and actor.init() or false
    if not ok then
        printf('\ar[RMInventory] network unavailable (actor init failed)')
        return false
    end
    if actor.publish_inventory then pcall(actor.publish_inventory, true) end
    if actor.request_all_inventories then pcall(actor.request_all_inventories) end
    lastPublishMs = nowMs()
    lastRequestMs = nowMs()
    started = true
    printf('\ag[RMInventory] peer sync online.')
    return true
end

---Pump requests, publish on change + interval, refresh peer set.
function M.update()
    if not started or not actor then return end
    if actor.process_pending_requests then pcall(actor.process_pending_requests) end
    -- The actor queues full inventory scans/sends as deferred work. EZInventory
    -- drains one task per main-loop tick; RMInventory must do the same or peers
    -- remain on their initial equipped-only fast snapshot indefinitely.
    if actor.deferred_tasks and #actor.deferred_tasks > 0 then
        local task = table.remove(actor.deferred_tasks, 1)
        if type(task) == 'function' then pcall(task) end
    end
    local now = nowMs()
    local changed = actor.inventory_has_changed and actor.inventory_has_changed() or false
    if changed and actor.publish_inventory then
        pcall(actor.publish_inventory, false)
        lastPublishMs = now
    elseif (now - lastPublishMs) >= PUBLISH_INTERVAL_MS and actor.publish_inventory then
        pcall(actor.publish_inventory, true)
        lastPublishMs = now
    end
    if (now - lastRequestMs) >= REQUEST_INTERVAL_MS and actor.request_all_inventories then
        pcall(actor.request_all_inventories)
        lastRequestMs = now
    end
end

---Peer list from the actor store. Each: {id, name, server, data}.
function M.peers()
    local result = {}
    if not started or not actor or not actor.peer_inventories then return result end
    local myName = (mq.TLO.Me.CleanName() or ''):lower()
    for peerId, inv in pairs(actor.peer_inventories) do
        if type(inv) == 'table' and inv.name then
            local peerName = tostring(inv.name)
            if peerName:lower() ~= myName then
                result[#result + 1] = {
                    id = peerId,
                    name = peerName,
                    server = tostring(inv.server or ''),
                    data = inv,
                }
            end
        end
    end
    table.sort(result, function(a, b) return a.name:lower() < b.name:lower() end)
    return result
end

return M
