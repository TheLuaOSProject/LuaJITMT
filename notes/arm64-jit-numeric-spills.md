# ARM64 scalar NUM spill admission

The scalar LOOP allocator certificate now admits numeric spills. Recorder
semantics remain unchanged and independent: no new heap operations, helper
calls, frames or side-trace capabilities are admitted. This is a bounded
allocation increment, not general ARM64 JIT completion.

## Allocation and snapshot invariants

`ra_spill` allocates NUM values in even, two-slot spans; its slots are 32 bits.
The immutable post-RA certificate validates the complete span, not only its
first slot, against `SPS_FIRST`, `SPS_LIMIT` and the actual frame capacity.
The high-water mark includes both halves and must produce exactly the recorded
16-byte-aligned frame adjustment. Snapshot locations selected through RENAME
history use the original producer type for the same width/alignment checks.
RENAME itself remains a register-only record.

A 256-entry `IRRef1` owner table rejects overlapping unrelated spans, including
an INT in either half of a NUM. The stock allocator never releases spill slots;
this is not a whole-trace restriction on reusable register lifetimes. The one
intentional alias is a PHI and its right input sharing the same complete span.
Duplicate spilled PHIs with the same RHS are already rejected by stock
`asm_phi` as unsupported spill shuffling.

Spilled PHIs have `RID_INIT`, share the right input's spill, and have a spilled
left input. The left slot may differ: `asm_phi_copyspill` copies the right slot
to it on each backedge. Register PHIs remain unique and unspilled inputs retain
the existing register/loop-snapshot RENAME correspondence checks.

Final register metadata is not authoritative for a spilled input:

- `asm_phi_shuffle` saves a spilled left input and clears its marker. A later
  preloop rename may change its register without adding a RENAME record.
- `ra_restore` can spill the RHS after its PHI register is assigned and emit a
  reload into that register. `ra_dest` later selects the producer register and
  stores to the owned slot. That producer register can differ from the PHI.

The first over-strict candidate rejected existing INT pressure and then NUM
FMA pressure on these two cases. Source review and actual emitted execution
established the slot-based rules; no runtime or allocator behavior changed.

The owner walk also runs during root-entry post-RA revalidation. Its 512-byte
stack table is cleared lazily on the first spill, so a spill-free trace does not
need that clear. A narrow repeated no-spill post-RA benchmark (one million
checks per sample, five interleaved samples) measured medians of 133.9 ns with
an eager clear and 134.2 ns with the lazy clear on this host, indistinguishable
from sample noise. This is not a complete root-entry performance measurement;
spill validation still has runtime cost.

## Executed proof

`arm64_jit_numeric_spills_contract.sh` builds the fixture against the selected
experimental archive. `LJ_ARM64_NUMERIC_SPILLS_ARCH=arm64e` selects the native
arm64e/BTI fixture; `LJ_ARM64_NUMERIC_SPILLS_ARCHIVE` overrides the archive.
Both native architectures passed with VM assertions and trace helpers enabled.

The ten real-loop cases include:

| Case | Allocated NUM entries | Frame bytes | RENAME records |
| --- | ---: | ---: | ---: |
| 24 recurrences, 8 invariant inputs, swap | 1 | 0 | 4 |
| 30 recurrences, 4 invariant inputs, swap | 3 | 16 | 23 |
| 31 recurrences, 8 invariant inputs, swap | 8 | 64 | 26 |
| 32 / 36 / 40 / 48 / 60 recurrences | 5 / 21 / 37 / 69 / 117 | 32 / 128 / 224 / 416 / 704 | 1 |
| 36 recurrences, FMA enabled | 27 | 176 | 29 |
| 36 NUM and 26 INT recurrences | 21 NUM, 35 INT | 256 | 0 |

The main 36-value case requires spilled PHIs, register PHIs with spilled left
inputs, and at least two separately spilled NUM sources; those branches cannot
silently stop executing. The FMA case requires emitted fused instructions and
matches each differing-register spilled RHS's actual SP store and subsequent
SP reload into its PHI register, including paired instructions.

Every returned value is compared exactly with a separate interpreter function
after warmup, native reuse, profile/STOPREQ handshakes, resumed native entry,
full GC, and type-guard exits. The mixed case additionally exits an overflow
guard with live NUM spills. Flush must restore the original root bytecode and
retire the native trace.

Allocated-IR mutations independently reject odd/reserved/out-of-frame NUM
spans, unrelated NUM overlap, INT half-slot overlap, wrong frame high-water
marks, spilled-PHI register/slot mismatches and missing left spill state. A
valid final NUM span at slots 254/255 is accepted; starting at 255 is rejected.
The recorder independently accepts a location-only corruption that post-RA
rejects. The existing synthetic IR test adds valid fixed/dynamic/final NUM
spans and their adjacent malformed cases. Its unspilled PHI and RENAME
corruptions remain active.

Compatibility runs passed the existing integer pressure/lifecycle fixture on
ARM64 and arm64e/BTI, the scalar contract on both, and the full synthetic IR
contract with ARM64 execution plus arm64e/BTI `-Werror` compilation. Scalar
NUM pressure now requires native admission; read/write/allocation/call
negatives remain interpreted. The generic scalar guard-page checks remain.
An independent reviewer also ran 30 invariant/swap layouts and nine mixed
NUM/INT layouts with native entry/exit counters and exact multi-result checks.

The existing emitted PHI copy contract passed on both architectures. A separate
negative-control build reverted only the earlier `asm_loop` boundary marker
fix: `t-arm64-jit-phi-pressure.lua` then failed on warm accumulator 5
(3.2388728692345969 versus 3.0534774712330863). The normal candidate passes.
This establishes that the pressure test exercises the repaired paired-store
boundary rather than merely compiling a trace.

## Remaining boundaries

The stock backend can still reject complex high-pressure PHI rotations with
`NYI: PHI shuffling too complex`. This increment does not change that backend
limit or widen any heap/call admission. Full experimental umbrella validation
is separate from the focused checks recorded above.
