# Main-TG arena stats scalar publication

`lj_gc2_stats_snapshot()` may run on a TG other than the main allocator's
owner. Its former `owned[]`/`needsweep[]` head loads and arena walks could race
owner relinking or unmapping. Acquiring each arena's next pointer did not pin
that arena. Its plain `binmask[]` read also raced the owner's plain writes.

The arena portion of the snapshot now acquires three owner-published scalars.
It never reads allocator list heads, bin nodes, or arena headers. The two arena
counts retain the existing independent 1,000,000 cap; the binmask retains its
existing value and meaning. Fields are independent diagnostic observations,
not one coherent allocator transaction or evidence permitting reclamation.
Settled counters exactly match their lists. During a transition a reader can
observe an older publication or a detached list's zero count.

## Publication and cost

`TGAlloc` appends `owned_count[]` and `needsweep_count[]` for both allocator
kinds. Their existing list owner publishes count changes with release stores;
there is no new lock, compare-and-swap, reader admission, or retry loop. Every
production `binmask[]` write now uses a release store, including staging merge
and rollback. Same-owner binmask reads remain plain loads. The ordinary bump
allocation path receives no counter update; arena insertion/removal adds O(1)
work. Operations that already walk lists count their work in that same pass.

The existing GC address-space check bounds distinct 64-KiB arenas below
2^32 even with 47-bit addresses. The uint32 counters cannot overflow for valid
allocator lists. Increment and nonempty-pop helpers fail closed on an
impossible overflow or underflow. Existing malformed-tail repair branches
publish the count of the list actually retained.

| Existing mutation | Scalar update |
| --- | --- |
| Fresh arena allocation | Increment owned after linkage. |
| Reclaimed adoption | Increment owned only after successful OPEN; a rejected OPEN leaves the old count. |
| Sweep preparation/retry | Publish zero for detached owned/NEEDSWEEP; count the prepared list in the existing pass, including reclaimed inputs. |
| Quarantine detach | Decrement NEEDSWEEP or publish zero at the end/repaired tail. |
| Sweep abort/restore | Increment owned and pop NEEDSWEEP after successful OPEN; failed staging restores bins/mask without changing counts. |
| Legacy scalar sweep | Pop NEEDSWEEP, account any duplicate owned unlink, then increment owned after linkage. |
| Dead-owner transfer | Add each existing transfer pass's moved count to the destination; clear source counts on detachment. |
| Partial allocator fini | Count retained arenas in the existing unmap pass and publish both retained-list counts; clear binmasks. |
| Complete fini | Reset the allocator only under its preexisting terminal lifetime contract. |

## Layout, initialization, and lifetime

All previous `TGAlloc` member offsets remain unchanged. On this Linux x64
build, `sizeof(TGAlloc)` increases from 672 to 688, `sizeof(TGState)` from
30,832 to 30,848, and `sizeof(GG_State)` from 42,224 to 42,240 bytes. Generated
VM offsets are rebuilt from the headers; downstream TG member offsets move
by 16 bytes.

The whole-allocator memset in `lj_arena_alloc_init()` is private initialization
or a reset after joined terminal destruction. A remote scalar reader must
retain the allocator's lifetime and must not overlap that memset. The main TG
is embedded in `GG_State`; `g->main_tg` is assigned during initialization and
does not change during a valid running universe. `close_state()` joins readers
and finishes subsystem/TG teardown before finalizing the main allocator and
freeing GG. Calling the snapshot concurrently with universe destruction is
outside this API's lifetime contract. Generic count accessors do not pin a
detached TG on the caller's behalf.

The bootstrap assignment `GG->main_tg.alloc = boot_alloc` copies every
preexisting arena count together with its list heads before publication.
Resetting the private `boot_alloc` afterward does not alter that copy.
`lj_tg_init_thread()` zeroes a new TG before publishing it. Adoption and restore
use private zeroed staging allocators, then copy only bins back to the live
allocator. These are all production whole-allocator/TG initialization, copy,
and reset sites; no concurrently observed scalar is overwritten by a live
whole-struct copy.

## Verification

The new `m9_gc_stats_arenas` canonical fixture exercises real allocator
operations, checks settled scalars against owner-only list/bin observations,
and covers both allocator kinds:

- Fresh allocation, initialization, PREP with a held reader and successful
  retry, sweep restoration, legacy sweep, and successful reclaimed adoption.
