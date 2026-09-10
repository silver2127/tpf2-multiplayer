function data()
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
		end,
	}
end
