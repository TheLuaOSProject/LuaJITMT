# Colocated array shrink freeze

- Shrinking a colocated inline array now freezes the hidden tail slots to
  `FORWARD` before the smaller `GCtab.asize` mirror is published.
- Live tail values are migrated into the rebuilt hash generation from the
  captured pre-forward value. Nil tail slots are frozen too, so a racing writer
  that still observed the old `asize` cannot publish into a soon-hidden inline
  cell and lose the write.
- The existing `LJ_TAB_TEST_HELPERS` resize hook now covers both split/grow and
  shrink publication windows in `tests/t-tab-colocated-resize.c`.

Validation:

- `tools/ci/lua_test.sh m5_tab_colocated_resize`
