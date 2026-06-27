# M5 PRNG Reseed Native Boundary

`math.randomseed()` without an explicit seed reads OS entropy for the calling
thread group's PRNG. On Linux this is normally `getrandom(2)`, with a
`/dev/urandom` fallback. Both can block or touch process-global kernel
resources, so the Lua-facing path now runs those calls as native-state regions.

The old `lj_prng_seed_secure(PRNGState *)` entry point remains non-throwing for
VM bootstrap, where no `lua_State` or thread group exists yet. The new
`lj_prng_seed_secure_l(lua_State *, PRNGState *)` entry point is used by
`math.randomseed()` when no seed argument is supplied. It accumulates safepoint
actions from native entropy calls, closes the `/dev/urandom` file descriptor
before checking STOPREQ, conditions the PRNG state after a successful seed, and
only then delivers pending shutdown interruption.

Validation:

- `tools/ci/m5_prng_seed_native.sh`
- `tools/ci/m5_math_random_tg.sh`
- `tools/ci/m5_concurrent_objects.sh`
