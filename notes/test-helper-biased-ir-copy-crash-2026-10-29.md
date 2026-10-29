# Nested snapshot test-helper crash localization (2026-10-29)

The saved `first-side-debug-fixture` crash is localized to a full IR copy from a biased `GCtrace.ir` pointer. The binary UUID (`D16817A9-21AD-3F61-9809-9331D5714EF6`) matches the crash report. The faulting frame is `_platform_memmove+96`, called from `lj_trace_test_arm64_nested_cell_snapshot_contract+880`.

In the matching executable, the helper loads the selected trace's pointer at struct offset `+0x20` and its instruction count at `+0x0c`. It computes `nins << 3` and passes that byte count, the trace IR pointer, and a new allocation to the authenticated stub bound to libSystem `_memcpy`. The call instruction is at absolute `0x10013a6c4` (function-relative `+0x36c`); its return PC at `0x10013a6c8` is function-relative `+0x370`, matching the report's caller `symbolLocation=880`. The crash state has `x22=262248` (`32781 * sizeof(IRIns)`), source `x1=0x101217ff0`, fault address `0x101218000`, and 148744 bytes remaining in the copy. This directly establishes that the full sized copy traversed into an inaccessible source page.

`GCtrace.ir` is explicitly biased by `REF_BIAS` in `src/lj_jit.h`; valid IR storage is indexed on the half-open range `[nk, nins)`. A copy starting at the biased base for `nins` elements reads the invalid prefix before `nk`. The old uncommitted helper source is unavailable, so the exact C expression is reconstructed from the binary. The dump and disassembly are preserved at `/Users/frityet/Scratch/ljmt-nested-reentry-integration-20261029/crash-localization/evidence.txt`.

The current committed helper uses an indexed, single-reference private IR copy and is not the crashing implementation. No production change is indicated by this finding.
