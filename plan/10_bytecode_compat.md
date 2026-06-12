# 10. Bytecode: New Opcodes & Backwards Compatibility

Requirement: extend the bytecode for the cell upvalue model (ADR-7) while
**loading and running old dumps** (`string.dump` output from stock LuaJIT
2.1, BCDUMP_VERSION 2 — lj_bcdump.h:39). Source compiled by the new parser
always emits the new form.

## 10.1 Dump format versioning

- `BCDUMP_VERSION` stays the loader's *maximum*; define
  `BCDUMP_VERSION_LEGACY 2`, `BCDUMP_VERSION_TRANS 3`,
  `BCDUMP_VERSION_LOCKLESS 4`.
- lj_bcread.c `bcread_header`: accept {2,3,4}. Version 2 sets per-proto
  legacy-upvalue metadata outside `pt->flags`; version 3 is the transitional
  no-cell-op format; version 4 is the current cell format.
- lj_bcwrite.c emits 4 always (the comment at lj_bcdump.h:35–38 demands a
  bump for incompatible additions; 4 < 0x80 because v2/v3 loaders *should*
  cleanly reject rather than misparse — they do: version mismatch error).
- No dump flag-bit changes; BCDUMP_F_KNOWN untouched (v4 implies cell ops may
  appear; that's keyed off the version and verifier, not a dump flag).

## 10.2 New opcodes (appended after the current last op, before BC__MAX)

Current count: 97 ops, BC__MAX < 128 (lj_bc.h:71–204) — opcode byte has
ample room. Append (order matters for dispatch table layout; append-only
keeps every existing opcode number stable, which is what makes v2 dumps
loadable byte-for-byte):
```
BC_CNEW   A         dst slot ← new cell (closed GCupval, nil)
BC_CGET   A, D      dst ← cellslot D's value      (ins_AD)
BC_CSET   A, D      cellslot A's value ← slot D   (ins_AD)
```
Interpreter handlers: 07 §7.6. Recorder: 08 §8.8.4. lj_bc.h BCDEF gains
three lines + BCMODE entries (mode: CNEW=dst/none, CGET=dst/var,
CSET=base/var — copy the patterns of KNIL/MOV/USETV respectively).
lj_snap.c / lj_debug.c opcode tables: extend the metadata arrays
(`lj_bc_mode`), bytecode verifier below, and dis_x64.lua-style listers in
src/jit/*.lua (bc.lua: add names + operand formats).

## 10.3 Parser emission rules (v4) — recap of 06 §6.4.2
Captured local ⇒ cell-capable local: FNEW promotes a raw parent slot to a
closed GCupval cell for source/v4 local captures, and later owner-frame
accesses use CGET/CSET. CGET/CSET tolerate raw slots before a closure is
actually created, which covers conditional closure creation and loop variables
whose loop opcodes overwrite the visible slot with the next raw value.
Self-captured `local function f() ... f ... end` emits CNEW/FNEW/CSET so the
function value is stored through the cell instead of overwriting it. Indexed
stores parsed before capture discovery materialize captured table/key locals
with CGET before TSET*. Closing UCLO with A != 0 is no longer emitted for
source cells; UCLO 0 remains as a return/jump carrier for now. Cell-mode protos
are marked PROTO_NOJIT until the recorder/snapshot work is audited.

## 10.4 Legacy v2 chunks (the compatibility deviation, DECIDED)

v2 bytecode uses open upvalues: UGET/USETx against `func_finduv`-created
aliases of parent stack slots, closed by UCLO/return (lj_func.c:37–110).
Under a single OS thread this machinery still runs verbatim (per-L
openupval lists; no global uvhead — 03 §3.3 deleted it; lj_func.c keeps
only the per-L list which is owner-private, so single-thread semantics are
exact during the migration).

Once a legacy closure must become owner-independent, a *newly created*
legacy closure captures **by value at FNEW**: `lj_func_newL_*` on a
legacy-upvalue proto allocates closed cells initialized from the current
slot values instead of calling func_finduv.
Consequences:
- Sharing between sibling closures created from the *same* live parent
  frame is broken for legacy chunks only (each FNEW snapshots).
- Parent-frame writes after FNEW are not seen by the closure.
- UCLO becomes a no-op for such closures (07 §7.7).
This is the documented deviation; rationale: the alternative (escape
analysis + close-on-publish hooks on every store) taxes the hot store path
for everyone. Mitigation knob: `-Dluajit.legacyuv=strict` makes FNEW on a
legacy proto *raise an error* instead, for users who prefer loud failure.
Closures created before the runtime activation transition keep their
existing upvalue identity in the transitional source path. The final v4
cell parser removes this open-upvalue case; until then, do not detach
source-visible locals at activation merely to satisfy legacy handling.
(Foreign coroutines with open legacy upvalues remain a transitional
compatibility hazard until source cells remove open upvalues; the owner
claim path must not silently detach source-visible locals.)

## 10.5 Verifier
lj_bcread must scan bytecode after endian normalization. Check: (a) every
opcode is `< BC__MAX`; (b) v2/v3 dumps must not contain CNEW/CGET/CSET;
(c) v4 protos with cell ops must not contain UCLO-with-nonzero-close-range;
(d) CNEW/CGET/CSET slot operands are within framesize, and CGET/CSET may not
self-overwrite their cell slot. Also reject per-proto flag bits outside
`PROTO_CHILD|PROTO_VARARG|PROTO_FFI`. These run in bcread_proto and are
always-on load-time checks.

## 10.6 string.dump / -b round-trip
bcwrite of a legacy-loaded proto re-emits v2?? NO — DECIDED: bcwrite
always writes v4; a legacy proto (open-uv descriptors, UCLO) is *not*
representable in v4 — so `string.dump` on a legacy-loaded function in the
lockless build raises "unable to dump legacy function" (matches existing
"unable to dump given function" precedent for builtins). jit/bcsave.lua
unchanged otherwise. Old luajit reading new dumps: rejects on version — as
upstream intends.

## 10.7 Tests (13 §13.4)
t-bc-01: load v2 dump (golden file generated by stock luajit, checked into
tests/golden/) single-thread: byte-exact behavior vs stock run.
t-bc-02: same chunk under mt_active: capture-at-FNEW semantics asserted.
t-bc-03: v4 dump round-trip; old-luajit rejection (subprocess if available,
else skip). t-uv-01..07: cell semantics incl. cross-thread mutation,
loop-var capture, debug.getlocal unwrap (06 §6.4.2).
