# Fixed multiple string.byte results

The live-peer ordinary survey at `d0ee5eb8c` showed native x86_64 roots for
fixed two/three-byte returns but no ARM64 roots. The blocker was the ARM64
fast-function preflight's single-result CALL requirement. The existing
semantic and post-allocation validators already prove each constant-offset
STRREF/XLOAD independently. This change accepts fixed positive result counts
only when their complete result window fits the caller frame. It changes no
emitter, x86_64 behavior, lock or trace snapshot rule.

The string-byte contract passes all four assertion/release ARM64/ARM64e
profiles. New C fixtures test 2/4/8/16 results starting at byte 1 and 2, plus
three results, with distinct weighted bytes including NUL and values above
127. They verify selected native root entry/exits, exact values, short/type
error exits, full GC and unchanged machine code. Every recorded byte pointer
is privately mutated out of bounds, and every byte load loses its read-only
flag; both independent admission passes reject each mutation. A raw U8
snapshot mutation remains rejected. The existing byte tests also pass.

The initial fixture used a FORL body whose invalid first iteration naturally
failed before native loop entry, so its cold-entry assertion was invalid.
A while loop with its increment after the byte reads remained closed because
an overflow snapshot would retain raw U8 values. The final while fixture
increments before the byte reads, so both valid and cold calls reach the
selected native root while retaining the existing snapshot restrictions.
Initial failed logs are retained; production was not weakened to admit them.

Evidence directory:
`/Users/frityet/Scratch/ljmt-expanded-parity-20261030/`.
The final four-profile log is `fixed-multiple-loophead-four-profiles.log`.
`fixed-multiple-ordinary.lua` tests 2/3/4/8/16 results from bytes 1 and 2,
against an interpreted oracle, with short and wrong-type cases. Ordinary
ARM64 and frozen x86_64 at `d0ee5eb8c` each observe eight selected root exits
before GC and eight after GC for every shape, with matching results/errors.
See `fixed-multiple-{arm64,x86_64}.log`. Ordinary ARM64 also passes all 509
stock source cases and all 509 original v2-bytecode cases, recorded in
`fixed-multiple-stock-{source,bytecode}.log`.

Dynamic-end multiple results, open-result CALLs and raw U8 snapshot values
remain separate work. This focused change is not covered by the in-flight
full gate frozen at `d0ee5eb8c` and does not establish full x86_64 parity.
