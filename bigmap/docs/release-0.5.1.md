Fixes awful performance on 32 GB machines after a big save loads.

**This release overwrites existing `plugins/tpf2_bigmap.cfg`, including user edits, on installation, upgrade and repair.** Close the game before installing. It shares the proxy and plugin host with TpF2 Multiplayer 0.6.1; installing either package updates both.

The terrain and material pagers kept a flat 12 GiB of RAM in reserve, a figure measured on a 94 GiB machine with no page file. On a 32 GiB machine a loaded big save leaves less than that free, so the pagers pinned their resident sets at the minimum (about 1 GiB of terrain tiles), evicted everything else, and the engine faulted the evicted tiles straight back in through a decode each, thousands per second: the game ran, but everything stuttered. The same flat figure also declared the commit charge "tight" for the whole session on a machine with a system-managed page file, which throttled the pagers on top.

- **Headroom sized to the machine.** The reserve is a seventh of installed RAM, clamped to 2 to 12 GiB: 12 GiB on 94 GiB as before, 4.6 GiB on 32 GiB, 2.3 GiB on 16 GiB. The commit-tight threshold follows the same idea (an eighth, 2 to 10 GiB).
- **The pager counts what it holds as its own room.** The resident ceiling used to be computed from free RAM alone, so a pager that had filled memory during the load saw little free RAM, shrank to its floor, and then could not grow back. It now budgets from free RAM plus its own resident set, keeping a quarter of that (or the headroom) free.
- **Memory pressure shrinks it.** When free RAM drops under the headroom the target shrinks by an eighth per second and eviction turns urgent, whatever the decode feedback says. Paging the engine out is worse than any decode.

On a 32 GiB machine with a 207,360-tile save the terrain pager now settles around 9 to 10 GiB resident with 4 to 5 GiB free instead of 1 GiB resident and a decode storm. The new `pressure=` and `free=` fields on the `resident target` log lines show the state; `cold restores/s` lines at 300 or more mean the working set still does not fit.

Everything else is 0.5.0.
