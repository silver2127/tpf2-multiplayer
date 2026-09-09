function data()
	return {
		info = {
			minorVersion = 0,
			severityAdd = "NONE",
			severityRemove = "NONE",
			name = "M3 determinism probe",
			description = [[
Recon tool: hashes observable sim state every in-game day and prints it
(via print()) for cross-run determinism comparison. Not a gameplay mod.
]],
			tags = { "Script Mod" },
			authors = {
				{
					name = "recon",
					role = "CREATOR",
				},
			},
			visible = true,
		},
		runFn = function(settings)
		end,
	}
end
