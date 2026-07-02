Package claim wait primitive
============================

- `require()` and `package.loadlib()` claim waiters no longer use blind 1ms
  native sleeps while another thread owns the same module/path claim.
- Each claim table has a stable generation word. Clearing an owned claim
  increments the relevant generation and wakes waiters through the platform
  futex/WaitOnAddress backend.
- Waiters still recheck the table claim and use a short timeout to preserve
  STOPREQ polling and tolerate unrelated claim-table movement, but the normal
  release path wakes immediately instead of waiting for the timeout.
