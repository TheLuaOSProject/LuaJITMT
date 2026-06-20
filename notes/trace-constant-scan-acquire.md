Trace constant scanner acquire slice

- Added `trace_traceno_acq()` for stored trace-number reads.
- Routed legacy GC, GC2, and trace-retirement trace constant scanners through
  acquired trace IR bases and constant bounds.
- Used coherent `IRIns` snapshots for opcode/type checks, while keeping KGC
  payload marking on the original IR slot with acquire GCRef loads.

Validation:

- make -C src clean && make -C src -j$(getconf _NPROCESSORS_ONLN)
- tools/ci/m5_jit_trace_publish.sh
- tools/ci/m3_gc2_paranoia.sh
- tools/ci/m9_m10_gc.sh

Note:

- An initial parallel validation attempt raced clean/rebuild scripts against
  each other and produced artifact errors. The listed validations were rerun
  sequentially.
