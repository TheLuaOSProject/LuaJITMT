# C closure publication color assertion

`lua_pushcclosure()` allocated a white `GCfunc`, published it to the target
stack with `lj_state_stack_pubtv()`, and only then asserted that the closure was
still white.

That assertion became stale once stack publication started routing through
`lj_gc_pubroot()`: during classic `GCSpropagate`/`GCSatomic`, publishing a new
stack root is allowed to mark the object immediately. The object is no longer
white after the barrier, but the barrier behavior is correct.

The assertion now runs after upvalues are copied and before the stack slot is
published. This still verifies allocation color while allowing publication to
preserve the live root during active classic GC.

Verification:

- `tools/ci/lua_test.sh m3_gc2_paranoia`
