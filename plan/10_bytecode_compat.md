# 10. Bytecode: Current Lockless Dump Format

Requirement: extend the bytecode for the cell upvalue model (ADR-7) while
using a single current dump format. The lockless build does not accept
pre-lockless v2/v3 dumps because their open-upvalue encoding cannot preserve
owner-independent local-cell semantics without a compatibility detour on every
closure creation path. Source compiled by the parser and `string.dump` output
both use the current lockless format.

## 10.1 Dump format versioning

- `BCDUMP_VERSION` is `BCDUMP_VERSION_LOCKLESS 4`.
- lj_bcread.c `bcread_header`: accept only version 4. Older versions fail at
  the header instead of entering prototype/upvalue compatibility handling.
- lj_bcwrite.c emits version 4 always.
- No dump flag-bit changes; BCDUMP_F_KNOWN untouched (v4 implies cell ops may
  appear; that's keyed off the version and verifier, not a dump flag).

## 10.2 New opcodes (appended after the current last op, before BC__MAX)

Current count: 97 ops, BC__MAX < 128 (lj_bc.h:71–204) — opcode byte has
ample room. Append (order matters for dispatch table layout):
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
source cells; UCLO 0 remains as a return/jump carrier for now. Original
implementation-plan wording marked loaded v4 cell protos `PROTO_NOJIT` until
the recorder/snapshot work was audited; the current audited boundary is that
source owner CGET/CSET, source child-cell upvalues, loaded v4 CGET/CSET, and
loaded v4 child-cell upvalues can trace on x64. Source and loaded v4
self-captured local-function CNEW/FNEW/CSET loops can also trace through the
first helper-backed M6 slice, while recursive function-valued local-cell
upvalue reads intentionally use `UREFC`/`ULOAD` instead of the old current
function constification shortcut. Mixed source/loaded FNEW traces with raw
immutable captures now sync the traced stack value before helper construction,
and mutable captures can trace once the owner slot is already promoted at trace
entry or when the hot trace itself performs the first mutable raw-slot promotion
with otherwise type-stable loop-carried slots.

## 10.4 Old Dump Rejection

v2/v3 bytecode used transitional/open-upvalue layouts that are not part of the
current lockless ABI. The loader rejects them at the header, before prototype
allocation, so runtime closure creation only needs to handle source/current
v4 descriptors:

- Source/current cell-capable locals use `PROTO2_CELLUV`.
- Non-cell local captures use the ordinary owner-private `func_finduv()` path.
- No `proto_legacyuv` metadata or writer-side legacy-upvalue walk is needed.

## 10.5 Verifier
lj_bcread must scan bytecode after endian normalization. Check: (a) every
opcode is `< BC__MAX`; (b) protos with cell ops must not contain
UCLO-with-nonzero-close-range; (c) CNEW/CGET/CSET slot operands are within
framesize, and CGET/CSET may not
self-overwrite their cell slot. Also reject per-proto flag bits outside
`PROTO_CHILD|PROTO_VARARG|PROTO_FFI`. These run in bcread_proto and are
always-on load-time checks.

## 10.6 string.dump / -b round-trip
bcwrite always writes v4. Old luajit reading new dumps rejects on version, as
upstream intends. The lockless loader reading old dumps rejects on version for
the same reason: carrying a second old-upvalue ABI would keep source-visible
compatibility logic in hot closure construction paths.

## 10.7 Tests (13 §13.4)
`tests/t-bcdump-current.c` checks v4 load/round-trip behavior and malformed
current dump rejection, including bad version bytes, forbidden proto flags,
cell slot bounds, self-overwriting CGET/CSET, and invalid closing UCLO with
cell ops. t-uv-01..07: cell semantics incl. cross-thread mutation, loop-var
capture, debug.getlocal unwrap (06 §6.4.2).
