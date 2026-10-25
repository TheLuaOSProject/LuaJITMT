# ARM64 JIT heap-read entry points

2026-10-25 correction: `lj_tab_gettv_rooted()` is a progress helper whose
contention paths may call `lj_tab_wait_l()`. Its existing recorder shape is
therefore not a no-wait native-read certificate. The bounded scalar
`lj_tab_getscalar_rooted_try()` is the suitable existing runtime helper for
the first numeric read capability: it returns a status, leaves the output
unchanged on refusal, and requires a status guard before the numeric VLOAD.
This correction supersedes the helper choice below; the original audit did
not enable that path.

Read-only source audit at `1355cd2d`. This does not enable any new IR or claim
native heap-read execution. Scalar capabilities continue to reject heap and
helper operations.

The common recorder already has a suitable active-MT table-read protocol in
`src/lj_record.c`: `rec_idx_mt_shared_sample` anchors the table and key in the
owner TG, calls `lj_tab_gettv_rooted`, and inspects the result before releasing
its anchors. `rec_idx_mt_shared_load` emits two `TMPREF` values, that same
helper call, an explicit post-call snapshot, and a typed `VLOAD`. The ordinary
active-MT lookup path reaches this protocol without the x64-only helper macro.
That macro controls other direct-access and store decisions; enabling it for
ARM64 as a whole would broaden unrelated paths.

ARM64 `asm_tvptr` already addresses the current TG's `tmptv`/`tmptv2`, and the
existing `CALLXS` work proves selected call/root lifecycles. Neither fact proves
the table helper's throwing-call and retry protocol. A future heap-read slice
must validate that helper's exact argument/root ownership, post-call snapshot,
result guard, STOPREQ and reentry behavior, including GC during retry and peer
resize/retirement. Scratch argument aliasing also needs an emitted-code proof:
the generated table-barrier regression already exposed why later argument
materialization can clobber an earlier argument register.

The raw paths in `src/lj_asm_arm64.h` still need separate work. `asm_fload`,
`asm_href`, `asm_hrefk`, and `asm_ahuvload` retain ordinary loads; `asm_ahustore`
retains ordinary stores. They must not become reachable merely because the
scalar validator learns pointer types. x64 has node-header/retiring guards,
helper-backed stores and explicit shared-cell cases that are absent from these
ARM64 paths. Pre-MT direct reads also need their own proof tied to the existing
first-attachment flush and poll boundary.

For minimal divergence, reuse the common rooted helper and its recorder
semantics before considering a new target-specific lookup algorithm. Keep
semantic provenance and immutable allocation/snapshot validation independent,
and keep unproved raw table accesses closed. No performance conclusion follows
from this audit; the helper and any later fast path need measurements under
matching workloads.
