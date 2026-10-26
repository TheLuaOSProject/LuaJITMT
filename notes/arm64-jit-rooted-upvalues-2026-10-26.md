# Rooted closed-upvalue reads and writes

Experimental ARM64 LOOP/FORL roots now admit UGET and USETV/USETS/USETN/USETP
through rooted, nonwaiting runtime helpers. The certificate is limited to the
current Lua function and root frame. Reads and writes cover nil, booleans,
integer/double numbers, strings, tables, functions, threads, userdata, opaque
cdata and lightuserdata. Other targets keep their existing recorder paths;
the two runtime helper entry points are common code.

## Runtime and recorder

The helpers bind the operation to the executing physical actor, lua_State and
TG. SMR starts before either the mutable function root or its uvptr edge is
loaded. Exact allocation leases protect the function, upvalue and visible
payload. The acquired uv->v must equal &uv->tv: true open cells refuse without
an effect. The function root, owner and exact function-to-cell edge are checked
again before the operation. No raw upvalue pointer escapes to generated code.

Stores publish a complete TValue with release ordering and apply the existing
GC2 pair barrier to the exact value snapshot just stored. They do not reread a
cell that another writer could already have replaced. Reads acquire the cell
word and publish the result into the output root while its value lease remains
held. Both paths preserve OS error state and release all leases/SMR on refusal.
No allocation, cooperative wait or native-phase release is added. The existing
GC2 barrier may perform bounded worker conversion and CAS retries; this is a
lockless helper, not a fixed-instruction wait-free primitive.

Recorder sampling uses a temporary enumerated root anchor and leaves the TG's
three native scratch TValues unchanged. Generated calls use the current
function's readonly SLOAD and distinct TG-local TMPREF slots. Semantic and
post-register-allocation checks independently bind each call to its source
bytecode, upvalue index, value provenance, scratch layout and snapshots. Full
GC-value spill rules also cover a spilled current-function load.

A refused operation resumes its original UGET/USET. A successful store keeps a
SNAPCOUNT_DONE snapshot at the following bytecode, including across loop
optimization and adjacent snapshot merging. A later type-guard exit therefore
cannot replay that store. Those effect continuations remain excluded from side
traces; the existing FFI snapshot rules retain their previous behavior.

USETN preserves the exact numeric TValue in the prototype constant pool.
Dual-number source literals such as 73 and +0 usually have integer tags, while
loaded bytecode can contain an integral double. The recorder retains that
distinction and both validators require the matching KINT or bit-exact KNUM.
Tests create private, unexecuted prototypes containing an integral double and
negative zero before warming them. The source spelling -0 emits UNM/USETV, so
it is not used as a substitute for a USETN constant-pool test.

## Validation

The complete combined regression passed on 2026-10-26 from base
`294063441c7f3f42cf8e6f2422079feb19ab9f01`, with binary diff SHA-256
`38a3fbbad2d5d754bf81422969a44b9fab69484fac9cd1de51f78c41f757c103`
and production-source diff SHA-256
`8508ad33ddf61d0feacc5b7e6a97f139935cfa3500f8b03630f4f22b945aced1`.
The driver exited successfully after verifying the frozen HEAD and diff.
Documentation was added after the run. It passed:

- The complete uninterrupted experimental gate, including all four new upvalue
  profiles, all four existing rooted-GC profiles, side/root publication and
  lifecycle, 222 numeric mode executions, and all four complete M7 FFI suites.
- ARM64 stock source and original-v2 bytecode import: 509 cases each.
- Signed x86-64/Rosetta rooted table/read/nil/raw-store and new upvalue helpers,
  lightuserdata segment races, and both 509-case stock corpora. The runtime and
  affected C object files contain no imported compiler atomic fallback.
- The complete bootstrap gate: 387 applicable stock cases, interpreter roots,
  safepoints, tmpbuf, threading/coroutines, GC2 hard assist, ARM64/arm64e
  callbacks and aggregate/variadic FFI, plus x64 errno/result-lifetime checks.

