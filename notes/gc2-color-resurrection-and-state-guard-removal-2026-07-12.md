# GC2 color-resurrection and compatibility-state removal checkpoint

Date: 2026-07-12

This checkpoint continues the GC2-only runtime migration without changing any
file under `plan/`. It removes the remaining target operation which changed an
object from the historical "other white" to `currentwhite`, removes two
diagnostic dead-color vetoes, and makes string-table resize depend on the
actual atomic GC2 topology owner instead of the retired collector state byte.

## Runtime invariant

Header white bits are compatibility metadata. They do not decide whether an
object can be looked up, published, recorded by the JIT, stored in a TValue,
resized, resurrected, or destroyed. GC2 mark memory, exact ownership roots,
publication barriers, and the string header claim are the authorities for
those operations.

Concretely:

- `func_finduv()` always performs the GC2 arena mark when it finds the exact
  open upvalue; it never recolors the header first.
- `ffi.typeinfo()` publishes a name through the table store and final GC2 table
  barrier without recoloring the canonical string.
- `lj_str_resize()` no longer treats `GCSsweepstring` as an exclusion. Its
  existing `StrTabHdr.resize` CAS is shared with the GC2 sweep/topology owner
  and remains the real serialization point.
- `lj_ir_kgc()` and `checklivetv()` no longer reject an otherwise valid object
  because its white bits resemble the retired collector's dead color.
- the conditional color-resurrection helper is deleted and added to the
  retired-symbol source/artifact gate, so it cannot silently return under a
  different call site.

This is a safety improvement over retaining dormant old-collector conventions:
racy or adversarial header colors can no longer trigger a recolor side effect,
an assertion-only semantic difference, or a compatibility-state resize veto.
No Lua or LuaJIT API/ABI changes are involved.

## Regression coverage

The focused tests deliberately manufacture values which the old collector
would have considered dead:

- the open-upvalue test creates a second closure over an other-white upvalue,
  requires exact cell reuse and an unchanged color, then closes and publishes
  that same cell through GC2;
- the FFI typeinfo test locates a real CType name string, poisons its white
  bits, repeatedly materializes the `name` field, and requires both correct
  output and unchanged color;
- the string-table test sets `g->gc.state = GCSsweepstring`, requires a real
  resize to complete, then retains the existing adversarial-color rehash
  coverage; and
- the obsolete two-thread resurrection-CAS test is removed from the generic
  atomic-header fixture. That fixture continues to stress all atomic flag
  read/modify/write operations; GC2 must not test a primitive whose semantics
  have intentionally ceased to exist.

The retired-symbol gate now rejects `lj_gc_resurrect_if_dead` in target source,
ordinary objects, amalgamation objects, shared/static libraries, and platform
archives, in addition to the already denied marker/sweeper entry graph.

## Validation

- Linux x86-64 GCC warning-clean default build passed.
- `m3_gcflags_atomic`, `m3_gc_root_pending`, `m5_strtab_cas`, and the focused
  `ffi.typeinfo` snapshot fixture passed.
- An isolated worktree at committed `2ce1a946` plus only this patch passed
  `m3_gcflags_atomic`, `m3_gc_root_pending`, `m5_strtab_cas`, and the primary
  `t-ffi-typeinfo-snapshot` fixture. This excludes concurrent signal/universe
  work from the result.
- The larger `m7_ffi_typeinfo_snapshot` group advances through that primary
  fixture and then reaches its existing vector-initialization assertion in
  `t-ffi-cconv-init-snapshot`. A clean `2ce1a946` worktree without this patch
  reproduces the same failure, so the aggregate group is not claimed here;
  that separate FFI regression remains to be repaired.
- `git diff --check` and the expanded source-only retired-symbol gate pass.
- An isolated Windows x86-64 UCRT release build plus only this patch passed the
  Wine binary and installed-archive smokes.
- An isolated macOS x86-64 osxcross Clang build plus only this patch passed the
  Darling runtime smoke (`OSX x64`) and loaded the `threading` library. A clean
  `2ce1a946` target passed the identical command. The earlier release-archive
  runner attempt built and installed successfully but timed out once without
  runtime output, so that particular archive-runner attempt is not claimed as
  a pass; the directly executed target result is the macOS runtime gate for
  this checkpoint.

The implementation is target-neutral C, while the supported release matrix
remains x86-64 Linux, macOS, and Windows.

## Remaining compatibility-color/state work

This does not yet delete the shared `marked` byte or every color-era field:

- constructors, x64 `TNEW`, and traced `FNEW` still initialize `curwhite()`;
- x64 interpreter/JIT table barriers and `tab_tsetm_barrier_needed()` still
  contain black/white fallback predicates around their GC2 barriers;
- `lj_trace_free_gc()` still derives a terminal choice from the `SFIXED` bit in
  `currentwhite`;
- `GCSpause` remains as a compatibility observation in GC2 idle/retired-SMR
  gates, and the `GCS*` enum/state storage has not yet been collapsed;
- the GC2 paranoia oracle still interprets white headers diagnostically; and
- finalized, fixed, weak, cdata-finalizer, and NEEDSCAN meanings still share
  the historical byte and require a deliberate flag-layout migration.

Those are subsequent GC2-only slices. None restores a retired marker/sweeper
entry graph, and none justifies reintroducing color-based resurrection.
