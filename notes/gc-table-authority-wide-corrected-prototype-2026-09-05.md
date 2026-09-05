# Table scan authority: corrected isolated AoS prototype, 2026-09-05

The corrected prototype passes the targeted VM/JIT addressing controls and the
bounded GC validation recorded here. It is an isolated functional experiment,
not a production candidate selected on performance. No new timing comparison
was run. The earlier three-file prototype and its timing results remain
[invalid for design selection](gc-table-authority-wide-prototype-2026-09-05.md).
Those frozen artifacts were not repaired or replaced.

This experiment starts from exact commit
`d680421c4cb50b85437d88255bc89358c5e3a6b1`, under
`/tmp/lj-wide-stamp-corrected-20260905-cqq3p87i`. Normal and strict trees have
identical tracked source: only `lj_arena.c`, `lj_arena.h`, `lj_gc2.c`,
`vm_x64.dasc`, and `lj_asm_x86.h` differ from that commit. No shared production,
scalar-read, arena reuse, or coalescing source was changed. The complete patch,
resolvable source manifests, every object/archive/executable hash, fixture
hashes, commands, results, and failures are in the
[evidence directory](evidence/gc-table-authority-wide-corrected-prototype-2026-09-05).
`artifact-manifest.json` covers the compact evidence files.

The C authority protocol is unchanged from the intended earlier design. An
aligned 128-bit atomic proof contains `{era64, covered_cycle32, dirty32}`. A
separate 64-bit exact token occupies offset 16. A retained table writer renews
`{E, MAX, C}` to `{E+1, 1, 0}` in the same CAS that invalidates coverage. A
scanner compares both era and serial before publishing coverage. The terminal
pair `{UINT64_MAX, UINT32_MAX}` still takes the sticky reclaim veto and explicit
saturated-request path. Global GC-cycle and exact-token namespace exhaustion
are separate, unchanged problems.

## Corrected emitted geometry and reset scope

`LJ_GC2_TABSTAMP_SHIFT` is now 5, with a static assertion tying it to the
32-byte entry size. Every emitted cell-to-entry calculation uses that constant.
All C indexing already uses typed entries or the embedded huge field.

| Source in the corrected tree | Operation and lifetime scope |
| --- | --- |
| `lj_arena.h:97`, `1604–1610` | 32-byte entry, 16-byte proof, token at +16; sidecar and huge-header geometry checked. |
| `lj_arena.c:379–390` | Existing private-incarnation preparation clears low state and era after token NONE admission. |
| `vm_x64.dasc:4543`, `4656` | Both TNEW token checks derive the entry with SHL 5. |
| `vm_x64.dasc:4724–4725` | TNEW clears both proof halves after exact construction/root claims and the second token check, before body, READY, or block publication. |
| `lj_asm_x86.h:1585`, `1646` | Both traced FNEW entry calculations use SHL 5. |
| `lj_asm_x86.h:1569–1572` | Both function and upvalue proofs are fully reset after their exact CONSTRUCT claims and token checks. |
| `lj_asm_x86.h:1578`, `1640` | Upvalue entry delta remains `fncells * sizeof(LJGC2TabStamp)`. |

Token generations survive all of these resets. The two ordinary stores used
for a private proof reset are permitted only after old exact body owners are
gone; they are not a concurrent 128-bit proof update. Header-only token readers
remain independent of proof bytes. Huge allocations use typed embedded-header
access and fresh mapping initialization, so no emitted cell-stride path applies
to their proof.

Both actual VM objects contain SHL 5 at text offsets `0xf57` and `0x10ee`, token
tests at +16, and consecutive state/era zero stores at `0x11e3`/`0x11ea`.
The actual recorded FNEW machine code contains both SHL 5 calculations and
four reset stores: state at byte offsets 1409/1416 and era at 1423/1431, before
function/upvalue header stores at 1920/1975. Full VM disassembly and both
recorded JIT code buffers are retained. Source order alone would be insufficient
because the JIT emitter writes instructions backwards.

The strict archive SHA-256 is
`eaf2538186e7dd3c9e1b852f1d99e6535a0b36cd96f953177c35b11a2b45edd8`;
its executable is
`6575563a20283339046abc9768734ea3eff02ace216ae224071bccc8a95d7a0f`.
The normal executable is
`ef493d1dfec730c9f0650aa0eda033e6cbc13ee0adb02fe836141d8ef575e599`.
Full identities for all variants are in `source-binary-snapshot.json`.

## Deterministic validation and negative controls

GCC 14 strict builds use `LJ_GC2_TEST_HELPERS`, `LJ_TAB_TEST_HELPERS`,
`LJ_FUNC_TEST_HELPERS`, `LJ_TRACE_TEST_HELPERS`, `LJ_ARENA_TEST_HELPERS`, and
`LUA_USE_ASSERT`. Fixture compilation uses `-O2 -g -Wall -Wextra -Werror
-mcx16`; normal builds use default optimization with no test helpers. Functional
processes are confined to CPUs 0–15. This corrected experiment did not run ASan
or a new performance benchmark.

- TNEW at cells 1536 and 1537 starts from private FREE storage carrying a
  poisoned old table header, nonzero high era, and a nonzero NONE token
  generation. The inline path must return the exact candidate, clear the full
  proof, preserve its token, and leave potentially aliased neighbor proofs and
  tokens unchanged. A real PENDING token instead requires a C fallback,
  unchanged candidate proof/token, and no candidate publication. All four
  cases pass.
