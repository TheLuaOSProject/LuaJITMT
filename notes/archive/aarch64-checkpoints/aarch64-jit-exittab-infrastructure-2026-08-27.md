# ARM64 authenticated exit-table infrastructure (2026-08-27)

## Scope and boundary

This tranche replaces ARM64's immutable, per-exit branch stubs with a
heap-backed target table and immutable executable gates. It is the smallest
substrate needed before any side trace can be attached without rewriting
published MAP_JIT code.

The side recorder remains deliberately closed:

```text
LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED = 1
```

Every currently admitted LOOP, FORL and fixed FUNCF root routes each guard
through the table, but all slots initially target the ordinary shared VM-exit
fallback. This tranche does not publish or enter a side trace and does not
expand the admitted IR language.

## Fixed executable layout

Each trace owns one shared four-word fallback followed by one eight-word
(32-byte) gate per emitted exit. The assembler reserves them above the trace
body:

```text
[generated trace][optional excluded tail-fill NOP]
[shared fallback: 4 words]
[gate 0: 8 words]
[gate 1: 8 words]
...
[optional high-end alignment NOP]
```

`T->exitstub` names the first gate. The fallback is therefore always
`T->exitstub - 4`, independent of the tail fixup's variable NOP padding.
`exitstub_trace_addr(T, i)` is `T->exitstub + 8*i`. Neither address is recovered
by parsing executable instructions.

The shared fallback is:

```text
bti j
ldr    x30, [x22, #k64_vm_exit_handler]
blr    x30                         ordinary ARM64
blraaz x30                         ARM64e
movz   w0, #traceno
```

The `BTI J` word is emitted unconditionally. It is a harmless hint without
branch tracking and is the required indirect-jump landing pad with BTI enabled.
The handler pointer in k64 is already signed with the zero discriminator on
ARM64e, matching `BLRAAZ`.

Each gate is:

```text
movz  w30, #exitno
str   x30, [sp, #SPOFS_TMP]
ldr   x30, literal(&exittab[i])
ldar  x30, [x30]
br    x30                         ordinary ARM64
braa  x30, x22                   ARM64e
nop
.quad &exittab[i]
```

LR/x30 is fixed assembler scratch and is not a semantic trace register.
`SPOFS_TMP == 0` is the existing reserved scratch word at the trace SP; normal
spill allocation begins at `SPS_FIRST == 2`. The assembler consumes one
high-end NOP when needed to establish eight-byte alignment. Each cell is then
32 bytes and the four-word fallback preserves that alignment for every inline
literal. The independent tail-fill NOP can lie between the generated body and
fallback when the return-tail reservation was one word larger than the selected
direct/indirect interpreter-return form.

## Representability and mcode accounting

The complete tail requirement is:

```text
optional alignment word + 4 fallback words + 8 * emitted exits
```

The backwards assembler records `mctoporig-mcp` in a 16-bit temporary snapshot
offset when it first crosses a snapshot. The complete tail is already part of
that distance, so tail construction rejects `need >= 65536` before allocating
the table or writing executable words. The boundary helper proves that 8,191
aligned exits need 65,532 words (65,533 when misaligned) and are representable;
8,192 exits need at least 65,540 words and are rejected.

The user-facing `maxsnap` option is clamped to `UINT16_MAX`, and snapshot growth
explicitly raises `SNAPOV` instead of allowing the 16-bit count to wrap.
Initial capacity errors request the entire alignment/fallback/gate tail plus
the normal redzone. Later mcode retries use `mctoporig-mcp`, not `mctop-mcp`,
so the already-written tail remains part of every reservation calculation.

All generic S14, S19 and S26 ARM64 emitters now check signed reach before
encoding in release builds. The inverted-loop raw B26 producers and final loop
fixup do the same. Final snapshot fixup checks each computed offset before its
16-bit cast. An oversized unpublished trace therefore raises `MCODEOV` rather
than relying on a debug assertion or publishing a truncated branch/offset.

## Publication and pointer authentication

