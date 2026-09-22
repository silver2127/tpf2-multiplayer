-- The Steam Workshop signpost for TpF2 Multiplayer. The mod itself cannot ship
-- through the Workshop: it needs native DLLs and a lobby program beside the
-- game, installed by the MSI. This item carries the name players search for,
-- the description and the link. It does nothing when enabled, on purpose:
-- subscribing must never break a game or a session.
function data()
	return {
		info = {
			minorVersion = 1,
			severityAdd = "NONE",
			severityRemove = "NONE",
			name = "Transport Fever 2 Multiplayer",
			description = [[
Real multiplayer for Transport Fever 2: several players build roads, railways, stations, vehicles and lines in ONE shared world at the same time. Free and open source (MIT).

THIS WORKSHOP ITEM IS A SIGNPOST. The mod needs native components that the Workshop cannot deliver, so subscribing here installs nothing that plays. Get the installer from the website:

https://silver2127.github.io/tpf2-multiplayer/

HOW TO PLAY
1. Every player runs the installer (Windows, Steam version of the game; Linux and Steam Deck through Proton with the Proton installer from the same page).
2. Start the game and open MULTIPLAYER on the title menu.
3. One player hosts and shares the code (or lists the game publicly); the others join with the code or from the server list.
4. The host's save goes to everyone automatically, mods included. Play.
There is also a public dedicated test server in the server list, running all day.

WHAT YOU GET
- Everyone builds in the same world at once; every road, station, vehicle and line appears for all players.
- One shared company, or one company per player with station permissions.
- Hot join: a player can join a running game; the session holds while they load.
- Works through home routers: hole punching, and a relay when the punch cannot work.
- Nothing phones home: the only network use is the public game list, the join rendezvous and your session.

REQUIREMENTS
- Transport Fever 2 build 35924 (Steam). Same mod version on every player's PC; a new version is installed the same way, with the installer from the website.
- One open UDP port helps the host, but is not required.

Source, releases and issue tracker: https://github.com/silver2127/tpf2-multiplayer
Unofficial fan project, not affiliated with Urban Games.
]],
			tags = { "Script Mod" },
			authors = { { name = "silver2127", role = "CREATOR" } },
			visible = true,
		},
		runFn = function(settings)
		end,
	}
end
