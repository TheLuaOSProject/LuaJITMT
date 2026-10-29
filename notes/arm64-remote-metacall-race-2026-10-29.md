# Remote CALLXS flush recorder assertion (2026-10-29)

The integrated experimental gate is not complete. Its first run at
`d0e4ecaa1` stopped on an unused optional test-helper warning; after the
test-only `edb6cd4dc` repair, the resume passed all contracts through live
trace flush/reuse and reached the first combined M7 FFI profile. That profile
passed the new integer-aggregate fixture and both C++ unwind variants, then
aborted in `tests/t-ffi-callxs-remote-flush.lua`:

```
LuaJIT ASSERT lj_record.c:308: rec_check_slots:
slot 42 const mismatch: stack fffb8001051b5390 vs IR fffb8001051b3e90
```

The worker in this test records scalar foreign calls while the main thread
replaces the cdata `__call` metamethod. This failure is distinct from the
earlier unresolved TDUP harness crash. No production fix or causal attribution
to the vararg/integer-aggregate changes is established yet.

The original M7 helper executable was overwritten by the suite's automatic
bootstrap restoration and its temporary library directory was removed.
Rebuilding the identical helper flags at root source `1487ee15b` reproduces
the original executable UUID exactly:
`61C51FC0-CF07-3F2A-AC15-7DEAB7732000`. That preserved executable aborts on the
first isolated replay with the same slot-42 constant mismatch (different
process addresses). Its SHA-256 is
`5de740bd0104a999072f9fb89d2a3b74361ae9d6264424041d028999986a3944`.
The flags are ARM64 bootstrap/experimental/assert plus TRACE, XSAVE, GC2 and
TAB test helpers, using Apple Clang and the macOS SDK at deployment target 13.

One ordinary assertion-build replay each at `3e3f3432a`, `d9352591a` and
`cbb707874` passed. Those runs have different helper layouts and timing; they
do not rule out a race or establish a regression boundary.

Evidence is retained at `/Users/frityet/Scratch/ljmt-remote-flush-20261029/`:
`original.ips`, `build.sh`, `build.log`, `tested-source.txt`, immutable
`runtime/luajit` and `runtime/libluajit.a` with hashes/UUID/modules,
`flush.dylib`, `run-1.log`, and the three ordinary-version logs.
The complete failed/resumed gate logs and exact resume driver are under
`/Users/frityet/Scratch/ljmt-vararg-aggregate-full-20261029/`.

Source review is investigating a mismatch between the sampled shared
metamethod and the VM-selected function slot. Any repair must preserve the
identity guard and actual callee/return semantics; weakening
`rec_check_slots` would not establish correctness.
