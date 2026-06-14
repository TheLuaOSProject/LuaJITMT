#!/bin/sh
# Run the current M6 JIT scaffold gates.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

"$ROOT/tools/ci/m6_dispatch_redispatch.sh"
"$ROOT/tools/ci/m6_jit_token.sh"
"$ROOT/tools/ci/m6_jit_cell_ops.sh"
"$ROOT/tools/ci/m6_jit_barrier_xpoll.sh"
"$ROOT/tools/ci/m6_jit_xbar_xpoll.sh"
"$ROOT/tools/ci/m6_jit_table_store_helper.sh"
"$ROOT/tools/ci/m6_jit_aref_pair_guard.sh"
"$ROOT/tools/ci/m6_jit_hrefk_nodehdr.sh"
"$ROOT/tools/ci/m6_jit_href_nodehdr.sh"
"$ROOT/tools/ci/m6_jit_alloc_account.sh"
"$ROOT/tools/ci/m6_jit_gc2_readiness.sh"
"$ROOT/tools/ci/m6_jit_gcstep_guard.sh"
"$ROOT/tools/ci/m6_jit_mcode_publish.sh"
"$ROOT/tools/ci/m6_jit_flush_hs.sh"

echo "M6 JIT scaffold tests passed"
