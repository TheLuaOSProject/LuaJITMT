# M5 PRNG Reseed Native Boundary

`math.randomseed()` without an explicit seed reads OS entropy for the calling
thread group's PRNG. On Linux this is normally `getrandom(2)`, with a
`/dev/urandom` fallback. Both can block or touch process-global kernel
resources, so the Lua-facing path now runs those calls as native-state regions.

The native-aware `lj_prng_seed_secure_l(lua_State *, PRNGState *)` entry point
is used by `math.randomseed()` when no seed argument is supplied. VM bootstrap
passes `NULL` before a `lua_State` or thread group exists, so the old
`lj_prng_seed_secure(PRNGState *)` compatibility entry point has been removed.
The single remaining entry point accumulates safepoint actions from native
entropy calls, closes the `/dev/urandom` file descriptor before checking
STOPREQ, conditions the PRNG state after a successful seed, and only then
delivers pending shutdown interruption.

2026-06-27 follow-up:

- The Lua-facing secure reseed path now uses a fresh STOPREQ helper. A
  pre-existing sticky shutdown flag no longer interrupts an otherwise
  successful no-argument `math.randomseed()` reseed when no native entropy call
  acknowledged a new STOPREQ action.
- The success path still conditions the PRNG state before delivering a fresh
  STOPREQ.
- `t-prng-seed-native.c` covers sticky STOPREQ reseed behavior, and the M5
  notes document why raw reseed STOPREQ checks outside the fresh helper.

Validation:

- `tools/ci/m5_prng_seed_native.sh`
- `tools/ci/m5_math_random_tg.sh`
- `tools/ci/m5_concurrent_objects.sh`
