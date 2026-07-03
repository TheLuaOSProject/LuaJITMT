# String table active-pin fallback drain

2026-07-03:

- `strtab_claim()` now drains active pins from listed TGs plus same-state main
  and current TG fallbacks before publishing a replacement string table header.
- This mirrors the root-pending fallback pattern: attach/detach-adjacent code
  can hold a valid TG-local active header marker before that TG is reachable
  from `gc2.tg_list`, or after it has been unlinked.
- The extra checks run only while a string-table resize/rehash claim is active.
  Ordinary intern lookups still use the existing TG-local header pin and
  per-bucket CAS path.
- `m5_strtab_cas` covers this with a TLS-only active TG that is not attached to
  the global TG list. Resize must wait until that marker is cleared before the
  old header can be retired.
