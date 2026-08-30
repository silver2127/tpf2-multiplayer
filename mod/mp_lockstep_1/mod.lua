function data()
	return {
		info = {
			minorVersion = 0,
			severityAdd = "NONE",
			severityRemove = "NONE",
			name = "MP Lockstep",
			description = [[
Lockstep multiplayer prototype. Does NOT replicate world state: it replicates
COMMANDS, each stamped with the game time at which every peer must execute it,
and relies on the simulation being deterministic (see docs/M3_RESULTS.md).

Run this INSTEAD of MP Bridge, never alongside it -- MP Bridge replicates state
and would fight this for control of the same world.
]],
			tags = { "Script Mod" },
			authors = { { name = "recon", role = "CREATOR" } },
			visible = true,
		},
		runFn = function(settings)
		end,
	}
end
