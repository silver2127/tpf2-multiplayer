# Native entity-only ownership

Target: Linux build 35924, GNU build ID
`3a0e156390b0e6f1e372051c24802c8493ae454a`.

The existing Linux `setPlayer` patch removed the unsupported-entity assertion,
but did not implement Windows' `0x60000000 | player` entity-only dispatch or
advertise `entity_owner_v1=1`. Shared Lua consequently refused company commands.
The live native/Windows company-creation check exposed this gap.

## Engine path

`0x1dc4ed0` is the Lua binding. Both nil-player and numeric-player conversion
converge at `0x1dc4f8f`, with the player in ESI, entity in `[rbp-0x1b0]`, and
engine in RBX. The overwritten 14 bytes are the Construction typeinfo LEA
(`0x5a03398`) and `lea r15,[rbp-0x100]`.

For ordinary values, the relay relocates these two instructions and resumes at
`0x1dc4f9d`, preserving the original Construction/Line/AssetGroup behavior.
For the explicit marker, it strips the high nibble, stores the player at
`[rbp-0x1ac]`, constructs the engine mutation scope at `[rbp-0x1a8]` through
`0x325ce90`, and sets its common-exit pointer `[rbp-0x1e8]`. It initializes R13
to the entity address and jumps to the generic setter path at `0x1dc51e2`.
The original call to `0x1dc4860` and common scope destructor at `0x1dc5245`
complete the operation. This avoids the recursive construction/line ownership
walk, preserving shared infrastructure owners.

The relay is allocated within rel32 reach, populated before becoming executable,
and installed only on the expected bytes and game build. The bridge advertises
the capability only after both the assertion fix and this extension succeed.
The slice's foreign-patch registry includes the new site.

## Checks

- `setplayer_entity` executes the actual generated machine code in a synthetic
  engine frame. It covers ordinary IDs, nil, other marker nibbles, boundary
  player IDs, entity pointer, engine pointer, scope construction and cleanup
  pointer, and relocated ordinary typeinfo/temporary addresses.
- `bridge_epoch` verifies that identity omits the capability before installation
  and publishes it afterward.
- All 52 soldier CTests passed. Live startup publishes the capability and runs
  under the native dedicated watchdog. The integration record tracks gameplay.
