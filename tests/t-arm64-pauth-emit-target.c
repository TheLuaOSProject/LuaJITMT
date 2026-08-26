/*
** Focused macOS ARM64e proof for assembler function-target normalization.
*/

#include <assert.h>
#include <stdio.h>

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)) && \
    defined(LUAJIT_MT_ARM64_BOOTSTRAP) && \
    defined(LUAJIT_MT_ARM64_JIT_EXPERIMENTAL) && \
    defined(LJ_ARM64_PAUTH_EMIT_TEST_HELPERS)

#include "lj_obj.h"
#include "lj_vm.h"

extern char *lj_asm_arm64_emit_target_direct_test(void);
extern char *lj_asm_arm64_emit_target_runtime_test(ASMFunction target);
extern uint64_t
lj_asm_arm64_emit_target_indirect_bits_test(ASMFunction target);

static volatile ASMFunction runtime_slot;

static void runtime_target(void)
{
}

static char *raw_function_address(ASMFunction target)
{
#if LJ_ABI_PAUTH
  return ptrauth_strip(ptrauth_nop_cast(char *, target),
		       ptrauth_key_function_pointer);
#else
  return (char *)target;
#endif
}

int main(void)
{
  ASMFunction runtime;
  char *raw_runtime;

  assert(lj_asm_arm64_emit_target_direct_test() ==
	 (char *)(void *)lj_vm_exit_handler);

  runtime_slot = runtime_target;
  runtime = runtime_slot;
  raw_runtime = raw_function_address(runtime);
#if LJ_ABI_PAUTH
  /* Prove this path starts with an actually signed runtime pointer. */
  assert(ptrauth_nop_cast(char *, runtime) != raw_runtime);
  /* The far-call constant must retain that PAC for generated BLRAAZ. */
  assert(lj_asm_arm64_emit_target_indirect_bits_test(runtime) ==
	 (uint64_t)(uintptr_t)ptrauth_nop_cast(char *, runtime));
#else
  assert(lj_asm_arm64_emit_target_indirect_bits_test(runtime) ==
	 (uint64_t)(uintptr_t)runtime);
#endif
  assert(lj_asm_arm64_emit_target_runtime_test(runtime) == raw_runtime);

  puts("t-arm64-pauth-emit-target OK");
  return 0;
}

#else

int main(void)
{
  puts("t-arm64-pauth-emit-target SKIP");
  return 0;
}

#endif
