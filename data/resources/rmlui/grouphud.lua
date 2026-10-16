--[[
	Group HUD example: dynamic data models.

	Demonstrates the M6.2 behaviour end to end:

	  * an array of records rendered through data-for,
	  * a model declared before the document parses (no reload on first use),
	  * empty-to-populated arrays appearing without a document rebuild,
	  * two scripts using the same model name without observing each other.

	Load with: /luamq run grouphud
]]

local mq = require 'mq'
local rmlui = require 'mq.rmlui'

local RML = 'mq://rmlui/group.rml'

---Builds the model payload for the current group state.
---@return table
local function BuildGroup()
	local members = {}
	for index = 1, 8 do
		members[index] = {
			name = string.format('Member%d', index),
			hp = 40 + index * 7,
			role = index % 2 == 0 and 'healer' or 'tank',
			debuff = {
				{ name = 'Snare', seconds = 12 },
			},
		}
	end

	return {
		name = 'Test Group',
		max = 8,
		-- RmlUi data expressions have no method-call syntax, so anything derived
		-- is supplied by the script rather than computed in the document.
		member_count = #members,
		members = members,
	}
end

local state = {
	members = 0,
	phase = 0,
}

-- The model is declared on open, so the document binds to it as it parses and
-- the first update below does not need a reload.
local doc = rmlui.open({
	document = RML,
	input = 'clickthrough',
	layer = 10,
	models = { 'group' },
	initial = {
		group = BuildGroup(),
	},
})

if not doc then
	mq.echo('grouphud: failed to open ' .. RML)
	return
end

mq.create_timer('grouphud', 2000, function()
	state.phase = state.phase + 1

	-- Cycle between empty, partially populated, and full so the empty-to-
	-- populated path is exercised in both directions.
	local counts = { 0, 3, 8, 5, 0 }
	local target = counts[state.phase % #counts + 1]

	local group = BuildGroup()
	if target < #group.members then
		for index = #group.members, target + 1, -1 do
			table.remove(group.members, index)
		end
	end
	group.member_count = target

	if not doc:model('group', group) then
		mq.echo('grouphud: model update was rejected (check the RmlUi quotas)')
	end

	state.members = target
	mq.echo(string.format('grouphud: %d members', target))
end)

mq.echo('grouphud: loaded, models declared before parse')
