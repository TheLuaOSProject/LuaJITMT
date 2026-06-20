# FFI Pointer-Struct Snapshot

The interpreter cdata pointer auto-deref path already asked
`lj_ctype_ptrstruct_snapshot()` whether an unlocked pointer target resolved to a
struct, but then immediately reread the current CType table with
`ctype_get(cts, cid)->info` before deciding to perform `->`.

That second read was unnecessary for the unlocked path and weakened the snapshot
boundary: the helper's sequence check is the proof that the returned `cid`
resolved to a struct for that reader epoch. `lj_cdata_index_l()` now keeps the
raw `ctype_get()` struct test only under the parser-lock fallback and otherwise
uses the snapshot result directly, including carrying that resolved `cid` into
the auto-deref retry.

`tools/ci/m7_ffi_typeinfo_snapshot.sh` rejects the old
snapshot-then-raw-reread pattern in `src/lj_cdata.c`.
