# Sole-main JIT automatic SWEEP diagnosis

The exact eb8 baseline fails because foreground rescue work is serviced too slowly under the cooperative SWEEP JIT schedule. Root preparation is complete. Traced hard checks and snapshot exits work, but each observed exit commits only 64 SSB entries before a fresh 50us native lease stops further work in that automatic batch. With no background worker, another roughly 512KiB of allocation precedes the next such service. Durable recovery grows to 258143 identities. This is a read-only diagnosis and source proposal; no runtime or fixture correction was made.

The reviewable alternatives and safety/accounting contract are in [PROPOSAL.md](PROPOSAL.md). The pending-root continuation proposal stays frozen and unimplemented. The separate canonical scheduler abort is outside this task.

## Exact inputs

Baseline tree: `/tmp/lj-gc-pending-root-design-20260905-blju2qsh/baseline`.

Archive: `/tmp/lj-gc-pending-root-design-20260905-blju2qsh/baseline/src/libluajit.a`, SHA256 `cbc7e955549f291850dd5693dce77ce1d1f56461ced87eacc77e069603880343`. This is the cbc7e955 eb8 default generation, not the earlier e7e3be fair-candidate benchmark archive. Runtime commit `eb8a5b2f9ce2fd6128f4dbeef25b03896b81cfcd`. All 225 source/generator inputs match ROOT's before-input set before and after this diagnosis. Twelve relevant full source files are copied in `source/` with their original line numbers.

The exact normal executable is copied from `/tmp/lj-worker-jit-matrix-20260905-a6_4rjri/results-baseline/automatic-retention`: SHA256 `da2762397f59fdd73903e15c1ff7c5767bf64e210b1fa2ed85902e95a7ff5437`. No helper defines. C fixture SHA256 `2e8e840fb4ba3a3b09168c06d828ff10ebafd41e9ff555b9737f34384fea3cf9`; Lua fixture SHA256 `519ebf714b0a33b9a436d3452a153a1bc3eea3322ebf0bf74d8e76fea4ab8cb2`. These are byte-identical copies. The prior 14-case manifest `e64ca159e9742fd23f2622617336aab3fed2eb05348a8b5f34d8a8a745dfceb1` was reverified unchanged (68 entries).

The prior runtime evidence remains the passing/failing oracle: combined 9/12 passes, matched eb8 1/2 passes, with sole-main JIT-on returning `INCOMPLETE_AUTO` at round 4 on normal/strict/ASan and eb8 normal. Normal candidate/baseline failure stdout was byte-identical. This new package contains six **GDB diagnostics**, not six additional runtime acceptance tests. Every inferior still returns original exit 2 after original cleanup; GDB itself returns zero on completed debug sessions. No alarm, external timeout or diagnostic error occurs.

## Real final-round state

`snapshot.jsonl`, `retained.jsonl` and `retained-v2.jsonl` independently preserve the round-4 endpoint: cycle 15 in SWEEP, completed count 14 against target 16 after exactly 262144 round allocations (286720 cumulative). Earlier rounds complete with counts 7, 10 and 13. The original cleanup subsequently completes count 17, reclaims all 16384 churn strings, verifies the 32 anchored strings and returns normally.

At the failed boundary:

- READY=1, root snapshot=1, root EOF=1, root cursor NULL; allocator prepare and sweep epoch are 15. The late pending-root head is a post-READY publication, not evidence that the initial boundary never ran.
- Global grey indices are equal and legacy gray/grayagain are NULL. Detached SSB has 896 remaining slots; private SSB is full with 1024 slots. The published SSB head is NULL; one detached node remains durable. Recovery count is 258143, failed flag zero; table rescan pending is 1 and marks-this-round is 147.
- Worker/cycle/handshake leaders, worker active, global SMR readers/reclaimer, TG table/string reader depths, TG native depth, FFI native depth, TG jit_base, recorder owner, finalizer owner/active, thread preparation and thread NEEDSCAN counts are all zero. HS epoch is 441 with no pending acknowledgement.
- Six traversable arenas remain in quarantine. The first is `0x7ffff7200000`, reclaim cell 616, retire epoch 439, publisher count zero beneath the existing closed-bit encoding. No arena completion occurs during the late 50 hard handoffs. Existing open-graph gates correctly prevent further reclamation.

