# Recorder optional ctype snapshot busy behavior

Cdata upvalue constification in `lj_record.c` and cdata pointer constant
folding in `lj_opt_fold.c` are optional recorder optimizations. If their ctype
metadata snapshot sees an active parser owner, they now decline the optimization
instead of aborting the trace with `CTBUSY`.

Correctness-critical metadata readers still abort when the parser token is busy.
This keeps parser-busy traces nonblocking while allowing later, explicit
metadata guards such as cdata element-size recording to decide whether the trace
must abort.

Validation target:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
