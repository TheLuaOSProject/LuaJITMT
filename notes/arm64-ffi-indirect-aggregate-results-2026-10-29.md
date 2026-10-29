# ARM64 Darwin indirect aggregate results

This slice adds one ARM64 Darwin FFI result class: ordinary non-union numeric
structs of 17–32 bytes with alignment at most 8, whose bounded CType walk finds
at least one integer or enum leaf. A positive integer leaf is required because
failure of the existing HFA classifier alone does not prove the type is
non-HFA. Over-aligned/ambiguous HFA layouts, unions, references, vectors,
variable-length members, unsupported leaves, varargs, and larger aggregates
remain interpreted. Existing direct HFA3/HFA4 and small direct aggregate
results retain their prior classification.

The recorder allocates and roots the declared CData result first, forms the
hidden result address from that exact CNEW payload, and records it as the first
hidden CALLXS argument. The CALLXS result is NIL; the returned value is the
rooted CData box. Semantic and post-RA admission independently bind the CNEW
type/size, payload offset, hidden first argument, CALLXS signature, snapshot
result, and absence of a post-call store. The aggregate is filled by the callee
through X8; visible arguments keep their original X0–X7 and stack positions.

The emitter reserves X8 from visible GPR argument allocation, including stack
argument temporaries, and checks already allocated visible values before setup.
The indirect target allocation excludes X8 as well; because `ra_alloc1()` may
retain an existing allocation outside its allowed set, emission explicitly
refuses an indirect target found in X8. The hidden pointer is moved into X8
before the visible argument moves execute in machine order. A native test uses
eight visible GPR arguments plus two stack arguments, alternate callees,
selected-root entry and finish evidence, full GC/re-entry, and interpreter
result/effect comparisons.

Private cloned-trace tests first admit the unmodified semantic and post-RA
proofs, then independently reject wrong result snapshot refs, CNEW CType,
payload base/offset, removed or reordered hidden argument, wrong CALLXS result
type, wrong post-RA payload class, and an X8 alias for a visible GPR argument.

The checked-in native fixture also covers a 32-byte `{uint64_t lo, mid, hi,
tail;}` return, including an alternate dynamic callee, full-GC exit/re-entry,
selected-root finish counts, stable code identity, interpreter comparison, and
exact C-side effect count.

Validation:

- The profile-bound native contract passed arm64 and arm64e assert and release
  configurations after adding 32-byte result coverage. Log:
  `/Users/frityet/Scratch/sret-profile-tmp/final-four-profile-ordinary-followup.log`.
- Separate ordinary builds (no trace/assembler/GC test-helper flags) passed
  matched 24-byte, 32-byte, and 24-byte/10-visible-argument probes on ARM64 and
  x86_64. Each case used a live peer, dynamic alternate callee, full GC, exact
  result and C-effect checks, a selected root containing CALLXS, selected-root
  exits, and unchanged mcode identity across GC and alternate calls. ARM64
  rows: roots 1/2/1, nins 62/62/75; x86_64 rows: roots 1/2/1, nins 67/67/75;
  each reported 38 selected-root exits and 512 C calls. Logs:
  `/Users/frityet/Scratch/sret-profile-tmp/ordinary-arm64-probe-final.log` and
  `/Users/frityet/Scratch/sret-profile-tmp/ordinary-x86_64-probe-final.log`.
- Ordinary ARM64 was built from this worktree with
  `-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT`
  and no test-helper flags. Its runtime SHA-256 is
  `680255525d48defca133822873fda43926ee2326a3c1b31d98815998f422fd39`.
  The ordinary x86_64 runtime was copied read-only from worktree revision
  `dae24b46aad9d4fc1d36cdce5b94d49d05d4743a` and has SHA-256
  `5998c9d47d84d717619d0ef50e339550c9cbe361fd2cbf3fcf8a6fadc4cfe04c`.
