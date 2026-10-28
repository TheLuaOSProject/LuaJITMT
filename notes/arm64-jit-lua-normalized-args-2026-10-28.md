# Normalized fixed Lua arguments on ARM64

## Scope

Fixed `BC_CALL` sites in the admitted linear Lua-leaf geometry now follow
Lua's existing argument normalization: retain the complete supplied argument
vector for caller replay, ignore extra arguments in the callee and fill missing
formals with nil. Visible nil, boolean, numeric, lightuserdata and GC values may
occupy unused parameters, be reassigned through raw CSET, or be copied/returned.
Arithmetic still requires numeric inputs at every source use, including dead
arithmetic and bodies without overflow snapshots.

This is an admission/recorder change. It adds no runtime lock, GC root scheme,
TValue representation, assembler write path or x64 special case. General Lua
call/return and x86_64 parity remain incomplete. Source checkpoint `31e865c0`
passes all four call profiles and the complete experimental gate. The complete
bootstrap gate passes on repeat after the retained callback timeout described
below; that first failure has not been explained or fixed.

## Independent proofs

`rec_call_setup` retains the complete incoming CALL snapshot without requiring
actual count to equal formal count. It still requires a root FORL, one fixed
result, fixed nonzero CALL C, zero prior child depth and a call window inside the
caller frame. `trace_arm64_lua_leaf_frame` independently checks the same source
window and uses actual count before FUNCF setup. Stock `rec_func_setup` remains
unchanged and performs the existing nil fill and formal-count maxslot update.

`arm64_lua_leaf_ir_args` and `arm64_lua_leaf_postra_args` each:

- derive actual count from the caller's source CALL;
- require every actual argument map entry, with no duplicate or unrestorable
  entries, and validate its exact visible TValue provenance;
- construct a separate formal vector initially filled with REF_NIL, replacing
  only formals for which an actual argument exists;
- independently walk the complete bounded leaf body to prove numeric uses,
  propagating copies and assignments and recognizing numeric literals.

The semantic type walk uses numeric flags; the allocated walk uses IR types.
Neither borrows the other's verdict. The whole-body checks matter for FP-only
leaves, which may have no child snapshot to trigger prefix replay. Existing
FP-only malformed caller maps replacing a number with a function are rejected
by both passes.

Child prefix replay uses each pass's own normalized formal vector. Raw CSET
requires exact visible old and new TValue references. These exclude internal
cell identities and prove the VM's raw-local branch. The existing independent
backward/forward liveness proofs still require CSET's old destination tag and
source value wherever the interpreter can read them after an exit. Source
predefinition, no child closures/upvalues, and exact caller/frame/return-link
checks remain unchanged.

Ignored GC arguments use the existing visible-value provenance and native
lifetime protections. TG native ownership, the GC2 native-phase gate and stack
restoration precede clearing jit_base. This change does not assume an ignored
argument is unrooted or unobservable: a different callee reached after an
identity guard must receive every original argument. Only the callee formal
vector discards extras. The existing same-reference, non-executable retained
CALL-map distinction remains bounded to the already proved child map; actual
identity-guard exits still require executable restoration locations.

## Focused evidence

`tests/t-arm64-jit-lua-normalized-args.c` passes 70 native shapes. These cover
extra numeric/primitive arguments, missing tail/all parameters, zero formals,
nil/boolean middle parameters, missing nil overwritten through CSET and twelve
payload types in unused, extra, overwrite and source-copy layouts. The payloads
are nil, false, true, INT, NUM, string, table, function, thread, userdata, cdata
and lightuserdata. Additional return shapes check omitted nil and eleven
payload types with a per-iteration equality check and numeric outer result.

The focused run records 504 native invocations: 112 child overflow exits,
70 callee-identity exits and 322 ordinary same-trace reuses, including 70 after
full GC. Native claims require successful entry, an exit-handler call and the
first exit from the expected parent. Child exits additionally match the source
PC, callee, return link and preloop/backedge partition. Every call checks stack,
cframe and native/TG cleanup. The replacement C callee observes exact actual
count, exact numeric values, primitive values and rawequal payload identity.

Private trace/prototype clones reject 1,466 semantic and 1,494 allocated
mutations, including 28 physical aliases. They test all supplied arguments,
both live VV resume operands, nil-vs-false normalization, the missing CSET old
tag, source/frame bounds and incompatible CALL forms. Eleven mutation classes
must run in both passes. There are 306 required-actual observations, 138 child
maps and 48 retained nonnumeric child values. An unused zero-formals backedge
CALL map with no identity guard and no child proof has no required argument
restoration authority; the fixture does not demand rejection of an irrelevant
omission there.

The ordinary Lua companion is deliberately a smaller subset: 31 publications,
60 overflow ranges and 182 exact-trace reuse checks. It makes no native claim.
The previous five C/Lua leaf/child/compound/argument/sparse suites pass again.
The original leaf ordinary fixture now expects its extra-argument case to
publish, leaving five deliberate refusal shapes.

A cdata-return equality consumer has no self-linked FORL publication across
eight attempts, with correct results. Its ISEQV comparison aborts in the existing FFI recorder
path. The other four cdata argument layouts pass natively. Assigning a
nonnumeric return to a loop-carried local also encounters the existing child
RET1-snapshot boundary. These probes do not expand that boundary or claim
general nonnumeric return consumers.

Evidence directory:
`/Users/frityet/Scratch/ljmt-lua-normalized-args-20261028`.
The baseline probes published only the exact numeric call; the other eleven
shapes all refused before this change and all publish afterward. Detailed logs
include `baseline.log`, `after-probe.log`, `normalized-final-focused.log`,
`existing.log` and the return-consumer probe dumps.

