-- Transport Fever 2 Multiplayer: company colour classes for the in-game
-- Multiplayer window (config/game_script/lockstep.lua, the companies row swatches).
--
-- !mpCo1 .. !mpCo200 carry the company chip colour: the 20 distinct colours
-- (Trubetskoy) then a golden-angle hue walk, the same as the lobby chips
-- (native/src/menu_hook.cpp coColor), the icon/window tints (slice_hook.cpp
-- IconCompanyColor) and the vehicle paint (companies.lua CM.cmCompanyColor).
-- Keep all FOUR in step (tools/palette_sync_test.py checks it). Text and background get the colour, so a swatch reads as a solid
-- block where the background is drawn and as coloured "##" where it is not.
local ssu = require "stylesheetutil"

local FIRST = { { 230, 25, 75 }, { 0, 130, 200 }, { 60, 180, 75 }, { 245, 130, 48 }, { 145, 30, 180 }, { 70, 240, 240 }, { 240, 50, 230 }, { 255, 225, 25 }, { 0, 128, 128 }, { 170, 110, 40 }, { 210, 245, 60 }, { 128, 0, 0 }, { 0, 0, 128 }, { 128, 128, 0 }, { 250, 190, 212 }, { 220, 190, 255 }, { 170, 255, 195 }, { 255, 215, 180 }, { 128, 128, 128 }, { 255, 250, 200 } }

local function companyColor(cid)
	local c = FIRST[cid]
	if c then return c[1] / 255, c[2] / 255, c[3] / 255 end
	local h = ((cid - 21) * 137.508) % 360
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
		-- !mpWinCoN: a TRANSLUCENT wash of the company colour for a whole window
		-- root (the slice tags a foreign entity's window with this, ICON COLOUR /
		-- FOREIGN WINDOWS). A low alpha so the company is obvious at a glance while
		-- the window's own content stays readable; no text-colour override.
		a("!mpWinCo" .. cid, {
			backgroundColor = { r, g, b, 0.30 },
		})
		-- WHERE THE COLOUR ACTUALLY PAINTS (2026-09-16). The slice tags a ROOT --
		-- the HUD item button, the entity window -- with !mpWinCoN; that root is
		-- a bare container and draws nothing itself (measured: its class list was
		-- empty before ours). The game's own sheets paint the parts, by name:
		--   * hud.lua: StationItem::StationIcon / VehicleDepotItem::Icon are the
		--     box-and-glyph image, MODULATED by backgroundColor1 (white; hover =
		--     grey). Setting it to the company colour tints the icon.
		--   * window.lua: Window::Title-bar is left transparent; a backgroundColor
		--     there colours the title bar without hiding the content.
		-- The game combines an ancestor's !class with descendant names itself
		-- ("!ui-couch BuildControlComp::CostsLabel"), so these are the same grammar.
		a("!mpWinCo" .. cid .. " StationItem::StationIcon, !mpWinCo" .. cid .. " VehicleDepotItem::Icon", {
			backgroundColor1 = { r, g, b, 1.0 },
		})
		a("!mpWinCo" .. cid .. " StationItem::StationIcon!hover, !mpWinCo" .. cid .. " VehicleDepotItem::Icon!hover", {
			backgroundColor1 = { r * 0.8, g * 0.8, b * 0.8, 1.0 },
		})
		a("Window!mpWinCo" .. cid .. " Window::Title-bar", {
			backgroundColor = { r, g, b, 0.85 },
		})
		-- The class on the icon ELEMENT (the slice appends it right after the game's
		-- own carrier class, StationItem::StationIcon!train and friends): the form the
		-- game itself uses, so it resolves when the icon is styled.
		a("StationItem::StationIcon!mpWinCo" .. cid .. ", VehicleDepotItem::Icon!mpWinCo" .. cid, {
			backgroundColor1 = { r, g, b, 1.0 },
		})
		a("StationItem::StationIcon!mpWinCo" .. cid .. "!hover, VehicleDepotItem::Icon!mpWinCo" .. cid .. "!hover", {
			backgroundColor1 = { r * 0.8, g * 0.8, b * 0.8, 1.0 },
		})
	end
	return result
end
