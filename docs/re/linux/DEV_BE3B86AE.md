# dev be3b86ae: loading-player company gate

This change needs no ELF hook, game address, struct offset or ABI adaptation.
The Windows diff extends menu_hook.cpp writePlayerNames with a file export
from g_players/g_stages under g_modelCs; its CreateFileW/WriteFile calls have
a native counterpart in lobby_linux.cpp WritePlayerNames/WriteFileAtomic.
No reverse engineering of game code is required for this file protocol.

Linux ApplyRoster already parses the same JSON players/stages fields into
Model::players/stages and calls WritePlayerNames on every roster event,
including stage-only changes. OriginLetterFor provides the same identity as
the company/bridge files, including relay-assigned multi-letter origins.
The new export snapshots both files under S().mtx, releases the lock before
I/O, and writes mp_loading.txt as letter=name=stage plus newline for each
nonempty stage. OneLine uses the existing Linux control-character sanitation.
The requesting player's row is included; the shared Lua reader ignores its
own origin. Empty output replaces the previous file when everyone is ready
or the loading players depart. POSIX write/close/rename publishes complete
contents atomically, and failures are logged independently of mp_players.txt.

The existing lobby_ready test exercises real roster parsing and file output
for receiving, world loading, catching up, own rows, relay identities,
stage-only completion, departure and missing stages. The upstream Lua test
exercises the reader, refusal before scheduling, password exception and
resumption after loading. Existing engine patch sites remain unchanged.