The Apple SDK compiler, deployment target 13.0, two build jobs and ad-hoc link
signing were used. The worktree was restored to an ARM64 experimental assertion
runtime with no test-helper defines, profile
`03aef62fac99b0913c21b1a7763d8234091f354dc3ea3518f9e064bd615fe26d`.
The new contract is `tools/ci/arm64_jit_rooted_upvalue_contract.sh` and is wired
into the experimental gate. Its profile identities were:

| Target | Runtime | Profile SHA-256 |
| --- | --- | --- |
| ARM64 | Assertions | `c596c1f9f0943a2c82fee34eaf41ebf48a8bab23bfb2c8460b9dcbc5570825eb` |
| arm64e/BTI | Assertions | `4f2e08c56d15c6ac9f440a02f2a02248ee0ed2471ba866d298786ab788ce27be` |
| ARM64 | Release | `89d9ad99cf69c0c0e40e13ecb269f39c2a8f21a5eeef8679d0928e68f9f84c9d` |
| arm64e/BTI | Release | `d20e2cc774bb855a652b792ff075c4973c10cd7709c147409f22b67f4196013d` |

The preceding clean rawset checkpoint `29406344` also completed the combined
experimental/stock/x64/bootstrap run, recorded separately in
`/tmp/ljmt-postrawset-full.log`. The new run above validates the upvalue changes
on their own frozen source identity.

The new focused contract runs on ARM64 and arm64e/BTI, with assertion and
release runtimes. It covers:

- Direct helper values, GC retention, invalid index/type/internal-marker and
  genuine open-cell refusal, root aliasing, cell joining, errno and cleanup.
- Native read and write traces for every visible value class, LOOP/FORL,
  literal bytecodes, exact numeric pool representation and full-width function
  spills. Trace IR identifies the rooted helpers and exit callbacks prove
  execution beyond trace creation.
- Thirteen independent semantic/post-RA selectors, including private source
  opcode/index corruption, CARG/root/status/result mismatches, wrong numeric
  representation, invalid locations and exact continuation/prototype binding.
- A committed counter increment followed by a table-value guard exit: the
  interpreter completes the iteration with exactly the expected counter delta.
- Debug type changes and upvaluejoin invalidation, collection/reentry, and
  two workers publishing/observing shared table-valued cells while both enter
  their respective native traces. A single shared event attachment counts the
  two roots separately. This does not claim atomic cross-thread increments or
  sustained concurrency/performance coverage.

Logs and drivers remain under /tmp:

- ljmt-upvalue-focus-6c.log and ljmt-upvalue-focus-existing.sh
- ljmt-upvalue-full.sh and ljmt-upvalue-full.log
- ljmt-upvalue-experimental-closed-upvalues.log
- ljmt-upvalue-stock-{source,import}-closed-upvalues.log
- ljmt-upvalue-x64-regression-closed-upvalues.log
- ljmt-upvalue-bootstrap-closed-upvalues.log
- ljmt-upvalue-development-profile-closed-upvalues.log

## Later callback regression

After the uninterrupted validation above, repeated runs of the concurrent
reader/writer test found a `rec_check_slots` mismatch at process 56. A peer
legacy VM-event/profile callback could set global VMEVENT and suppress one
bytecode of an active recorder, leaving the recorder's function slot where
the VM had already returned a table. The later callback checkpoint replaces
that dispatch suppression with a TG-local marker and includes deterministic
before/after overlap fixtures. See
[`arm64-jit-callback-recording-2026-10-26.md`](arm64-jit-callback-recording-2026-10-26.md)
for the reproduction, fix and subsequent validation. The earlier passing run
above did not establish the absence of this intermittent recorder race.

## Remaining scope

This adds closed closure-upvalue operations, not general upvalue/cell JIT
support. Actual shared-cell CGET/CSET locals, open cells, closure/cell creation,
other root/call geometries and side traces retain their existing admission
limits. Ordinary non-cell CGET/CSET scalar aliases were already supported;
that does not establish native access to a real LJ_TUPVAL cell. General table
traversal/metamethods, remaining FFI ABI classes and the broader parity/stress
matrices also remain incomplete. The full ARM64 port is still in progress.
