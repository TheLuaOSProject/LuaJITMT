# Direct-userdata pure-loop load reuse: bounded Linux/x64 study

The minimal candidate is sound within the existing pre-MT pure root-loop
contract. It changes only `src/lj_opt_mem.c`, adding two recognized field shapes:
a typed userdata SLOAD/KGC's UDATA_META, and TAB_NODE whose object is exactly
that typed UDATA_META FLOAD. Ordinary entry metatable/null/node/key/value/type
and method identity guards remain. Special-userdata receiver/subtype checks
remain. The table-valued `__index` result's own node vector remains repeated.
There is no eligibility, MT policy, polling, collector, recorder, flag-lifecycle,
callback, allocation, store-alias or ABI change.

Use `candidate-v2.patch`, SHA-256
`c53b908a2f6bd57294452ad9de58db34e25db1ac4414a90c1d78bda2d0df614c`.
The resulting `lj_opt_mem.c` is
`00787841d7305b758edacb399e16fe5494b9d923525baa5f6b9523af7acbfc2c`.
The guarded source baseline is e3428257 plus ROOT's exact recorder patch
(`lj_record.c` SHA `07116bc933781976c91453a9ca89a46aef4a81d80bf71ab2f6e5269383fcce87`),
equivalent to the runtime source committed as 9f68fa8d. All 224 tracked
runtime/generator inputs match the owner's guarded baseline; the only candidate
change is `lj_opt_mem.c`. Candidate normal/assertion/ASan inputs match each other.
`runtime-input-identity.json` records every byte identity. Shared files and builds
were untouched; this study made no commits.

## Why these two field shapes can cross the poll

The existing protected unroller already sets `J->loop_cdata_fload` only after
classifying the entire surviving original root body, before copied iterations.
Despite its name, that classification does not require a cdata instruction.
The candidate reuses this existing permission and leaves its lifetime unchanged.
It still requires a root, an already emitted LOOP followed by mode-zero XPOLL,
and the full shared-runtime predicate false at the actual fold, after fallible
snapshot allocation. XBAR and ordinary conflicting-field/table-store searches
still bound CSE. The private flag is cleared immediately after `lj_vm_cpcall`
on both success and error, before free/retry/rollback/propagation.

The new leaf predicate requires `irt_type(ud->t) == IRT_UDATA` and opcode SLOAD
or KGC. It does not accept a PGC/P64 address, upvalue load, HLOAD, pointer-derived
object or arbitrary table. The metatable FLOAD must itself be typed TAB. The
node predicate checks that same FLOAD and the same leaf again. There is no
recursive graph walk and no table-valued `__index`/`__newindex` expansion.
CSE still requires equal field and substituted receiver operands; a changed
loop-carried receiver does not become invariant merely because its type matches.
SLOAD's existing type guard or a trace-rooted KGC supplies the receiver identity.

The existing classifier rejects Lua stores, USTORE, metadata FSTORE, NEWREF,
allocations (including allocations that would later sink), opaque/helper/native
calls, XSAVE, profiling and unlisted operations. Its only accepted writes remain
NUM/INT stores into a direct typed cdata SLOAD/KGC's scalar payload after the
header at a positive bounded constant offset. Valid in-bounds FFI writes of that
form cannot change a userdata header, metatable pointer, table node vector,
method slot or trace root. This is the existing supported-FFI premise, not a new
bounds check for forged pointers or invalid memory accesses.

The accepted operations have the same reviewed direct x64 lowering as the prior
cdata proof, so this extension adds no callback, allocation or hidden GC-step
opportunity. Inlined Lua functions qualify only if their resulting IR meets the
same full-body rule. Owner mutation between entries is observed by the retained
entry chain. Failed polls/guards leave via existing snapshots; root reentry from
a side targets root mcode and runs those guards again. First attachment, global
GC-worker activation, phase-gate exclusion and profile delivery retain their
previous protocols, including the separately repaired mode-zero TG poll. This
is a reuse optimization, not new lifetime authority or progress machinery.

The prior independent proof was read from
`notes/evidence/jit-cdata-pure-2026-09-05/isolated/independent-review/final-review.md`.
Its first-attach progress counterexample predates the separate 8d342cd6 repair;
its warning that the global worker API does not universally flush remains valid.
No broader MT recording or remote-root wait claim follows from this patch.

## Native execution and mutation evidence

Each guarded/control, normal candidate, assertion candidate and Clang-ASan
candidate runs 175 positive processes (`*-validation.json`): ROOT's exact
44-kind/mode fixture in both JIT modes (88), 36 direct-receiver read/mutation
combinations in both modes (72), seven previous pure-cdata mutations, seven
previous excluded cdata bodies and the installed side-to-root case (15).
That is 700 positive processes, with no source failures. The original fixture
bytes are unchanged. The direct derivative passes the receiver explicitly and
uses constant member names, and adds actual IR shape assertions. Its nine
read modes cover method replacement/removal/nonfunction, metatable
replacement/removal, resize+GC, old-method lifetime and table-valued entry
mutation. Original post-mutation native exit and exact-result/call assertions
remain. Both JIT-off and JIT-on behavior are retained.

