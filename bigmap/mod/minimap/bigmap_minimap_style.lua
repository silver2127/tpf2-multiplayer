-- tpf2_bigmap minimap: written into res/config/style_sheet by the Big Maps
-- plugin (tpf2_bigmap.dll) at game start, beside the minimap's game script, and
-- removed with it. The first line is the marker the plugin looks for before it
-- touches this file.
--
-- The minimap's toolbar button in the game's round disk, as game-menu.lua draws
-- LineManagerButton and VehicleManagerButton: a script cannot make its button one
-- of those types, so the class carries the same images and colours, and clears
-- ToggleButton's square hover/active fill.
local ssu = require "stylesheetutil"

function data()
	local result = { }
	local a = ssu.makeAdder(result)

	a("ToggleButton!bigmapToolbarDisk", {
		backgroundImage1 = { fileName = "ui/design/buttons/disk_big_behind.tga" },
		backgroundImage2 = { fileName = "ui/design/buttons/disk_big_surface.tga" },
		borderImage = { fileName = "ui/design/buttons/disk_big_contour.tga" },
		backgroundColor = ssu.makeColor(0, 0, 0, 0),
		backgroundColor1 = ssu.makeColor(15, 35, 50, 90),
		backgroundColor2 = ssu.makeColor(15, 35, 50),
		borderColor = ssu.makeColor(255, 255, 255, 128),
		-- the 34 px icon inside the 60 px disk
		padding = { 13, 13, 13, 13 },
	})
	a("ToggleButton!bigmapToolbarDisk:hover", {
		backgroundColor = ssu.makeColor(0, 0, 0, 0),
		backgroundColor1 = ssu.makeColor(183, 188, 193, 128),
		borderColor = ssu.makeColor(255, 255, 255),
	})
	a("ToggleButton!bigmapToolbarDisk:active", {
		backgroundColor = ssu.makeColor(0, 0, 0, 0),
		backgroundColor1 = ssu.makeColor(15, 35, 50, 90),
		backgroundColor2 = ssu.makeColor(110, 122, 132),
	})
	return result
end
