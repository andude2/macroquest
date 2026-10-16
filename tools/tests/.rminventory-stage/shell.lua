---RMInventory shell presenter — RML rebuild of EZInventory.
---Paper-doll view (self) + live peer compare panel, wired to the
---EZInventory inventory_actor for scanning and peer sync.
---No EZInventory boot needed: gather_inventory() scans synchronously.
local mq = require('mq')
local rmlOk, rmlLib = pcall(require, 'mq.rmlui')
local rml = rmlOk and rmlLib or nil

local actorOk, actor = pcall(require, 'EZInventory.modules.inventory_actor')
if not actorOk then actor = nil end
local slotsOk, slotsMod = pcall(require, 'EZInventory.logic.item_utils')
local getSlotName = slotsOk and slotsMod.getSlotNameFromID
    or function(id) return 'Slot ' .. tostring(id) end
local Net = require('rminventory.net')
Net.setup({ actor = actor })
local indexOk, Index = pcall(require, 'EZInventory.logic.inventory_index')
if not indexOk then Index = nil end

local Shell = {
    _version = '0.2',
    _name = 'RMInventoryShell',
}

local window = nil
local miniWindow = nil
local windowOpen = true
local lastPublishMs = 0
local lastScanMs = 0
local lastFilter = ''
local lastObservedFilter = ''
local lastFilterInputMs = 0
local FILTER_DEBOUNCE_MS = 300
local lastActiveView = 'gear'
local PUBLISH_INTERVAL_MS = 2000
local SCAN_INTERVAL_MS = 15000

-- Toggle-pill + startup settings, persisted across sessions.
local settings = { showToggle = true, startMinimized = true }
local settingsPath = string.format('%s/RMInventory/settings.lua', mq.configDir)
do
    local ok, loaded = pcall(function()
        local chunk = loadfile(settingsPath)
        return chunk and chunk() or nil
    end)
    if ok and type(loaded) == 'table' then
        if loaded.showToggle ~= nil then settings.showToggle = loaded.showToggle end
        if loaded.startMinimized ~= nil then settings.startMinimized = loaded.startMinimized end
    end
    windowOpen = not settings.startMinimized
end

local function saveSettings()
    pcall(function()
        local dir = string.format('%s/RMInventory', mq.configDir)
        -- Windows mkdir creates intermediates by default; no -p flag.
        os.execute(string.format('mkdir "%s"', dir))
        local file = io.open(settingsPath, 'w')
        if file then
            file:write(string.format(
                'return { showToggle = %s, startMinimized = %s }\n',
                tostring(settings.showToggle), tostring(settings.startMinimized)))
            file:close()
        end
    end)
end

local equippedBySlot = {}
local itemLinks = {}
local detailItems = {}
local detailItem = nil
local detailOwner = ''
local detailLocation = ''
local charLine = ''
local updatedLine = ''
local peerLine = '0 peers'
local dollRows = {}
local compareRows = {}
local resultRows = {}
local resultRefs = {}
local resultRecords = {}
local resultTotal = 0
local lastSourceFilter = 'all'
local RESULT_PAGE_SIZE = 16
local resultPage = 1
local resultPageCount = 1
local lastResultsBuildMs = 0
local RESULTS_REFRESH_MS = 15000
local lastResultSort = 'name'
local lastResultAscending = true
local lastIndexMs = 0
local INDEX_INTERVAL_MS = 15000

-- Trade request state. Single-item proxy_give is fire-and-forget at the
-- protocol level (no completion callback to the initiator), so track
-- outstanding requests locally with expiry; the giver's own failures are
-- invisible to us by design.
local tradeRefs = {}
local outstanding = {}
local tradeLine = ''
local OUTSTANDING_TTL_MS = 120000

local function setTradeLine(msg)
    tradeLine = msg or ''
end

