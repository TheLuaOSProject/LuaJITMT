# Constant negative `string.byte` indices on ARM64

The bounded native string-byte proof accepts the recorder's negative-index
shape for `byte(s, KSHORT-negative)`. It also accepts an explicit third
argument only when that argument is the same negative KSHORT or nil.
The semantic and post-RA validators independently bind the call's string
formal and exact negative index to the IR. They require the same guarded
string for `STR_LEN` and `STRREF`, `len + index`, an earlier `offset >= 0`
guard, guarded subtraction, and an exact width check of one. The exclusive end
is `len + index + 1`; for index -1, the optimizer folds this to the existing
`STR_LEN` reference. Dynamic ends and unequal explicit ends remain outside the
proof.

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
- The follow-up contract rerun additionally exercises native `byte(s,-1)`,
  `byte(s,-2,-2)`, and `byte(s,-2,nil)` roots, comparing each with a separate
  interpreted oracle across valid bytes, GC reuse, and short/empty exits.
  The test extension retains dynamic and unequal-end forms outside the
  admission proof.
- `followup-negative-end-four-profiles.log`: the final rerun after the
  `-1`/explicit-end additions passed all four ARM64/arm64e assertion and
  release profiles.

The restored ordinary ARM64 executable hash is
`abb08194d140c16f45faa33ce4dc402c66cd02ac88671f09d57ed65db6f7165c`.
This is a bounded negative-index case, not full `string.byte` or x86_64 parity.
