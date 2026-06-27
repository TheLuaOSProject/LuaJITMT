#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
"$ROOT/tools/ci/m5_tab_store_waits.sh"
exec "$ROOT/tools/ci/lua_test.sh" m5_tab_cas_store
