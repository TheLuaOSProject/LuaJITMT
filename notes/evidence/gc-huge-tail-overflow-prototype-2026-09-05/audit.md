# Reserved Huge-tail overflow authority, isolated Linux study

The tail variant passes the bounded authority, geometry, failure, lifetime,
ASan, and stock controls. It removes the separate Huge W calloc/free without
changing the proof-access protocol. The cost result is a tradeoff: ordinary
allocation timing is close in this sample, but a promoted object can fault in
an extra 4 KiB page, and 16 logical-size residues per 64 KiB quantum require
an extra 64 KiB of virtual reservation. This is a reviewable prototype, not a
production integration or a claim that the runtime is fully nonblocking.

This artifact is frozen at
`/tmp/lj-dense-huge-tail-20260905-djqhqfe8`. The base is
`28de50a622e489019fa22845d6454e029b210582`, plus the final dense-W candidate
patch SHA-256
`424a3f5ebcf908e51a4a54fe6faed00947fb1593135ca2e89f86d406d1b65e20`.
The paired calloc control and tail candidate have the same current arena,
scalar, and GC code at that base. Later tag-only meta and truthful FNEW fixture
changes were not overlaid on these frozen builds. A subsequent permanent-test
integration will use a separate tree; it must not inherit a broader validation
claim from this study.

`source-manifest.json` identifies the four changed production files and both
integrated patches. `source-inventory.json` verifies all 210 tracked `src/`
files in all five positive source/build variants: exactly `lj_arena.c/.h` and
`lj_gc2.c/.h` differ from the base. The VM and assembler files remain identical
to the base. Three patch dry runs pass. The exact current Huge flag assignment
still excludes `LJ_AF_EMPTY_RECLAIMED`; the first mechanical application of the
older dense patch failed on that changed arena context, and that failure is
preserved in `calloc-apply.log`.

The complete tail source patch is `tail-integrated.patch` (SHA-256
`dca24e13fdc47d886a1ffc07b42a604c4e430090fa5f2fd9b1f8111678a8ee4a`).
`tail-vs-calloc.patch` contains only the storage change in `lj_arena.c/.h`.
The source-only patch does not yet register durable CI regressions.

## Storage and lifetime proof

The inline stamp stays 16 bytes, token offset stays 8, GCAhdr stays 128 bytes,
and payload starts at mapping base plus 128. Dense small storage stays 128 KiB
per traversable arena. Small W remains monotone for the mapping/cell lifetime;
the common inline CAS64, VM/FNEW resets, token generation, admitted mode/era
confirmation, pre-sentinel W invalidation, and full namespace sticky veto are
unchanged from the dense candidate.

For every Huge allocation, including plain Huge, the size-only helper now uses
`M(s) = round_up(s + 128 + 16, 65536)`. It rejects sizes at or below the Huge
threshold and sizes above `(SIZE_MAX & ~65535) - 144` before addition or
rounding. Existing mapping span/address restrictions and HugeTab logical size
packing still apply; this arithmetic does not promise that enormous virtual
maps can succeed. Huge live_cells continues to describe the mapped extent.

Traversable Huge publishes W at `mapping_base + M(s) - 16` before any locator
publication. Fresh private anonymous Linux mappings already contain the full
zero W, so initialization does not touch its page. This depends on retaining
the fresh-zero mapping invariant; recycled dirty storage is not an admissible
replacement for this initialization. Plain Huge keeps a NULL W pointer even
though the universal size-only contract reserves the tail for it too.

The W address depends on physical mapping extent, not current logical payload
end. It therefore stays fixed across accepted same-extent direct reallocs.
Both direct and HugeTab realloc decisions call the single updated mapsize
helper. Replacement maps copy only the old/new minimum logical payload, never
W, and start with a fresh zero proof. HugeTab still stores exact logical size;
range and cdata lookup, counted reader extents, copying, and GC live-byte
accounting do not acquire authority over padding or W.

Realloc coverage has a deliberate qualification. Existing HugeTab allocation
realloc refuses published TRAVERSABLE objects. The promoted-W plus retained
reader test requires that refusal and unchanged proof, size, reader and BUSY
state. Successful promoted-W same-extent resize is tested only through the
caller-owned private direct allocator path. The plain Huge path separately
tests retained readers, replacement, old logical extent, deferred free and
handoff. These controls do not authorize published mutable Huge table resize.

