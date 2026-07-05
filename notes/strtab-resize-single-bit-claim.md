# String Table Resize Single-Bit Claim

`StrTabHdr.resize` now has exactly two valid states: `0` and
`LJ_STRTAB_RESIZE`. The old shared-header reader-count protocol has been
replaced by TG-local active header markers, so lower bits in `resize` are stale
state rather than a live synchronization mechanism.

`strtab_claim()` now attempts only `0 -> LJ_STRTAB_RESIZE`. If another resize,
rehash, or string sweep owns the claim, the caller observes a nonzero state and
uses the existing retry/fallback path. The claim still drains TG-local active
markers before replacing or destructively sweeping a header; that bridge remains
required until resize and sweep are converted to helper-copy/deferred-unlink
forms.

The important invariant is that ordinary string interning no longer performs
shared-header reader-count RMWs. Interners use `strtab_enter()` to publish a
TG-local active marker, recheck the current header, and then walk/insert by
per-bucket acquire/CAS.

Focused verification:

- `LJ_TEST_DISABLE_BUILD_CACHE=1 LUA=luajit tools/ci/lua_test.sh m5_strtab_cas m5_strtab_prep m5_strtab_gc_stress`