- Traced numeric FNEW at both cell parities applies equivalent guards to its
  function/upvalue pair, requires zero allocation helper calls for the accepted
  trace, and tests actual PENDING ownership on each start. Both cases pass.
  An additional emitted-code audit requires both era resets before either
  header, and retains the executed code buffers.
- A fresh control containing the original defective three-file patch fails
  all four accepted TNEW/FNEW high-cell tests. Even cells corrupt a neighbor
  proof; odd cells corrupt a neighbor token. This directly reproduces the
  original omitted-stride defect. No original frozen binary was rebuilt.
- A worker paused before publishing an old scan at era 17/serial 1 cannot
  certify a raw child store after a real public barrier renews MAX to
  era 18/serial 1. Child and grandchild marks plus a second traversal are
  required. Both small and huge cases pass. A fresh corrected-layout control
  that ignores only the era comparison fails the real child-mark assertion in
  both storage kinds.
- The same rooted table survives twelve forced renewals, each followed by a
  complete collection. Every round reaches IDLE with recovery zero and no
  reclaim veto, clears an unreachable weak value, and preserves the new
  descendant graph. Small and huge cases pass. The exact 32-bit baseline fails
  the no-veto assertion for each kind after the real exhaustion bump.
- Existing coalescing, full traversal, recovery, table-store guard, and full
  TNEW fixtures pass. Direct terminal-authority injections in the coalescing
  and traversal adapters set both era MAX and serial MAX; their assertions
  are retained. Normal and strict stock suites each pass 387 tests with
  `-joff` and 509 with `-jon`.

These are forced namespace/reuse states under the relevant lifetime
preconditions, not a claim that the fixtures naturally performed billions of
writes. The paused scanner is an actual worker; its late publication and the
post-write barrier are production operations. The high-cell controls seed
private old-incarnation metadata after prior body owners are absent.

## Full FNEW fixture: preserved failures and valid setup

The unmodified full FNEW fixture initially fails its SSB capacity assertion in
both exact baseline and corrected layouts. It fills a real producer buffer,
uses preserve-abort, and then assumes a new cycle has fresh capacity.
Preserve-abort deliberately carries that buffer. The isolated adapter publishes
the old buffer with `lj_gc2_flush_ssb`; it does not rewind or discard entries.
A later persistent-root snapshot also requires explicitly requesting native
exit and draining carried work before its one-quantum close. Both earlier
assertion failures and each intermediate adapter are preserved.

After those scheduling prerequisites, the baseline full fixture passed while
the corrected AoS run timed out at 50 seconds. GDB found SWEEP with bridge/root
snapshot complete, zero grey/SSB/recovery/table-token work, and a quarantined
arena containing six READY objects stuck in CONSTRUCT/LINKING. Those same six
descriptors were created in the exact baseline, shifted by two cells because
of the header-size difference. Baseline completion did not establish valid
fixture authority.

The cause is the fixture temporarily setting `g->allocf_arena = 0` while
`g->allocf` remains `lj_arena_allocf`, in both the VM branch-target and direct
bump-gate tests. Arena allocation still publishes CONSTRUCT/LINKING, but
`gc_root_construct_claimed_at` treats the false allocator identity as EXEMPT
and therefore does not commit those lanes. The resulting state has no live
constructor that could complete it. No production repair or manual descriptor
completion was attempted.

The separate `fnew-consistent-setup.patch` uses a real MT-entry contender to
reach the same VM fallback branch without changing allocator identity. An
exact `lj_tab_test_new0_calls == before + 1` assertion proves the C fallback;
the direct closure case retains its original fast/fallback counters and return
semantics. All original semantic, capacity, root-snapshot, and publication
assertions remain. Both exact baseline and corrected AoS now pass the complete
adapted fixture, with added assertions that returned allocation calls leave no
unfinished constructor lanes. This adaptation no longer directly tests the
`allocf_arena` identity gate.

The paired negative retains the original false allocator flag and adds the
same constructor-completion assertion. It fails immediately in both layouts.
`fnew-consistent-results.json`, `fnew-construct-results.json`, the original
capacity/root failures, 50-second timeouts, and GDB observations preserve this
distinction. A passing run of the invalid allocator-identity sequence is not
included in the acceptance results above.

Three fixture compile setup errors are retained: an uninitialized pending-token
ticket warning, a nested `main` macro conflict in a new wrapper, and omitted
arguments in the new mcode-search call. Only those fixture setup errors were
corrected. Neither runtime source nor a semantic assertion was relaxed to
obtain a pass.

## Remaining cost and readiness limits

The corrected AoS layout still doubles the small sidecar from 64 to 128 KiB;
the arena plus sidecar grows from 128 to 192 KiB. `GCAhdr` grows from 128 to
160 bytes and first usable cell moves from 616 to 618. Every shared proof
snapshot/update uses CX16. These explicit memory and instruction costs are
unchanged by correcting the emitted paths, but the invalid earlier timing
ratios cannot quantify their real workload impact. No corrected cost comparison
is supplied by this note.

The prototype also retains an outdated two-64-bit-word comment above its new
stamp declaration; the actual types, static assertions, patch, and this note
describe the measured 128-bit proof plus separate token. It was left in the
frozen source rather than silently changing source identity after validation.
Production selection still needs cost/memory comparison with the separate
persistent-overflow design, an implementation review, and broader concurrency
validation. This work neither resolves all finite namespaces nor claims that
the whole collector is nonblocking.
