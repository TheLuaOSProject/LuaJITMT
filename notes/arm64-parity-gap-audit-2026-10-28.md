# ARM64 JIT parity gaps beyond sampled rows

Read-only source audit at `50787dd9765e6a06a69330ece1d135ad823413d0`.
The separate fixed-two-result slice and varargs work were still pending
integration at this checkpoint. The inventory at `f040711ab` showed 16/20
ARM64 workload rows with selected-root native witnesses versus 18/20 on x64;
the two remaining sampled differences were varargs and multiple results.
That count is an observation, not a capability boundary. This audit did not
build or run either architecture.

## Concrete structural delta

`trace_root_startins_valid()` in `src/lj_trace.c` is a direct topology
comparison. x64 accepts `FORL`, `ITERL`, `ITERN`, `LOOP`, `FUNCF`, and
`RET`/`RET0`/`RET1` roots, and accepts `CALL`/`CALLM`/`ITERC` stitched starts
when `exitno != 0`. ARM64 accepts only ordinary `LOOP`, `FORL`, or `FUNCF`
roots with `exitno == 0`. `trace_root_arm64_generation_valid()` repeats the
same three-op restriction. The ARM64 asm dispatch then sends `LOOP`/`FORL` to
the scalar certificate, `FUNCF` to its own shape checker, and `CALLXS` traces
to selected FFI admission.

That ARM64 function-entry shape (`arm64_ir_funcf_shape()` and
`arm64_postra_funcf_admit()`) allows numeric/integer SLOADs, scalar arithmetic,
NOP, and a final XPOLL; it does not admit branch, heap, upvalue, ordinary
call, or general return IR. Loop roots have a broad but still explicit
whitelist in `arm64_scalar_{ir,postra}_admit_mode()`. Consequently, ordinary
x64 trace topologies and the general bytecode/IR shapes they carry remain a
larger parity target than the residual rows in a 20-workload inventory.

Recorder boundaries reinforce this. ARM64 `rec_call_setup()` adds the
pre-call replay snapshot for Lua callees only when the exact leaf recognizer
accepts them and requires the current loop to be `FORL`, fixed `CALL`,
single-result geometry at the bounded depth. Other calls may record through
common LuaJIT logic, but the ARM64 scalar validator will not admit their
general `IR_CALLN`/`IR_CALLS` effects. Existing rooted helper and CALLXS
certificates are deliberate exact exceptions. A scalable next call slice
needs a source-derived frame/effect model for arbitrary call sequences,
callee identity, variable result windows and resumes, then independent
semantic and post-RA checks. Extending the leaf-recognizer's opcode list alone
will not establish that caller/callee exits and side effects resume correctly.

The ARM64 side certificate is also explicitly first-side/scalar topology.
`arm64_side_scalar_topology()` permits a direct root-linked exit or a
self-linked first loop side; `arm64_side_scalar_context_{ir,postra}()` binds
the side to one parent continuation, root prototype, and depth-zero recorder
state. This is meaningful validated coverage, but it does not provide the
x64-style general side/stitch graph. The next topology slice should prove a
second side or stitch edge end-to-end across publication, entry, exit,
replacement, and retirement rather than widening the `link` cases alone.

## Heap, iterator, and FFI comparison

Heap parity has useful certified slices already: rooted scalar table reads,
selected stores, strings, local cells, and selected CData callables/FFI
arguments. It does not amount to arbitrary HREF/AREF/FLOAD/HLOAD/ULOAD or
heap-valued recurrence. `arm64_scalar_ir_admit_mode()` admits exact rooted
helper IR and rejects other operation shapes. x64 has target-specific
shared-table helper lowering in `lj_asm_x86.h` guarded by
`LJ_HAS_X64_MT_JIT_HELPERS`; those helper/lifetime rules are evidence to
compare against, not permission to copy raw x64 loads into ARM64.

Table iteration is not yet a proven architecture delta. The sampled table
iterator row has no selected-root witness on either architecture. Also,
`lj_record_next()` explicitly refuses `lj_record_mt_shared_tab()` on both
targets because the shared traversal needs generation-aware key/value copies.
So the inventory alone does not justify claiming x64 native shared-table
iteration. A discriminating next probe must separate a private/non-shared
`next()` loop from the actively shared resize case and require an authentic
native exit witness.

FFI has broad tested scalar coverage on ARM64, including selected integer,
numeric, float, pointer, boxed-64-bit, CALLM, callbacks, errno and unwind
profiles. The known remaining ARM64 JIT ABI gap is aggregate/sret (and other
unsupported vector/complex or exact conversion shapes); the aggregate note
explicitly covers interpreter FFI only. Do not infer an x64 native aggregate
CALLXS witness from x64 interpreter FFI or from generic call syntax. Confirm
one real x64 aggregate-call trace before making that an architecture-delta
claim. A safe implementation route is a separate exact AAPCS64 classifier
and source/result/snapshot certificate composed with existing XSAVE lifecycle,
not a broad CALLXS opening.

## Highest-impact route

After the pending varargs and fixed-two-result slices land, the useful
architectural change is a general trace-source model with bounded frame and
CFG data, rather than another list of accepted source examples:

1. Model root, side and stitched continuations as explicit source locations
   and caller/callee frame edges. Reconstruct each snapshot's active frame
   chain from its own footer and bytecode return links.
2. Replay bytecode dataflow per frame (parameters, copies, branches, calls,
   results, returns) and bind every surviving IR value and snapshot slot to
   that replay. Model effectful calls with exact helper/ABI effects and
   interpreter resume positions.
3. Independently replay that model from the immutable allocated view, then
   validate registers, spills, roots, aliases, rename scopes, and actual
   emitted exit protocol. Keep effects outside the certified model closed.
4. Grow topology one edge at a time with actual native publication/entry,
   forced exits, post-GC reuse and concurrent mutation tests. Retain the
   current per-operation rooted heap and CALLXS protocols as effect modules.

The highest-return first slice is arbitrary fixed Lua call/result chains
inside an already admitted scalar loop, because x64 has an observed native
nested-call witness and recent ARM64 leaf/return work already provides the
frame proof machinery. In parallel, use a targeted private-table `next()`
probe to establish whether there is a non-shared iterator delta; shared table
iteration remains a shared-protocol project on both targets. Keep aggregate
CALLXS after its x64 native status is verified.

## Evidence limits

The saved parity inventory has 260 result comparisons per architecture at its
integrated checkpoint and records live selected-root exits, but it is not a
full-program native coverage measure. Earlier x64 compatibility logs establish
509-case source and bytecode-import corpus success and several exact native
fixtures; these do not prove full x64 native coverage for every gap named
above. No x64 aggregate CALLXS, iterator, arbitrary trace stitch, or generic
Lua call-graph native witness was established by this audit.