The direct warm roots retain one metatable load and at least one node load at
entry. Candidate copied bodies lose exactly that metatable and its immediate
node load. File/buffer/plain table-valued `__index` nodes remain copied. The
normal direct cost roots change CLibrary 29→23 IR, file/buffer 36→31 and plain
32→27; raw before/after IR and mcode are retained. The subtype FLOAD/EQ remains
inside file/buffer loops.

`negative-controls.json` adds 24 positive candidate/assertion/ASan processes:
seven late-effect userdata bodies retain their copied metatable proof for
allocation, Lua store, new key, indirect scalar write, table.clear helper,
CALLXS and libm call. Actual native roots, the named effect opcode and resulting
state are checked. The eighth body demonstrates the existing allowed direct
scalar cdata-payload store alongside a userdata lookup. Two deliberate controls
fail immediately and specifically: guarded source fails the new hoist-shape
assertion, and disabling native reentry after warming fails the original-native
exit witness. Neither is claimed as a positive process.

There are 18 additional valid lifecycle runtime processes across the three
candidate variants: phase gate and global worker (2), protected OOM (1), first
attachment with/without LOOP (2), and profiling (1) each. Existing fixtures are
used unchanged; they exercise the shared flag/poll/lifecycle machinery, not a
new userdata-specific phase fixture. With the 24 effect controls, the qualified
functional total is 742 positive processes plus two intentional negative exits.
No new stock-suite or canonical registration run is claimed in this study.
ASan is Clang -O1 on the runtime, with uninstrumented host generators checked by
nm, and `detect_leaks=1:abort_on_error=1`; there are no suppressions.

## Costs and the excluded captured-receiver workload

The seven fresh alternating pairs on CPU 31 use default normal mixed builds,
best of five CPU times per process, a 20-million-lookup direct harness and the
unchanged original harness for controls. All 112 cost processes pass actual
warm native-exit checks; CALLXS controls also require actual CALLXS IR. Every
sample, not just minima or medians, is preserved in `cost-results.json`.

| Direct receiver, constant member | Guarded median ns | Candidate median ns | Median paired change |
| --- | ---: | ---: | ---: |
| CLibrary | 1.13965 | 0.68390 | -39.99% |
| File | 2.27900 | 1.04845 | -53.99% |
| Buffer | 2.27900 | 1.04845 | -54.00% |
| Plain userdata | 1.54515 | 0.91170 | -40.99% |

These are lookup microbenchmarks on a shared host, not method-call throughput,
concurrent MT, full-suite or stock-parity measurements. The whole-body allowlist
was deliberately not extended for the original captured-receiver harness:
UREFC/ULOAD occur in both lookups, and dynamic HREF additionally occurs for file.
The captured CLibrary cost remains 2.05115→2.05110 ns; actual CALLXS medians are
84.256→83.936 ns (median paired -0.16%, noise), and ffi_struct is unchanged at
0.0206 seconds/30 million. Normalized original-harness IR is identical for
captured CLibrary, file and CALLXS, accounting only for pointer addresses.

Captured file minima remain bimodal despite identical eligibility and IR/mcode
shape. The seven guarded values span 2.5083–9.56795 ns; candidate values span
2.50695–9.56785 ns. Its median shifts cannot be attributed to this optimization.
A separate owner-only C diagnostic snapshots the real table-valued `__index`
node/bucket geometry after warmup, before timing. All 16 fresh processes find
bucket 3, slot 3, depth 1 in a 16-node vector, including both 2.51 ns and 9.57 ns
runs. That falsifies the simple collision-chain-depth hypothesis. It does not
resolve address/layout or host-performance causes. Raw results/module source
and binary hash are retained; no slow samples were dropped.

## Rejected ideas and setup failures

- Extending eligibility to captured upvalues, dynamic HREF or general tables
  merely to recover the original harness was rejected. Those operations need
  their own proof; this candidate intentionally does not optimize them.
- Reusing the table-valued method's own node vector was considered but left
  out. The immediate metatable chain already has a substantial direct-receiver
  benefit without recursive provenance or further table-field exceptions.
- Initial v1 used nonexistent `irt_isudata`, failed compilation and produced
  no runnable candidate. V2 uses the existing `irt_type` convention. Its source
  was unchanged throughout successful normal/assertion/ASan validation.
- The first normal first-attach command incorrectly supplied `loop`; the fixture
  accepts no argument for that mode. That setup assertion and corrected runs
  are preserved separately.
- Initial strict/ASan C lifecycle builds omitted the runtime's helper macros.
  Their first-attach offset assertion failed. All of those mismatched C runs,
  including earlier passing phase/OOM rows, are disqualified. A header probe
  shows mt_active offsets 5536 without helpers versus 5568 with helpers; global
  sizes are 5584 versus 5616. Rebuilding fixtures with all matching flags passes
  every lifecycle case. Pure Lua tests never had this C-fixture mismatch.
- An initial IR parser admitted hexadecimal mcode address lines starting with
  four decimal digits; a second comparison exposed a decimal pointer operand
  in the native-trace-enter call. Final parsing restricts four-digit IR labels
  followed by whitespace and normalizes only those explicit pointer forms.
  Initial finalization also used an incorrect upstream manifest key; the final
  224-input comparison uses `candidate_matching_all_three` and succeeds.

`artifact-manifest.json` inventories text/source evidence and hash-only ELF
identities. The candidate is a bounded optimization handoff; ROOT's separate
CLibrary builtin-identity/cache/lifecycle work remains separate.
