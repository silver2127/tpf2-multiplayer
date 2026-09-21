# The Steam Workshop signpost

`tpf2_multiplayer_link_1` is the Workshop item for **Transport Fever 2 Multiplayer**.
It is a signpost, not the mod: the mod needs native DLLs and the lobby program
beside the game, which the Workshop cannot deliver, so the item carries the name
players search for, the description and the link to the installer. Its `runFn`
does nothing, on purpose: subscribing must never break a game or a session, and a
save that lists it loads everywhere.

Published 2026-09-20 as Workshop item **3805379261**
(https://steamcommunity.com/sharedfiles/filedetails/?id=3805379261); the staging
folder's `workshop_fileid.txt` holds the same id, which is how the uploader knows to
update rather than create. The preview is the game's logo with MULTIPLAYER under it,
1280x720 (since 2026-09-21).

The shipped mod (`mod/mp_lockstep_1`) stays out of the Workshop: a Workshop copy
would sit beside the installer's copy under another folder name and both would
hook the game.

`WORKSHOP_DESCRIPTION.bbcode` is the item's page description in Steam's BBCode
(headers, rules, lists): paste it into the item's description box on the Workshop
site after publishing, and keep its version line current. Steam refuses a description over
8,000 characters ("There was a problem trying to save the title and description");
keep the file under about 7,800. The in-game uploader only
sends `mod.lua`'s plain-text description, which is the fallback shown until then.

## Publishing (the game's own uploader, from the publisher's Steam account)

1. Copy `tpf2_multiplayer_link_1` (mod.lua, image_00.tga and workshop_preview.jpg -- the uploader refuses a folder without the jpg) into the account's
   **staging area**: `<Steam>\userdata\<account>\1066780\local\staging_area\`.
   The uploader lists only that folder (a copy under `local\mods` shows in the
   mod list as `!tpf2_multiplayer_link` but cannot be published; 2026-09-20).
2. Start the game. Open the Mod Browser from the title menu, find the mod under the
   local mods and press **Publish** (the game calls it "Mod publishing"; the
   button says Publish, then Start upload). Accept the Workshop terms once.
3. On Steam, the new item's page appears under the account's Workshop items: set the
   visibility to Public, and paste the description again there if the game shipped
   only the first lines (Workshop descriptions accept BBCode; the mod.lua text is
   plain and reads fine as it is).
4. Note the item id from its URL and put it in this README.

Updating the text later: edit `mod.lua`, copy it over the staging-area folder again
and publish again from the same account; the game updates the existing item.

Item id: not yet published.
