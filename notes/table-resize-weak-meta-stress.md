2026-06-28

- Extended `tests/t-tab-resize-stress.lua` with `weakmeta`, a behavior case
  combining weak-key resizing, concurrent writers, GC stepping, and metatable
  `__index` reachability.
- The case keeps odd weak keys rooted, drops even keys, and verifies after full
  GC that rooted weak-key entries survive while unrooted values disappear. It
  also nils local references to the metatable and fallback table so the only
  strong edge is the resized weak table's metatable chain.
- This replaces the old source-shape guard style with a runtime probe for the
  resize/weak/metatable edge that must stay safe under racy Lua code.
