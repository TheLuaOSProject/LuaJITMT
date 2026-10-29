# ARM64 open-result CALLM leaf vectors

This checkpoint admits one bounded open-result call edge in a loop-root trace:
an exact `CALL B=0` producer immediately followed by `CALLM B=2,C=0`. The
producer destination must equal the consumer's first open argument slot, and
the complete producer result and consumer argument windows must fit the source
prototype frame. The shared result-vector capacity is three. CALLM result
counts, open/dynamic `CALLM`, `CALLM` with another fixed prefix, tail calls,
and non-leaf producer bodies remain closed.

The producer result can be certified in either of two ways. If recording made
an exact child-frame exit snapshot, the existing child verifier binds the
producer's terminal return vector and continuation. A pure producer may have
no child exit snapshot. For that case each independent admission walk finds
the producer's own pre-CALL function-identity snapshot by its exact source
footer and function slot, validates the source argument map, then replays the
guarded Lua leaf through its source return using the semantic or allocated
evaluator. Every replayed result must appear, in order, in the current
CALLM guard snapshot at the producer's exact caller-frame slots. No extra
guard or poll is inserted to manufacture this evidence.

The native C fixture exercises fixed one-, two- and three-result cases. It
checks the runner's selected loop trace and exits, changes the producer,
and repeats it after full GC while an MT peer remains alive. The exact code
address, bytes, size and generation survive those calls. Independent semantic
and post-allocation mutations reject the CALLM result arity or fixed prefix,
producer result mode or overlapping destination, producer return count,
missing/aliased argument-map slots, and a producer-footer PC borrowed from its
consumer. The ordinary Lua companion
compares one-, two-, and three-result cases against an interpreted oracle and
checks publication and exact trace reuse; those results are not counted as
native execution evidence.

Validation was run through `tools/ci/arm64_jit_lua_leaf_contract.sh`: ARM64
and arm64e assertion and release profiles. Each result count has actual
selected-root entry/exit evidence; the companion's publication checks remain
separate. The final matrix reports 25 native calls, 124 semantic
negatives and 132 post-allocation negatives per profile, with no cold retries.
Logs are in `/Users/frityet/Scratch/ljmt-callm-integrated-20261029/`.

The earlier failed one-/two-result C attempts declared every consumer as
`function(a,b,c)`, while the ordinary companion declared the matching number
of parameters. The C fixture now matches those ordinary sources. It passes
all three counts without the separate NOJIT hot-counter fix. The old failures
were a different case involving missing-parameter normalization; their logs
remain under `/tmp/open-callm-leaf-contract-countfix2.log`, and that case is
not covered by this native parity claim. The runtime helper also uses the
actual bounded argument count instead of demanding room for three slots.
The producer replay path remains bounded to one through three results.

## Combined integration

The CALLM checkpoint combined with the NOJIT hot-counter fix at `f759b42a3`
passes the FUNCF contract and the full four-profile Lua leaf contract. The
restored ordinary runtime passes all 509 stock source tests and all 509 tests
through original-v2 bytecode import. Five consecutive ordinary topology runs
pass; the original `open-result-callm-loop` has the selected loop root and 81
selected-root exits in each measured phase. The runtime, source revision and
hash are frozen in `ljmt-callm-integrated-20261029/arm64/` under Scratch.

The separate consumer-identity exit fixture binds the exact CALLM guard after
the producer completes, checks its complete restorable vector, and exercises
interpreter continuation and native reentry with unchanged code bytes,
address, size and generation across full GC. See
`arm64-open-callm-consumer-exits-2026-10-29.md`.

An exact ordinary comparison also identifies four remaining coverage gaps:
one/two results passed to a consumer with three declared parameters, and three
results passed to a consumer with one/two parameters. All four are correct by
the interpreter oracle, publish and execute on x86_64, and do not publish a
root on this ARM64 checkpoint. The probes and both logs are in the integration
Scratch directory (`parameter-count-probe.lua`, `parameter-count-*.log`).
They are not included in the matching-arity parity claim above.
