/* Call the raw MAP_JIT body; its registers are all ABI caller-save. */
	.text
	.p2align 2
	.globl _lj_test_arm64_phi_spill_call
_lj_test_arm64_phi_spill_call:
	.long 0xd503245f	/* bti c */
#if defined(__arm64e__)
	pacibsp
#endif
	stp x29, x30, [sp, #-16]!
	mov x16, x0
	mov x0, x1
	mov w1, w2
	blr x16
	ldp x29, x30, [sp], #16
#if defined(__arm64e__)
	retab
#else
	ret
#endif
	.subsections_via_symbols
