GC2 FINREG userdata next-link atomics
=====================================

Slice
-----

- Added `gc2_finreg_udata_next_acq()` and `gc2_finreg_udata_next_rel()`
  beside the existing `gc2_finreg_udata_obj_*()` helpers.
- Routed `GC2FinRegUDataNode.next` traversal and publication through the
  helpers in the legacy userdata separator, GC2 fini drain, register, and
  forget paths.
- Added a scoped `m3_gc2_scaffold.sh` invariant coverage for only those FINREG userdata
  functions, avoiding unrelated GC2 node lists in `lj_gc2.c`.

Related build fix
-----------------

The full GC2 scaffold exposed two pre-existing amalgamated-build issues:

- `TraceMCodeView` was defined in both `lj_trace.c` and `lib_jit.c` when
  included through `ljamalg.c`; the shared view type now lives in `lj_jit.h`.
- The standalone frontend calls safepoint/thread helpers across the
  `ljamalg.o` boundary, so those declarations use `LJ_FUNCA`.

Validation
----------

- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `make -C src amalg`
- `tools/ci/m3_gc2_scaffold.sh`
- `tools/ci/m9_gc_stats.sh`