local function pruneOutstanding(now)
    local kept = {}
    for _, entry in ipairs(outstanding) do
        if (now - (entry.at or 0)) < OUTSTANDING_TTL_MS then
            kept[#kept + 1] = entry
        end
    end
    outstanding = kept
end

local function requestTrade(key)
    local ref = tradeRefs[key]
    if not ref or not ref.item or not ref.peer or ref.peer == '' then return end
    local me = mq.TLO.Me.CleanName() or ''
    if ref.peer:lower() == me:lower() then return end
    local item = ref.item
    local args = {
        name = item.name,
        to = me,
        fromBank = item.bankslotid ~= nil,
        bagid = item.bagid,
        slotid = item.slotid,
        bankslotid = item.bankslotid,
    }
    local callOk, sent = false, false
    if actor and actor.send_inventory_command then
        callOk, sent = pcall(actor.send_inventory_command, ref.peer, 'proxy_give', { args })
    end
    if callOk and sent == true then
        outstanding[#outstanding + 1] = { name = item.name, peer = ref.peer, at = nowMs() }
        setTradeLine('')
    else
        setTradeLine(string.format('Trade request failed: %s from %s', item.name or '?', ref.peer))
    end
end
local selectedSlot = nil
local selectedSlotName = ''
local itemCount = 0

local function nowMs()
    local ok, t = pcall(function() return mq.gettime() end)
    if ok and tonumber(t) then return tonumber(t) end
    return math.floor((os.clock() or 0) * 1000)
end

local function matchesFilter(item, filter)
    if not filter or filter == '' then return true end
    if (item.name or ''):lower():find(filter, 1, true) then return true end
    for i = 1, 6 do
        local aug = item['aug' .. i .. 'Name']
        if aug and aug ~= '' and aug:lower():find(filter, 1, true) then
            return true
        end
    end
    return false
end

local function rescan()
    if not actor or not actor.gather_inventory then return false end
    local stateOk, gameState = pcall(function()
        return mq.TLO.MacroQuest.GameState()
    end)
    if stateOk and gameState and gameState ~= 'INGAME' then return false end

    -- This refresh only feeds the equipped paper doll. A full bag/bank scan
    -- here every 15 seconds duplicates the actor's throttled enriched sync and
    -- needlessly stalls the game while the Results view is being searched.
    local ok, data = pcall(actor.gather_inventory, {
        includeExtendedStats = false,
        onlyEquippedWithStats = true,
        scanStage = 'fast',
    })
    if not ok or type(data) ~= 'table' or type(data.equipped) ~= 'table'
        or type(data.name) ~= 'string' or data.name == '' then
        return false
    end

    -- Build a complete replacement off to the side. A failed/partial scan
    -- must not erase the last good local gear while peer snapshots keep
    -- updating independently.
    local nextEquipped = {}
    local nextItemLinks = {}
    local nextDetailItems = {}
    local nextTradeRefs = {}
    local nextItemCount = 0
    for _, item in ipairs(data.equipped or {}) do
        local slotid = tonumber(item.slotid)
        if slotid then
            nextEquipped[slotid] = item
            nextItemCount = nextItemCount + 1
        end
    end

    local me = mq.TLO.Me
    local nextCharLine = string.format('%s · %s %d',
        me.CleanName() or 'Unknown',
        me.Class.ShortName() or '???',
        me.Level() or 0)

    equippedBySlot = nextEquipped
    itemLinks = nextItemLinks
    detailItems = nextDetailItems
    tradeRefs = nextTradeRefs
    itemCount = nextItemCount
    charLine = nextCharLine
    updatedLine = 'Updated ' .. os.date('%H:%M:%S')
    return true
end

local function rebuild(filter)
    dollRows = {}

    -- Paper-doll grid mirroring the in-game equipment window layout.
    local layoutOk, layout = pcall(function()
        return slotsMod.getEquippedSlotLayout and slotsMod.getEquippedSlotLayout() or {}
    end)
    if layoutOk and type(layout) == 'table' then
        for _, lrow in ipairs(layout) do
            local row = { cells = {} }
            for _, slotid in ipairs(lrow) do
                if slotid == '' or slotid == nil then
                    -- Same full member set as live cells: RmlUi fixes the
                    -- array struct schema from the first row, and row 7 of
                    -- the doll layout starts with a spacer.
                    row.cells[#row.cells + 1] = {
                        spacer = true, key = '', slotid = -1, slot = '',
                        empty = true, dim = false, selected = false,
                        has_icon = false, icon_src = '',
                    }
                else
                    local item = equippedBySlot[slotid]
                    local match = item and matchesFilter(item, filter)
                    local icon = (item and tonumber(item.icon)) or 0
                    local key = item and string.format('%s_%d', item.name or 'unnamed', slotid) or ''
                    if item then itemLinks[key] = item.itemlink end
                    row.cells[#row.cells + 1] = {
                        spacer = false,
                        key = key,
                        slotid = slotid,
                        slot = getSlotName(slotid),
                        empty = item == nil or not match,
                        dim = item ~= nil and not match,
                        selected = slotid == selectedSlot,
                        has_icon = match and icon > 0,
                        icon_src = (match and icon > 0) and string.format('eq://item/%d', icon) or '',
                    }
                end
            end
            dollRows[#dollRows + 1] = row
        end
    end
end

local function doInspectLink(link)
    if not link or link == '' then return end
    pcall(function()
        local links = mq.ExtractLinks(link)
        if links and #links > 0 then mq.ExecuteTextLink(links[1]) end
    end)
end

-- Item detail panel (mirrors EZInventory's item inspector content).
local function dnum(v) return tonumber(v) or 0 end

local function daugSum(item, key)
    local s = 0
    for i = 1, 6 do s = s + dnum(item['aug' .. i .. key]) end
    return s
end

local function dstat(base, aug)
    local t = dnum(base) + dnum(aug)
    if t <= 0 then return nil end
    return tostring(t)
end

local function dheroic(base, aug)
    local t = dnum(base) + dnum(aug)
    if t <= 0 then return nil end
    if dnum(aug) > 0 then return string.format('+%d (+%d)', t, dnum(aug)) end
    return string.format('+%d', t)
end

local function dnonEmpty(v)
    if v == nil or v == '' then return '—' end
    return tostring(v)
end

local function buildDetail()
    if not detailItem then
        return { show = false }
    end
    local item = detailItem
    local flags = {}
    if dnum(item.nodrop) == 1 or item.nodrop == true then flags[#flags + 1] = 'NO DROP' end
    if dnum(item.tradeskills) == 1 or item.tradeskills == true then flags[#flags + 1] = 'TRADESKILL' end
    if dnum(item.tribute) > 0 then flags[#flags + 1] = 'TRIBUTE' end
    local classes = 'ALL'
    if not item.allClasses then
        if type(item.classes) == 'table' and #item.classes > 0 then
            classes = table.concat(item.classes, ' ')
        else
            classes = dnonEmpty(item.classes)
        end
    end
    local summary = {
        { k = 'Owner', v = dnonEmpty(detailOwner) },
        { k = 'Location', v = dnonEmpty(detailLocation) },
        { k = 'Type', v = dnonEmpty(item.itemtype or item.itemType or item.type) },
        { k = 'Classes', v = classes },
    }
    local qty = math.max(1, dnum(item.qty or item.quantity) > 0 and dnum(item.qty or item.quantity) or 1)
    summary[#summary + 1] = { k = 'Qty', v = tostring(qty) }
    if dnum(item.value) > 0 then
        summary[#summary + 1] = { k = 'Value', v = string.format('%d pp', math.floor(dnum(item.value) / 1000)) }
    end
    local function statRow(label, v) if v then return { k = label, v = v, h = '' } end return nil end
    local util = {}
    for _, r in ipairs({
        statRow('AC', dstat(item.ac, daugSum(item, 'AC'))),
        statRow('HP', dstat(item.hp, daugSum(item, 'HP'))),
        statRow('Mana', dstat(item.mana, daugSum(item, 'Mana'))),
        statRow('End', dstat(item.endurance, daugSum(item, 'Endurance'))),
    }) do if r then util[#util + 1] = r end end
    local attrs, resists = {}, {}
    local function hrow(list, label, base, abase, hbase, habase)
        local v = dstat(base, daugSum(item, abase))
        local h = dheroic(hbase, daugSum(item, habase))
        if v or h then list[#list + 1] = { k = label, v = v or '—', h = h or '' } end
    end
    hrow(attrs, 'STR', item.str, 'STR', item.heroicStr, 'HeroicStr')
    hrow(attrs, 'STA', item.sta, 'STA', item.heroicSta, 'HeroicSta')
    hrow(attrs, 'AGI', item.agi, 'AGI', item.heroicAgi, 'HeroicAgi')
    hrow(attrs, 'DEX', item.dex, 'DEX', item.heroicDex, 'HeroicDex')
    hrow(attrs, 'WIS', item.wis, 'WIS', item.heroicWis, 'HeroicWis')
    hrow(attrs, 'INT', item.int, 'INT', item.heroicInt, 'HeroicInt')
    hrow(attrs, 'CHA', item.cha, 'CHA', item.heroicCha, 'HeroicCha')
    hrow(resists, 'MR', item.svMagic, 'SvMagic', item.heroicSvMagic, 'HeroicSvMagic')
    hrow(resists, 'FR', item.svFire, 'SvFire', item.heroicSvFire, 'HeroicSvFire')
    hrow(resists, 'CR', item.svCold, 'SvCold', item.heroicSvCold, 'HeroicSvCold')
    hrow(resists, 'DR', item.svDisease, 'SvDisease', item.heroicSvDisease, 'HeroicSvDisease')
    hrow(resists, 'PR', item.svPoison, 'SvPoison', item.heroicSvPoison, 'HeroicSvPoison')
    hrow(resists, 'Corr', item.svCorruption, 'SvCorruption', item.heroicSvCorruption, 'HeroicSvCorruption')
    local augs = {}
    for i = 1, 6 do
        local visible = item['aug' .. i .. 'SlotVisible']
        local name = item['aug' .. i .. 'Name']
        if visible ~= nil or (name and name ~= '') then
            local stype = item['aug' .. i .. 'SlotType'] or item['aug' .. i .. 'Type'] or item['aug' .. i .. 'AugType']
            local label = string.format('Slot %d', i)
            if stype ~= nil and stype ~= '' then label = label .. ': Type ' .. tostring(stype) end
            augs[#augs + 1] = {
                idx = i,
                label = label,
                name = (name and name ~= '') and name or 'Empty',
                is_empty = not (name and name ~= ''),
                has_link = name and name ~= '' and item['aug' .. i .. 'link'] ~= nil
                    and item['aug' .. i .. 'link'] ~= '',
            }
        end
    end
    local icon = tonumber(item.icon) or 0
    return {
        show = true,
        icon_src = icon > 0 and string.format('eq://item/%d', icon) or '',
        has_icon = icon > 0,
        name = dnonEmpty(item.name),
        flags = #flags > 0 and table.concat(flags, '  ') or 'Tradable',
        summary = summary,
        has_stats = (#util + #attrs + #resists) > 0,
        util = util,
        attrs = attrs,
        resists = resists,
        has_augs = #augs > 0,
        augs = augs,
    }
end

local function setDetail(item, owner, location)
    detailItem = item
    detailOwner = owner or ''
    detailLocation = location or ''
    pcall(function()
        window:model('inv', { detail = buildDetail() })
    end)
end

local function statStr(item, field)
    if item and tonumber(item[field]) then return tostring(item[field]) end
    return '—'
end

local function makeCompareRow(peerName, item, slotid, isSelf)
    local icon = (item and tonumber(item.icon)) or 0
    local key = item and string.format('cmp_%s_%d', peerName, slotid) or ''
    if item then
        itemLinks[key] = item.itemlink
        detailItems[key] = { item = item, owner = peerName }
        tradeRefs[key] = { peer = peerName, item = item, loc = 'equipped' }
    end
    return {
        key = key,
        peer = peerName,
        is_self = isSelf == true,
        can_trade = not isSelf and item ~= nil,
        empty = item == nil,
        name = item and (item.name or 'Unknown') or '—',
        has_icon = icon > 0,
        icon_src = icon > 0 and string.format('eq://item/%d', icon) or '',
        ac = statStr(item, 'ac'),
        hp = statStr(item, 'hp'),
        mana = statStr(item, 'mana'),
    }
end

local function buildCompare(slotid)
    compareRows = {}
    selectedSlot = slotid
    selectedSlotName = getSlotName(slotid)
    compareRows[#compareRows + 1] = makeCompareRow('Me', equippedBySlot[slotid], slotid, true)
    for _, peer in ipairs(Net.peers()) do
        local found = nil
        for _, item in ipairs(peer.data.equipped or {}) do
            if tonumber(item.slotid) == slotid then found = item break end
        end
        compareRows[#compareRows + 1] = makeCompareRow(peer.name, found, slotid, false)
    end
end

local augRows = {}
local emptyRows = {}
local augRefs = {}
local AUG_CAP = 100
local EMPTY_CAP = 50
local lastAugDiagMs = 0
local AUG_DIAG_MS = 10000

---Shared search index (one throttled build feeds results + augs).
local sharedIndex = nil

local function ensureIndex(now)
    if not Index or not actor then return nil end
    if sharedIndex and (now - lastIndexMs) < INDEX_INTERVAL_MS then return sharedIndex end
    lastIndexMs = now
    local ok, built = pcall(Index.build, actor, nil, nil, os.time())
    if not ok or type(built) ~= 'table' then return nil end
    -- Normalize once per index build, not once per row for every keystroke or
    -- repeatedly inside the sort comparator.
    for _, record in ipairs(built.records or {}) do
        local name = tostring(record.name or '')
        record._rmlSearchText = (name .. ' ' .. tostring(record.augmentSearch or '')):lower()
        record._rmlSortName = name:lower()
        record._rmlSortOwner = tostring(record.character or ''):lower()
        record._rmlSortLocation = tostring(record.locationLabel or ''):lower()
    end
    sharedIndex = built
    return built
end

---Augment finder over the shared index. Section 1: augments whose name
---matches; section 2: host items with empty aug slots (host matches query,
---or everything when the query is empty).
local function buildAugs(filter, now)
    augRows = {}
    emptyRows = {}
    augRefs = {}
    local built = ensureIndex(now)
    if not built then return end
    local query = (filter or ''):lower()
    local augCount, emptyCount = 0, 0
    for _, record in ipairs(built.records or {}) do
        local item = record.item or {}
        -- Section 1: aug name matches.
        if query ~= '' then
            for _, aug in ipairs(record.augments or {}) do
                if aug.name and aug.name:lower():find(query, 1, true) then
                    augCount = augCount + 1
                    if #augRows < AUG_CAP then
                        local key = string.format('aug_%d', augCount)
                        augRefs[key] = { item = item, owner = record.character, location = record.locationLabel }
                        augRows[#augRows + 1] = {
                            key = key,
                            aug = aug.name,
                            host = record.name,
                            owner = record.character,
                            location = record.locationLabel,
                        }
                    end
                end
            end
        end
        -- Section 2: hosts with empty aug slots.
        if (record.emptyAugmentSlots or 0) > 0
            and (query == '' or record.name:lower():find(query, 1, true)) then
            emptyCount = emptyCount + 1
            if #emptyRows < EMPTY_CAP then
                local key = string.format('empty_%d', emptyCount)
                augRefs[key] = { item = item, owner = record.character, location = record.locationLabel }
                emptyRows[#emptyRows + 1] = {
                    key = key,
                    host = record.name,
                    owner = record.character,
                    location = record.locationLabel,
                    count = record.emptyAugmentSlots,
                }
            end
        end
    end
    -- Diagnostic: aug tab empty while query active. Distinguishes "no aug
    -- data in the index" (peer fast scans) from a matching bug.
    if query ~= '' and #augRows == 0 and (nowMs() - lastAugDiagMs) > AUG_DIAG_MS then
        lastAugDiagMs = nowMs()
        local withAugs, withSearchHit, total = 0, 0, 0
        for _, record in ipairs(built.records or {}) do
            total = total + 1
            if record.augments and #record.augments > 0 then withAugs = withAugs + 1 end
            if record.augmentSearch and record.augmentSearch:find(query, 1, true) then
                withSearchHit = withSearchHit + 1
            end
        end
        printf('\ay[RMInventory] aug diag: %d records, %d with augments, %d augmentSearch hits for %q',
            total, withAugs, withSearchHit, query)
    end
end

local function openCompare(slotid)
    if not slotid then return end
    buildCompare(slotid)
    -- Doll rebuild picks up the new selection highlight on next publish.
    rebuild(lastFilter or '')
    setDetail(equippedBySlot[slotid], 'Me', 'Equipped: ' .. selectedSlotName)
    pcall(function()
        window:model('inv', {
            doll_rows = dollRows,
            compare_title = selectedSlotName,
            compare_rows = compareRows,
        })
    end)
end

local function matchesInventorySearch(record, terms)
    if #terms == 0 then return true end
    local searchable = record._rmlSearchText or tostring(record.name or ''):lower()
    for i = 1, #terms do
        if not searchable:find(terms[i], 1, true) then return false end
    end
    return true
end

local function rebuildResults(filter, now, sourceFilter)
    resultRecords = {}
    resultTotal = 0
    local built = ensureIndex(now)
    if not built then return end
    sourceFilter = sourceFilter or 'all'
    local terms = {}
    for term in tostring(filter or ''):lower():gmatch('%S+') do
        terms[#terms + 1] = term
    end
    for _, record in ipairs(built.records or {}) do
        local inScope = sourceFilter == 'all'
            or (sourceFilter == 'equipped' and record.location == 'equipped')
            or (sourceFilter == 'inventory' and (record.location == 'inventory' or record.location == 'bags'))
            or (sourceFilter == 'bank' and (record.location == 'bank' or record.location == 'sharedBank'))
        if inScope and matchesInventorySearch(record, terms) then
            resultTotal = resultTotal + 1
            resultRecords[#resultRecords + 1] = record
        end
    end
end

local function renderResultPage(page)
    resultPageCount = math.max(1, math.ceil(resultTotal / RESULT_PAGE_SIZE))
    resultPage = math.max(1, math.min(resultPageCount, math.floor(tonumber(page) or 1)))
    resultRows = {}
    resultRefs = {}
    local first = (resultPage - 1) * RESULT_PAGE_SIZE + 1
    local last = math.min(resultTotal, first + RESULT_PAGE_SIZE - 1)
    local myName = (mq.TLO.Me.CleanName() or ''):lower()
    for index = first, last do
        local record = resultRecords[index]
        local item = record.item or {}
        local icon = tonumber(item.icon) or 0
        local key = string.format('res_%d', index)
        local isSelfEquip = record.location == 'equipped'
            and tostring(record.character or ''):lower() == myName
        resultRefs[key] = {
            item = item,
            owner = record.character,
            location = record.locationLabel,
            slotid = tonumber(item.slotid),
            isSelfEquip = isSelfEquip,
        }
        tradeRefs[key] = { peer = record.character, item = item, loc = record.location }
        resultRows[#resultRows + 1] = {
            key = key,
            name = record.name,
            owner = record.character,
            location = record.locationLabel,
            can_trade = tostring(record.character or ''):lower() ~= myName,
            quantity = tonumber(record.quantity) or 1,
            location_short = record.location == 'equipped' and 'EQUIPPED'
                or (record.location == 'bags' or record.location == 'inventory') and 'INVENTORY'
                or 'BANK',
            has_icon = icon > 0,
            icon_src = icon > 0 and string.format('eq://item/%d', icon) or '',
        }
    end
end

local function sortResultRecords(sortBy, ascending)
    table.sort(resultRecords, function(a, b)
        local av, bv
        if sortBy == 'owner' then
            av, bv = a._rmlSortOwner or '', b._rmlSortOwner or ''
        elseif sortBy == 'location' then
            av, bv = a._rmlSortLocation or '', b._rmlSortLocation or ''
        elseif sortBy == 'quantity' then
            av, bv = tonumber(a.quantity) or 1, tonumber(b.quantity) or 1
        else
            av, bv = a._rmlSortName or '', b._rmlSortName or ''
        end
        if av ~= bv then
            if ascending then return av < bv end
            return av > bv
        end
        local an, bn = a._rmlSortName or '', b._rmlSortName or ''
        if an ~= bn then return an < bn end
        local ao, bo = a._rmlSortOwner or '', b._rmlSortOwner or ''
        if ao ~= bo then return ao < bo end
        return (a._rmlSortLocation or '') < (b._rmlSortLocation or '')
    end)
end

local function publish(force)
    if not window or not window:is_valid() then return end
    local now = nowMs()
    local ok, inputFilter = pcall(function()
        return window:get_model('inv', 'filter_text')
    end)
    inputFilter = (ok and type(inputFilter) == 'string') and inputFilter:lower() or ''
    if inputFilter ~= lastObservedFilter then
        lastObservedFilter = inputFilter
        lastFilterInputMs = now
    end
    local filterPending = inputFilter ~= lastFilter
    local filterChanged = filterPending and (now - lastFilterInputMs) >= FILTER_DEBOUNCE_MS
    local filter = filterChanged and inputFilter or (filterPending and lastFilter or inputFilter)
    local okSource, sourceFilter = pcall(function()
        return window:get_model('inv', 'source_filter')
    end)
    sourceFilter = okSource and type(sourceFilter) == 'string' and sourceFilter or 'all'
    local sourceChanged = sourceFilter ~= lastSourceFilter
    local okView, activeView = pcall(function()
        return window:get_model('inv', 'active_view')
    end)
    activeView = okView and type(activeView) == 'string' and activeView or 'gear'
    if activeView ~= 'results' and activeView ~= 'augs' then activeView = 'gear' end
    local viewChanged = activeView ~= lastActiveView
    local okPage, requestedPage = pcall(function()
        return window:get_model('inv', 'result_page')
    end)
    requestedPage = okPage and math.max(1, math.floor(tonumber(requestedPage) or 1)) or resultPage
    local pageChanged = requestedPage ~= resultPage
    local okSort, resultSort = pcall(function()
        return window:get_model('inv', 'result_sort')
    end)
    resultSort = okSort and (resultSort == 'owner' or resultSort == 'location' or resultSort == 'quantity')
        and resultSort or 'name'
    local okAscending, sortAscending = pcall(function()
        return window:get_model('inv', 'sort_ascending')
    end)
    if not okAscending or type(sortAscending) ~= 'boolean' then sortAscending = lastResultAscending end
    local sortChanged = resultSort ~= lastResultSort or sortAscending ~= lastResultAscending
    local scanDue = (now - lastScanMs) >= SCAN_INTERVAL_MS
    if filterPending and not filterChanged and not force and not sourceChanged
        and not viewChanged and not pageChanged and not sortChanged and not scanDue then
        return
    end
    if not force and not filterChanged and not sourceChanged and not viewChanged and not pageChanged and not sortChanged and not scanDue
        and (now - lastPublishMs) < PUBLISH_INTERVAL_MS then
        return
    end
    local scanSucceeded = true
    if force or scanDue then
        scanSucceeded = rescan()
        if scanSucceeded then
            lastScanMs = now
        else
            -- Retry transient in-zone / incomplete snapshots quickly, but
            -- leave the last complete local equipment view intact meanwhile.
            lastScanMs = now - SCAN_INTERVAL_MS + 3000
        end
    end
    local inventoryPayloadChanged = false
    if force or filterChanged or sourceChanged or (scanDue and scanSucceeded) then
        lastFilter = filter
        lastSourceFilter = sourceFilter
        rebuild(filter)
        inventoryPayloadChanged = true
        peerLine = string.format('%d peers', #Net.peers())
        -- Keep the compare panel fresh as data arrives.
        if selectedSlot then
            buildCompare(selectedSlot)
        end
    end
    pruneOutstanding(now)
    local tradeText = tradeLine
    if #outstanding > 0 then
        local last = outstanding[#outstanding]
        tradeText = string.format('Requested %s from %s · %d pending',
            last.name or '?', last.peer or '?', #outstanding)
    end
    -- A search becomes its own page; clearing it leaves the current page alone.
    if filterChanged and filter ~= '' then activeView = 'results' end
    local resultsActive = activeView == 'results'
    local showAugs = activeView == 'augs'
    if showAugs then
        buildAugs(filter, now)
    end
    local resultPayloadChanged = false
    if resultsActive then
        local refreshResults = force or filterChanged or sourceChanged or viewChanged
            or (scanDue and scanSucceeded)
            or (now - lastResultsBuildMs) >= RESULTS_REFRESH_MS
        if refreshResults then
            rebuildResults(filter, now, sourceFilter)
            sortResultRecords(resultSort, sortAscending)
            if filterChanged or sourceChanged or viewChanged then requestedPage = 1 end
            renderResultPage(requestedPage)
            lastResultsBuildMs = now
            resultPayloadChanged = true
        elseif sortChanged then
            sortResultRecords(resultSort, sortAscending)
            renderResultPage(1)
            resultPayloadChanged = true
        elseif pageChanged then
            renderResultPage(requestedPage)
            resultPayloadChanged = true
        end
    end
    lastPublishMs = now
    local values = {
            char_line = charLine,
            updated_line = updatedLine,
            peer_line = peerLine,
            item_count = string.format('%d/23 equipped', itemCount),
            compare_title = selectedSlot and selectedSlotName or 'Select a slot to compare',
            active_view = activeView,
            aug_rows = augRows,
            empty_rows = emptyRows,
            trade_line = tradeText,
    }
    if inventoryPayloadChanged then
        values.doll_rows = dollRows
        values.compare_rows = compareRows
    end
    if resultPayloadChanged then
        values.result_page = resultPage
        values.result_page_count = resultPageCount
        values.result_page_label = string.format('Page %d / %d', resultPage, resultPageCount)
        values.result_sort = resultSort
        values.sort_ascending = sortAscending
        values.sort_direction_label = sortAscending and 'Ascending ↑' or 'Descending ↓'
        values.result_has_prev = resultPage > 1
        values.result_has_next = resultPage < resultPageCount
        values.results_title = string.format('%d result%s', resultTotal, resultTotal == 1 and '' or 's')
        values.result_count = string.format('%d items', resultTotal)
        values.result_empty = resultTotal == 0
        values.result_rows = resultRows
        lastResultSort = resultSort
        lastResultAscending = sortAscending
    end
    pcall(function() window:model('inv', values) end)
    lastActiveView = activeView
end

local function selectedSlotId()
    local ok, sel = pcall(function()
        return window:get_model('inv', 'selected_slot')
    end)
    if not ok then return nil end
    return tonumber(sel)
end

local function bindEvents()
    window:on('click', 'sort-direction', function()
        local ok, ascending = pcall(function() return window:get_model('inv', 'sort_ascending') end)
        if ok then pcall(function() window:model('inv', { sort_ascending = not (ascending == true) }) end) end
        lastPublishMs = 0
    end)
    window:on('click', 'inv-search-clear', function()
        pcall(function() window:model('inv', { filter_text = '' }) end)
        lastObservedFilter = ''
        lastFilterInputMs = nowMs() - FILTER_DEBOUNCE_MS
        lastPublishMs = 0
        publish(false)
    end)
    window:on('click', 'result-page-prev', function()
        local ok, page = pcall(function() return window:get_model('inv', 'result_page') end)
        if ok then pcall(function() window:model('inv', { result_page = math.max(1, (tonumber(page) or 1) - 1) }) end) end
    end)
    window:on('click', 'result-page-next', function()
        local ok, page = pcall(function() return window:get_model('inv', 'result_page') end)
        if ok then pcall(function() window:model('inv', { result_page = math.min(resultPageCount, (tonumber(page) or 1) + 1) }) end) end
    end)
    for _, id in ipairs({ 'tab-gear', 'tab-results', 'tab-augs' }) do
        window:on('click', id, function() lastPublishMs = 0 end)
    end
    for _, id in ipairs({ 'scope-all', 'scope-equipped', 'scope-inventory', 'scope-bank' }) do
        window:on('click', id, function()
            pcall(function() window:model('inv', { active_view = 'results' }) end)
            lastPublishMs = 0
        end)
    end
    window:on('click', 'inv-doll', function()
        local slotid = selectedSlotId()
        if slotid then openCompare(slotid) end
    end)
    window:on('click', 'inv-compare', function(event)
        if event.target_id == 'trade-btn' then
            local ok, key = pcall(function()
                return window:get_model('inv', 'selected_trade')
            end)
            if ok then requestTrade(key) end
            return
        end
        local ok, key = pcall(function()
            return window:get_model('inv', 'selected_item')
        end)
        if not ok or not key or key == '' then return end
        local entry = detailItems[key]
        if entry then
            setDetail(entry.item, entry.owner, 'Equipped')
        end
    end)
    window:on('click', 'inv-results', function(event)
        if event.target_id == 'result-page-prev' or event.target_id == 'result-page-next'
            or event.target_id == 'sort-name' or event.target_id == 'sort-owner'
            or event.target_id == 'sort-location' or event.target_id == 'sort-quantity'
            or event.target_id == 'sort-direction' then return end
        if event.target_id == 'trade-btn' then
            local ok, key = pcall(function()
                return window:get_model('inv', 'selected_trade')
            end)
            if ok then requestTrade(key) end
            return
        end
        local ok, key = pcall(function()
            return window:get_model('inv', 'selected_item')
        end)
        if not ok or not key or key == '' then return end
        local ref = resultRefs[key]
        if not ref then return end
        if ref.isSelfEquip and ref.slotid then
            openCompare(ref.slotid)
        else
            setDetail(ref.item, ref.owner, ref.location)
        end
    end)
    window:on('click', 'results-eqlink', function()
        if detailItem then doInspectLink(detailItem.itemlink) end
    end)
    window:on('click', 'results-detail-augs', function()
        local ok, idx = pcall(function() return window:get_model('inv', 'selected_aug') end)
        idx = ok and tonumber(idx) or nil
        if idx and detailItem then doInspectLink(detailItem['aug' .. idx .. 'link']) end
    end)
    window:on('click', 'inv-augs', function(event)
        local ok, key = pcall(function()
            return window:get_model('inv', 'selected_item')
        end)
        if not ok or not key or key == '' then return end
        local ref = augRefs[key]
        if ref then
            setDetail(ref.item, ref.owner, ref.location)
        end
    end)
    window:on('click', 'detail-eqlink', function()
        if detailItem then
            doInspectLink(detailItem.itemlink)
        end
    end)
    window:on('click', 'detail-augs', function()
        local ok, idx = pcall(function()
            return window:get_model('inv', 'selected_aug')
        end)
        if not ok then return end
        idx = tonumber(idx)
        if not idx or not detailItem then return end
        doInspectLink(detailItem['aug' .. idx .. 'link'])
    end)
    window:on('click', 'inv-refresh', function()
        lastScanMs = 0
        publish(true)
    end)
    window:on('click', 'close-window', function() Shell.Close() end)
end

local function openWindow()
    if not rml then
        printf('\ar[RMInventory] mq.rmlui unavailable')
        return false
    end
    if window and window:is_valid() then return true end
    window = rml.open({
        document = 'lua://rminventory/ui/shell.rml',
        input = 'interactive', visible = windowOpen, layer = 10,
        id = 'rminventory', persist = true, geometry_element = 'window',
        models = { 'inv' },
        initial = { inv = {
            char_line = '', updated_line = '', peer_line = '', item_count = '',
            filter_text = '', selected_item = '', selected_slot = -1, selected_aug = -1,
            source_filter = 'all',
            selected_trade = '', trade_line = '',
            active_view = 'gear',
            aug_rows = {}, empty_rows = {},
            compare_title = 'Select a slot to compare', compare_rows = {},
            results_title = '', result_rows = {},
            result_count = '0 items', result_empty = false,
            result_page = 1, result_page_count = 1, result_page_label = 'Page 1 / 1',
            result_has_prev = false, result_has_next = false,
            result_sort = 'name', sort_ascending = true, sort_direction_label = 'Ascending ↑',
            doll_rows = {}, detail = { show = false },
        }},
    })
    if not window then
        printf('\ar[RMInventory] Failed to open RML document')
        return false
    end
    window:set_text('window-title', 'RMInventory')
    window:set_style('window', 'right', 'auto')
    window:set_style('window', 'width', '1180px')
    bindEvents()
    lastScanMs = 0
    publish(true)
    return true
end

local function openMiniWindow()
    if not rml then return false end
    if miniWindow and miniWindow:is_valid() then return true end
    miniWindow = rml.open({
        document = 'lua://rminventory/ui/mini.rml',
        input = 'interactive', visible = true, layer = 10,
        id = 'rminventory_mini', persist = true, geometry_element = 'mini',
        models = { 'mini' },
        initial = { mini = { open = windowOpen } },
    })
    if not miniWindow then return false end
    miniWindow:set_style('mini', 'right', 'auto')
    miniWindow:set_style('mini', 'bottom', 'auto')
    miniWindow:on('click', 'mini', function() Shell.Toggle() end)
    return true
end

function Shell.Pulse()
    -- Keep peer sync and deferred enriched scans alive even while the UI is
    -- hidden/minimized; visibility should only control rendering.
    Net.update()
    if settings.showToggle then
        if openMiniWindow() and miniWindow:is_valid() then
            pcall(function()
                miniWindow:model('mini', { open = windowOpen })
            end)
        end
    elseif miniWindow and miniWindow:is_valid() then
        pcall(function() miniWindow:hide() end)
    end
    if not windowOpen then return end
    if not openWindow() then return end
    publish(false)
end

---Print the raw actor snapshots beside the records the shared index produced.
---This is intentionally on-demand so the per-character report does not spam
---the chat log during normal updates.
function Shell.Diagnostics()
    if not actor or not Index then
        printf('\ar[RMInventory] diagnostics unavailable (actor or index missing)')
        return false
    end

    local now = nowMs()
    local ok, built = pcall(Index.build, actor, nil, nil, os.time())
    if not ok or type(built) ~= 'table' then
        printf('\ar[RMInventory] diagnostics failed to build inventory index')
        return false
    end
    local sources, seen = {}, {}
    local function addSource(data)
        if type(data) ~= 'table' then return end
        local name = tostring(data.name or 'Unknown')
        local server = tostring(data.server or 'Unknown')
        local key = server .. '|' .. name
        if seen[key] then return end
        seen[key] = true

        local equipped = #(data.equipped or {})
        local inventory = #(data.inventory or {})
        local bank = #(data.bank or {})
        local bags = 0
        for _, bagItems in pairs(data.bags or {}) do
            bags = bags + #(bagItems or {})
        end
        sources[#sources + 1] = {
            key = key, name = name, server = server,
            stage = tostring(data.config and data.config.scanStage or 'unknown'),
            equipped = equipped, inventory = inventory, bags = bags, bank = bank,
            raw = equipped + inventory + bags + bank,
        }
    end

    if actor.get_cached_inventory then
        local cacheOk, selfData = pcall(actor.get_cached_inventory, true)
        if cacheOk then addSource(selfData) end
    end
    for _, data in pairs(actor.peer_inventories or {}) do addSource(data) end

    local indexedByKey, indexTotal = {}, 0
    for _, record in ipairs(built.records or {}) do
        local name = tostring(record.character or 'Unknown')
        local key = tostring(record.server or 'Unknown') .. '|' .. name
        indexedByKey[key] = (indexedByKey[key] or 0) + 1
        indexTotal = indexTotal + 1
    end
    table.sort(sources, function(a, b)
        if a.name:lower() ~= b.name:lower() then return a.name:lower() < b.name:lower() end
        return a.server:lower() < b.server:lower()
    end)

    local rawTotal = 0
    for _, source in ipairs(sources) do rawTotal = rawTotal + source.raw end
    printf('[RMInventory] diag: peers=%d snapshots=%d raw=%d indexed=%d',
        #Net.peers(), #sources, rawTotal, indexTotal)
    for _, source in ipairs(sources) do
        printf('[RMInventory] diag: %s/%s stage=%s raw=%d (eq=%d inv=%d bags=%d bank=%d) indexed=%d',
            source.name, source.server, source.stage, source.raw,
            source.equipped, source.inventory, source.bags, source.bank,
            indexedByKey[source.key] or 0)
    end
    return true
end

function Shell.StartNetwork()
    return Net.start()
end

function Shell.Toggle()
    if windowOpen then Shell.Close() else Shell.Open() end
end

function Shell.IsOpen() return windowOpen end

function Shell.Open()
    windowOpen = true
    if openWindow() then window:show() end
end

function Shell.Close()
    windowOpen = false
    if window then window:hide() end
end

function Shell.SetShowToggle(value)
    settings.showToggle = value == true
    saveSettings()
    if not settings.showToggle and miniWindow then miniWindow:hide() end
end

function Shell.SetStartMinimized(value)
    settings.startMinimized = value == true
    saveSettings()
end

function Shell.GetSettings()
    return { showToggle = settings.showToggle, startMinimized = settings.startMinimized }
end

function Shell.Shutdown()
    if window then window:close() window = nil end
    if miniWindow then miniWindow:close() miniWindow = nil end
end

return Shell
