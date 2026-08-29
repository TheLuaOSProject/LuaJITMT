# Generic CALLXS SysV small aggregates (2026-07-19)

This tranche extends the production generic CALLXS path; it does not add a C
declaration, symbol, or signature matcher.

## Admitted ABI class

On SysV x86-64, fixed struct/union arguments and results classified into one
SysV eightbyte class are now admitted when all of the following hold:

- the aggregate size is 1, 2, 4, or 8 bytes;
- recursive field/array/subtype classification yields one INTEGER or SSE
  eightbyte and no MEMORY class;
- no complex or vector member is encountered; these CT_ARRAY-like types are
  rejected explicitly instead of being mistaken for ordinary arrays while the
  interpreter classifier still marks their vector ABI classes NYI;
- the source argument is exact aggregate cdata (an exact cdata reference is
  also accepted).

The recorder mirrors `lj_ccall.c`'s recursive class merge, including unaligned
MEMORY rejection, nested fields, arrays, bitfields, unions, and INTEGER
precedence over SSE. It lowers the one classified eightbyte through a raw
integer or FP carrier in the existing CARG/CALLXS machinery. Results are stored
without conversion into a preallocated, XSAVE-rooted cdata object before the
native-leave operation.

One-class register exhaustion needs no new descriptor: there is only one
component, so the existing independent SysV GPR/XMM allocator either consumes
its one register or places the whole value on the stack. Thus the ABI's
all-register-or-stack rollback rule is exact for this subset. Focused tests
cover both exhausted GPR and exhausted XMM cases and verify the native effect
counter equals the Lua iteration count.

## Deliberate fail-closed boundary

The following still leave the trace and use the interpreter:

- two-eightbyte (9--16 byte) aggregates;
- non-power-of-two payload widths, to avoid rounded JIT loads outside the exact
  payload;
- aggregates classified MEMORY, including unaligned members;
- implicit table/string aggregate conversion and non-exact aggregate cdata;
- Win64 aggregate arguments/results.

The next SysV tranche needs an immutable aggregate grouping descriptor (or an
equivalent IR encoding) understood by stack-slot sizing and x64 call lowering.
That grouping is required to tentatively allocate both INTEGER/SSE components,
roll back both register cursors if either class overflows, put the entire
aggregate on the stack, and reconstruct mixed two-register results. Flattening
those components into unrelated scalar CARG nodes would be ABI-incorrect and
must not be used as a shortcut.

## Evidence

`tests/t-ffi-callxs-sysv-small-aggregate.lua` and its C library cover generic
INTEGER, SSE, recursively nested, union class-merge, GPR-exhaustion, and
XMM-exhaustion cases. Separate one-, two-, and four-byte INTEGER rows plus a
four-byte SSE row prove every admitted narrow argument and result carrier; the
one- and two-byte arguments follow six GPR scalars to prove their complete
8-byte SysV stack slots too. A native-only result-mode flip forces a guard exit
after CALLXS and verifies that the foreign effect still occurs exactly once.
Every row mechanically requires XSAVE and CALLXS and checks an exact
per-function native effect count, so a post-call exit cannot silently replay or
omit the foreign call.
