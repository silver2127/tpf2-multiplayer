# Persistent person-target set order

The second first-terminal trace confirmed a distinct input-order difference
before resident-set iteration. For affected buildings `[20183,20835,20842]`,
GetSimPersonsForTarget returned `[],[20839,20840],[]` on native Linux and
`[],[20840,20839],[]` on Windows. Consequently the first insertions into Info
map4 were `[20839,20840,20858]` versus `[20840,20839,20858]`. Correcting the final
temporary map's traversal cannot repair a different first-insertion sequence.
Raw pointers, list nodes, counts and first-insertion events are preserved in
`post-release-desync/first-terminal-insertion-trace` in the local desync cache.

Native GetSimPersonsForTarget170cba0 finds a target in the outer std map at
SimPersonSystem+1d8 and returns its inner std::unordered_set. Windows a92930
uses the analogous map at System+200. Native std hashing uses the integer value;
Windows uses FNV-1a over four little-endian bytes. The standard libraries also
have different bucket-growth and global list-order rules. Therefore changing a
hash or sorting the currently visible IDs cannot reproduce general Windows
order: insertion, deletion and rehash history is required.

The implementation keeps private Windows ordering metadata and retains all
original native lookup/storage operations. It patches six verified sites:

| Site | Purpose |
| --- | --- |
| 170c4fd,7bytes | Bind an empty target-map owner after constructor initialization |
| 16ef930,13bytes | Forget that exact owner before its original outer-map cleanup |
| 170d520,9bytes | Observe AddToTarget only after the original writer succeeds |
| 170cf70,5bytes | Observe RemoveFromTarget after original node/entry retirement |
| 2e69deb,11bytes | Supply original native nodes in Windows order at MakePersonCapacityData's target-set head |
| 2e6a3ec,6bytes | Advance those nodes in the same order, replaying original TEST/branch flags |

Both target writers have only two direct callers: EntityAdded/ComponentChanged
and their corresponding removal callbacks. Target is SimPerson+50. Native
AddToTarget asserts if insertion reports a duplicate; removal asserts unless
one ID was erased. Thus a successful insertion must increase the captured
count by one. Removal destroys an empty target set and its outer entry; the
post-call observer touches only private metadata and does not read freed game
nodes.

Lifetime coverage includes initialization, ordinary destruction and failed
construction. Normal destructor170bf00 clears System+1d8 through16ef930 at
170bf9f. Constructor cold cleanup7dd4dc does the same at7dd584. The shared clear
hook checks exact registered owner addresses, leaving unrelated maps unchanged.
An owner is never inferred from a timeout or thread identity. New loads start
with empty owners and build history through the original EntityAdded callbacks.
Address reuse requires cleanup and new constructor binding. The complete
constructor, cold cleanup, destructor, writer callbacks, lookup and reader
functions are pinned along with ELF build ID
3a0e156390b0e6f1e372051c24802c8493ae454a.

The other direct GetSimPersonsForTarget users are the emptiness assertion in
NotePersonCapacityToBeRemoved170cc70 and the Lua binding1aa14b0. Their native
behavior remains intact. Other unordered input sources exist independently:
MakeLineData2e69050 consumes GetSimPersonsForLine170cad0 (System+138), and
transport-network gathering uses170e0e0 (System+90). This module does not alter
those containers or claim to fix their ordering.

The shared windows_entity_set_order_linux helper models original Windows
428d60 insertion,954a40 erasure and42fb20 rehash. New buckets append to the
iteration list; a new key prepends within its existing bucket; rehash traverses
the prior iteration list and regroups with those same rules. Initial capacity
is8 buckets, growing8x below512 and2x thereafter. Erasing the final key executes
the original clear path and returns to8 buckets. Tokens identifying native
nodes are non-owned; no game node is relinked or freed by the model.

The compiled helper matches original Windows code for72 streams/94,060
operations, comparing each result, complete order and bucket count, including
4096 IDs, random/signed inputs, duplicate insertion, erasure, empty reset and
reinsertion. Artifacts and executable oracle sources are in
`post-release-desync/target-order-audit`. Checked-in Windows fixtures contain
2459 operations and30 complete order/bucket checkpoints.

The normal native fixture executes the installed writer/erase/clear entry
hooks and all three inline stubs. It checks48 full GP/XMM/flags/MXCSR/stack
cases across both stack alignments, original native node lifetimes, destroyed
empty outer entries, nesting, thread transfer, three concurrent owners,
address reuse, incomplete-capture fallback, one log per runtime error and six
partial-write rollback points. Root's separate ordering foreign-unwind fixture
covers an original writer exception followed by original cleanup. No RAII
object or held metadata mutex surrounds an original engine call.

Before reordered traversal, every live native node, ID and count must match
captured history. Missed initialization/mutations, allocation failure, incorrect
lifetime reuse or unmatched nonempty sets produce explicit status and once-per-
error runtime log messages, retaining native traversal. Such a fallback does
not establish multiplayer parity and must be investigated. All byte guards run
before any installation; failures restore all attempted windows and report an
incomplete restoration explicitly. Install only during loader initialization;
this change does not retrofit history into a running world.
