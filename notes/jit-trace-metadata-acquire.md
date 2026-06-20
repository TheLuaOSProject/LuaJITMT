JIT trace metadata acquire slice

- Added acquire helpers for published trace metadata: nins, nk, nsnap,
  linktype, and root.
- Routed public jit.util trace metadata/bounds readers through the helpers:
  traceinfo, traceir, tracek, tracesnap, and traceexitstub.
- Left IR instruction payload snapshots for a separate pass; those need a
  coherent multi-field read strategy rather than isolated field loads.

Validation:

- make -C src -j$(getconf _NPROCESSORS_ONLN)
- tools/ci/m5_jit_trace_publish.sh
- direct jit.util traceinfo/tracesnap/traceir/tracek/traceexitstub smoke
- tools/ci/m6_jit.sh