`retained-v2.jsonl` enumerates the exact recovery cells across all 558 reachable allocator-list arenas (551 owned traversable, 6 quarantine, 1 owned plain); the 2048-arena diagnostic cap was not reached. It independently reconstructs **258143 PENDING recovery identities**, exactly matching the global count. Every one has block=1, READY=1, lifetime LIVE and root MEMBER. Header kinds are 258139 tables and 4 functions. No identity is in CONSTRUCT/MUTATING or a transient root state at that boundary. This distinguishes the retained graph from the separate constructor-deferral mechanism.

Every SSB slot is preserved in that JSON too: 42 unique identities among 896 detached entries and 48 among 1024 private entries. The exact next detached slot names table `0x7ffff7a5ba80`, arena `0x7ffff7a50000`, cell 2984, READY/LIVE/MEMBER/PENDING, 33 array slots and COUNTED table rescan. The pending-root head is table `0x7fffeeee9060`, arena `0x7fffeeee0000`, cell 2310, with the same completed publication states and array value 262144. The payload samples and full recovery-cell lists remain inspectable; no RSS inference is used.

## Compiled execution and the actual refusal

`handoff-events-v2.jsonl` observes all 50 cycle-15 hard exits. Each enters from `newtab_rooted` through `lj_gc2_flush_alloc_checkpoint`, with a nonzero JIT base, and then reaches `gc2_step_auto` with JIT base NULL and step limit 64. Each immediate bounded drain advances SSB count by exactly 64, recovery-drained by zero and arena-finished by zero. `hard-handoff-accounting.json` verifies every one of those 50 triples. Recovery grows from 7497 at hard check 1 to 255106 at hard check 50, then to 258143 at the final bound. Early useful work remains separately recorded: cycle 14 completes and cycle 15 prepares/reclaims arenas before compiled production outruns service.

`lease-decision.jsonl` independently proves executing trace code: caller PC `0x55552724ff78` lies in the executable `memfd:luajit-mcode` mapping, immediately after a generated call to `lj_tab_new1`; the adjacent native instructions and mappings are recorded. The entry also has nonzero JIT base and real hard-check count 1. This evidence exceeds the fixture's requested engine-enable flag.

The one-shot refusal breakpoint is at ELF offset `0x81a7f`, runtime address `0x5555555d5a7f`, exactly `gc2_jit_sweep_turn_deferred+0x3f`. The instructions immediately before it are `call lj_thr_now_ns`, `cmp %rbx,%rax`, `setb %al`, `movzbl %al,%eax`; the stopped instruction is `pop %rbx`, followed by `ret`. EAX is already 1 and RBX is the exact published deadline 50401592892190. Its live stack is `gc2_jit_sweep_turn_deferred -> gc2_worker_drain_inner -> lj_gc2_step_explicit -> gc2_step_auto -> lj_trace_exit -> lj_vm_exit_handler`; worker_active is 1 and jit_base is NULL. Thus the true refusal was decided by production instructions before debugger delay, with the ordinary owner still held.

## Diagnostic perturbation and bounds

All commands and GDB scripts are included. The inferior arguments remain `0 0 0 PEER_LUA`, `RETENTION_JIT=1`, the exact baseline `LUA_PATH`, no `ASAN_OPTIONS`, CPU affinity 0-15, 45s fixture alarm, 50s external timeout and original complete cleanup. GDB disables ASLR, uses no inferior function calls and performs no state/fixture/source writes. Breakpoints and memory reads alter timing and are explicitly separate from runtime tests. New output files preserve every generation:

| Diagnostic | Observed failure | Important limit |
| --- | --- | --- |
| snapshot | round 4, cycle 15, 258143 recovery | Boundary stops only; original logical endpoint. |
| handoff-events | round 3, cycle 13, 254362 recovery | Breakpoints were installed from startup; timing changed earlier execution even before logging was active. Preserved as a perturbed observation. |
| handoff-events-v2 | round 4, cycle 15, 258143 recovery | Event breakpoints enabled only at round 4; 50 exact handoffs, but early arena total differs from boundary-only run. |
| retained | round 4, cycle 15, 258143 recovery | Complete arena recovery-cell enumeration and queue frontiers. |
| lease-decision | round 4, cycle 15, 256139 recovery | One pause after committed refusal lets the lease expire before later steps and admits extra recovery work. It remains a diagnostic, not a repair. |
| retained-v2 | round 4, cycle 15, 258143 recovery | Adds complete READY/block/type verification for all recovery identities. |

The C/Lua fixture sources and production archives were never rebuilt or edited. No sanitizer GDB run is substituted for the preserved normal/strict/ASan runtime matrix. The observations support a concrete scheduling/accounting defect and the proposed bounded repair scope; they do not establish an infinite deadlock, general latency bounds, full string reclamation or completion of the overall lockless-GC goal.
