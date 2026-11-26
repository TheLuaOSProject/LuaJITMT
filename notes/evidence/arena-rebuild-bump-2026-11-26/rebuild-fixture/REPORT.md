# Rebuild around a private bump interval

The final candidate passes all ten deterministic cases. The frozen baseline fails eight regression cases with ordinary exit 1 and passes both original-tail controls. All 20 final full-library invocations have empty stderr. No performance matrix or speedup claim is included.

The fixture is `tests/t-arena-rebuild-bump.c`, SHA256 `d2b3bd4b50720b84093141be3f928160b1065c49ff031ff743d54e3a71f27ba8`. It uses real allocator operations throughout: no private cursor edits, synthetic links, manufactured free-run bodies, or forced sweep state. Allocation history independently classifies every usable cell as live, private bump storage, or expected free capacity. Every published bin cell must belong to the last category exactly once. Missing capacity is reported as a regression result; overlapping/duplicate bins or changed live storage are hard assertions.

Both PLAIN and TRAVERSABLE run each layout:

| Layout | Frozen baseline | Final candidate |
| --- | --- | --- |
| Interior 512 B remainder, later guarded 1 KiB free block | 0/64 free cells; exact suffix missed; arenas 1 to 2 | 64/64 cells; exact suffix reused; arenas 1 to 1 |
| Same interior cursor exhausted before rebuild | 0/64 cells; exact suffix missed; arenas 1 to 2 | 64/64 cells; exact suffix reused; arenas 1 to 1 |
| One scanner callback crossing both sides of the private interval, plus separate earlier/later runs | 96/224 cells; only earlier 1 KiB run reused; arenas 1 to 2 | 224/224 cells; both complements and all separate runs reused; arenas 1 to 1 |
| Original arena-tail interval, followed by exhaustion and exact bin reuse | PASS | PASS |
| Real allocator transfer into an existing interior destination bump | 3479/3543 cells; destination suffix missed; arenas 2 to 3 | 3543/3543 cells; destination suffix, transferred tail and source block reused; arenas 2 to 2 |

Direct rebuild cases run three successive rebuilds and verify the complete `(arena, cell, end)` cursor, every byte inside the private interval, and its block/mark, READY, late, cdata, lifetime, root and recovery state. Live payload bytes and start metadata are also checked. Interior cases place root MEMBER and recovery PENDING ownership on separate live guards; rebuild and subsequent allocations must preserve them. Those test-owned side lanes are relinquished before terminal cleanup.

The crossing case verifies that the real free-run scanner supplies one interval with a start before `bump.cell` and an end after `bump.end`. A freed 512 B prefix, zero-state private 512 B remainder and adjacent freed 1 KiB block form that interval. A guarded earlier free run and another disjoint later free run check complete reconstruction beyond the intersecting callback. Reuse consumes the exact private half, all three original 1 KiB addresses once, and the exact free prefix, without new arenas.

The transfer case exercises the production trigger `lj_arena_alloc_transfer(dst, src)`. Source is a distinct quiescent allocator with one live guard, one freed 1 KiB block and its real original bump tail. The test verifies that transfer clears source cursor/list/bin ownership, preserves the source-tail boundary and all live source contents, leaves the destination interior cursor/private storage unchanged, and reconstructs every outside free cell. Subsequent real allocations consume the destination private half and exact suffix, the complete transferred tail, and exact source free block without a third arena.

Baseline runtime files were verified against commit `4423c15f24688cbeafcc498e2cb9370b21cea950`: all 213 recorded `src/` inputs match the commit byte for byte. The immutable GCC tree is `/tmp/lj-plain-bin-capacity-20261126-wyqryfj8/gcc`; its library SHA256 is `2a29b71710786af7ee2f8a06eb402e1923ea72e31fba6d4d7ee390d05a7b0b28`. Baseline fixture executable SHA256 is `fe13ce74f14480a5d536643aa583c4d2308fa0760189fcafd8d42ea1e52a7e32`.

The final candidate GCC tree is `/tmp/lj-arena-rebuild-bump-20261126-1d94ydej/gcc`. Arena source SHA256 is `471b215c6e54082b6c27c17bb3fb155e4baffc6de819842bbc578e979048dfbf`; library SHA256 is `c880ce0ddee055eb92d71a31753ab13d883e95aea8f79d5ff3ba3805192029df`; fixture executable SHA256 is `246405c872b2d7349b1ddf80a95ce2267c34e15bee70d592c43d0b390f28c14c`.

Each final executable was compiled with GCC `-O2 -g -Wall -Wextra -Werror -UNDEBUG`, frozen variant headers, the corresponding complete static library, and identical link flags. Compile commands/output, dependency source snapshots and executable hashes are preserved. Each case runs independently on CPU 8 with a 30-second external timeout, 60-second internal alarm, disabled core dumps and a 1 GiB address-space ceiling. Per-case records include exact command, executable identity, UTC start/end, elapsed and CPU time, and child peak RSS. These durations demonstrate bounded diagnostics, not comparative performance.

The final standalone candidate smoke also passes all ten cases. The initial smoke failure is preserved separately in `initial-tail-assumption/`: an earlier fixture draft incorrectly demanded a null arena pointer after exact bin consumption, although an exhausted cursor may retain its arena pointer. The test was corrected to accept an absent or empty private interval. No runtime change was made for that fixture assertion; its source, binaries and failure remain separate from the final regression results.

`baseline-results.json` and `candidate-results.json` index the 20 final process records; each has raw stdout/stderr and a command/hash/timing JSON record. `verification.json` records final checks of 2,473 declared build inputs, four build outputs, the committed baseline runtime and expected exits. Complete build manifests, relevant runtime source snapshots, the unchanged final fixture and the initial correction record are included. Root owns the independent configuration/sanitizer matrix and suite integration.
