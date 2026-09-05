# Root review: GC helper fixture setup and publication accounting

The exact two-file v4 repair is accepted on runtime commit
`79345529bd932e68f8159ec17224467a10cad09b`, with shared documentation HEAD
`e3b2ec6afc4f6819fad7fad84dc179c250196155` at canonical execution. No runtime
source or collector admission gate changes in this integration.

The hard-assist test previously treated a best-effort preserving abort as
completion. The owner's read-only state observations instead show the prior
cycle still in MARK with close intent. The next worker claim correctly refuses
that pending close, so the test was measuring a different state than intended.
The repair runs the real public completion driver and asserts IDLE and cleared
close intent before arming another case. Original assist/check increments,
batch cadence and legacy-color assertions remain.

The allocation test's meta-store counts were stale. Actual source stacks for
hardware watchpoints account for nine filtered old edges on the missing-key
store and eleven more on the existing-key store. Exact parent pushes remain
one and then three cumulatively. The new checks also require stable generational
IDLE, marked input objects, the actual parent in the active SSB, and the stored
table's type and identity. Young-edge marking tests remain unchanged.

The subsequent assist-frontier failure had a separate setup cause: graph
construction exhausted the free SSB pool. The explicit flush legitimately
converted older published requests to recycle buffers, marking the child before
the measured assist. A real full collection after graph construction drains
that setup. The repaired test requires a free SSB buffer and an entirely
unmarked graph in the fresh MARK cycle. It retains the one-item assist frontier
child=1/grandchild=0 and eventual grandchild marking. No queue, counter or
admission state is fabricated by this repair. Earlier failed candidates and
diagnostic continuations remain separate failures in the owner's archive.

## Validation and provenance

The frozen patch SHA256 is
`9ee974526e9d6a534e8f84da92df0cf19f8f76000f99ab09db7477aad53368ce`.
Promotion verifies the shared fixture bytes exactly match the owner's v4:

- `t-gc2-interp-hard-check.c`: `6584c6cc748f3ae27b93409e24755296d20eaa5ecc1ef6e0576017194021943a`.
- `t-gc2-alloc-account.c`: `931232437a4fa22103d3673dcf31f1ad5c2689ebbd70110ce1a743a786f5ddda`.

The owner has ten final complete runtime passes: both fixtures against exact
843 optimized helper, assertion/APICHECK and target-only ASan builds, then exact
793 assertion/APICHECK and target-only ASan builds. Six unchanged controls fail
at their original assertions. Its verifier checks six sets of 224 runtime and
generator inputs and 1,496 unique artifacts, inputs and headers. The owner's
final review corrects the initial unfinished-SWEEP hypothesis and the earlier
overbroad statement that both tests require helper archives.

Root canonical results are deliberately component-level:

| Registered suite | Observed result |
| --- | --- |
| `m6_jit_alloc_account` | Both repaired fixtures pass in the real GC2-helper-only build. The unchanged idle-reclaim-entry fixture then times out at its original 20-second limit. The remaining two cooperative fixtures are unrun. The suite exits 1. |
| `m10_generational` | Pass: generational Lua with JIT off and on, and allocation accounting against the freshly restored default archive. The suite exits 0. |

Thus this package adds five positive canonical components to the owner's ten
final passes; it does not establish a complete M6 pass. Compile commands,
environments, runner/source/archive/ELF identities, complete stdout/stderr and
elapsed time are preserved in `canonical.json` and adjacent logs. The default
build is restored by the M10 suite. Different Git-version metadata means its
ELFs must retain their own recorded identities.

## Remaining iterator progress failure

Root separately compiles the unchanged idle-entry fixture against the exact
793 assertion/APICHECK archive and interrupts it under GDB. This diagnostic
uses a separately identified ELF, not the canonical GC2-only executable.
The reclaimer is paused at the genuine post-zero-native-sample hook inside
IDLE reclamation. Main is in `lj_tab_next_rooted`, reached through
`lj_tab_itern_rooted` and `lj_vm_IITERN`, at the fixture's unchanged
"closed IDLE ITERN shadow" case. The debugger exiting zero is not a passing
fixture result. The earlier guess of a function-copy lease wait is superseded
by this stack evidence; later exact admission-branch analysis is a separate
study. Existing earlier GC-control studies already retain matching timeouts.

Ordinary scalar iteration waiting on that unrelated suspended reclaimer is
open runtime progress work. The iterator is not moved outside the paused
window, and its expected count and sum are not relaxed. These two fixture
repairs do not solve that dependency or establish general collector completion,
performance, Windows/macOS coverage, or release readiness.
