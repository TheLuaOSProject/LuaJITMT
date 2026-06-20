Trace flush metadata acquire slice

- Added `trace_traceno_rel()` for stored trace-number release updates.
- Routed trace flush/root/side/dependency scans through acquired trace-number,
  root, exittab, nsnap, and IR-base snapshots.
- Switched scoped/full flush trace-number clears to release stores.
- Left trace construction/save raw where fields are still recorder-token owned
  and not yet published.

Validation:

- make -C src -j$(getconf _NPROCESSORS_ONLN)
- tools/ci/m6_jit_flush_hs.sh
- tools/ci/m6_jit_mcode_publish.sh
