#!/bin/sh
# Run the current M6 JIT scaffold gates.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

"$ROOT/tools/ci/m6_jit_token.sh"
"$ROOT/tools/ci/m6_jit_barrier_xpoll.sh"
"$ROOT/tools/ci/m6_jit_gcstep_guard.sh"

echo "M6 JIT scaffold tests passed"