The table has one `MCode *` slot per emitted exit. For a root this is `nsnap`;
for a side body it is `nsnap + 1`, retaining the backend's extra stack-check
exit even while side recording remains closed.

Slot updates are release stores and gates consume them with `LDAR`. No target
update changes executable bytes, calls `lj_mcode_patch()`, toggles MAP_JIT write
protection, or synchronizes an instruction cache.

ARM64e table entries use a distinct authentication domain:

- root VM entry is signed with the exact `GCtrace *` discriminator;
- exit-table targets are signed with the owning `global_State *` discriminator;
- k64 VM helper pointers retain the zero discriminator.

The gate has x22/GL available, so it authenticates a slot target with
`BRAA x30, x22`. A child trace's `T->mcauth` must never be copied into a parent
slot because that pointer uses the child trace as its discriminator. The raw
child mcode address is signed again for the global-state exit domain. PAC is
stripped only for software identity and mcode-lifetime range checks; stripping
never grants execution authority.

The shared fallback always begins with `BTI J`. Under the authenticated,
branch-tracked ABI, generated child heads also begin with `BTI J` through the
existing `emit_branch_track()` path, as required before a future `BRAA` target
can be published. Ordinary non-BTI ARM64 does not require that child landing
instruction.

## Exit handler ABI

The gate writes the exit number to the original trace stack at `SPOFS_TMP`.
After saving the fixed 512-byte `ExitState` register prefix, `vm_exit_handler`
reconstructs the original SP and loads that word directly as `exitno`.

The authenticated call from the shared fallback leaves LR pointing at its
final `MOVZ w0, #traceno`, so the handler still decodes the parent trace number
from `[lr]`. The old formula based on per-exit BL spacing is removed. The C
call remains:

```text
lj_trace_exit(J, ex, L, parent, exitno)
```

The TG-local `jit_base` lease remains published through restore, SMR and TEXIT
delivery exactly as in the already validated native-exit path.

## Ownership and retirement

The recorder token owns the heap table while it is attached to `J->cur`.
`trace_save()` transfers the table and gate pointers with the compact trace
body, then clears the current-trace pointers. The production abort,
owner-detach and synchronous `MCODELM` retry paths share the same exact cleanup
logic. The deterministic retry fixture directly validates allocation/free
balance after a complete table and gate layout has been built; live root flush
and close validate published ownership transfer, retirement and exact free.

The exact slot count is derived from the immutable published topology:

```text
nsnap + (root != 0)
```

This extra-slot rule is ARM64-specific. The macOS x64 backend allocates only
`nsnap` per-trace heap slots and retains its existing exact accounting.

The production scoped/full side-flush paths release-reset an inbound parent
slot to the parent's shared fallback before child retirement. An acquire reader
may already have observed the old child target; the existing TG `jit_base`
lease, trace-body SMR, retirement epoch and mcode-area grace rules keep that
target resident until the native reader quiesces. Mcode-area reference scans
strip authenticated slot targets before range comparison.

Final body destruction release-clears `exittab` and `exitstub`, then frees the
heap vector. GC preservation leases and marks the vector while its body is live
or grace-retained. Because side recording is still closed, current runtime
coverage exercises those rules for live roots and uses a synthetic side-shaped
trace to verify exact extra-slot freeing and PAC-stripped reference scanning.
Real parent-slot reset and side-table preservation remain required validation
when the first bounded side trace is opened.

## Entry certificate

The ARM64 root-entry two-pass view now acquires `exittab` and `exitstub` along
with mcode. Both views must agree and require:

- a non-null, naturally aligned table pointer with `TRACE_EXITTAB_MCODE` clear;
- a non-null, naturally aligned gate base;
- a representable `nsnap * 32` gate span;
- a four-word fallback address immediately before the gate base; and
- that fallback address starts at the generated body end or after the one
  optional excluded tail NOP.

The hot entry certificate deliberately does not walk allocator registries or
recover an mcode-area upper bound. Heap ownership and the complete immutable
gate span are construction invariants proved by the live fixture and emitted
layout checks; the entry path proves stable pointer identity, ownership bits,
alignment, overflow safety and body-to-fallback geometry without adding a
registry traversal to every native entry.

