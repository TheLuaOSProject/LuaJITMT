/* Immutable landing used by t-arm64-jit-exittab.c. */

	.text
	.p2align 2
	.globl _lj_test_arm64_exittab_trap
_lj_test_arm64_exittab_trap:
	.long 0xd503249f	/* bti j */
Llj_test_arm64_exittab_trap_again:
	.long 0xd4200000	/* brk #0 */
	b Llj_test_arm64_exittab_trap_again

	.subsections_via_symbols
