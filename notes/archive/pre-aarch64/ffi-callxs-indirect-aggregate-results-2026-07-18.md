# Descriptor-driven indirect aggregate CALLXS results (2026-07-18)

## Status

The production generic x64 FFI recorder now admits the first aggregate result
class to `IR_CALLXS`: fixed-size struct and union results whose ABI storage is
unconditionally indirect. No file under `plan/` was changed.

This is not a declaration catalogue or an explicit C-call shape matcher. The
admission decision uses only the target ABI and the resolved result type's
copied size/alignment facts:

- System V x64 admits fixed aggregates larger than 16 bytes;
- Win64 admits fixed aggregates whose size is not 1, 2, 4, or 8 bytes.

System V aggregates at or below 16 bytes remain interpreted because packing,
field alignment, and INTEGER/SSE eightbyte classification decide whether they
return in memory or as one or two registers. Win64's scalar-sized aggregates
also remain interpreted until register aggregate results have a descriptor-aware
representation.

## Collect first, emit after allocation

`crec_call_args()` was split into two operations. The collector performs every
ordinary argument CType snapshot, conversion, promotion, and guard into a
bounded local TRef vector. Only after that succeeds does the recorder allocate
the aggregate result cdata and emit the `CARG` tree.

This order preserves the existing replay contract: an argument-conversion exit
does not allocate a result object, while an allocation exit restores the
original callable and arguments. The emitter reserves one argument slot for an
indirect result and prepends the result payload pointer as ABI argument zero.
It uses zero as the "no hidden argument" sentinel; `TREF_NIL` is a real ABI
placeholder and must not be confused with absence.

The ordinary x64 backend then applies its existing generic scalar argument
lowering. The hidden pointer naturally occupies RDI under System V or RCX/the
first positional slot under Win64 and shifts every public argument. Vararg
metadata remains attached to the callable independently of this argument
tree.

## Rooted direct-fill protocol

The result uses the same preallocated root lifecycle as boxed pointer, enum,
and 64-bit scalar CALLXS results:

```text
argument conversions and guards
DONE replay snapshot of the original call
IR_CNEW for the exact declared result CType
hidden Lua stack root
XSAVE DONE snapshot
remove hidden logical slot
DONE entry-rejection replay snapshot
publish native frame(result_root = result cdata)
guard entry admitted
CALLXS IRT_NIL(payload pointer, public arguments...)
construct caller result from the existing result cdata
DONE post-call snapshot
native leave
```

The callee writes directly into the cdata payload. There is no aggregate copy,
`XSTORE`, allocation, guard, or helper between the foreign return and native
leave. `CALLXS` has `IRT_NIL` because the ABI result channel is the hidden
pointer; an ABI-mandated mirror in RAX is intentionally ignored.

The XSAVE slot and native-frame `result_root` keep the exact cdata alive before
entry, throughout ACTIVE and SUSPENDED callback states, during remote GC and
trace flush, and through POSTCALL restoration. Entry rejection restores only
the original call and lets the interpreter allocate its own result. A fresh
STOPREQ or callback error may abandon a partially written private box, but
central unwind still clears its frame and pin exactly once and never exposes it
as a completed Lua result.

Aggregate payload bytes are opaque FFI storage rather than GC-managed object
edges, matching interpreted cdata semantics, so no Lua write barrier is needed
for the foreign stores.

## CType identity and alignment

Result resolution takes one stable snapshot containing both the raw layout
child and all accumulated attributes. Semantic classification continues to use
the raw child, preserving enum identity, while allocation uses the combined
alignment. The cdata retains the function declaration's exact result CType ID,
matching the interpreter for typedefs and attributes. Stripping the outer
`CTA_ALIGN` wrapper before allocation would pass the correct type to Lua while
silently under-aligning the ABI result buffer. Incomplete, variable-length,
and zero-size results fail closed to the interpreter.

