# Rooted local-cell CGET/CSET on ARM64

Experimental ARM64 LOOP/FORL roots now record reads and writes of captured
locals through rooted helpers. This is the true closed-cell path, in addition
to the existing ordinary-local CGET/CSET aliases. The owning frame must have
materialized its cell before trace entry. Nested frames, side traces, open
cells and in-trace cell creation or promotion remain outside this certificate.

## Runtime and recording

The generated call passes REF_BASE and an exact local slot number. The helper
accepts only the executing physical actor's lua_State/TG. During native
execution the base must equal the TG's published jit_base and current L;
outside native execution it must equal L->base. Aligned stack bounds are checked
before reading the slot. Root-entry admission separately binds its frame size
and source prototype.

The helper enters SMR before loading the slot, leases its exact TUPVAL
allocation, and requires the acquired v pointer to equal the cell's own tv.
It then leases the visible payload and rechecks the frame, owner and exact
stack-to-cell edge before its effect. Reads release-publish the result into
the output root and publish/mark it before releasing the value lease. Stores
release-publish one complete TValue and apply the existing pair barrier to
that exact snapshot. Refusal has no cell/output effect; cleanup preserves
errno, roots and SMR. No lock, scheduler wait or native-phase release is added.

The recorder copies active shared TUPVAL operands as raw tags without
dereferencing their object header. Payload sampling uses the new leased
helper and an enumerated TG anchor, leaving all three native scratch words
unchanged. Ordinary local aliases retain their existing path.

## Independent admission

Both semantic and post-register-allocation passes separately bind each helper
to CGET.D or CSET.A, its exact constant slot, REF_BASE, source prototype frame,
scratch root, status guard and continuation. Store RHS provenance comes from
the source slot/snapshot. Read results retain the existing visible-value,
full-width register/spill and native GC-phase rules.

The incoming P32 cell SLOAD is a type guard only. It has no IR consumers,
register, spill, PHI, rename or snapshot value. Snapshots also cannot restore
anything into that cell's stack slot: the actual stack holds its identity
throughout the trace. Generic raw upvalue loads/stores remain rejected.

A refused operation resumes the original bytecode. A successful CSET retains
an exact next-PC SNAPCOUNT_DONE snapshot, including adjacent CGET setup and
loop snapshot copying. The existing rooted-continuation retention rule now
recognizes this helper. A later table type guard cannot replay the store.

## Initial validation

The ARM64 experimental assertion build passed direct helper tests and native
LOOP/FORL reads/writes for numbers (including negative zero), booleans, nil,
strings, tables, functions, threads, userdata, opaque cdata and lightuserdata.
Native exit callbacks counted 20 exits per warmed case and trace IR identified
the new helpers. The suite also checked distinct cells for successive
invocations, collection/reentry, a post-store type exit with an exact counter,
and a live parent cell shared with a peer closure. This is bounded concurrency
evidence, not atomic cross-thread read-modify-write or a performance claim.

Fifteen admission selectors passed independent semantic and post-RA mutations,
including source/index/argument/snapshot/layout corruption and malformed map
bounds. The original rooted-upvalue native suite passed on the same production
sources. Direct/native evidence is in /tmp/ljmt-cell-first-5.log; the corrected
fixture continuation and original upvalue regression are in
/tmp/ljmt-cell-existing.log. The assertion runtime/archive was bound to profile
9cb40dca382e5db3ddd3e073b1a174519d6abd3d3d31710910492ae1bf72b36f
in the first driver and
f76fcf817cc8cb99ec33193c99c1706efe51d531699a56a56a5f502c03725bd6
in the continuation (driver labels differ).

Initial iterations caught an overly narrow REF_BASE argument check, missing
completed-store snapshot retention, and a missing prohibition on restoring
visible values over the cell identity slot. Those production defects were
fixed before the passing evidence above. Fixture-only mutation/reset errors
were then corrected; later broad profile validation is recorded separately.

## Completed validation

