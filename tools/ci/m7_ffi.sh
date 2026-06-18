#!/bin/sh
# Run the current M7 FFI concurrency gates.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

"$ROOT/tools/ci/m7_ffi_cdef_token.sh"
"$ROOT/tools/ci/m7_ffi_no_cts_l.sh"
"$ROOT/tools/ci/m7_ffi_cdef_dup_stack.sh"
"$ROOT/tools/ci/m7_ffi_cparse_rollback.sh"
"$ROOT/tools/ci/m7_ffi_ctype_intern_l.sh"
"$ROOT/tools/ci/m7_ffi_ctype_hash_publish.sh"
"$ROOT/tools/ci/m7_ffi_ctype_tab_retire.sh"
"$ROOT/tools/ci/m7_ffi_ctype_ticket_intern.sh"
"$ROOT/tools/ci/m7_ffi_ctype_name_claim.sh"
"$ROOT/tools/ci/m7_ffi_ctype_pointer_ids.sh"
"$ROOT/tools/ci/m7_ffi_cdata_alloc.sh"
"$ROOT/tools/ci/m7_ffi_jit_cnew.sh"
"$ROOT/tools/ci/m7_ffi_snap_restore_l.sh"
"$ROOT/tools/ci/m7_ffi_finreg.sh"
"$ROOT/tools/ci/m7_ffi_pin.sh"
"$ROOT/tools/ci/m7_ffi_metatype.sh"
"$ROOT/tools/ci/m7_ffi_cdata_get_l.sh"
"$ROOT/tools/ci/m7_ffi_cdata_set_l.sh"
"$ROOT/tools/ci/m7_ffi_carith_l.sh"
"$ROOT/tools/ci/m7_ffi_clib_cache.sh"
"$ROOT/tools/ci/m7_ffi_callback_install.sh"
"$ROOT/tools/ci/m7_ffi_callback_runtime.sh"
"$ROOT/tools/ci/m7_ffi_blocking.sh"

echo "M7 FFI gates passed"