An over-aligned aggregate supplies its fixed size as the second `IR_CNEW`
operand. The backend therefore uses `lj_cdata_newv()` and returns an interior
GCcdata header whose payload satisfies the declared alignment. Ordinary
aggregates retain the cheaper fixed allocation path.

The interpreter now uses the same combined result info when it preallocates
aggregate or complex return storage. Previously it retained the declared CType
ID but called the ordinary allocator, so a cold over-aligned sret call could
hand native code an under-aligned result buffer. This is a common FFI
correctness fix rather than a JIT-only semantic change.

Non-aggregate boxed results deliberately remain different: the interpreter
strips outer attributes from integer, pointer, and enum results before boxing.
CALLXS therefore allocates those results with the resolved raw CType ID too.
Keeping the declared ID for every rooted result would make interpreter and JIT
`ffi.typeof()` disagree and could claim alignment the fixed box did not have.

## Compatibility and cost

Lua receives the same by-value cdata type and field bytes as the interpreter.
The public LuaJIT API/ABI, FFI declarations, calling conventions, and bytecode
semantics are unchanged. Non-x64 targets and every excluded aggregate class
continue through the exact interpreter fallback.

The steady generated path pays one result allocation, which interpreted
aggregate return already requires, plus the existing native enter/leave
protocol. It avoids the interpreter call-state allocation/classification and
avoids a post-return aggregate copy. No lock, peer wait, signature dispatch,
or declaration-specific wrapper was added.

The separately documented temporary internal-arena-only `lua_Alloc` policy is
unchanged.

## Evidence

The authentic fixture exports two unrelated 24-byte struct result types with
different field layouts and scalar argument mixtures, plus an exact 24-byte,
32-byte-aligned result which forces the interior aligned-cdata allocation
path. A zero-public-argument result proves the hidden pointer can be the whole
argument tree, a union proves the admission is not struct-tag-specific, and an
eight-GPR input forces public arguments onto the stack after the hidden ABI
slot. Tests require exact CType identity, size/alignment, every field value,
exact native effect counts, and production `XSAVE`/`CALLXS` IR. Separate
ignored, excess-fixed, open, CALLM, CALLT, and CALLMT cases cover the admitted
caller/result modes.

The C IR inspector distinguishes scalar boxed results (one raw `XSTORE`) from
indirect aggregate results (`IRT_NIL`, no `XSTORE`, and exactly one payload
reference in the CALLXS argument tree). It additionally requires the
over-aligned trace's `CNEW` to carry the exact 24-byte size operand and checks
the returned payload address modulo 32. Forced POSTCALL, deliberate entry
rejection, and fresh STOPREQ cases verify exact restoration, no completed-call
replay, and empty native-frame/trace-pin state.

A blocked generated sret call is also held ACTIVE while a remote thread
completes a full GC and global `jit.flush()`. The result box is reachable only
through the certified native frame during that interval; exact counters remain
at eight before release and prove the completed call is never replayed.

Focused validation passed the complete production CALLXS lifecycle suite, the
full generic scalar/boxed ABI matrix, target-runtime Clang ASan, the documented
Clang UBSan profile, and clean default, `LUAJIT_DISABLE_JIT`, and
`LUAJIT_DISABLE_FFI` builds. The sanitizer fixtures instrumented both LuaJIT
and the native aggregate library.

## Remaining aggregate work

This tranche deliberately does not flatten aggregate pieces into unrelated
scalar `CARG`s. That would break System V whole-aggregate register rollback and
Win64 positional rules. The remaining direct-ABI work needs an immutable,
trace-owned call descriptor for:

- System V recursive INTEGER/SSE/SSEUP/MEMORY classification;
- one- and two-eightbyte register results, including mixed GPR/XMM returns;
- by-value aggregate arguments and atomic stack fallback;
- Win64 scalar-sized aggregate returns and caller-owned by-reference argument
  temporaries;
- complex float/double and vector arguments/results;
- remaining protected, continuation, root-tail, terminal, and non-Lua caller
  snapshot topologies.