W now shares the complete mapping lifetime. Both Huge unmap functions remove
the standalone free, but preserve the existing terminal/token/descriptor
certificate and errno behavior. Failed pre-publication insertion releases one
whole new M(s); old readers or external deferred free retain the old mapping
and W. Transfer keeps the same mapping and pointer. Terminal cleanup unmaps
once after its existing admission obligations settle. No interior tail pointer
is passed to free. Header-only exact-token completion remains W-blind even
when body-proof bytes physically exist in the same mapping.

No allocation occurs after a payload store when an inline stamp promotes.
Removing the Huge calloc removes one private allocation failure and allocator
dependency. The real small-sidecar failure still fails privately; Huge failures
are instead tested at mmap, checked geometry, and locator publication. The
tail does not make mmap, munmap, page faults, the plain-arena writer, or global
reclamation progress nonblocking. Exhausting the full finite W namespace
still retains the permanent global veto; this study does not clear it.

## Correctness evidence and limits

`t-huge-tail.c` and `targeted.py` are the new narrow storage fixture. Both strict
and Clang ASan builds pass geometry, payload, bounds, resize, and failure modes.
The geometry matrix uses both kinds, three successive mapping quanta, and nine
deltas around the new boundary. It checks complete advertised-byte writes,
alignment, exact map extent, zero W, plain NULL, arithmetic rejection and errno.
The 65,393-byte causal probe seeds W and then writes every payload byte.
At 20,000 bytes mincore proves the W page is initially nonresident before its
first proof load. Logical/range/cdata admission rejects payload end, padding,
W and the final mapped byte; a zero-length extent at logical end remains valid.

The fixture covers private resize, published TRAVERSABLE refusal, plain reader
handoff, failed replacement, failed HugeTab insertion, transfer and terminal
cleanup. The mmap64/munmap wrappers check one complete mapping unmap, including
failed new maps, and calloc/free wrappers require no Huge metadata allocation
or interior-pointer free. The first mmap failure probe wrapped only mmap and
missed this runtime's mmap64 symbol; its failed assertion and old source are
preserved in `targeted-1.log`, `tail-targeted.json.attempt1`, and
`t-huge-tail.c.attempt1`. Both wrappers are present in the passing final fixture.

The adapted dense authority controls pass for small and Huge, legacy and exact
token modes, old inline and wide scanners paused at proof completion, peers
finishing while a mode publisher is paused, and twelve forced era-rollover full
collections reaching IDLE with zero recovery and real descendant/weak checks.
Post-store promotion is tested while calloc is denied. Existing coalescing
controls retain full finite-namespace veto coverage. Deterministic functional
setups can suppress automatic scheduling before explicit full collections;
this is separate from the GC-enabled cost workload below.

Both strict and ASan runs pass emitted TNEW reuse at cells 1536 and 1537,
pending-token refusals for both parities, the existing TNEW fixture, and the
independent emitted FNEW high-cell reuse controls at both cells. The obsolete
full FNEW fixture that falsified allocator identity is not run here. The later
committed truthful FNEW helper/fixture is an integration prerequisite for that
full-suite claim; this study does not silently substitute a poisoned setup.

Full traversal, recovery, table-store guard, arena Huge, HugeTab, realloc,
GC sweep, and terminal orphan fixtures pass under both builds. During Huge
header-only DEFER_FREE completion in MARK/WEAK/SWEEP, every mapping page after
the first header page is PROT_NONE, including the real tail W; the first-page
gct is poisoned. Small FREE completion protects its W page separately. These
tests preserve proof storage without granting body authority to header-only
consumers.

The initial broad runs had compilation errors for a missing copied TNEW include
and, in strict mode, missing terminal-orphan linker wrapper flags. They are
retained in `tail-validation.json` and `tail-asan-validation.json`; neither is
represented as a successful first run. Final follow-ups in
`tail-validation-tnew.json`, `tail-asan-validation-tnew.json`, and
`tail-validation-terminal-orphan.json` pass. `final-validation.json` resolves
each final case while retaining the earlier failures. The cost executable also
initially missed `lj_prng.h`; its compile failure is preserved separately.

Three intentional bad-source controls all fail as intended, with successful
builds and exact commands in `negative-results.json`:

- Restoring the old mapsize makes the last advertised payload byte overwrite W
  and fails the proof comparison.
- Restoring only the old private-realloc extent comparison fails the required
  move before an expanded payload can reach the stationary tail.
- Adding body-proof admission to Huge DEFER_FREE token completion faults on the
  protected real W page. The stopped stack is
  CAS128 -> wide snapshot -> table authority -> deferred exact token consumer.