Entry does not scan mutable slot contents. That would add work to every root
entry and would reject a legitimate future child target. Exact instruction
layout, initial fallback targets and release/acquire routing are instead proved
by focused construction and runtime fixtures.

## Executed validation

The focused synthetic contract proves the exact fallback/gate words,
literal addresses, cell spacing, direct exit-number transport and `LDAR`.
Linked disassembly distinguishes ordinary `BR` from ARM64e
`BRAA x30, x22`, and retains BTI landings.

A live admitted root proves that:

- `exittab` is registered heap memory outside RX/RW mcode;
- `TRACE_EXITTAB_MCODE` is clear;
- every initial slot strips to the shared fallback, never to its own gate;
- a normal XPOLL/profile request traverses the default table and restores the
  VM state;
- side count remains zero.

A decisive retarget witness replaces deterministic terminal exit 8 with a test
`BTI J; BRK` landing under the JIT token. Ordinary ARM64 and a correctly
global-signed ARM64e pointer must reach the trap. ARM64e raw, zero-signed,
trace-signed and wrong-global pointers must fault at the authenticated gate
before the landing. Reset must restore the same root's normal exit behavior.

The following contracts passed on this Apple Silicon host on 2026-08-27:

- `tools/ci/arm64_jit_exit_contract.sh`: ordinary and ARM64e synthetic layout,
  8,191/8,192-exit representation boundary, live default exits, authenticated
  retargeting and rejection matrix, reset, deterministic `MCODELM` retry, and
  ordinary/ARM64e retirement fixtures.
- `LJ_ARM64_LIVE_FLUSH_RUNS=2 tools/ci/arm64_jit_live_flush_reuse_contract.sh`:
  ordinary live root flush/reuse twice and ARM64e once, including reset before
  reclamation, grace retention, exact production free, and a fresh default
  table after trace-number reuse.
- `tools/ci/arm64_jit_root_entry_contract.sh`: strict LOOP, FORL and true FUNCF
  root certificates, source-generation mutations and request races on ordinary
  ARM64 and authenticated ARM64e.
- `tools/ci/arm64_jit_native_loop_contract.sh`: ordinary and ARM64e LOOP/XPOLL
  lifecycle through the placement-independent gates, including randomized
  mcode allocation hints.
- `tools/ci/arm64_jit_funcf_record_contract.sh`: true FUNCF publication and
  native entry under both ABIs, with the fixture checking its two default table
  slots and immutable gates independently of its interpreter-return branch
  placement.
- `tools/ci/arm64_jit_mcode_retire_contract.sh`: native execution pin and mcode
  residency through explicit epoch reclamation.
- `tools/ci/arm64_jit_fail_closed_gate.sh`: the complete admitted ARM64 umbrella
  passed after integrating exit, root-entry and live flush/reuse contracts.

Builds continue to emit the pre-existing `ccall_rawchild_wait` unused-function
warning in helper configurations; no new warning was introduced by this
tranche.

Plain arena allocations intentionally have no per-object lifetime lane, and an
arena address can remain registered after a vector is freed, so neither signal
is treated as a per-allocation liveness certificate. Exact production counters
are used instead.

The forced `MCODELM` case is a deterministic post-layout error injection. It
proves recorder state recovery and exact table ownership, but does not claim a
real second MAP_JIT area was allocated. Forcing that allocator outcome is
nondeterministic on ARM64e because a new area need not land inside the current
branch-reach window. Full-tail reservation accounting is checked separately by
the boundary helper, source contract and live builds.

## Remaining work after this tranche

This infrastructure removes the first P0 side-trace blocker, but it does not
make ARM64 side traces safe by itself. Recorder ingress still rejects a parent;
the IR/post-RA certificates are root-only; root entry currently requires no
children; and no side-attachment certificate validates parent/root generation,
snapshot bounds, topology, mcode residency and BTI before the final slot store.
The first side milestone must remain a separately gated, first-level integer
LOOP/FORL language before calls, allocation, heap access, FFI, side-of-side or
stitching are considered.
