# Frozen wide prototype: emitted-path audit and invalidation

The original AoS prototype is not a correct integrated implementation. Four
hard-coded sidecar index shifts were omitted when the entry grew from 16 to
32 bytes. The source and the actual generated VM object agree on this error.
The original runtime sources, executables, archives, tests and timing records
are unchanged. No repaired prototype or replacement timing is represented by
those artifacts.

Exact source boundary: d680421c plus the original three-file wide patch,
tree `/tmp/lj-wide-stamp-20260905-_gc0aoyc/wide`. The object/source/binary hashes,
disassembly command and relevant bytes are in `emitted-layout-audit.json`.
`wide-tnew.disasm` is the existing unstripped VM object's TNEW function. The
normal executable was stripped, so an initial `nm` inspection reported no
symbols; inspecting the existing VM object required no rebuild.

| Source site | Intended purpose | Frozen prototype result |
| --- | --- | --- |
| `vm_x64.dasc:348–353` | Arena pointer, state and token offsets | `offsetof` adjusts correctly; token is at +16. |
| `vm_x64.dasc:4541–4542` | TNEW token precheck before bump reservation | Cell SHL 4 is wrong; tests a word in another entry. |
| `vm_x64.dasc:4654–4656` | TNEW post-claim token check and reset address | Cell SHL 4 is wrong again. |
| `vm_x64.dasc:4722` | Clear old proof before body/READY/block publication | Writes one qword at the incorrectly derived address. |
| `lj_asm_x86.h:1582` | FNEW stamp base after exact CONSTRUCT claims | Cell SHL 4 is wrong. |
| `lj_asm_x86.h:1643` | FNEW token precheck before bump reservation | Cell SHL 4 is wrong. |
| `lj_asm_x86.h:1571,1578,1633,1639` | FNEW token field offsets | `offsetof` adjusts correctly to +16, but the entry base is wrong. |
| `lj_asm_x86.h:1575,1637` | FNEW upvalue stamp relative to function stamp | `fncells * sizeof(LJGC2TabStamp)` adjusts correctly; the first base is still wrong. |
| `lj_asm_x86.h:1568–1569` | FNEW reset of function/upvalue old proofs | Both emit one qword; both start from wrongly derived entry bases. |
| `lj_asm_x86.h:1285–1291` | Emit qword zero helper | Correct eight-byte operation for low `state`; it does not clear era. |
| `lj_arena.c:379–391` | C private-incarnation preparation | Correct C indexing; explicitly clears both low state and high era. |
| `lj_arena.c:297–305,322–346` | C stamp lookup and token occupancy | Typed array indexing automatically uses 32-byte stride. |
| `lj_arena.h:1105–1151` | Generic stamp/token lookup | Typed array indexing and huge embedded field access adjust correctly. |
| `lj_gc2.c:19119–19125` | Exact-token small scanner | Typed array indexing and token member access adjust correctly. |
| `lj_arena.c:1782–1804,1851–1885` | Sidecar allocate/zero/free and token checks | `sizeof`, typed indexing and fields adjust correctly. |
| `lj_arena.c:3676–3686` | Huge header-only token lease stamp | `sizeof(GCAhdr)` and embedded field adjust correctly. |

All other C stamp/token consumers found by a complete source search use the
same typed accessors, `side->cell[cell]`, or embedded `huge_tabstamp` fields.
The header static assertions were changed to the new measured geometry. The
generated `host/buildvm_arch.h` merely reproduces the VM source operations;
it does not repair the omitted stride. Interpreter FNEW calls the C allocator;
the additional FNEW omission is in its traced one-numeric-upvalue emitter.

For allocation cell `i`, correct stamp address is `side + 32*i`. Both emitted
paths instead derive `side + 16*i`. If `i` is even, a low-word proof reset hits
the proof of entry `i/2`. If `i` is odd, it hits the exact token of entry
`(i-1)/2`. The +16 token test then observes the next real entry's low proof
instead of that token. This can bypass the actual allocation's rescan owner
and can erase an unrelated token. The actual cell's old proof is not reset.
FNEW's correct 32-byte relative upvalue delta does not repair its wrong base.
These consequences follow directly from the measured layout and emitted
addresses; no hypothetical reader interleaving is needed to establish the
addressing defect.

The normal wide VM object has these decisive instructions:

```text
0x0f57  49 c1 e3 04             shl $4,%r11
0x0f5b  42 f6 44 18 10 03       testb $3,0x10(%rax,%r11,1)
0x10ee  49 c1 e1 04             shl $4,%r9
0x10f5  f6 41 10 03             testb $3,0x10(%rcx)
0x11e3  48 c7 01 00 00 00 00    movq $0,(%rcx)
```

The separate low-word-versus-full reset question does not excuse the stride
bug. With a correct entry address, clearing low state while preserving a high
era can be safe after the exact old body owners are gone and token NONE is
validated: coverage is zero, and no old body scanner can carry a matching
ticket across reuse. It would differ from the C prototype's explicit full
reset and would need a documented uniform policy and forced-reuse controls.
A carried terminal high era also shortens the new incarnation's remaining
namespace. In the frozen build, the actual entry's low coverage can remain
unchanged and another token can be overwritten, so that argument does not
apply.

The previous rollover fixture exercises the new C proof protocol, but it did
not force reused promoted cells through TNEW/FNEW or check neighboring sidecar
tokens. Passing stock/ASan/coalescing logs therefore failed to catch this
integration omission. Their pass status remains a factual run result; it is
not acceptance evidence for the intended full implementation. All reported
wide/baseline timing ratios are withdrawn as design-selection evidence. The
compiled structure sizes and sidecar allocation sizes remain valid independent
layout measurements.

A corrected study needs exact emitted indexing tied to the stamp stride,
defined full/low reset semantics in every path, and deterministic controls
which seed high eras and old proof/token words at both the actual candidate
and potentially aliased cells. Cover even and odd high cell indices, both
FNEW starts, actual token NONE generations, a non-NONE exact token refusal,
and reuse after old lifetime owners have gone. The original four-shift build
must fail those controls. Any corrected runtime must have fresh source/object
and binary hashes and new strict/stock results before performance is revisited.

The optional overflow-sidecar reviewer has this inventory. Keeping the normal
entry exactly 16 bytes preserves the existing stride and token offset, but
does not by itself solve reused promoted cells: VM TNEW and emitted FNEW reset
inline state directly. Either every reuse reset must reset the extension under
the correct exclusive lifetime, or wide authority must remain monotone for the
mapping/cell and promotion must invalidate it before exposing the inline
sentinel. That is a separate design decision; no sparse sidecar is implemented
by this study.
