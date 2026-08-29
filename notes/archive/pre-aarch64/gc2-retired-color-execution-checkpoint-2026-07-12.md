# GC2 retired-color execution checkpoint, 2026-07-12

This checkpoint tightens the GC2-only runtime invariant without editing the
plan files. It is deliberately bounded: the retired marker/sweeper entry graph
must remain physically absent, and three surviving color-driven destruction
paths are removed, but the compatibility state byte, header color bits, and
all of their remaining bridge users are not deleted wholesale here.

## Invariant and plan divergence

For supported x86-64 Linux, macOS, and Windows target artifacts:

- GC2 is the only product-runtime collector and the only authority that may
  decide object liveness or destruction;
- string-table topology maintenance cannot opportunistically run a color
  sweep;
- closing an upvalue cannot free it from an `isdead()` verdict;
- the unused exported whole-arena color sweep cannot be linked or called; and
- retired marker/sweeper entry-point names are denied in target source,
  ordinary objects/archives, and amalgamation objects/archives.

The symbol gate is an extension beyond the narrative runtime test in the
plan. It makes physical absence a build property instead of relying on a
workload to demonstrate that the old entry graph was not reached. Header
colors are treated as compatibility metadata, never as a liveness verdict.
This divergence improves the stated GC2-only safety objective. No file under
`plan/` was changed.

## Removed executable behavior

### Security string rehash is topology-only

`lj_str_rehash_chain()` no longer checks `GCSsweepstring`, calculates
`otherwhite()`, recolors strings, or calls `lj_str_free()` while rechaining a
collision bucket. Its only job is now to switch primary hashes to the keyed
secondary hash and publish the new bucket topology.

The regression constructs an overlong primary chain under the string sweep
topology claim, gives every member the compatibility other-white value, sets
the compatibility state to `GCSsweepstring`, and triggers secondary rehash.
Every original body must retain its type, bytes, secondary-hash state, and
canonical pointer identity. The removed implementation freed those bodies.

### Closing an upvalue always transfers it to GC2 ownership

`lj_func_closeuv()` no longer asserts a nonblack header and no longer calls
`lj_func_freeuv()` when `isdead()` observes the other white. Every cell is
removed from the thread-open chain and global open-upvalue ring, closed, put on
the pending ownership/root path, and passed through the GC2 value barrier.
GC2's mark domain is the sole liveness authority.

The pending-root regression now forces the open upvalue to the other-white
header value before close. It verifies that the cell closes, preserves that
irrelevant compatibility color, appears on the per-TG pending chain, flushes
to the ownership spine, and remains the same body. The old branch freed it
instead of closing it.

### Unused whole-arena color sweep deleted

`lj_gc_sweep_gc2_all_arena_bodies()` and its public declaration are deleted.
Its `unmarked_only == 0` branch walked every arena allocation, applied
`isdead()`, inferred an object destructor, unlinked the body, and freed it.
There was no production caller, and retaining an exported latent color sweep
contradicted the GC2-only invariant.

The remaining arena helper is explicitly the GC2 unmarked-body bridge. It
flushes/repairs ownership roots, consults arena sweep state and mark bits, and
conservatively marks unclassified opaque allocations. It contains no color
liveness decision.

## Physical-absence gate

`tools/ci/gc2_no_retired_symbols.sh` scans target C/header/DynASM/assembly
sources and any target artifacts passed by its caller. It denies exact names
and compiler-clone/version suffixes for:

- `gc_mark`, `gc_mark_claim_white`, `gc_propagate_gray`, and `propagatemark`;
- `gc_sweep` and `gc_sweepstr`;
- the retired table/function/prototype/thread/trace/userdata traversers;
- `lj_gc_markobj`, `lj_gc_markobj_deep`, `lj_gc_preserveobj`, and
  `lj_gc_mark_trace_slot`; and
- the deleted `lj_gc_sweep_gc2_all_arena_bodies` export.

The artifact parser accepts ELF, Mach-O, COFF/PE, and archives through
`llvm-nm`, native `nm`, or an explicit cross `NM`. It normalizes Mach-O leading
underscores and COFF import prefixes, and it fails closed on a missing or
unreadable artifact.

The `m3_gc2_no_legacy_runtime` case now performs both of these sequences:

1. ordinary build: scan target source, `lj_gc.o`, and `libluajit.a`, then run
   the JIT/interpreter/weak/threading/full-GC2/close workload;
2. amalgamation build: scan target source, `ljamalg.o`, and
   `libluajit.a`, then run the same workload against the amalgamation archive.

The suite restores an ordinary default build even when either sequence fails.

## Explicit host/minilua exception

`src/host/minilua.c` deliberately retains its stock Lua collector and is the
only source excluded from the retired-symbol source scan. `minilua` is a
build-host bootstrap interpreter used to run DynASM and generate build inputs.
It is neither linked into `luajit`, `libluajit`, the Windows DLL, nor any
supported target runtime artifact. Its collector manages only the transient
build tool's private heap.

This exception does not cover target generator input: `vm_x64.dasc`, the rest
of target source, and generated `src/host/buildvm_arch.h` are scanned. CI also
passes target artifacts only; it intentionally does not pass the host
`minilua` object or executable to the artifact scanner. Replacing or trimming
the bootstrap interpreter can be a later build-tool cleanup, but it must not
be confused with permitting the old collector in the shipped runtime.

