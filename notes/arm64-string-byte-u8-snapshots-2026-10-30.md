# ARM64 raw `string.byte` snapshot restoration — 2026-10-30

This slice admits one narrowly proven raw `IRT_U8` value in scalar root-loop snapshots: the result of an exact one-result `string.byte(s, index)` CALL, mapped either in its CALL result slot or in the immediate `CGET`/`MOV` copy. It does not admit arbitrary byte loads, side/child snapshots, or U8 values from other producers.

The semantic and allocated validators independently bind the snapshot bytecode PC and VM slot to the source CALL, require the readonly U8 `XLOAD` to read the proven `STRREF`, and retain the existing dynamic-index and string-length/bounds proof. The source helper refuses a reused result/copy register before the snapshot. The allocated pass repeats the source proof against its own IR/map view and validates a GPR or a one-slot spill.

No change to `snap_restoreval` was needed. Its existing integer path uses `irt_isinteger`, which includes U8, and restores the zero-extended byte as an integer TValue. The ARM64 XLOAD uses `LDRB`; spill restoration reads the 32-bit integer slot. The generic snapshot checker continues to reject unproven U8 values.

## Evidence

- Matched ordinary probe with no trace/dump observer: both saved x64 and ARM64 binaries returned `8125.5` for candidate and interpreter oracle. x64 published trace 1 (31 IR instructions, 10 exits) containing `XLOAD`; ARM64 published no trace. Logs: `/Users/frityet/Scratch/ljmt-u8-observer-free-x86_64.log` and `/Users/frityet/Scratch/ljmt-u8-observer-free-arm64.log`.
- The earlier diagnostic dump confirms the x64 loop snapshot directly maps the raw XLOAD result at the comparison exit: `/Users/frityet/Scratch/ljmt-u8-single-probe.dump`. That diagnostic used `jit.dump` and trace event callbacks; the observer-free run above is the native parity witness.
- The C contract fixture keeps the threading peer active, proves a live raw-U8 snapshot in the selected LOOP root, exercises both comparison directions, cold/type/bounds exits, collection and same-code re-entry, and checks independent semantic-map and post-RA-map mutations. It also verifies allocated GPR rejection, a valid odd one-slot spill at the end of an aligned spill block, an out-of-range spill refusal, and a U8 rename into an FPR refusal. The spill and rename cases are private allocated-view mutations, not claims that the natural candidate allocator spilled the byte.
- Four-profile contract passed with the final fixture and source: `/Users/frityet/Scratch/ljmt-u8-snapshot-final-profile.log` (arm64-assert, arm64e-assert, arm64-release, arm64e-release).

The matched ordinary saved binaries predate this branch's admission change. The live-peer four-profile C contract is the post-change native/GC/exit evidence.
