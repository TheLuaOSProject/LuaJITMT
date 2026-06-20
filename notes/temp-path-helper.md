Temporary path helper
=====================

Context
-------

Some Lua suite cases manually created temp paths, removed stale files before
use, and removed outputs again at the end. That cleanup was easy to skip if an
assertion failed mid-case.

Fix
---

`suite_utils.with_temp_paths()` creates named temporary paths, removes stale
files before running the callback, and removes the paths after success or
failure. M4 shutdown markers and M9 benchmark regression CSVs now use it.

Validation
----------

* `tools/ci/lua_test.sh m4_threading_shutdown m9_bench_regression`
