-- Transport Fever 2 Multiplayer: company colour classes for the in-game
-- Multiplayer window (config/game_script/lockstep.lua, the companies row swatches)
-- and the per-owner tints (native/src/slice_hook.cpp: icons, station labels, windows).
--
-- !mpCo1 .. !mpCo200 carry the company chip colour: the 20 distinct colours
-- (Trubetskoy) then a golden-angle hue walk, the same as the lobby chips
-- (native/src/menu_hook.cpp coColor), the icon/window tints (slice_hook.cpp
-- IconCompanyColor) and the vehicle paint (companies.lua CM.cmCompanyColor).
-- Keep all FOUR in step (tools/palette_sync_test.py checks it).
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

-- THE HUD ICON GLYPH, RECOLOURED WITHOUT TOUCHING THE BLUE BOX (2026-09-16).
-- Each station/depot icon is ONE image (~65% blue box, ~15% white glyph), so a
-- backgroundColor modulate darkens the whole box -- not wanted. Instead the box
-- stays as the game draws it (backgroundImage1, untouched), and a company-coloured
-- copy of ONLY the glyph is overlaid as a SECOND image layer: backgroundImage2 =
-- a white-on-transparent glyph mask (res/textures/ui/hud/mp_glyph_*, generated
-- from the game's own icons), modulated by backgroundColor2 = the company colour.
-- The mask (image2) is keyed by the carrier class the game already sets; the
-- colour (color2) by the company class the slice appends. A carrier with no
-- company class overlays the glyph in white (invisible over the existing white
-- glyph), so an untinted icon is unchanged.
--   station carrier class -> mask (class "train-cargo" -> file train_cargo)
local STATION_CARRIERS = { "train", "train_cargo", "bus", "tram_and_bus", "tram",
                           "truck", "aircraft", "aircraft_cargo", "ship", "ship_cargo" }
--   depot carrier class -> mask (class "rail" -> depot_train)
local DEPOT_CARRIERS = { road = "depot_road", rail = "depot_train", tram = "depot_tram",
                         air = "depot_air", water = "depot_water" }

local function classFor(carrier) return (carrier:gsub("_", "-")) end   -- the game's class uses '-'

function data()
	local result = { }
	local a = ssu.makeAdder(result)

	-- Native in-game dashboard: scoped classes leave every stock window alone.
	-- The engine supplies transparency, local background blur and the window shadow.
	a("Window!mpDashWindow", { backgroundColor = ssu.makeColor(5, 25, 40, 175) })
	a("!mpDashBody", { padding = { 0, 14, 10, 14 }, minSize = { 520, -1 } })
	a("!mpDashBody > BoxLayout", { innerSpacing = { 0, 6 } })
	a("!mpDashBody TextView", { fontSize = 13 })
	a("!mpDashBody Button", { padding = { 4, 8, 4, 8 }, backgroundColor = ssu.makeColor(0, 0, 0, 0) })
	a("!mpDashBody Button:hover", { backgroundColor = ssu.makeColor(255, 255, 255, 35) })
	a("!mpDashBody Button:active", { backgroundColor = ssu.makeColor(255, 255, 255, 65) })
	a("!mpDashTabs Button!mpDashTab", {
		padding = { 4, 8, 4, 8 }, borderWidth = { 0, 0, 2, 0 }, borderColor = ssu.makeColor(255, 255, 255, 0)
	})
	a("!mpDashTabs Button!mpDashSelected", { borderColor = ssu.makeColor(255, 255, 255, 210) })
	a("!mpDashSection", { padding = { 10, 0, 8, 0 }, minSize = { -1, 120 } })
	a("!mpDashSection BoxLayout", { innerSpacing = { 8, 6 } })
	a("!mpDashBody TextView!mpDashChatLog", { padding = { 10, 12, 10, 12 }, minSize = { 492, 144 }, backgroundColor = ssu.makeColor(0, 0, 0, 50) })
	a("!mpDashBody Button!mpDashPrimary", { backgroundColor = ssu.makeColor(45, 90, 120, 180) })
	a("!mpDashTable", { backgroundColor = ssu.makeColor(0, 0, 0, 40) })
	a("!mpDashTable TextView", { padding = { 5, 10, 5, 10 } })
	a("!mpDashFooter", { padding = { 8, 0, 0, 0 }, borderWidth = { 1, 0, 0, 0 }, borderColor = ssu.makeColor(255, 255, 255, 22) })
	a("!mpDashFooter TextView", { fontSize = 12, color = ssu.makeColor(190, 205, 218) })
	a("!mpDashBody TextView!mpDashAlert", { padding = { 8, 10, 8, 10 }, backgroundColor = ssu.makeColor(130, 75, 10, 130) })

	a("!mpDashBody TextView", { padding = { 3, 4, 3, 4 } })
	a("!mpDashTabs TextView", { padding = { 4, 8, 4, 8 }, fontSize = 13 })
	a("!mpDashShell > BoxLayout", { innerSpacing = { 0, 0 } })
	a("!mpDashCompact", { padding = { 2, 6, 5, 6 }, minSize = { 220, -1 } })
	a("!mpDashCompact Button", { padding = { 3, 8, 3, 8 }, backgroundColor = ssu.makeColor(255, 255, 255, 12) })
	a("!mpDashCompact Button:hover", { backgroundColor = ssu.makeColor(255, 255, 255, 35) })
	a("!mpDashCompact TextView", { fontSize = 12, padding = { 3, 5, 3, 5 } })

	-- 1. the glyph overlay per carrier (image2 + a white default so untinted = unchanged)
	for _, c in ipairs(STATION_CARRIERS) do
		a("StationItem::StationIcon!" .. classFor(c), {
			backgroundImage2 = { fileName = "ui/hud/mp_glyph_" .. c .. ".tga" },
			backgroundColor2 = { 1, 1, 1, 1 },
		})
	end
	for cls, mask in pairs(DEPOT_CARRIERS) do
		a("VehicleDepotItem::Icon!" .. cls, {
			backgroundImage2 = { fileName = "ui/hud/mp_glyph_" .. mask .. ".tga" },
			backgroundColor2 = { 1, 1, 1, 1 },
		})
	end

	for cid = 1, 200 do
		local r, g, b = companyColor(cid)
		a("!mpCo" .. cid, {
			backgroundColor = { r, g, b, 1.0 },
			color = { r, g, b, 1.0 },
		})
		-- a translucent wash for a foreign entity WINDOW only (Window-scoped, so it
		-- does NOT paint the HUD icon button root, which the slice also tags with
		-- !mpWinCoN -- that produced a tinted box larger than the icon, 2026-09-16).
		a("Window!mpWinCo" .. cid, {
			backgroundColor = { r, g, b, 0.30 },
		})
		-- 2. the glyph colour (color2) when the icon carries this company's class.
		-- Later than the carrier rules above, so it wins the color2 tie; the box
		-- (image1) is never touched, so the blue border stays and the box height
		-- no longer shows as a coloured block.
		a("StationItem::StationIcon!mpWinCo" .. cid .. ", VehicleDepotItem::Icon!mpWinCo" .. cid, {
			backgroundColor2 = { r, g, b, 1.0 },
		})
		-- a foreign entity's window: the title bar (the game leaves it transparent),
		-- coloured without hiding the content.
		a("Window!mpWinCo" .. cid .. " Window::Title-bar", {
			backgroundColor = { r, g, b, 0.85 },
		})
	end
	return result
end
