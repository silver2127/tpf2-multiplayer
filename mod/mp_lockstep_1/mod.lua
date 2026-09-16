function data()
    if os.getenv("TPF2MP_RELEASE_ROOT") then
        require("mp/update_bootstrap").setup()
        local path = os.getenv("TPF2MP_RELEASE_ROOT") .. "/mod/res/scripts/mp/mod_data.lua"
        return assert(loadfile(path, "t", _ENV))()()
    end
	return {
		info = {
			minorVersion = 0,
			severityAdd = "NONE",
			severityRemove = "NONE",
			name = "Transport Fever 2 Multiplayer",
			description = [[
Multiplayer for Transport Fever 2. Replicates player COMMANDS, not
world state: each command is applied at the same game time on every machine in
the session. Works with the TpF2 Multiplayer DLLs installed by its MSI
(github.com/silver2127/tpf2-multiplayer). When no other player is connected,
nothing is cancelled or replayed.
]],
			tags = { "Script Mod" },
			authors = { { name = "recon", role = "CREATOR" } },
			visible = true,
		},
		runFn = function(settings)
			-- Experimental, deliberately limited to the script verified by our
			-- compatibility tests. Resource modifiers leave Workshop files intact.
			addModifier("loadGameScript", function(fileName, script)
				local name = fileName:gsub("\\", "/")
				if name ~= "natural_town_growth.lua" and not name:match("/natural_town_growth%.lua$") then
					return script
				end
				return require("mp/deterministic_script").wrap(script, "natural_town_growth", {
					naturalTownGrowth = true,
				})
			end)
		end,
	}
end
