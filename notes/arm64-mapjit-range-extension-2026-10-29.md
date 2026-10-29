# ARM64 macOS MAP_JIT range recovery

## Cause

The allocator failure was not the configured `maxmcode` limit or retirement-sidecar allocation. The preserved first-failure log at `/Users/frityet/Scratch/ljmt-cap-investigation-20261029/baseline-allocator-sites.log` records `mcode_alloc_at()` returning the same non-NULL mapping (`0x107a10000`) for all 16 distinct hints while the active allocation window was `0xfeac0000..0x106ab0000`. The current MCode area was `0x103200000`; the request was 64 KiB and total active MCode was 192 KiB. The candidate lies outside the old window, but roughly 72 MiB from the existing area, within the AArch64 direct branch span. Because ordinary POSIX `mmap()` hints are advisory, freeing that mapping and retrying could not help; the allocator then requested a flush and the 64-child fixture stopped at three children.

The SDK defines `MAP_JIT` for `mmap()` but does not expose `MAP_FIXED_NOREPLACE`. `VM_FLAGS_FIXED` is an exact Mach allocation only when the address is free and does not replace existing mappings, but it does not establish the `MAP_JIT` mapping/protection contract. The fix therefore retains the existing MAP_JIT `mmap()` and its W^X transitions.

## Allocator rule

`LJ_TARGET_ARM64` uses `LJ_TARGET_JUMPRANGE=27`; `MCODE_RANGE64` is 128 MiB minus 64 KiB. In the macOS MAP_JIT path only, if a mapping is outside the current window, the allocator scans the active `J->mcarea` chain and computes the complete address span of every current MCode area plus the candidate. It slides the 64 KiB-aligned allocation window only if that entire span fits inside `MCODE_RANGE64`. Otherwise it frees the candidate and follows the existing fail/flush path.

This keeps every active MCode area in one conservative direct-branch span. ARM64 side-parent links still independently encode and check their B26 displacement. Calls to distant C functions already use an authenticated indirect call when BL is out of range; interpreter-exit tails likewise retain their indirect fallback. No emitted code, branch range, MAP_JIT protection mode, retirement ownership, or GC protocol is widened.

The active-area walk occurs during serialized MCode reservation/trace compilation and reads each next link with `mcode_area_next_acq`, matching the release publication in area setup. The detached retirement list is not used as a link target: a flush detaches the active chain, and retired bodies remain mapped only for extant readers, not for newly published side links.

## Evidence

- `tools/ci/arm64_jit_side_ir_admission_contract.sh` with explicit profiles `arm64-assert arm64e-assert arm64-release arm64e-release` passed all four after the allocator and depth-three root-flush changes. Log: `/Users/frityet/Scratch/ljmt-cap-allocator-validation-20261029/final-four-profile.log`.
- The test-only span helper checks an exact aligned 128 MiB-minus-64 KiB window, an unaligned accepted span, an over-range span, reversed bounds, and address overflow. A second synthetic linked-area check exercises the same active-area walk and accepts/rejects candidates on either side of the bound.
- The capacity fixture still requires the 64-child root and its depth-three topology. It now prints how many real out-of-old-window MAP_JIT recoveries occurred in that run. The path is layout-dependent. In the preserved `/Users/frityet/Scratch/ljmt-cap-allocator-validation-20261029/repeat-assert.log`, a temporary counter assertion was positive in the first three of four fresh processes; the fourth built the 64-child graph without needing a recovery and failed only that temporary counter assertion. The assertion was removed because an OS-layout-dependent branch must not make the contract flaky. The finalized contract reports the count without requiring it to be positive on every process. The original repeated baseline had 11 successful capacity graphs and a 12th failure with the exact 16-hint MCODEAL evidence above.
- `tools/ci/arm64_jit_mcode_retire_contract.sh` passed after the allocator change (`t-jit-mcode-retire OK`; admitted root pin and explicit epoch reclaim).
- The retained final fixture builds a fresh depth-three graph with all three descendants still linked, flushes the canonical root, verifies the root and descendants are unrunnable, runs the interpreter path through a full GC, then requires native root re-entry in a new generation.

## Scope

This recovers from a macOS MAP_JIT placement just outside an existing ARM64 MCode window when all active code remains mutually reachable. It does not make arbitrary out-of-span placement valid; those candidates still fail closed and use the existing flush/new-range behavior.
