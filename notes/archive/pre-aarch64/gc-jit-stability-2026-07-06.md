# GC/JIT stability batch, 2026-07-06

Focus: table resize/JIT stress failures that surfaced while GC2 and legacy GC
overlap owner-thread stack scans and retired raw allocation lists.

Changes:

- Retired raw records for table nodes/arrays, trace vectors, mcode records and
  related raw roots now use registered-memory validation before following list
  links or marking side allocations. This avoids dereferencing stale retired
  nodes while preserving valid records through the arena owner lists.
- Legacy thread traversal now shares GC2's stack validator before touching the
  raw stack allocation. The validator covers registered TGs, the main TG and the
  current TLS TG, including current bump arenas, so valid fresh stacks are not
  silently skipped.
- GC2 sweep bridges now distinguish root-spine body preservation from semantic
  root payload tracing. Sweep-time semantic roots are traced synchronously before
  owner arena sweep can reuse child cells.
- Loop optimization treats `IR_XPOLL` like other side-effect IR and validates
  loop substitution references before indexing the substitution table. Malformed
  loop-copy state now aborts recording with `LJ_TRERR_RETRY` instead of reading
  past `subst`.
- Added opt-in `remotejitgc` coverage for remote worker stack roots during JIT
  table resize and owner-side full GC.

Verification run:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- 20 iterations of `LJ_M5_TAB_RESIZE_STRESS_CASES=jitstore,jitread,jititer`
  with 3 threads, GC workers enabled and JIT reps at 1800.
- `src/luajit tools/test.lua m5_tab_resize_jit_stress m5_tab_resize_remote_stack_gc`
- Mixed table/JIT stress combinations covering `tablelib`, `tableclear`,
  `jitstore`, `jitread` and `jititer`.
