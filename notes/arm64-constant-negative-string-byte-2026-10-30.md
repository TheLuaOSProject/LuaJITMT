# Constant negative `string.byte` indices on ARM64

The bounded native string-byte proof now accepts the recorder's negative-index
shape only for a fixed two-argument source call `byte(s, KSHORT-negative)`.
The semantic and post-RA validators independently bind the call's string
formal and exact negative index to the IR. They require the same guarded
string for `STR_LEN` and `STRREF`, `len + index`, an earlier `offset >= 0`
guard, an exclusive end of `len + index + 1`, guarded subtraction, and an exact
width check of one. Three-argument calls with an explicit or dynamic end remain
outside this proof.

The focused fixture compares against a separately interpreted oracle for
normal data, embedded NUL, a high byte, and a two-byte string. Short and empty
strings take the selected native guard exit and resume with the interpreter
error. It verifies the same root and machine-code bytes after full GC. Nine
copied IR mutations independently require semantic and post-RA rejection for
source slot, length field, index literal, end literal, offset base, lower
guard, checked range, width result, and STRREF base changes.

Validation logs are in
`/Users/frityet/Scratch/ljmt-constant-negative-byte-20261030/`:

- `constant-negative-four-profiles.log`: full string-byte contract passed
  ARM64 and arm64e assertion and release profiles.
- `arm64-stock-source.log` and `arm64-stock-bytecode.log`: ordinary assertion
  ARM64 passed all 509 source tests and all 509 tests imported from original-v2
  bytecode.

The restored ordinary ARM64 executable hash is
`abb08194d140c16f45faa33ce4dc402c66cd02ac88671f09d57ed65db6f7165c`.
This is a bounded negative-index case, not full `string.byte` or x86_64 parity.