## Counterfactual validation

Eight separately rebuilt helper runtimes fail the relevant fixture as expected:

| Disabled or changed proof | Observed failure |
| --- | --- |
| Semantic full actual count | Missing extra argument accepted by semantic admission. |
| Allocated full actual count | Missing extra argument accepted by allocated admission. |
| Semantic whole-body numeric typing | FP-only function-valued argument mutation accepted. |
| Allocated whole-body numeric typing | The same independent allocated mutation accepted. |
| Semantic missing-formal nil seed changed to false | Missing-write trace cannot publish. |
| Allocated missing-formal nil seed changed to false | Missing-write trace cannot publish. |
| Semantic CSET reverted to numeric-only inputs | Missing-write trace cannot publish. |
| Allocated CSET reverted to numeric-only inputs | Missing-write trace cannot publish. |

The production header was restored byte for byte, rebuilt, and all six C/Lua
call contracts passed. The per-variant logs and source copies are retained
alongside `counterfactuals.log`. The test source uses numeric temporary plus
CSET for `b=a*3`; the missing nil destination is a live old-tag input after the
overflow resume, not an arithmetic destination exempt from restoration.

## Frozen matrix and main integration

Frozen source `31e865c06594c1ac43597138ce2b9ad9d31dac9c` passes the complete
experimental gate, all 26 focused table/FFI fixtures in four profiles, and all
four combined M7 FFI profiles. Each call profile repeats the 70 shapes,
504 native invocations, 112 child exits, 70 identity exits, 1,466 semantic and
1,494 allocated negatives. The complete bootstrap gate passes on repeat with
unchanged deadlines and assertions; its first timeout remains open below.

| Leaf contract profile | Profile hash prefix |
| --- | --- |
| ARM64 assertion | `4b20e5234549` |
| arm64e assertion with BTI | `b8549f58a5e7` |
| ARM64 release | `aff882c34d2c` |
| arm64e release with BTI | `482d836dbb12` |

ARM64 and signed x86_64 each pass both 509-case source and original-bytecode
import corpora. Ordinary ARM64 passes nine Lua contracts: leaf calls, child
exits, compound exits, fixed arguments, sparse exits, normalized arguments,
bitops, rooted cells and rooted upvalues. Signed x64 additionally passes the
fixed-argument, sparse-child and normalized-argument Lua companions. These
ordinary results allow interpreter fallback and are separate from native C
fixture evidence.

`v2.1` was fast-forwarded and independently rebuilt. Main passes both 509-case
corpora and all nine ordinary Lua contracts. Main and worktree ordinary ARM64
assertion executables share SHA-256:
`20fcbe8af5ab1f3035fabe8d25ab7544720201add375db1ba15bf2b78d7ee27a`.
Both pass signature verification, import no checked compiler atomic helper
and contain none of the checked test-helper symbols. The final documentation
commit does not alter the source used for these builds.

Logs are `matrix.log`, `matrix-continuation.log`, `experimental.log`,
`table-ffi-four-profile.log`, `bootstrap.log`, `bootstrap-retry.log`,
`*-stock-*.log`, `x64-lua-normalized-args.log` and `main-verify.log` in the
evidence directory. The first matrix stopped at the bootstrap timeout; the
continuation reran that complete gate and the remaining checks on the same
clean, frozen source, then restored the ordinary assertion runtime.

## Unexplained callback timeout

The first bootstrap gate stopped when `t-ffi-callback-auto-unwind.cpp` exceeded
its unchanged 30-second timeout. The suite reported signal 14, with no fixture
output or assertion. This is the existing TLS-less C++ callback error-unwind,
TG detach/reclamation and concurrent-close lifetime test. The original binary
and failure log are retained in `callback-timeout/`; the binary SHA-256 is
`0dd5c9798a772315a7b4d51cc44a71438ca7a406f5c6e2e9cd7ecb86c1483e84`.

The exact binary passed 100 repetitions followed by 1,000 more, typically in
0.07-0.09 seconds. A watchdog would have sampled a run lasting three seconds;
none did. A complete bootstrap retry also passed, with an external watcher
ready to sample callback fixtures after five seconds while preserving their
original 30-second deadline. It observed no stall. There was no production or
test change between the failed and successful complete bootstrap runs.

This establishes a passing repeat, not a fix or root cause. The affected
profile has JIT disabled, so the new Lua-call admission code does not execute
there. The source audit identifies possible waits in first-worker join,
auto-attach/state ownership, callback unwind/detach, shutdown's live-token
handshake, or close; it does not identify which wait occurred in the original
failure. A future recurrence needs a stack capture and callback/TG/shutdown
state before timeout. The prior rooted-length post-GC witness is a separate
unexplained observation.

## Remaining work

Callee branches, nested Lua frames, tail calls, varargs, multiple returns and
additional child exit source positions still need independent source/frame
proofs. General trace topology, iterators and broader FFI admission remain
incomplete. Earlier sparse/fixed-argument notes describe superseded arity and
numeric-only restrictions at their historical checkpoints.

The prior rooted-length post-full-GC native-witness failure remains unexplained.
Later passing matrices and 1,100 isolated repetitions do not establish a fix.

Source review identifies terminal RET1 child snapshots as the next bounded
return step: independently replay the complete prefix, prove the exact visible
returned value and its required slot, and retain all caller, frame, source and
physical-location checks. The current prefix/liveness routines deliberately
exclude that terminal position. This is a design direction, not implemented
or native-verified terminal-return snapshot admission.
