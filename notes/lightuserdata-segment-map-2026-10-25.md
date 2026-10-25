# Atomic lightuserdata segment map

The old shared interner used a plain map pointer, a plain segment count and a
reallocated vector. Distinct TGs can call `lua_pushlightuserdata` concurrently;
its per-state claim is released before interning and does not serialize the
universe. Readers could therefore observe a changed/freed map or a segment
whose prefix had not been published. The plain count also allowed competing
writers to assign different meanings to the same token.

A native ARM64 baseline at the production state in `0020a110` reproduces
observable corruption. Four worker TGs add 200 distinct upper prefixes while
a fifth decodes four preexisting tokens. The reader fails with
`prior token changed while map grew`. This baseline uses public push/decode
APIs and no new runtime hook. Log: `/tmp/ljmt-lightud-map-baseline.log`;
driver and fixtures: `/tmp/ljmt-lightud-map-baseline.{sh,c,lua}`.

## Publication and lifetime

The 64-bit `GCState` now contains 255 aligned uint32 prefix slots. Segment 255
remains reserved for internal markers. Every real `lightudup` prefix has its
low seven bits clear, so `UINT32_MAX` is an impossible prefix, including for
NULL and the highest possible 64-bit pointer prefix. `lua_newstate` initializes
all slots to that sentinel immediately after zeroing `GG_State`, before any
state/TG publication.

Interning starts at a deterministic prefix hash and probes at most 255 slots.
An acquire load finds an existing prefix or a candidate empty slot. A strong
32-bit compare-and-exchange publishes the complete prefix with release
ordering; its acquire failure result either identifies an equal-prefix winner
or advances to the next slot. Since occupied slots never change or become
empty, equal prefixes converge on one ID. The reader acquire-loads that exact
slot before reconstructing the original pointer. A compile-time assertion
requires the 32-bit atomic to be always lock-free.

The map cannot move and performs no allocation, resize, wait or lock during
successful interning. Full capacity retains the existing `LJ_ERR_BADLU`
error. The old standalone GC2 memory mark and close-time free are removed:
the inline map belongs to the lifetime-pinned `GG_State` allocation and its
existing size accounting. No interior-pointer marking is introduced. The
32-bit direct-pointer representation is unchanged.

Measured ARM64 experimental layout changes are `GCState` 96 -> 1112 bytes,
`global_State` 5600 -> 6608 and `GG_State` 42800 -> 43808. Padding absorbs part
of the field-size change; the actual universe allocation grows by 1008 bytes.
Internal-header consumers must rebuild. The assembler's generated offsets
and range assertions compile in the ARM64/arm64e profiles tested below.

## Tests and current validation

`t-lightud-segment-map-race.c` verifies all 255 prefixes, capacity exhaustion,
reuse after the error, NULL and the maximal upper prefix. Synthetic addresses
are opaque values and are never dereferenced. The concurrent Lua fixture has
four writers and one decoder. A test-only hook parks all four writers after
they observe the same empty slot for one common prefix, immediately before
CAS. The fixture requires exactly four arrivals at that slot before release,
then checks canonical token identity and exact decoded pointers. Writers also
add 200 distinct prefixes while the reader checks old tokens. After joining,
the original returned tokens are decoded again and compared with reinterned
values. The rendezvous has bounded waits and compiles out of production.

The complete rooted GC contract passes on ARM64 and arm64e/BTI, assertion
and release, with this map fixture and every existing rooted read/store,
lightuserdata, raw nil-deletion and GC lifetime test. Log:
`/tmp/ljmt-lightud-map-contract.log`. The new fixture is also registered as
`m5_lightuserdata_segment_map` and included in the bootstrap gate.

The complete experimental gate and both 509-case stock corpora pass at
`7f377f51`; signed x86-64 also passes the capacity, concurrent decode and
forced CAS-race fixture and both stock corpora. Bootstrap passes after the M5
registration was corrected to retain its ARM64 opt-in and JIT-mode build
flags. Runtime sources were unchanged during that correction and rerun. See
[combined validation](lightuserdata-validation-2026-10-25.md) for exact logs
and the initial harness failure. These results are not sanitizer coverage,
native Intel evidence or overall port completion.
