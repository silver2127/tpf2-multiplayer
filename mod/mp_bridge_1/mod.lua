function data()
	return {
		info = {
			minorVersion = 0,
			severityAdd = "NONE",
			severityRemove = "NONE",
			name = "MP Bridge",
			description = [[
Multiplayer bridge: replicates constructions (with params) and vehicles
between game instances via the tpf2 bridge DLL. Works in any instance —
identity is auto-detected from tpf2_instance.txt (written by the DLL).
]],
			tags = { "Script Mod" },
			authors = { { name = "recon", role = "CREATOR" } },
			visible = true,
		},
		runFn = function(settings)
		end,
	}
end
