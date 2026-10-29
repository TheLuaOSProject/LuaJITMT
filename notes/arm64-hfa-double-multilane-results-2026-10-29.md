# ARM64 three/four-double HFA results — 2026-10-29

The Darwin ARM64 result classifier now admits direct homogeneous aggregates of
three or four `double` members (24 or 32 bytes). It assigns one numeric result
lane per member, carried in `D0` through `D2` or `D0` through `D3`, and stores
each lane into the retained CData result box. The existing result proof still
binds the exact declared CType and result allocation/store chain; the new
cases use distinct exactly representable lane values and are counted only when
the selected candidate root finishes native execution.

The argument classifier is unchanged: it remains limited to one eightbyte.
These results use the direct HFA return convention and do not enable indirect
aggregate results, five-member HFAs, or over-aligned HFA results. The test
fixture checks that five-member and 16-byte-aligned triple-double result
signatures remain unsupported.

AAPCS64 defines an HFA as one to four members of the same floating-point base
type and assigns members to consecutive SIMD/FP registers. Apple Clang's
ARM64 assembly for these fixtures returns the three-member case in `D0-D2`
and the four-member case in `D0-D3`. See AAPCS64 sections 5.10.5 and 6.9:
<https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst>.

## Validation

- `tools/ci/arm64_jit_integer_aggregate_args_contract.sh` passed for
  `arm64-assert`, `arm64e-assert`, `arm64-release`, and `arm64e-release`.
  Full log: `/Users/frityet/Scratch/ljmt-hfa-double-multilane-20261029/four-final/four-profile.log`.
- Ordinary experimental ARM64 build and the 13-row aggregate probe passed.
  Every row completed 25 interpreted-oracle comparisons and 2,752 foreign
  calls. The three-double and four-double retained-result rows each recorded
  two selected-root CALLXS entries, two CData result boxes, respectively six
  and eight XSTOREs, and 25 selected-root exits at the post-call CGET.
  Probe: `/Users/frityet/Scratch/ljmt-hfa-double-multilane-20261029/ordinary-arm64/probe.tsv`.
- Strict ordinary x86_64 build (`CFLAGS=-Werror`) and the same portable probe
  passed. Oracle/effect checks passed for all rows; x86_64 has no native root
  for the three/four-double result rows in this probe. Its result classifier
  was not widened by this ARM64 change.
  Probe: `/Users/frityet/Scratch/ljmt-hfa-double-multilane-20261029/ordinary-x64/probe.tsv`.

The four-profile gate uses the project profile helper and restores the
worktree build profile at completion. It validates semantic and post-RA
mutation rejection alongside native result execution and the existing
aggregate regressions.
