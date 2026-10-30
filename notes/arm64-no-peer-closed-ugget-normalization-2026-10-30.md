# ARM64 no-peer closed-upvalue normalization

## Change

On experimental ARM64, a closed `UGET` in a root recorder with no parent,
side trace, or pending return is normalized through the existing rooted
upvalue helper path. Stores, open cells, nested/child recording, and
non-root contexts keep their previous recorder path. The normalization does
not admit raw `UREFC`/`ULOAD` in ARM64 semantic or post-RA admission. The
existing typed `VLOAD`, source, snapshot, and function guards remain
responsible for deciding which uses can publish.

This reuses `lj_func_getuv_rooted_try` for every execution of the captured
cell. A replacement value is re-read; the recorded type or exact builtin
guard exits when it no longer matches. First peer activation still retires
the existing no-peer trace before shared recording. The rooted helper costs
more runtime work than x86-64's raw upvalue load; no performance comparison
was run.

## Coverage

The ordinary no-peer matrix checks captured NUM, INT, STR, FUNC, NIL, boolean,
table, and cdata cells. It also checks mutable type replacement and runs
captured `math.sin`, `string.byte`, and `string.sub` loops. Each candidate is
checked for a selected loop root, exact output before and after full GC, and
selected-root exits. Function-cell replacement is checked for all three
builtin cases; the scalar read cases change cell contents, including a NUM to
STR type change.

The rooted-upvalue admission fixture adds `math.sin` and `string.byte` no-peer
shapes. Those clones still pass independent semantic and post-RA admission;
the existing source, helper, snapshot, and allocation mutations remain in
the contract. The no-peer-to-peer transition retains its captured
`string.sub` replacement and retirement checks, and also holds a closed
numeric-cell root through the transition. It changes that cell from NUM to
STR and back before peer activation, checks both old roots are retired, then
verifies the new numeric root executes after peer GC.

## Validation

- `tools/ci/arm64_jit_rooted_upvalue_contract.sh` passed ARM64 and arm64e,
  assertion and release profiles after the change. Final output is preserved
  at `/Users/frityet/Scratch/ljmt-no-peer-closed-ugget-contract-20261030/final-four-profiles.log`.
- Ordinary no-helper ARM64 build: source `codex/arm64-no-peer-closed-ugget`,
  binary SHA-256 `186bd07b639d621bae83aff26de9da24fefb2d89d41d5b9815393373e58b4426`.
  The no-peer value/builtin matrix passed with 32 selected exits per root;
  the peer transition reported 40 no-peer and 40 peer exits. Logs are
  `ordinary-arm64/no-peer-matrix-final.log` and
  `ordinary-arm64/peer-transition-final.log` under the evidence directory.
- Ordinary strict x86-64 build: binary SHA-256
  `66bc93e0dd0bf4fc08351571cd10fabf96f735626c8b0dc776c1fd0348504438`.
  The same no-peer matrix passed with 32 raw-upvalue root exits per case,
  matching the existing x86-64 representation. Its log is
  `ordinary-x86_64/no-peer-matrix-final.log`.
- Both ordinary builds used the same checkout source and matching copied
  `jit` modules under
  `/Users/frityet/Scratch/ljmt-no-peer-closed-ugget-contract-20261030/`.

Unsupported IR shapes still fail the existing admission checks. This change
only selects the enumerated rooted-load representation for eligible
root-context closed reads; it does not broaden the operation whitelist.