Fresh normal static builds for both matched variants pass 387 stock cases with
JIT disabled and 509 with JIT enabled. Build/functional work used CPUs 0-15.
Clang ASan used O1, frame pointers, runtime assertions and all relevant helper
macros; only build-time generators ran with `ASAN_OPTIONS=detect_leaks=0`.
Every ASan runtime fixture ran with `detect_leaks=1:abort_on_error=1`, with no
runtime suppressions. Normal cost/stock archives have no test-helper or assert
macros. No Windows or macOS validation was performed.

## Matched normal cost and memory study

`cost-results.json` preserves 380 completed fresh-process commands, exit status,
wall duration and raw measurement lines. `summarize-cost.py` regenerates
`cost-summary.json` and the complete 30-row `cost-summary.md`. The normal static
builds use the same flags and base, GCC 14.2, glibc 2.41, x86-64 Linux, 4 KiB
pages. Every measurement is pinned to CPU 31; other agents performed functional
work on 0-15 and root used CPU 30. There was no full-system isolation or profiler.
Each process has a 60-second bound. None failed or timed out.

Direct private map tests retain 512 objects, then free them, for five sizes and
plain/traversable kinds with untouched/payload/wide touch modes. Seven fresh
alternating AB/BA pairs cover every combination. Allocation and free use process
CPU time, with source/type/extent checks in the same loop for both variants.
The wide mode physically touches the proof; it is a storage/page-cost case,
not a GC-table throughput measurement. The direct allocator has no GC universe.

GC-enabled runtime tests create and root 256 real userdata objects, write the
entire advertised user area, check values, collect with roots retained, release
roots and collect twice, then close. There are three alternating fresh pairs at
each size. No GC suppression is used. The cycle counter stayed at one through
the timed loop in all samples; the explicit collections outside timing advanced
it to two and four. This is not evidence of collector work inside allocation
timing. All explicit collections reached IDLE with zero recovery and no veto.
Settled live bytes agree exactly between variants (24,384 after release), as
does settled tracked total (149,068). Userdata does not exercise promoted tables.

RSS and private dirty are read from smaps_rollup, virtual size from status,
faults from getrusage, and heap storage from mallinfo2, outside allocation
timing. Memory comparisons use within-process deltas from the common pre-loop
snapshot, then paired differences. libc snapshot setup contributes to these
absolute deltas; small RSS differences vary across forks. Mapped reservation
is reported separately from OS temporary alignment reservations and from RSS.

Observed ordinary traversable payload allocation median paired changes are
-1.5%, -0.5%, -1.4%, -1.1%, and -0.3% for logical sizes 20,000, 65,392, 65,393,
65,408, and 65,409. All five ranges overlap zero except 20,000 (maximum rounds
to -0.0%). Untouched 65,393-byte traversable allocation has a +10.1% paired
median in this sample, with -4.1% to +10.7% pair variation. Other untouched
traversable medians range -3.4% to -2.3%. Runtime userdata medians range -1.3%
to +0.4%, with only three pairs. These small noisy experiments do not establish
parity or a general allocation speedup.

Every direct traversable sample changes 512 W calloc/free calls to zero; every
runtime sample changes 256 to zero. The measured heap-used reduction is exactly
16 KiB for 512 direct objects and 8 KiB for 256 runtime objects, consistent with
glibc's 32-byte allocation chunks for each 16-byte W. Initial untouched tail
RSS does not acquire one new page per object. At logical 65,393 and 65,408 the
per-object reserved mapping grows from 64 to 128 KiB for both plain and
traversable kinds: +32 MiB for 512 direct objects or +16 MiB for 256 runtime
objects. That is virtual reservation, not a doubling of resident memory.

When W is explicitly touched beyond the payload's pages, the tail samples add
1,964-2,044 KiB paired median RSS and 506-507 minor faults for 512 objects.
Allocation median increases are +8.3% at 20,000, +5.3% at 65,393, +5.8% at
65,408, and +5.0% at 65,409. At 65,392, W shares the already touched final payload
page and the median is +0.6%, with no added page pattern. Freeing the extra pages
also costs work: at 20,000 bytes the wide-touch free median is +31.4%, whereas
ordinary payload free is -8.4%. The raw summary retains every size and range.

The tail therefore removes a heap dependency and per-object heap allocation
without making promoted resident storage uniformly smaller. The universal
size-only reservation is the narrow implementation with an auditable free/
realloc contract. A kind-dependent or differently packed storage optimization
would need a new authority and failure audit. No d680 ordinary dense timing
was reused as a measurement of this current tail build.
