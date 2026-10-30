# ARM64 NUM FORL with a guarded integer loop body

The ARM64 NUM `FORL` admission helpers previously rejected every trace containing `IR_USE`, `IR_ADDOV`, `IR_SUBOV`, or `IR_MULOV`. That closed ordinary loops whose numeric index recurrence was valid but whose body also updated an integer accumulator.

This slice removes that blanket body-op veto from the independent semantic and post-RA NUM `FORL` shape checks. The NUM index, stop, step, comparison direction, PHI, snapshots, and frame geometry checks remain unchanged. Integer arithmetic still passes the existing generic semantic and post-RA checks: operand types and references, guarded overflow operation, USE-to-ADDOV relationship, allocated operands, and snapshots. No NUM-specific accumulator grammar was added.

The focused fixture uses `for i = 0.5, n, 0.5 do c = c + 1 end`, with a separately JIT-disabled oracle. It checks native root entry, full-GC reuse of the same mcode, and the overflow transition from an INT accumulator to the interpreter's widened numeric result. Private semantic and post-RA mutations independently reject a detached INT PHI, a NUM operand substituted into the INT ADDOV, and a removed ADDOV overflow guard.

## Evidence

- `tools/ci/arm64_jit_forl_record_contract.sh` passed two repetitions each for ARM64 and ARM64e, both assertion and release profiles. The log is `/Users/frityet/Scratch/ljmt-num-forl-int-body-20261030/assert-profile.log` and `/Users/frityet/Scratch/ljmt-num-forl-int-body-20261030/release-profile.log`.
- Ordinary ARM64 build flags: `TARGET_SYS=Darwin`, `TARGET_FLAGS='-arch arm64'`, `MACOSX_DEPLOYMENT_TARGET=13.0`, `XCFLAGS='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL'`. The live-peer oracle probe reported `root=1`, 8 normal exits, 7 post-GC exits, overflow result `2147483650`, and no aborts.
- An ordinary x86_64/Rosetta build from this working tree (base commit `89226814d407a341911b0591a7e7883245b54a8a` plus this patch) reported `root=1`, 8 normal exits, 8 post-GC exits, the same overflow result, and one expected abort (`7:0`). Build flags were `TARGET_SYS=Darwin`, `TARGET_FLAGS='-arch x86_64'`, `MACOSX_DEPLOYMENT_TARGET=13.0`, `XCFLAGS='-Werror -Wno-error=unused-but-set-variable'`. Without the warning exception, this non-assert build stops on the pre-existing `lj_api.c:879` `topofs` unused-but-set warning. The x86_64 executable SHA-256 is `7763fdae49bd0d8a67fc61aa586f4be32495d9f4e21ccf8b25c845825f4e5efa`.
- Ordinary ARM64 executable SHA-256: `036619007825d04b0bd003be8109b219c0dadaebadddb80a786a75094ffc92aa`.

The probe source is `/Users/frityet/Scratch/ljmt-deep-callm-integration-20261029/int-accum-num-forl-probe.lua`. The current-source x86_64 observation above is separate from earlier frozen-baseline comparisons; this patch only changes ARM64 admission. The warning exception applies to the non-assert build described above. The integration checkpoint at `89226814d` also passed an ordinary assertion x86_64 build with plain `-Werror`, as recorded in `arm64-num-child-byte-ordinary-checkpoint-2026-10-30.md`.
