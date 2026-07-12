# Test runner does not clean after an amalgamated build (2026-07-12)

Status: open harness issue; implementation work has a safe explicit-clean
workaround.

After `make -C src amalg`, invoking a focused suite whose setup calls the
default build without first cleaning can leave `ljamalg.o` in `libluajit.a`
while also adding the split `lj_*.o` objects. Linking `luajit` then fails with
multiple definitions for the VM, GC2, table, JIT, and API symbols.

Observed sequence:

1. `make -C src clean && make -C src -j2 amalg` succeeds.
2. `./src/luajit tools/test.lua m5_tab_forward_filter` performs an incremental
   default build.
3. The final link contains both `ljamalg.o` and split objects and fails with
   duplicate symbols.

The source candidate is unaffected. Running `make -C src clean` before the
suite restores a normal default archive and the tests proceed. The harness
should either make build mode part of its cache identity or force a clean when
switching between amalgamated and split builds.
