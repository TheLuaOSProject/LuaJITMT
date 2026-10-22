/* ABI envelope for the isolated production TBAR lowering. */
	.text
	.p2align 2
	.globl _lj_test_arm64_tbar_call
_lj_test_arm64_tbar_call:
	.long 0xd503245f	/* bti c */
#if defined(__arm64e__)
	pacibsp
#endif
	stp x19, x20, [sp, #-160]!
	stp x21, x22, [sp, #16]
	stp x23, x24, [sp, #32]
	stp x25, x26, [sp, #48]
	stp x27, x28, [sp, #64]
	stp x29, x30, [sp, #80]
	stp d8, d9, [sp, #96]
	stp d10, d11, [sp, #112]
	stp d12, d13, [sp, #128]
	stp d14, d15, [sp, #144]
	mov x16, x0
	mov x22, x1
	mov x25, x2
	blr x16		/* Test entry is a raw MAP_JIT address. */
	ldp d14, d15, [sp, #144]
	ldp d12, d13, [sp, #128]
	ldp d10, d11, [sp, #112]
	ldp d8, d9, [sp, #96]
	ldp x29, x30, [sp, #80]
	ldp x27, x28, [sp, #64]
	ldp x25, x26, [sp, #48]
	ldp x23, x24, [sp, #32]
	ldp x21, x22, [sp, #16]
	ldp x19, x20, [sp], #160
#if defined(__arm64e__)
	retab
#else
	ret
#endif
	.subsections_via_symbols
