# ARM64 nil-terminated custom `ITERL` leaves (2026-10-29)

The ARM64 leaf source recognizer now permits one bounded terminal `RET0` as the cold arm of the existing single conditional branch. It still requires the canonical `RET1` arm, exact branch geometry, and existing one-result frame window. Semantic and post-RA replays independently check the terminal bytecode and frame/result continuation. The iterator replay additionally binds the LOOP snapshot's live visible key to the source register read by the body, then binds the child input/result to the unique correctly typed loop PHI. The runtime entry check is unchanged. At a LOOP body footer, the exact hidden-control and visible-key map slots must both contain the copied scalar value; mixed child-frame snapshots are validated separately because they may precede that copy.

The positive ARM64 fixture exercises a native `ITERL` root, calls the nested leaf with `n=1, limit=64`, and checks that the selected root's exit footer is exactly the leaf's terminal `BC_RET0`. It also checks result parity, a live MT peer, identity and short/full-GC/flush paths, and private semantic/post-RA mutations. The mutations directly cover the live LOOP visible slot, the selected PHI's input/result operands, terminal source bytecode, and the existing frame/source bindings.

## Validation

- `tools/ci/arm64_jit_iterl_contract.sh`: all four profiles passed: arm64 assert, arm64e assert, arm64 release, arm64e release. Each reports 92 native calls and 52/52 private semantic/post-RA rejections. Full log: `/Users/frityet/Scratch/lj-nil-final4-20261029/four-profile.log`.
- `tools/ci/arm64_jit_lua_leaf_contract.sh`: all four profiles passed, including existing generic leaf, nested-call, vararg, sparse-argument, and return-exit regressions. Each reports `native=16`, `semantic-negative=100`, `postRA-negative=108`, `cold-retries=0`. Full log: `/Users/frityet/Scratch/lj-nil-leaf-regressions-20261029/leaf-four-profile.log`.
- The same ordinary Lua probe, with JIT-disabled loader-chunk interpreter oracle, published one candidate `JITERL` root and recorded selected-root exits on ARM64 and Rosetta x86_64. It completed under a 30-second subprocess alarm. Exact logs and probe: `/Users/frityet/Scratch/lj-nil-ordinary-20261029/arm64-exact.log`, `/Users/frityet/Scratch/lj-nil-ordinary-20261029/x86_64-exact.log`, and `/Users/frityet/Scratch/lj-nil-ordinary-20261029/nil-iter-probe.lua`. Binary/archive hashes and the source diff are recorded in the same directory. The ordinary Lua `jit.util.tracesnap` observer mapped these exits to `ADDVV`; it did not identify a terminal `RET0` exit. The exact RET0 footer evidence comes from the focused ARM64 C fixture above, not from the ordinary Lua observer.
- The initial long diagnostic stopped at the alternate `leaf_other` call because that fixture closure returned a value for every key and therefore could not terminate. This was a fixture mismatch, not a VM failure. The saved log is `/Users/frityet/Scratch/lj-nil-long-run-20261029/focused.log`; the alternate candidate and oracle now both use the same nil-terminating conditional closure. The focused profile runner bounds each fixture subprocess to 300 seconds.

The ordinary x86_64 build emitted the existing `lj_api.c:879` unused-but-set warning for `topofs`; it completed and ran the probe successfully. Ordinary validation is a bounded topology witness; the four-profile ARM64 C test supplies the exact selected-root cold-return footer and mutation evidence.

## LOOP control-map follow-up

Independent review found that the child mixed-frame proof was not enough to
cover the LOOP/body footer: its snapshot is after `ITERL` copied visible A to
hidden A-1, and the next `ITERC` consumes A-1. The semantic and post-RA walks
now independently require the exact unflagged LOOP map entries for control
`iter.base+LJ_FR2` and visible key `iter.base+1+LJ_FR2` to reference the same
scalar IR value. The child mixed-frame reconstruction remains phase-specific
and does not assume those caller slots have already been copied.

The private-clone regression changes the control entry to an existing valid
numeric SLOAD, leaving the source/footer and visible key intact. Before the
check, semantic admission accepted this mutation. With the check, semantic and
post-RA admission both reject it. The fixture evidence records the genuine
footer at body PC 6 with `ITERL A=6`, map slots 7 and 8 both referencing IR
32778, and the following `ITERC A=6`; the altered slot points at the distinct
state SLOAD 32774. The exact source/map diagnostic is retained in
`/Users/frityet/Scratch/lj-nil-control-slot-20261029/source/focused.log`.

The final four-profile ITERL contract passes 92 native calls and 56/56 private
rejections per profile at
`/Users/frityet/Scratch/lj-nil-control-slot-20261029/four/four-profile.log`.
