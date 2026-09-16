# TownSystem time seed compatibility

The combined people compatibility candidate matched all 718 person records
through simulation time 600 and all sampled moving-person transforms at 720.
The first remaining town-growth mismatch was at 756: native added a building
(581 buildings, 718 edges), while Windows added a street (580 buildings, 719
edges). Both were still identical at 744. The extra Windows street was
`-3073.0,-3462.3,44.2>-3112.2,-3541.0,42.3`; all 718 original geometry strings
remained identical. This is a distinct simulation decision, not hash formatting.

Native build 35924, GNU build ID
`3a0e156390b0e6f1e372051c24802c8493ae454a`, has an inline MT initializer in
TownSystem function `0x1746790`. It calls GetTime at `0x174681d`, combines tag
21 with that signed 32-bit time using native integer hashes, and stores the
result at `0x1746854`. This MT is passed as the fifth argument to
TownDeveloper::Develop at `0x1747917`. The separate person MT constructor hooks
do not intercept this inline initializer.

The Windows counterpart is TownSystem `0xab1d20`, which calls GetTime at
`0xab1dab`, initializes its MT at `0xab1e10`, and passes the same local MT to
TownDeveloper at `0xab253e`. Its seed uses FNV-1a over each integer's four
little-endian bytes, followed by the classic 64-bit hash-combine operation:

```
K = 0x9e3779b9
combine(s, h) = s XOR (h + K + (s << 6) + (s >> 2))  [mod 2^64]
windowsSeed = low32(combine(combine(0, FNV32_LE(21)), FNV32_LE(time)))
nativeSeed = (timeBits + 0x53a3cbac) XOR 0x9e3779ce  [mod 2^32]
```

An independent oracle executed the original Windows seed instructions for
100,017 signed/boundary/random time patterns and matched
`Tpf2mpWindowsTimeSeed(21, timeBits)`. The folded Windows constants are
`0x2ceadadbf054a7e9` and `0x45f16db3af769df3`.

The hook verifies the complete 339-byte native entry/seed/initializer prefix
before changing the single six-byte `MOV [RBP-0xa00],EDX` instruction. Its
dispatcher recovers the original time bits from the native seed without
reading the clock again, replaces only RDX's low 32 bits, and replays the
original store through a near trampoline. The inline 624-word MT initializer
continues unchanged. All other GP bits, flags, XMM registers and stack state
are preserved; no shared Lua or RNG distribution code is changed by this hook.

`town_seed_test.cpp` exercises the actual installed jump and original seed
store at both possible stack alignments, with every GP/XMM register live. It
also executes the original native instructions from GetTime's return through
the complete inline MT initializer for 1,035 boundary/random times, compares
all 624 state words and the index, and checks 646,875 outputs against the
standard MT sequence seeded with the Windows value. Wrong builds and changed
entry, GetTime, hash, store and initializer bytes are refused before patching.

These checks establish seed and hook compatibility. A clean live replay past
the first yearly town-growth event is required to establish that this seed
correction resolves the observed growth divergence.
