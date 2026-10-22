-- Summoner's Rift: the stacking item auras say what their stacks add up to.
-- Their Spell.dbc text gives one stack's worth ("... by 1 ... for each stack",
-- import/rift/scripts/rift_league_spells.py) and the 3.3.5 aura tooltip cannot
-- multiply it, so the line is rewritten from the aura's own stack count:
--   94160  the Bloodthirster: 1 attack damage and 0.2% life steal a stack
--   94129  Rod of Ages: 20 health, 20 mana and 2 ability power a stack

local function Decimal(value)
	local text = string.format("%.1f", value);
	return (text:gsub("%.0$", ""));
end

local STACKED = {
	[94160] = function(n)
		return string.format("Attack damage increased by %d and life steal by %s%%.", n, Decimal(n * 0.2));
	end,
	[94129] = function(n)
		return string.format("Health and mana increased by %d and ability power by %d.", n * 20, n * 2);
	end,
};

local function Rewrite(tooltip, count, spellId)
	local text = spellId and STACKED[spellId];
	if ( not text ) then
		return;
	end
	local name = tooltip:GetName();
	for i = 2, tooltip:NumLines() do
		local line = _G[name.."TextLeft"..i];
		local current = line and line:GetText();
		if ( current and current:find("for each stack", 1, true) ) then
			line:SetText(text(math.max(count or 1, 1)));
			tooltip:Show();
			return;
		end
	end
end

-- The player's buff frame asks with SetUnitAura, the target frame with SetUnitBuff.
hooksecurefunc(GameTooltip, "SetUnitAura", function(self, unit, index, filter)
	local _, _, _, count, _, _, _, _, _, _, spellId = UnitAura(unit, index, filter);
	Rewrite(self, count, spellId);
end);

hooksecurefunc(GameTooltip, "SetUnitBuff", function(self, unit, index, filter)
	local _, _, _, count, _, _, _, _, _, _, spellId = UnitBuff(unit, index, filter);
	Rewrite(self, count, spellId);
end);
