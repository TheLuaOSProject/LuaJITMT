# Dynamic-end multiple string.byte results

The ordinary x86_64 survey had a native root for two byte results with a
parameter end, while ARM64 still refused after fixed multiple results were
opened. The bounds helper previously required every loaded offset to equal
the range's starting offset. It also missed the recorder's folded subtraction
when that starting offset was zero.

Both admission passes now independently recognize either a dominating exact
`end == positive_constant` guard, or `end-base == positive_width` after a
checked subtraction with nonnegative base. A constant byte offset must be
inside that exact interval: `base <= offset < base+width`. The comparison
uses wide arithmetic. The existing caller separately proves this end is
within the same immutable string, by an unsigned length guard or the exact
negative-end normalization. No emitter or snapshot rule changes.

The complete string-byte contract passes all four assertion/release
ARM64/ARM64e profiles. The C fixtures cover 2/4/8/16 results starting at bytes
1 and 2 with positive/negative INT and fractional NUM ends, in addition to
all fixed-result cases. They check weighted NUL/high bytes, selected native
entries and cold exits, full GC and unchanged machine code. Each byte's
bounds and read-only capability are independently corrupted and rejected.
The width equality is also corrupted. A separate private KINT first denotes
the valid last byte (accepted by both passes), then exactly one byte beyond
it (rejected by both), avoiding aliasing the original base/width constant.
Raw U8 snapshot values remain refused.

Evidence is under `/Users/frityet/Scratch/ljmt-expanded-parity-20261030/`.
The final focused log is `dynamic-multiple-normalized-four-profiles.log`;
ordinary restoration profile is
`b0a9da4d9e1ba182bf16024b16bc154275d939e010f76863be1ca9a369ca4dbf`.
The first fixture run stopped because its guard finder recognized only UGE;
the NUM lowering used the equivalent ULE order. That retained failure log is
`dynamic-multiple-four-profiles.log`; only the fixture finder changed.

`dynamic-multiple-ordinary.lua` compares 40 live-peer shapes with their
interpreted oracles: 2/3/4/8/16 results, starts 1/2, four end kinds.
Ordinary ARM64 and frozen x86_64 at `d0ee5eb8c` each observe eight selected
root exits before GC and eight after GC for each shape, with identical
results/errors. See `dynamic-multiple-{arm64,x86_64}.log`.
Ordinary ARM64 additionally passes all 509 stock source and 509 original
v2-bytecode cases (`dynamic-multiple-stock-{source,bytecode}.log`).

This change is separate from the full gate frozen at `d0ee5eb8c`. General
dynamic-start multiple results, open-result calls and raw byte snapshots are
still outside this slice; full x86_64 parity remains incomplete.
