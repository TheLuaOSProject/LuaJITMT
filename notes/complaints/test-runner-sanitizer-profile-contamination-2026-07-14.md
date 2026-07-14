# Test runner can relabel a sanitizer build as default (2026-07-14)

Status: open harness issue; validation has a safe explicit-clean workaround.

When an existing checkout already has a persisted `default` build signature,
an out-of-band target sanitizer build can be reused by a later focused suite
as though it were the ordinary default build.  One reproducing sequence is:

```sh
make -C src clean
make -C src -j2 CC=clang CCDEBUG=-g \
  TARGET_CFLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address' \
  TARGET_LDFLAGS='-fsanitize=address' \
  TARGET_SHLDFLAGS='-fsanitize=address'
/usr/bin/luajit tools/test.lua m6_jit_gc2_readiness
```

The suite's ordinary build step can leave the ASAN objects and
`libluajit.a` in place.  A C fixture then compiled by plain `cc` fails at link
time on undefined `__asan_*` symbols, before any runtime test executes.  This
is build-profile contamination, not a LuaJIT correctness failure.

`tests/lib/ljtest.lua` persists only `XCFLAGS` in
`src/.lj-test-build-signature`; compiler selection and `TARGET_CFLAGS`,
`TARGET_LDFLAGS`, and `TARGET_SHLDFLAGS` are absent.  `make -C src clean` also
does not remove that harness stamp.  Although the runner can detect outputs
newer than the stamp, it consults that check only in the `opts.clean` cache-hit
path.  The normal non-clean default path sees the old default signature,
invokes an up-to-date `make`, and writes a fresh default stamp over the
sanitized outputs.

Before returning from a manual sanitizer build to focused suites, use an
explicit normal rebuild:

```sh
make -C src clean
make -C src -j2
```

For a fresh `tools/test.lua` process, removing
`src/.lj-test-build-signature` before the suite is also sufficient because the
signature mismatch forces a clean rebuild.  An already-running runner may
retain its in-memory signature, so the explicit rebuild is the general safe
workaround.  A harness fix should
invalidate any signature whose outputs are newer than its stamp on every build
path, and should include all build-shaping compiler and flag variables when
the harness itself supplies them.
