# ARM64 fast-string TG tmpbuf checkpoint

Date: 2026-08-26

## Scope and outcome

The ARM64 interpreter fast paths for `string.reverse`, `string.lower` and
`string.upper` now reset the temporary string buffer owned by the running
`TGState`. They no longer address `global_State.tmpbuf`, so concurrent Lua
executors cannot share the buffer's write cursor, allocator carrier or backing
allocation through these fast functions.

This checkpoint intentionally changes no VM-state, dispatch, hotcount, JIT
entry/exit or assembler-emitter code. The global buffer remains in the common
state layout for bootstrap and architectures which still use it.

## Address and ordering contract

The interpreter's fixed x25 register points to `TGState.dispatch`, not to the
start of `TGState`. In the current layout `TGState.tmpbuf` is at absolute offset
`0x4c98`, while `dispatch` is at `0x80`, making the relative offset `0x4c18`.
That does not fit an unshifted AArch64 12-bit ADD immediate. The VM therefore
uses the same layout-relative split already used for large TG root offsets:

```text
add  x14, x25, #(DISPATCH_TG(tmpbuf) & 0xfff000)
add  x0,  x14, #(DISPATCH_TG(tmpbuf) & 0xfff)
```

Static assertions bound the relative offset to 24 bits and certify the SBuf
field offsets used by the following reset. The reset matches the C
`lj_buf_tmp_()` contract on weakly ordered ARM64:

```text
add   x14, x0, #offsetof(SBuf, b)
ldar  x8, [x14]                 // Acquire current backing pointer.
str   x23, [x0, #offsetof(SBuf, L)]
stlr  x8, [x0]                 // Release-publish w = b; w is at offset zero.
```

`SBuf.L` is owner-private and is consumed by the immediately following helper.
The release store of `w` orders that carrier store before a buffer helper can
allocate or acknowledge a safepoint. This is the ARM64 equivalent of the
ordinary TG-relative MOV sequence in the x64 VM, where TSO supplies the needed
load/store ordering.

## Validation contract

`tools/ci/arm64_tmpbuf_contract.sh` checks both source and generated code. It:

- rejects `GL->tmpbuf`, `offsetof(global_State, tmpbuf)` and a single
  out-of-range `DISPATCH_TG(tmpbuf)` ADD in `ffstring_op`;
- requires the split x25-relative address formation and the LDAR/STR/STLR reset;
- derives the actual TG and SBuf offsets with a compile-time-layout probe;
- verifies the exact instruction sequence independently in the reverse, lower
  and upper symbols;
- verifies that the inspected thin ARM64 object is current and is the object in
  `libluajit.a`; and
- rejects compiler atomic-runtime imports.

`tests/t-arm64-tmpbuf-thread.lua` constructs exact expected byte strings without
using the three functions under test. Four workers synchronize at a start
barrier and repeatedly transform distinct mixed-case payloads of 31, 4096 and
65536 bytes. Each result is compared byte-for-byte, a rotating set remains live
across reuse, and periodic full collections exercise buffer backing retention.
The case runs only against the native disabled-JIT ARM64 bootstrap and is a
mandatory part of `arm64_bootstrap_gate.sh`.

## Validation evidence

The focused contract and runtime suite passed against a thin native ARM64
disabled-JIT build. The full `tools/ci/arm64_bootstrap_gate.sh` then passed,
including the new mandatory checks and the existing interpreter, stock,
threading, coroutine, FFI callback, C API and signal regressions. Its final
restore left `src/luajit`, `src/lj_vm.o` and `src/libluajit.a` as thin ARM64
artifacts with JIT disabled; the focused contract and four-worker runtime were
run once more against those restored artifacts and passed.

For preservation, a thin x86_64 assert build completed under Rosetta, the stock
suite reported 509 passing cases, and JIT-enabled numeric/string smoke checks
passed. The ARM source-only contract also passes without inspecting an object,
so non-ARM builds retain a cheap source-shape regression without requiring an
ARM artifact.

## Deferred work

The TG audit separately identified transitional global `vmstate`, `jit_base`
and JIT-exit dispatch paths. They require distinct ownership and trace-lifetime
reviews and are not implied fixed by this interpreter-only buffer change.
