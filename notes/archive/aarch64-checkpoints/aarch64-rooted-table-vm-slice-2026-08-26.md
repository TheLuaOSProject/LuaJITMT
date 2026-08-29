# Apple ARM64 rooted table/continuation VM slice (2026-08-26)

## Claim boundary

This checkpoint replaces the active JIT-disabled ARM64 interpreter's legacy
table indexing, mutation, traversal, and selected result-copy paths with the
fork's generation-safe rooted/pair helper protocols. It is the first half of
stack/root publication work, not the complete Stage 2 gate: call/return frame
topology, ordinary collectable-producing opcodes, fast-function result ranges,
and safepoint acknowledgement remain disabled or intentionally red.

The correctness-first ARM path deliberately gives up the stock inline table
fast paths. Those paths dereferenced `GCtab.array`/`GCtab.node` generations
without retaining them and were not valid while a GC worker or another table
operation could retire a vector. Performance recovery comes only after the
helper-backed behavior has a positive native stress gate.

## Implemented paths

`src/vm_arm64.dasc` now uses authoritative TValue roots and reloads `BASE`
after every helper which may wait, allocate, or relocate the Lua stack:

- `GGET` resolves the mutable function environment through
  `lj_meta_tgetenv_rooted`; `TGETV`, `TGETS`, and `TGETB` use
  `lj_meta_tgettv_rooted`; `TGETR` uses `lj_tab_gettv_rooted`.
- `GSET` uses `lj_meta_tsetenvtv_pair`; `TSETV`, `TSETS`, and `TSETB` use
  `lj_meta_tsettv_pair`. The pair helper owns resolution, the keyed CAS, weak
  handling, and table/value barriers.
- `TSETR` stores through `lj_tab_storetv_forvm_array`, reconstructs the table
  and source roots after return, and performs the required
  `lj_gc_pubtabtv_vm` post-store barrier. `TSETM` uses the self-contained
  `lj_tab_storetvn_forvm_array` range protocol.
- `ITERN` uses `lj_tab_itern_rooted` and re-reads the following loop bytecode
  after a possible retry; fast `next`, `rawget`, and `ipairs_aux` use rooted
  point/pair readers.
- table `LEN` uses `lj_tab_len_rooted` rather than a naked structural scan.
- `cont_ra`, both `cont_cat` result placements, terminal `CAT`, the
  `__newindex` third argument, table-length output, and synthesized iterator
  slots use `lj_state_stack_pubtv`. Completed rooted reads additionally
  invalidate the TG stack scan for scalar/nil overwrites.

The helper-call ABI treats x0-x17 as destroyed. Values which must survive a
call are held in the ARM callee-saved VM registers, and bytecode destinations
are re-decoded from `PC[-1]` after stack-relocating calls.

## Native validation

The following passed on the Apple ARM64 host with
`MACOSX_DEPLOYMENT_TARGET=13.0` and
`-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_DISABLE_JIT -DLUA_USE_ASSERT`:

- a clean build and link, with all intended rooted/pair/publication symbols in
  the generated VM object and no compiler atomic-runtime import;
- the vendored stock suite, 387/387;
- threading API, hook redispatch, coroutine handoff, and 4 x 80 FFI callback
  rounds;
- a 3,000-round JIT-off table/metamethod stress with one GC worker, full
  collections, dynamic/string/integer reads and writes, `rawget`, `next`,
  `ipairs`, length, globals, `__index`/`__newindex`, `table.remove`/TSETR, and
  constructor multires/TSETM.

The first version of the stress expected table `__len` semantics, but this
bootstrap uses the default Lua 5.1 compatibility mode where table `__len` is
not enabled. The corrected test exercises structural table length instead;
this was a test expectation, not a VM failure.

## Remaining blockers before safepoint acknowledgement

The interpreter still cannot safely certify and acknowledge a concurrent root
scan. The next slice must release-publish changed stack slots and invalidate
`TG.stack_dirty_epoch` for:

- call/tail-call setup, return/result copies, protected-return prepend, and
  fast-function result adjustment;
- taken test-copy, `MOV`, string/cdata constants, `UGET`, `FNEW`, `TNEW`,
  `TDUP`, `ITERC`, `VARG`, all `RET` forms, and `IFUNCV` frame construction;
- allocation results which create a new collectable stack root, using
  `lj_state_stack_pubtv` or an equivalent release/dirty/root sequence.

Separate active JIT-off debts remain in the ARM fast paths for
`getmetatable`, `setmetatable`, and distinct table/userdata equality: their
mutable metatable edges still use stock plain loads/stores. The Lua 5.2-only
`pairs`/`ipairs` metatable prechecks need the same treatment before that build
mode is supported. ISNEXT's control sentinel is non-collectable and needs no
root publication, but its bytecode despecialization stores must later use the
fork's bytecode publication helper.

Only after these paths and the full ARM root-publication runtime/artefact gate
are green may `vm_safepoint` begin acknowledging TG requests.
