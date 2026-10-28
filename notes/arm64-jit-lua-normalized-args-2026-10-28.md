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
call/return and x86_64 parity remain incomplete. The focused assertion profile
passes; the frozen four-profile and complete gates are pending at this commit.

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

A cdata-return equality consumer remains interpreted across eight attempts,
with correct results. Its ISEQV comparison aborts in the existing FFI recorder
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

## Remaining work

Callee branches, nested Lua frames, tail calls, varargs, multiple returns and
additional child exit source positions still need independent source/frame
proofs. General trace topology, iterators and broader FFI admission remain
incomplete. Earlier sparse/fixed-argument notes describe superseded arity and
numeric-only restrictions at their historical checkpoints.

The prior rooted-length post-full-GC native-witness failure remains unexplained.
Later passing matrices and 1,100 isolated repetitions do not establish a fix.
