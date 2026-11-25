/* Inspect the next MOV r/m64,r64 at a Linux x64 single-step trap. */
static int next_store_is(const ucontext_t *ctx, const void *dst, uint64_t value)
{
  static const int greg[] = {
    REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
    REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15
  };
  const greg_t *regs = ctx->uc_mcontext.gregs;
  const unsigned char *pc = (const unsigned char *)regs[REG_RIP];
  unsigned rex = *pc++, modrm, mod, base, src;
  int32_t disp = 0;
  uintptr_t addr;
  if ((rex & 0xf8u) != 0x48u || *pc++ != 0x89)
    return 0;
  modrm = *pc++;
  mod = modrm >> 6;
  base = modrm & 7u;
  src = ((modrm >> 3) & 7u) | ((rex & 4u) << 1);
  if (mod == 3 || (uint64_t)regs[greg[src]] != value)
    return 0;
  if (base == 4) {
    unsigned sib = *pc++;
    if (((sib >> 3) & 7u) != 4 || (rex & 2u))
      return 0;  /* Only a base and displacement are used for these stores. */
    base = sib & 7u;
  }
  if (mod == 0 && base == 5)
    return 0;
  if (mod == 1)
    disp = (int8_t)*pc;
  else if (mod == 2)
    memcpy(&disp, pc, sizeof(disp));
  addr = (uintptr_t)regs[greg[base | ((rex & 1u) << 3)]] + (uintptr_t)disp;
  return addr == (uintptr_t)dst;
}
