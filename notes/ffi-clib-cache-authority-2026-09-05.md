# Native C-library cache authority and close, 2026-09-05

Native namespace lookup could keep returning its recorded symbol or extern
address after the original debug cache was changed. It also ignored semantic
library close. Capturing the real namespace `__index` or `__newindex` and
calling it directly exposed both defects with installed root and side traces.
The preceding receiver repair retained the correct namespace but did not give
these mutable values runtime authority.

`recff_clib_index` now retains the original cache table and key. Before MT it
uses ordinary raw table guards. During shared execution it calls the existing
`lj_tab_gettv_rooted_hit_try` through rooted native scratch cells, checks its
status, then guards the returned type and exact expected cdata identity.
Numeric constants return the actual loaded number, preserving signed zero,
NaN, infinities and larger numeric overrides. A fresh signed volatile lifecycle
load follows lookup and precedes the exported result or extern access. Closing
sets a sticky high bit; physical library retirement remains at joined teardown.

The helper's post-call snapshot preserves preceding Lua effects on refusal,
type/identity failure or close. It attempts admission once and leaves input
cells intact on a miss. Recording uses three exact owner anchors with protected
rollback. A late extern-store conversion refreshes the current Lua base pointer
after fallible work; this is a robustness refinement, without a separately
demonstrated stale-pointer defect. A raced pre-MT recording attempt is rejected
if generic recording emits a waiting helper or the shared predicate changes.
The existing activation token/abort/flush protocol protects later publication.

The independent [source review](evidence/ffi-clib-cache-authority-2026-09-05/source-review/review.md)
traces root publication, helper admission, exact value guards, no replay,
mutable lifecycle loads and first activation. Its initial v1 source review
also approves the one-line current-base refinement. Final v2 validation uses
all 224 matching runtime/generator inputs across normal, assertion and Clang
ASan builds and the integrated source. Relative to `5c455f20`, only
`lj_crecord.c` and `lj_ircall.h` change.

Validation includes:

- 444 independent positive runtime processes across the three frozen builds.
  They exercise mutable original-cache values, nil refill, ignored setfenv,
  prior close, real successful native lookup followed immediately by close,
  root/installed-side generations, exact prefix effects and extern targets.
  Supplemental cases cover NaN and both infinities before/during MT, and real
  original-cache storage growth from mask 1 to 16383 with a changed node
  address, followed by replacement and full collection.
- 309 additional ROOT processes across those builds. They include both stock
  modes (387 off / 509 on per build), special-method and receiver guards,
  pure userdata/cdata exclusions, recorder metadata contention, extern
  snapshots, cache retirement, callback stack relocation/unwind and actual
  remote CALLXS pointer/bool/sret collection and flush. Fifteen of these inject
  allocation errors at each recorder anchor stage or a contract-preserving
  lookup refusal, verify complete cleanup and exact effects, then require
  successful later native execution.
- 153 passing cases in the new shared canonical `m7_ffi_clib_cache_authority`
  entry, including default build preparation, in 45.821 seconds. Six permanent
  fixture files are byte-identical to the independently validated sources.
- Two separate native shape probes retain IR and machine code. Both pre-MT
  and active-MT loops retain volatile lifecycle XLOAD/GE instructions and a
  repeated memory comparison followed by signed JL. The shared loop checks
  lifecycle after the actual hit_try call. These are structural observations,
  separate from the cost study below.

This totals 906 positive functional runtime processes. The exact `5c455f20`
baseline passes all 60 interpreter controls, fails 58 native semantic controls,
and passes the two expected ignored-setfenv native controls. Earlier controls,
two deliberate native-witness failures, and all fixture-development failures
remain preserved and qualified in the [independent handoff](evidence/ffi-clib-cache-authority-2026-09-05/isolated/handoff.md).
Missing-module failures from initial observer builds with an unresolved hidden
assertion symbol are excluded; the final observer retains matching flags and a local unchanged
assertion formatter. ASan/LSan uses `detect_leaks=1:abort_on_error=1` without
suppressions, with instrumented targets and uninstrumented host generators.

[Final validation](evidence/ffi-clib-cache-authority-2026-09-05/root/final-validation.json)
binds the integration, commands, outputs and binary identities. Final source
hashes are `62a718fad1c9c271c8225cb1d2255fddf526dde17eff3ecfe2318c2a2249eaa5`
for `lj_crecord.c` and
`822ee730b3589500bf463c9b345cd91fd3c4c5ce056a297734b90f121bd14a69`
for `lj_ircall.h`. The archive contains 166 text artifacts and 68 hash-only
identities; executable/archive bytes are not copied into Git. The older source
review's explicit `content` entries are verified as UTF-8 source/documents
before normalization to text. Other non-text storage labels remain hash-only.

Seven alternating fresh-process pairs per workload use CPU 31 on a shared
host, GC enabled, and the best of five CPU-clock rounds. The exact prior
runtime is compared with v2; no failed timed process is discarded.

| Workload | Before, median ns | After, median ns | Median paired change |
| --- | ---: | ---: | ---: |
| Pre-MT direct namespace lookup | 0.684 | 1.094 | +59.98% |
| Pre-MT captured namespace lookup | 2.051 | 2.507 | +22.22% |
| Namespace lookup plus actual foreign call | 84.198 | 85.100 | +0.97% |
| `ffi_struct` scalar access | 0.687 | 0.687 | 0.00% |
| Active-MT captured builtin lookup | 0.688 | 251.144 | +36,403.49% |

The shared baseline's speed comes from omitting the mutable cache and close
checks, which the semantic controls show to be incorrect. The repaired shared
lookup is nevertheless about 365 times slower in this microbenchmark and is
a substantial performance follow-up. The 70 raw processes, all pair values,
native shapes and [cost summary](../bench/ffi-clib-cache-authority-2026-09-05/cost-summary.json)
are retained. The rounded `ffi_struct` harness output limits its precision.
These measurements do not establish full-suite or stock performance parity.

Generated lookup has no SMR/table wait, but publication can retry CAS and grow
GC queues through a nonthrowing allocator path. It is not allocation-free,
fixed-cost or wait-free. An abandoned raced recorder attempt may still enter
the generic waiting sampler before rejection. General shared-MT metamethod
recording remains refused. Full-GC lifetime cases do not establish collector
overlap within the helper.

Automatic safety review blocked an additional attachment-during-recording test,
citing possible cybersecurity risk. That extra test was not performed; the
source proof and previously recorded combined first-activation coverage remain
separate evidence. This limitation is not reported as a passing schedule.
Validation is Linux x64 only and does not establish release readiness.