- A test-only OPEN pause lets a real COMMITTED rescue enter after staging but
  before OPEN. Both failed adoption and failed restore keep the previously
  published count, restore their bins, and make later progress after release.
  The pause compiles away in production.
- Transfer with nonempty owned, NEEDSWEEP, quarantine, and reclaimed source
  lists, plus existing destination owned and NEEDSWEEP lists.
- Partial fini with a reader retaining one owned and one NEEDSWEEP arena,
  followed by release, terminal reconciliation, and complete fini.
- A root-empty synthetic GG embeds a real allocator. The remote production
  snapshot runs after all five owned/NEEDSWEEP arena mappings become
  `PROT_NONE`, proving it does not touch those nodes. A separate concurrent
  reader runs across 128 owner prepare/restore rounds. This isolates allocator
  evidence from unrelated Lua root-spine traversal.
- Explicit synthetic counts exercise the unchanged cap without allocating a
  million arenas. Real `luaL_newstate()` checks bootstrap/main-TG counts.

Existing fixtures that manually inject or temporarily detach allocator lists
now seed their diagnostic counts explicitly. They do not introduce a second
production mutation mechanism.

Passed in an isolated Linux x64 build:

- `m9_gc_stats_arenas`, including clean helper build, strict C fixture compile,
  execution, and restoration of the normal build.
- Direct new fixture and `t-arena-gcsweep` with arena/GC2 test helpers and
  internal assertions.
- Standalone `t-arena-sweep`, `t-arena-hugetab`, `t-arena-alloc`,
  `t-arena-realloc`, and `t-arena-allocf` with arena helpers/assertions.
- Public `t-gc-stats.lua` with JIT disabled and enabled.
- Negative control: changing only the snapshot reader back to the former
  list-walking implementation makes the protected-arena fixture exit with
  SIGSEGV; the scalar reader passes that same fixture.
- Target-only GCC ThreadSanitizer build of the complete fixture: pass with
  `halt_on_error=1 second_deadlock_stack=1`. Host build tools remain
  unsanitized. Target flags are `-O1 -g -Werror -Wno-error=tsan
  -fsanitize=thread -fno-omit-frame-pointer`, with sanitizer linker flags and
  `LJ_ARENA_TEST_HELPERS`/`LUA_USE_ASSERT`. Only the established
  `atomic_thread_fence` instrumentation warning is demoted. Replacing only the
  stats reader with the previous implementation and running the fixture's
  `concurrent` mode reports the exact `alloc->owned[]` read/write race between
  `lj_gc2_stats_snapshot()` and `lj_arena_alloc_prepare_sweep_kind()` and exits
  66. These scoped results are not a claim about every runtime race.
- `git diff --check`.

The final combined assertion build also exposed an unstated placement
assumption in `t-arena-gcsweep`: the typed-destructor fixture selected any
same-arena triple but expected an LFUNC0 batch followed by one mixed
LFUNC1/CLOSED_UV batch. A valid triple at cells 2323/2326/2329 is in one lifetime
word, so both the leaf-guard build and a control removing only that guard
correctly commit one batch for three objects and fail the old two-batch
assertion. A cross-word pair also invalidates later batch-rejection fixture
assumptions. The original failing process's cell positions were not captured;
the valid same-word control demonstrates this failure mode independently.

Fixture selection now explicitly requires LFUNC0 in a separate lifetime word
from the LFUNC1/CLOSED_UV pair and requires that pair to share a word, with a
bounded retry to select that geometry. All exact batch/object counts, byte
accounting, body preservation, and grace checks remain unchanged. The
corrected full fixture passed ten runs with the combined leaf guard and ten
runs with a single-object pre-leaf control against the same assertion archive.

The isolated source contains stability commit `09d09e63`, the retained sweep
admission optimization later committed as `09cef065`, and this stats change.
It predates the separate leaf-publication guard. An additional
`t-arena-state` pilot on that source exceeded 30 seconds; subsequent lifecycle
pilots did not run after that timeout. This is unresolved in this evidence,
not a pass or a claim that the stats change caused it. The final combined
leaf-guard build must check it separately.

Local runtime/source hashes are recorded in
`/tmp/lj-gc-stats-20260904-0aovdm_t/runtime-manifest.json`. This work proves the
allocator portion's scalar access contract; the snapshot still performs its
existing bounded root-spine diagnostic walk. It does not establish complete
collector locklessness or new platform/release readiness.
