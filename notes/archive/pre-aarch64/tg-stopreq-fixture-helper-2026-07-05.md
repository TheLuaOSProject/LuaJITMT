# TG STOPREQ Fixture Helper - 2026-07-05

`tests/lib/tg_stopreq_fixture_helpers.h` is now the shared C-test surface for
ordinary fixture STOPREQ setup, cleanup, and sticky-bit checks. Native-boundary
tests use `ljt_tg_set_stopreq()`, `ljt_tg_clear_stopreq()`, and
`ljt_tg_has_stopreq()` instead of hand-rolling flag byte updates.

The helper keeps tests aligned with the runtime `lj_tg_flags_*` accessor style
and avoids multiple local clear variants. This is test-only plumbing; it does
not change VM STOPREQ semantics.

The safepoint model fixtures still contain raw flag manipulation where the raw
signal byte is the behavior under test. Those sites intentionally validate the
low-level publication and acknowledgement path rather than just preparing a
fixture state.