Production implementation `8607c74c7769fd8599aaff923927f1842d44b1e7`
passed an uninterrupted frozen-source combined run. The driver verified that
HEAD and all tracked files were unchanged at completion. Four build jobs,
the selected macOS SDK compiler, deployment target 13.0 and ad-hoc signing
were used. `/tmp/ljmt-cell-full.log` is the driver record.

- The complete experimental gate passed, including the new cell contract,
  existing rooted values/upvalues, callback recording, scalar roots and sides,
  publication/lifecycle and all four M7 FFI profiles. Its detailed log is
  `/tmp/ljmt-cell-experimental.log`.
- ARM64 stock source and original-v2 import each passed all 509 cases:
  `/tmp/ljmt-cell-stock-source.log` and `/tmp/ljmt-cell-stock-import.log`.
- The signed x86-64/Rosetta build passed rooted stores, nil/no-metatable reads,
  bounded reads, raw nil stores, closed-upvalue helpers, the new direct cell
  helper fixture, the lightuserdata segment race, and both 509-case corpora.
  No imported atomic fallback was found in the runtime or inspected helper
  objects. Log: `/tmp/ljmt-cell-x64-regression.log`; bound profile
  `ae056eb004e415cc5fccc773fe462df35ece48d4b87bf22828a19b01f5c4b2c0`.
  This is targeted Rosetta regression evidence, not a native Intel performance
  measurement or a complete x64 umbrella.
- The complete bootstrap gate passed, including all 387 applicable stock
  cases, threading/TG roots, interpreter GC2, callbacks and interpreter FFI:
  `/tmp/ljmt-cell-bootstrap.log`.
- The driver restored an ordinary ARM64 experimental/assertion runtime with
  no test-helper defines. Native cells and the full rooted-upvalue suite passed;
  no `__atomic`/libatomic imports or function/recorded-hook test symbols were
  present. Log: `/tmp/ljmt-cell-development.log`; profile
  `6d21e069982e37c994b2b39076a8a24b7b4f3b2be8b832d14849339da9cff28f`.
  Object disassembly was retained at `/tmp/ljmt-cell-development-lj_func.asm`.

The standalone four-profile contract also passed before that combined run
(`/tmp/ljmt-cell-matrix.log`). A later test-only change,
`4cde29165d4b48eb71f0205c1e431cd41daac1ef`, added explicit raw-cell snapshot
references and NORESTORE mutations in both independent admission passes.
Snapshots are chosen after the cell guard, so a forward-reference check cannot
explain their rejection. The unchanged production implementation passed the
complete cell contract again, uninterrupted on that frozen test checkpoint:
`/tmp/ljmt-cell-final-matrix.log`.

Final four-profile runtime/archive bindings:

| Profile | Bound identity |
| --- | --- |
| ARM64 assert | `450c3922cb15bcc4d4b9fc40d9792cb951dfedf19a4e8762996858428db4c68d` |
| arm64e/BTI assert | `696fe384f2fb852394fca6ae616458552b39233188914133a7dacaece8fb36fe` |
| ARM64 release | `b9ac5ac592f16c0e629e37000acb0621e9284bb945e28eb0c44ac69b589fe0b5` |
| arm64e/BTI release | `4bf2385b85c95cd9c5ef68eb2506578f9cbedf42e2f3f194746127283dab598f` |

Each profile ran direct helpers plus native value/GC/concurrency cases, all 15
admission selectors, ordinary native cell execution, and the original native
closed-upvalue suite. The contract restored the ordinary ARM64 experimental
assertion build afterward. `tools/ci/arm64_jit_rooted_cell_contract.sh` remains
wired into the experimental umbrella.

## Remaining scope

The Darwin concurrent profiler-stop SIGPROF race and the earlier unexplained
120-second upvalue repetition timeout remain open as documented in
[the callback checkpoint](arm64-jit-callback-recording-2026-10-26.md).
The full general ARM64 backend remains incomplete.
