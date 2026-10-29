# ARM64 ABI, calls, sides and iterator integration

The integration branch combines the validated partial-bank HFA baseline with
captured pair-return function calls, nested side traces, small integer stack
composites, naturally aligned HFA stack groups, literal narrow-stack argument
regressions, and nil-terminated custom iterators. The implementation remains
bounded; this checkpoint does not establish full x86_64 behavior parity.

## Production changes and review

- Fixed Darwin ABI plans preserve complete HFA groups and exact packed member
  offsets. Non-HFA small integer composites use eight-byte stack slots while
  reading/storing only their declared payload widths. Interpreter layout and
  both independent admission passes agree with the native C/AppleClang oracle.
- Function-entry traces can consume two results from a guarded captured child.
  The JFUNCF entry check validates the explicit callee frame extent without
  depending on a caller-owned `L->top`. A review fix preserves values read
  before the pair-result call overwrites their slots.
- Nested sides retain immediate-parent IR_BASE ancestry and the canonical
  root's allocation list. Publication, entry and retirement are checked at the
  supported depth. The ordinary probe binds exact root/direct/nested machine
  code and observes exits from the selected nested trace with a live peer.
- A bounded conditional iterator leaf can return nil through its final RET0.
  Review found a missing LOOP control-slot binding: a private clone replacing
  control with the state value was accepted. `c60f376f9` restores independent
  semantic/post-RA control==visible-key checks at the exact LOOP/body footer.
  Mixed in-flight child snapshots retain their separate liveness rules.

The literal-stack investigation required no compiler change. Its existing
typed U8/U16 carriers already execute natively; the proposed direct-KINT
experiment was removed. Only regression tests and corrected notes remain.

## Validation completed

At `31ae5f501`, the combined aggregate contract passed ARM64/ARM64e assert and
release, followed by the existing interpreter aggregate/vararg/parser-wait/GC
contract. At `c60f376f9`, the merged iterator and complete Lua leaf suites each
passed all four profiles. Iterator profiles verify 92 native calls, 56/56
private rejection cases, and the exact selected-root cold RET0 footer.

The ordinary matrix at `c60f376f9` passed on ARM64 and x86_64/Rosetta: 509 stock
source tests and 509 imported-bytecode tests per architecture, snapshot/getpos,
varargs, topology/inventory, aggregate and nested-side probes. The strengthened
one-float HFA peer selects its own JFORL root, checks CALLXS and exact native
code identity, and observes a selected-root exit after full GC while another
thread remains attached. ARM64's ordinary nil-iterator topology also has a
runner-owned JITERL root and selected exits. The ordinary Lua observer does
not establish the exact RET0 footer; that evidence comes from the C fixture.
Strict GDBJIT compilation of trace, assembler, C-call and C-recorder units
passed on ARM64 and ARM64e. Frozen ordinary runtimes/libraries and hashes are
saved with the matrix.

## Umbrella gate continuation

The calls/sides gate was resumed after two fixture repairs documented in
`arm64-calls-sides-integration-2026-10-29.md`: a global pause could stop a newly
admitted function-entry trace instead of the selected loop, and a two-slot
numeric fixture could flush its selected pair by heating another side. The
repairs target the selected trace and preserve its identity; production
handshake and cache policies are unchanged. The numeric fixture passed all
222 default executions after its repair.

The remaining umbrella stages, beginning with the exit contract, are running
at `3a14f2afc`. This is a segmented validation record across coherent source
checkpoints, not a claim of one uninterrupted gate run at the final revision.

Evidence directories:

- `/Users/frityet/Scratch/ljmt-calls-sides-integrated-20261029/` contains the
  initial/resumed gate logs and selected-pause/numeric-side diagnostics.
- `/Users/frityet/Scratch/ljmt-abi-continuation-20261029/` contains the merged
  aggregate, interpreter, iterator, leaf and ordinary matrix logs/statuses,
  tested source IDs, frozen artifacts, and the remaining gate continuation.

Remaining native differences include broader open-result Lua calls, deeper
side ancestry, recursion/return/stitch topologies, and additional aggregate
shapes. The next aggregate audit distinguishes x64 native witnesses from
signatures both targets merely execute through the interpreter.
