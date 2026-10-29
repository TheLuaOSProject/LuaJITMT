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

The native C fixture currently exercises the fixed three-result case. It
checks the runner's selected loop trace and exits, repeats it after full GC
while an MT peer remains alive, and independently rejects a changed CALLM
arity, a corrupted CALLM argument-map value, and a producer-footer PC borrowed
from its consumer in semantic and post-RA views. The ordinary Lua companion
compares one-, two-, and three-result cases against an interpreted oracle and
checks publication and exact trace reuse; those results are not counted as
native execution evidence.

Validation was run through `tools/ci/arm64_jit_lua_leaf_contract.sh`: ARM64
and arm64e assertion and release profiles. The focused three-result shape has
actual selected-root entry/exit evidence; the companion's publication checks
remain separate from that native witness. One- and two-result native entry
remain unverified because their current fixture runs do not reach root-entry
preflight (zero entry starts and zero aborts); this is tracked separately from
the semantic/post-RA admission proof. The producer replay path is bounded to
fixed return counts one through three.
