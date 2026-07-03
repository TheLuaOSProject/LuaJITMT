Compiler-Text Count Helper Cleanup
==================================

Context
-------

Historical context: the M6 JIT token case previously checked generated compiler
text for repeated `XPOLL` markers and x64 loop-poll instructions.

Fix
---

Superseded: generated-text count helpers were removed from active coverage.
The token case now validates token acquisition/release, traceability, and
nonblocking behavior directly.

Validation
----------

* `tools/ci/lua_test.sh m6_jit_token`
