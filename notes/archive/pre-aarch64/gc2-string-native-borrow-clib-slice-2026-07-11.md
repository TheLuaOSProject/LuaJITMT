# GC2 native string borrows: bounded clib lifetime slice (2026-07-11)

This slice fixes the concrete temporary-string borrows in `src/lj_clib.c`.
It does **not** complete the raw-string lifetime protocol and does not make it
safe to enable `LJ_GC2_STRING_BODY_RECLAIM` yet.

## Fixed clib paths

Every formatter-created or linker-script-created `GCstr` below now remains in
an explicitly published Lua stack slot until its final native byte consumer
returns. The caller restores the saved stack top immediately afterwards.

- POSIX/macOS/Cygwin synthesized `lib` prefix and `.so`/`.dylib`/`.dll` names
  remain rooted across `dlopen`.
- The pathname copied from the first `dlerror` remains rooted across the
  linker-script `fopen`/`fgets`/`fclose` sequence.
- A parsed `GROUP(...)` or `INPUT(...)` target remains rooted across `fclose`
  and the retry `dlopen`.
- Windows synthesized `.dll` names remain rooted across `LoadLibraryExA` and,
  on failure, through construction of the reported error.
- Win32 x86 fastcall/stdcall decorated names remain rooted across
  `GetProcAddress`.

The new stack slots are published with `lj_state_stack_pubtv`; merely moving
`L->top` is not sufficient for GC2's concurrent publication/barrier contract.
The patch does not insert an allocating or OS-error-producing operation between
the native wrappers' existing `dlerror`/`GetLastError` capture and use, so the
existing error-reporting and `LastError` restoration behavior is preserved.

## Why this is only a bounded first slice

Stack rooting is precise and cheap here because `lj_clib.c` owns each temporary
and has a single, structured final consumer. It also makes the objects visible
to the current stack scanner while the TG is in native code. It is not a
general solution for raw `strdata()` pointers: traced values may live only in
registers, racy table snapshots can lose their last semantic edge, foreign
calls can reenter Lua, and non-local error unwinds can bypass a lexical cleanup.

The final design still needs a nested, unwind-aware per-TG string-borrow epoch
or equivalent precise hazard protocol. An outer begin must publish before a
native ACK/allocation can close the relevant grace period; the matching end
must occur only after the final byte read, and error/callback/JIT exit paths
must restore the saved hazard depth. Reclamation must defer a retired string
body while any live TG has an older active borrow.

## Remaining high-priority raw-borrow inventory

- `lj_ccall_func` argument conversion and the foreign call itself, including
  callback nesting, retain Lua string byte pointers in `CCallState`.
- JIT/interpreter FFI helpers (`lj_ffi_jit_strlen`, `lj_ffi_jit_memcpy`, and
  related copy/string paths) can receive dynamic Lua-string pointers without a
  durable stack root.
- JIT-callable buffer, formatting, and serialization helpers can allocate or
  poll after receiving a register-only `GCstr *`/interior pointer.
- `lj_serialize_put` and table/string buffer snapshots need protection when a
  concurrent racy mutation removes the source table edge.
- The C parser keeps temporary identifier, string, asm-redirection, and error
  representation strings across allocations; a broad unwind-contained borrow
  around `lj_cparse` is a likely first implementation.
- `lj_ctype_repr*` callers often create one or two unpushed representation
  strings and then enter allocating error formatting; these should be rooted
  immediately or covered by the same precise borrow protocol.
- API late-publication windows (`lua_push[l]string`, `lua_pushvfstring`, and
  several temporary table/metafield keys) need exact publication roots while a
  state claim is dropped or reacquired.
- Lua bytecode/parser and string-library temporaries, including the bytecode
  reader chunk name and intermediate `string.rep` result, still need their
  individual lifetime proofs or roots.

Until those classes and the canonical-string retirement protocol are closed,
the string-body reclamation safety gate must remain disabled.
