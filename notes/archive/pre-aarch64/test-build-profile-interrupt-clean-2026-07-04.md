# Test build profile interruption cleanup

The Lua test harness build cache now cleans whenever the requested `XCFLAGS`
profile does not match the recorded build signature, even if the final
`src/luajit`/`src/libluajit.a` outputs are missing.

That missing-output state can happen after an interrupted rebuild: generated
headers such as `lj_libdef.h` may already exist for the old profile, while the
final binary/archive do not. Reusing those generated headers across a
`LUAJIT_DISABLE_JIT` switch can leave JIT-only `jit.util` entries in the
no-JIT profile. Cleaning on any signature mismatch keeps generated headers,
objects, and binaries in one profile.
