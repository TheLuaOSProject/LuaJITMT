#!/bin/sh
# Compatibility entrypoint. Source-text CI guards were removed; see notes/ci-source-search-policy.md.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
exec "$ROOT/tools/ci/lua_test.sh" m5_tab_array_publish
