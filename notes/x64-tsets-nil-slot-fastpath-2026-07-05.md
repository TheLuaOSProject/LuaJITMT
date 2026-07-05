# x64 TSETS Nil-Slot Fast Path - 2026-07-05

`BC_TSETS_Z` previously sent an existing string-key slot with a nil value
straight to `vmeta_tsets`. That was semantically conservative, because a nil old
value is absent for `__newindex`, but it also meant ordinary `obj.field = value`
lost the table-store helper path whenever the old slot had been cleared.

The x64 VM path now matches the array-store treatment:

- `TFORWARD` still falls back to VM meta/slow handling.
- Nil old value plus no metatable can use the direct/helper store gate.
- Nil old value plus a cached `nomm[MM_newindex]` bit uses the VM string-hash
  helper.
- Nil old value with possible `__newindex` still falls back to `vmeta_tsets`.

This keeps stock Lua table semantics: an existing hash node whose value is nil
does not suppress `__newindex` unless the table has no such metamethod. The VM
helper remains responsible for stale-generation retry, CAS publication, and
table/barrier cooperation when the direct store gate is closed.
