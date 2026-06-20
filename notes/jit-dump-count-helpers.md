JIT dump count helpers
======================

Context
-------

The M6 JIT token case checks generated dump output for repeated `XPOLL` markers
and x64 loop-poll instructions. Those are behavioral result checks, but the
suite was hand-counting markers inline.

Fix
---

`suite_utils` now has text and dump count assertions. `suite_jit` has a named
x64 loop-poll count assertion. The M6 JIT token case uses those helpers for
FUNCF-depth XPOLL and loop-label poll checks.

Validation
----------

* `tools/ci/lua_test.sh m6_jit_token`
