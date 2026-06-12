#!/bin/sh
# Run the current M5 concurrent-object scaffold gates.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

"$ROOT/tools/ci/m5_nbtab_model.sh"
"$ROOT/tools/ci/m5_itype_nan.sh"
"$ROOT/tools/ci/m0_guardrails.sh"

echo "M5 concurrent-object scaffold tests passed"