## Validation performed

All builds and runtime tests below used an isolated source snapshot so other
agents' concurrent build products were not disturbed.

- Linux/GCC ordinary build passed with warnings enabled. Source, `lj_gc.o`,
  and `libluajit.a` passed the retired-symbol gate.
- `m3_gc_root_pending` passed the adversarial-color upvalue close regression.
- `m5_strtab_cas` and `t-strtab-rehash` passed the adversarial-color secondary
  rehash and existing canonical identity/SMR regressions.
- The integrated `m3_gc2_no_legacy_runtime` ordinary and amalgamation builds,
  symbol gates, JIT/GC2 workload, worker survival, sticky trace, and
  `lua_close()` all passed. Each workload completed roughly 360 GC2 cycles.
- Linux/Clang ordinary build, target symbol gate, pending-root fixture,
  string-table fixture, and GC2-only runtime fixture passed.
- A target-only Clang ASAN+UBSan build at `-O1 -g` passed the pending-root and
  string-table regressions with leak detection disabled. The standard LuaJIT
  intentional `function`, `pointer-overflow`, and `shift` UB families were
  disabled; alignment and the remaining UBSan families stayed enabled.
- Windows x86-64 UCRT default and amalgamation builds passed. COFF object,
  PE DLL, import archive, and amalgamation object scans passed using
  `x86_64-w64-mingw32ucrt-nm`. Wine passed JIT/GC2 allocation smokes and the
  full `t-gc2-no-legacy-runtime` C workload against the amalgamation DLL.
- macOS x86-64 default and amalgamation builds passed with osxcross Clang.
  Mach-O object/archive/executable scans passed with the osxcross `nm`.
  Darling passed JIT/GC2 allocation smokes and the full
  `t-gc2-no-legacy-runtime` C workload against the amalgamation archive.
- `git diff --check`, shell syntax validation, and the source-only symbol gate
  passed.
- A negative artifact probe against the deliberately excluded host `minilua`
  executable failed as intended on its local `propagatemark` symbol.

A stricter broad UBSan run is not claimed: after the two focused fixtures had
passed, the broader JIT workload reached the pre-existing intentionally
misaligned load in `lib_jit.c:1054`. Likewise, instrumenting host minilua and
buildvm themselves exposes stock generator UB/leak reports, so sanitizer flags
were correctly applied to target objects only. Neither observation is caused
by this checkpoint, but both matter when interpreting future sanitizer jobs.

## Remaining target color/state slices

Physical marker/sweeper absence does not mean the header-color migration is
finished. A fresh supported-x64 audit found these remaining slices:

- Object constructors still write `curwhite()` in C, x64 `TNEW`, and traced
  `FNEW`; state initialization still installs `currentwhite`, and debug/API
  assertions still expect white newborn objects. These preserve current
  header/VM assumptions but must eventually become neutral initialization.
- `func_finduv()` and `ffi.typeinfo()` still call
  `lj_gc_resurrect_if_dead()` before a GC2 mark/publication operation. The
  bitmap action is authoritative; the other-white mutation is a remaining
  executable compatibility side effect to remove.
- x64 interpreter table stores and the x64 JIT `asm_obar()` retain
  `LJ_GC_BLACK`/`LJ_GC_WHITES` fallback tests around GC2 barriers.
  `tab_tsetm_barrier_needed()` has the same black-header fallback. Production
  target code no longer blackens headers, so GC2 mark/phase/TG gates do the
  real work, but the color branches remain executable under adversarial bits.
- `lj_str_resize()` still treats `GCSsweepstring` as a no-resize condition.
  Production assigns only `GCSpause`; the real exclusion is the atomic string
  topology owner. The compatibility-state guard can be removed after all
  synthetic users migrate.
- `lj_trace_free_gc()` still reads the `LJ_GC_SFIXED` bit from
  `currentwhite` to choose terminal publication behavior. Production never
  assigns that bit; an explicit GC2 shutdown/retirement mode should replace
  this latent color-era convention.
- `lj_gc2_publish_idle_threshold()` and retired-SMR readiness still require
  `GCSpause`, and sweep close republishes `GCSpause`. These are compatibility
  state observations layered on GC2 phase/worker gates, not an old collector,
  but they keep the `GCS*` enum/state field alive.
- `lj_ir`, API/state assertions, `lj_obj.h` debug consistency, and the GC2
  paranoia root oracle still interpret white/dead headers. They are diagnostic
  rather than runtime collection authority and need GC2-native replacements.
- Weak/finalized/fixed/need-rescan meanings still share the historical
  `marked` byte. Those bits are live GC2 semantics and cannot be erased merely
  by deleting colors; the byte needs a deliberate ABI-preserving flag-layout
  migration.
- Non-x64 backend generators still contain their stock color barrier code.
  Those architectures are outside the current supported target set, but a
  future portability expansion must migrate them before enabling the ports.

No remaining item above can start or traverse the retired collector entry
graph. They are the next color/state removal slices, not a reason to restore
the old GC.
