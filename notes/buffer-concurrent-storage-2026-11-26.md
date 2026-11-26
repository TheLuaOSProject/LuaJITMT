# Concurrent buffer storage review

This Linux x64 change is based on `394159a5ca21ebc8be65bca055245dc2ca8e4526`.
It completes the pending public-buffer rewrite and fixes issues found while
reviewing it. It improves shared-buffer safety and progress; it does not close
the performance milestone or establish release readiness.

Public buffers now publish one backing allocation and both FIFO offsets with a
128-bit CAS. Internal appends claim unused tail bytes. If that claim is held,
another writer can copy the committed prefix and publish its own replacement.
Readers retain the selected storage and any copy-on-write owner. Reset and
consumption do not allow old, published bytes to be overwritten while a reader
still uses them. `get` commits only its observed prefix; `decode` retains its
input through protected error cleanup and commits against the same read offset.

The review moved replacement allocation after retryable admission, retained the
candidate through its publication barriers, and retained the removed allocation
through allocator retirement. Large copy-on-write reads use typed nonmarking
leases. Opportunistic publication marking and userdata rescanning avoid turning
ordinary metadata contention into a permanent reclamation veto. `get` cannot
safepoint with an unpublished result string. The JIT reset and pointer helpers
prepare throwing-call snapshots, and interpreter result slots publish GC roots.

The draft decoder copied and interned the entire remaining suffix on every
record. The final decoder reads retained storage directly. A separate pre-existing
bug stored dictionary indexes as Lua numbers but read their raw low words.
Both string and metatable dictionary lookups now convert the numeric value.
The [dictionary control](evidence/buffer-concurrent-storage-2026-11-26/review/dictionary-control/results.json)
fails with `duplicate table key` on the preceding commit and passes on the final
candidate and stock LuaJIT. It exercises a nonzero metatable index as well.

Validation uses the exact final runtime sources in seven builds: GCC release,
Clang assertions, ASan, TSan, and builds without FFI, JIT, or buffers. The direct
checks all pass. GCC, assertions and ASan each pass the shared-runtime CI script,
including 509 stock source cases, 387 eligible interpreter cases and 509 genuine
stock bytecode cases. Full GC traversal passes with assertions and ASan.
The canonical `m5_buffer_publish` and `m5_buffer_progress` gates pass and restore
the default build. The final source and binary checks are in
[final-verification.json](evidence/buffer-concurrent-storage-2026-11-26/review/final-verification.json).

New public tests check complete append records, unchanged snapshot prefixes,
exactly-once get/decode consumption, dictionaries, errors, FFI ownership and large
storage. A C fixture suspends a real tail writer, a replacement publisher and
the metadata reclaimer. Peers finish before those owners resume. Subsequent full
collections preserve the published contents without a reclamation veto. Native
loop witnesses cover reset, set, put, get and skip. ASan runs disable leak
detection; TSan does not instrument generated native trace instructions.

Performance remains a substantial open cost. These are final paired GCC results
on CPU 8 against the preceding fork commit, not against stock:

| JIT workload | Previous ns/op | Final ns/op | Final / previous |
| --- | ---: | ---: | ---: |
| Append 1 byte, long run | 3.19 | 62.27 | 19.49 |
| Append 16 bytes, long run | 3.10 | 65.69 | 21.20 |
| Reset/put/get 64 bytes | 142.10 | 543.44 | 3.82 |
| Reset/put/get 1 KiB | 275.55 | 2500.55 | 9.07 |
| Set/get 64 bytes | 177.54 | 500.25 | 2.82 |
| FIFO get 1 byte | 120.61 | 128.29 | 1.06 |

Streaming decode stays linear from 1,000 to 100,000 records. With JIT disabled,
the final medians are 927–941 ns/record versus about 836 ns. The method decoder
remains interpreted in the JIT-enabled process. Main-run timing variation is
recorded, so cross-version differences should not be attributed solely to code.
An attempted append fast path showed no clear benefit and added a failed attempt
on full backing; it was removed. The final version retains only the decoder's
reuse of its existing input lease for cursor commit.

All 152 final timing rows, assertions, paired ranges and earlier experiments are
retained in the [performance report](evidence/buffer-concurrent-storage-2026-11-26/performance/v4/summary.txt).
Reducing publication and storage-replacement costs is required follow-up work.
The documented [buffer FFI pointer lifetime](https://luajit.org/ext_buffer.html)
continues to apply: raw pointers expire on buffer modification and do not anchor
their owners.

The evidence manifest records 654 text artifacts and 50 binary/archive hashes.
Its SHA256 is `0bdb20cdeed881494866fe7b2a559bea02f25c219e547610c0138d403a2de48c`.
Both the benchmark's 228 baseline inputs and 228 final inputs were compared with
the corresponding Git/source state before archiving. Unrelated worktree files
were preserved.
