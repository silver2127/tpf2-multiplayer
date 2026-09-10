-- Transport Fever 2 Multiplayer: company colour classes for the in-game
-- Multiplayer window (config/game_script/lockstep.lua, the companies row swatches).
--
-- !mpCo1 .. !mpCo200 carry the company chip colour: the same six fixed colours
-- and golden-angle hue walk as the lobby chips (native/src/menu_hook.cpp coColor)
-- and the vehicle paint (scripts/mp/companies.lua CM.cmCompanyColor). Keep the
-- three in step. Text and background get the colour, so a swatch reads as a solid
-- block where the background is drawn and as coloured "##" where it is not.
local ssu = require "stylesheetutil"

local FIRST = { { 220, 80, 80 }, { 80, 140, 230 }, { 90, 190, 110 }, { 230, 180, 60 }, { 180, 100, 220 }, { 80, 200, 200 } }

local function companyColor(cid)
	local c = FIRST[cid]
	if c then return c[1] / 255, c[2] / 255, c[3] / 255 end
	local h = ((cid - 7) * 137.508) % 360
	local sat, val = 0.62, 0.85
	local C = val * sat
	local X = C * (1 - math.abs((h / 60) % 2 - 1))
	local m = val - C
	local r, g, b
	if h < 60 then r, g, b = C, X, 0 elseif h < 120 then r, g, b = X, C, 0 elseif h < 180 then r, g, b = 0, C, X
	elseif h < 240 then r, g, b = 0, X, C elseif h < 300 then r, g, b = X, 0, C else r, g, b = C, 0, X end
	return r + m, g + m, b + m
end

function data()
	local result = { }
	local a = ssu.makeAdder(result)
	for cid = 1, 200 do
		local r, g, b = companyColor(cid)
		a("!mpCo" .. cid, {
			backgroundColor = { r, g, b, 1.0 },
			color = { r, g, b, 1.0 },
		})
	end
	return result
end
