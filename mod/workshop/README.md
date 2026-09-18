# The Steam Workshop signpost

`tpf2_multiplayer_link_1` is the Workshop item for **Transport Fever 2 Multiplayer**.
It is a signpost, not the mod: the mod needs native DLLs and the lobby program
beside the game, which the Workshop cannot deliver, so the item carries the name
players search for, the description and the link to the installer. Its `runFn`
does nothing, on purpose: subscribing must never break a game or a session, and a
save that lists it loads everywhere.

The shipped mod (`mod/mp_lockstep_1`) stays out of the Workshop: a Workshop copy
would sit beside the installer's copy under another folder name and both would
hook the game.

## Publishing (the game's own uploader, from the publisher's Steam account)

1. Copy `tpf2_multiplayer_link_1` (mod.lua and image_00.tga) into the account's
   local mods folder: `<Steam>\userdata\<account>\1066780\local\mods\`.
2. Start the game. Open the Mod Browser from the title menu, find the mod under the
   local mods and press **Publish** (the game calls it "Mod publishing"; the
   button says Publish, then Start upload). Accept the Workshop terms once.
3. On Steam, the new item's page appears under the account's Workshop items: set the
   visibility to Public, and paste the description again there if the game shipped
   only the first lines (Workshop descriptions accept BBCode; the mod.lua text is
   plain and reads fine as it is).
4. Note the item id from its URL and put it in this README.

Updating the text later: edit `mod.lua`, copy it over the staged folder again and
publish again from the same account; the game updates the existing item.

Item id: not yet published.
