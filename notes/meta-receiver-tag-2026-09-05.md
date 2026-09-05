# Skip table admission for non-table meta receivers

The initial positive-read optimization now classifies the receiver with an
acquire TValue load before trying either table reader. This removes the
unnecessary source-SMR attempt on ordinary cdata receivers. Function-environment
mode still uses its original capture path.

Only the copied tag is inspected. Both exact readers receive the original
authoritative receiver and key cells and retain all source, owner, vector,
lifetime and output checks. A changing tag either reaches those readers'
revalidation or falls through to the general chain, which captures its own
current inputs. The hint supplies no GC pointer authority and is never used
as a replacement root. Stable small table hits can still finish while global
SMR is closed. Output aliases and failed-attempt semantics are unchanged.

The frozen base is `28de50a6` plus this one production-body change. FNEW test
repairs in the shared tree are excluded. Normal static executable SHA-256 is
`c69a8c5e59189b33a0a8ef0f0f75ddd9d336098f1d17daded34a48b7cf906e24`;
the assert/helper executable is
`1c7df63685f9fca009a82d7a5420e5b54894a339369025e99d5adffcf04f1246`.
The strict rooted-get and paused-reclaimer scalar fixtures pass. Normal stock
suites pass 387 interpreter and 509 JIT tests, and both canonical rooted-meta
and x64 rooted-read suites pass. No new ASan or exhaustive concurrent tag-race
claim is made for this classification-only change; the exact underlying
readers retain their previously recorded validation.

Seven alternating fresh-process pairs per case compare this normal build with
the frozen direct-positive-hit build. The unmodified filtered harness uses
JIT off, scale 0.005, GC enabled, its permanent 8,192-key graph, and the minimum
of five internal rounds. All 70 processes exit 0 within 30-second bounds.
CPU 30 is pinned while functional work runs on CPUs 0–15 on the shared host.
All 224 tracked src/dynasm inputs match except this `lj_meta.c` change and
the earlier documentation-only `lj_tab.h` clarification, preserved separately.
Both executable hashes remain unchanged after measurement.

| Case | Before median ns/op | After median ns/op | Paired geometric ratio |
| --- | ---: | ---: | ---: |
| FFI struct access | 1,564.91 | 1,496.95 | 0.95585 |
| Existing-key stores | 273.41 | 273.68 | 1.00049 |
| Hash reads | 404.12 | 405.33 | 1.00395 |
| Existing-key reads | 627.23 | 623.45 | 0.99770 |
| Array read/write | 167.95 | 167.74 | 0.99785 |

The roughly 4.4% FFI reduction supports avoiding the redundant attempt. Most
of the interpreter FFI cost remains in generic metamethod lifetime admission;
this small change does not establish stock parity or a fully nonblocking
metamethod path.

Builds, patch, commands and raw functional output are in
`notes/evidence/meta-receiver-tag-2026-09-05/`. Source comparison, normal raw
pairs, driver, results and hash manifest are in
`bench/meta-receiver-tag-2026-09-05/`.
