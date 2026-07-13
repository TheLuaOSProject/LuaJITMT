# Terminal FREE/FREEING word fast path

## Motivation

After packed free-range preflights, a fresh frozen profile of pushed
`ffb765be` measured the former hot functions at only 1.38 ns/op for
`arena_clear_extent_range()` and 2.77 ns/op for `arena_set_free_run()`. The next
independent quarantine costs were:

```text
arena_quarantine_bitmap_ready (inlined)   3.51 ns/op
arena_quarantine_apply_bitmap (inlined)   8.67 ns/op
arena_count_live_cells                    9.68 ns/op
```

Pre-grace typed destruction leaves the common dead arena in an especially
strong state: every old block start has irreversible lifetime FREE and packed
sweep state FREEING, with no root or recovery owner. The scalar finish path
nevertheless reclassified every start, then scanned the entire applied arena
again to rediscover a zero live-cell count.

## Exact word certificate

For a 32-bit half of block bitmap `x`, a portable Morton expansion places each
bit at the even position of a 64-bit word:

```text
v = (v | v << 16) & 0x0000ffff0000ffff
v = (v | v <<  8) & 0x00ff00ff00ff00ff
v = (v | v <<  4) & 0x0f0f0f0f0f0f0f0f
v = (v | v <<  2) & 0x3333333333333333
v = (v | v <<  1) & 0x5555555555555555
```

`v | (v << 1)` is therefore the exact packed sweep word containing FREEING
(`11`) at each block bit and WHITE (`00`) everywhere else. This uses ordinary
portable integer operations and has no BMI/PDEP dependency.

A bitmap word is terminal only for a managed traversable arena and when:

- both raw root words and both raw recovery words are zero;
- all four raw lifetime words are zero (FREE); and
- both sweep words equal the exact expanded block halves.

Exact equality is important: occupied-pair summaries would incorrectly admit
LIVE (`01`) or RETIRED (`10`). Block-zero words pass only with WHITE sweep and
zero root/recovery/lifetime state across the complete word. Compile-time
geometry assertions bind the two 32-lane planes and four 16-lane lifetime
planes to this indexing.

Mark, late, READY, destructor identity, and cdata are deliberately not
certificate inputs. For a certified start the scalar classifier ignores mark,
accepts/consumes late, clears READY, clears destructor identity at old block
starts only, and retains cdata until free-run selection.

## Two independent uses

Before the clean generation commit, bitmap readiness may skip a certified
word. This authorizes no mutation. A racing publisher first dirties the exact
count/PENDING generation and therefore defeats the later clean CAS. A
block-zero readiness word remains the scalar no-op even for an unmanaged arena.

After the exact `CLOSED|SEALED -> SEALED` commit, bitmap application computes a
fresh certificate for every word; it never carries the readiness result across
the linearization point. For a certified word the scalar result is exactly
`live=0, freeing=block`, so the fast transform preserves the original order:

1. acquire the old mark and late publication edge;
2. `READY &= 0`;
3. release-publish `block = 0`;
4. clear destructor planes under the old block mask only;
5. release-publish `mark = (old_mark & ~block) | block`; and
6. `late &= 0`.

Cdata is untouched. The late value is not needed for classification, but its
acquire load is retained before structural mutation to preserve the scalar
ordering edge from release-published physical-free provenance.

Any failed word runs the unchanged scalar implementation and permanently
clears the `all_terminal` latch, while later certified words may still use the
fast transform. Plain arenas never enter the fast apply and always retain the
scalar live-cell recount.

If all 64 managed words independently certify after commit, every applied
block word is zero and every root/recovery/lifetime lane was zero. The ordinary
live-cell counter could then see only empty state 0 or mark-only free state 1,
so its exact result is zero. `hdr.live_cells = 0` is therefore safe without the
third arena scan. Sweep state is reset only after all block releases, exactly
as before.

Postcommit rescue admissions observe COMMITTED ownership. FREE is irreversible
for the old incarnation and rejects body/status rescue; no root, recovery, or
non-FREE lifetime can appear behind the certificate. A duplicate late OR races
the final atomic AND normally: it is either consumed or remains visible for
the sealed/reclaimed retry path.

## Focused coverage

Test-only wrappers are compiled solely under `LJ_ARENA_TEST_HELPERS`; the
production library exports no new symbol. The focused fixture covers:

- exact block-zero acceptance and rejection of stray nonblock sweep state;
- portable dilation at block bits 0, 31, 32, and 63;
- root, recovery, and non-FREE lifetime vetoes;
- rejection of a LIVE/FREEING mismatch at an old block start;
- fast READY/block/destructor/mark/late output, including preservation of
  block-zero destructor identity and all cdata;
- unchanged scalar retention for a WHITE start;
- forced scalar/nonterminal behavior for a plain arena; and
- the public quarantine-finish path, including exact `hdr.live_cells == 0`,
  reclaimed publication, and removal of READY/destructor identity.

## Validation and performance

The final source passed:

- `m3_gc2_recovery` in the normal and assert/paranoia configurations;
- all four `m3_gc2_paranoia` C oracles plus 509 JIT and 387 no-JIT Lua cases;
- `m3_gc2_worker_scheduler` in C and JIT/no-JIT Lua configurations;
- `m6_jit_gc2_readiness`; and
- forced-clean `m2_arena_sweep` and `m2_arena_gcsweep` builds.

The default production build was then restored. Five-run medians on the same
loop microbenchmark were:

```text
fork, active GC       305.83 ns/op
fork, stopped GC       94.17 ns/op
stock, active GC       37.72 ns/op
stock, stopped GC      20.73 ns/op
```

The active result is 4.22% faster than the pushed packed-preflight checkpoint
(`319.30 ns/op`) and 21.42% faster than the preceding typed-sweep checkpoint
(`389.19 ns/op`). Active-minus-stopped overhead improves by 5.71%. The fork is
still 8.108x stock with active GC and 4.543x stock with valid stopped-GC
controls, so these measurements justify the optimization but do not satisfy
the b1.2.0 performance gate by themselves.
