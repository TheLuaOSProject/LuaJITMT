/*
** Focused regression test for one-upvalue FNEW allocation/publication paths.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_gc2.h"
#include "lj_arena.h"
#include "lj_dispatch.h"
#include "lj_func.h"
#include "lj_target.h"
#include "lj_tg.h"
#include "lj_vm.h"

#include "lib/lua_fixture_helpers.h"

/* Built by the M6 harness with LJ_FUNC_TEST_HELPERS enabled. */

static void run_script(lua_State *L, const char *code, const char *label)
{
  UNUSED(label);
  ljt_lua_dostring(L, code);
}

static int is_rex(uint8_t byte)
{
  return (byte & 0xf0u) == 0x40u;
}

static size_t find_bitmap_op(const uint8_t *mc, size_t len, size_t start,
			     int locked, uint8_t opcode, uint32_t disp)
{
  size_t i;
  for (i = start; i < len; i++) {
    size_t j = i;
    uint32_t got;
    if (locked) {
      if (mc[j++] != 0xf0u)
	continue;
    } else {
      if (mc[j] == 0xf0u || (i != 0 && mc[i-1] == 0xf0u) ||
	  (i > 1 && is_rex(mc[i-1]) && mc[i-2] == 0xf0u))
	continue;
    }
    if (j < len && is_rex(mc[j]))
      j++;
    if (j + 7u > len || mc[j] != 0x0fu || mc[j+1] != opcode)
      continue;
    got = (uint32_t)mc[j+3] | ((uint32_t)mc[j+4] << 8) |
	  ((uint32_t)mc[j+5] << 16) | ((uint32_t)mc[j+6] << 24);
    if (got == disp)
      return i;
  }
  return (size_t)-1;
}

static size_t find_gct_store(const uint8_t *mc, size_t len, uint8_t gct)
{
  size_t i;
  for (i = 0; i + 4u < len; i++) {
    uint8_t modrm, rm;
    size_t disp;
    if (mc[i] != 0xc6u)
      continue;
    modrm = mc[i+1];
    if ((modrm & 0xf8u) != 0x40u)  /* MOV byte [base+disp8], imm8. */
      continue;
    rm = modrm & 7u;
    disp = i + 2u + (rm == 4u);  /* Skip SIB for rsp/r12 bases. */
    if (disp + 1u < len && mc[disp] == offsetof(GCupval, gct) &&
	mc[disp+1] == gct)
      return i;
  }
  return (size_t)-1;
}

static size_t find_i32_store(const uint8_t *mc, size_t len, size_t start,
			     uint32_t field, uint32_t value)
{
  size_t i;
  for (i = start; i + 10u <= len; i++) {
    uint8_t modrm, rm;
    size_t disp;
    uint32_t got_field, got_value;
    if (mc[i] != 0xc7u)
      continue;
    modrm = mc[i+1];
    if ((modrm & 0xf8u) != 0x80u)  /* MOV dword [base+disp32], imm32. */
      continue;
    rm = modrm & 7u;
    disp = i + 2u + (rm == 4u);
    if (disp + 8u > len)
      continue;
    got_field = (uint32_t)mc[disp] | ((uint32_t)mc[disp+1] << 8) |
		((uint32_t)mc[disp+2] << 16) | ((uint32_t)mc[disp+3] << 24);
    got_value = (uint32_t)mc[disp+4] | ((uint32_t)mc[disp+5] << 8) |
		((uint32_t)mc[disp+6] << 16) | ((uint32_t)mc[disp+7] << 24);
    if (got_field == field && got_value == value)
      return i;
  }
  return (size_t)-1;
}

typedef struct X64MemInsn {
  size_t start, end;
  uint8_t reg, base;
  int indexed;
  int32_t disp;
} X64MemInsn;

static int decode_x64_mem_modrm(const uint8_t *mc, size_t len, size_t *jp,
				uint8_t rex, uint8_t modrm,
				X64MemInsn *ins)
{
  size_t j = *jp;
  uint8_t mod = modrm >> 6;
  uint8_t rm = modrm & 7u;
  uint8_t base_lo = rm;
  int disp32 = 0;

  if (mod == 3u)
    return 0;
  ins->reg = (uint8_t)(((modrm >> 3) & 7u) | ((rex & 4u) << 1));
  ins->indexed = 0;
  if (rm == 4u) {
    uint8_t sib, index;
    if (j >= len)
      return 0;
    sib = mc[j++];
    index = (sib >> 3) & 7u;
    base_lo = sib & 7u;
    ins->indexed = !(index == 4u && (rex & 2u) == 0);
    if (mod == 0u && base_lo == 5u)
      disp32 = 1;
  } else if (mod == 0u && rm == 5u) {
    disp32 = 1;  /* RIP-relative: no base register. */
  }
  ins->base = (mod == 0u && base_lo == 5u) ? 0xffu :
    (uint8_t)(base_lo | ((rex & 1u) << 3));
  if (mod == 1u) {
    if (j >= len)
      return 0;
    ins->disp = (int8_t)mc[j++];
  } else if (mod == 2u || disp32) {
    if (j + 4u > len)
      return 0;
    ins->disp = (int32_t)((uint32_t)mc[j] |
	((uint32_t)mc[j+1] << 8) | ((uint32_t)mc[j+2] << 16) |
	((uint32_t)mc[j+3] << 24));
    j += 4u;
  } else {
    ins->disp = 0;
  }
  *jp = j;
  return 1;
}

static int decode_x64_mov_store(const uint8_t *mc, size_t len, size_t pos,
				 X64MemInsn *ins)
{
  size_t j = pos;
  uint8_t rex = 0, modrm;
  if (j < len && is_rex(mc[j]))
    rex = mc[j++];
  if (!(rex & 8u) || j >= len || mc[j++] != 0x89u || j >= len)
    return 0;
  modrm = mc[j++];
  ins->start = pos;
  if (!decode_x64_mem_modrm(mc, len, &j, rex, modrm, ins))
    return 0;
  ins->end = j;
  return 1;
}

static size_t find_locked_cmpxchg(const uint8_t *mc, size_t len, size_t start,
				   int32_t field, int required_base,
				   X64MemInsn *found)
{
  size_t i;
  for (i = start; i + 5u < len; i++) {
    size_t j = i;
    uint8_t rex = 0, modrm;
    X64MemInsn ins;
    if (mc[j++] != 0xf0u)
      continue;
    if (j < len && is_rex(mc[j]))
      rex = mc[j++];
    if (!(rex & 0x08u) || j + 3u > len ||
	mc[j] != 0x0fu || mc[j+1] != 0xb1u)
      continue;
    j += 2u;
    modrm = mc[j++];
    ins.start = i;
    if (!decode_x64_mem_modrm(mc, len, &j, rex, modrm, &ins))
      continue;
    ins.end = j;
    if (!ins.indexed && (required_base < 0 || ins.base == required_base) &&
	ins.disp == field) {
      if (found)
	*found = ins;
      return i;
    }
  }
  return (size_t)-1;
}

static int decode_backward_jne(const uint8_t *mc, size_t len, size_t pos,
			       size_t floor, size_t *targetp, size_t *endp)
{
  size_t end;
  int32_t rel;
  int64_t target;
  if (pos + 2u <= len && mc[pos] == 0x75u) {
    rel = (int8_t)mc[pos+1];
    end = pos + 2u;
  } else if (pos + 6u <= len && mc[pos] == 0x0fu && mc[pos+1] == 0x85u) {
    rel = (int32_t)((uint32_t)mc[pos+2] |
	((uint32_t)mc[pos+3] << 8) | ((uint32_t)mc[pos+4] << 16) |
	((uint32_t)mc[pos+5] << 24));
    end = pos + 6u;
  } else {
    return 0;
  }
  target = (int64_t)end + rel;
  if (target < (int64_t)floor || target >= (int64_t)pos)
    return 0;
  *targetp = (size_t)target;
  *endp = end;
  return 1;
}

static int decode_x64_mov_rr(const uint8_t *mc, size_t len, size_t pos,
			     uint8_t *dstp, uint8_t *srcp, size_t *endp)
{
  size_t j = pos;
  uint8_t rex = 0, opcode, modrm, reg, rm;
  if (j < len && is_rex(mc[j]))
    rex = mc[j++];
  if (!(rex & 8u) || j + 2u > len)
    return 0;
  opcode = mc[j++];
  if (opcode != 0x8bu && opcode != 0x89u)
    return 0;
  modrm = mc[j++];
  if ((modrm >> 6) != 3u)
    return 0;
  reg = (uint8_t)(((modrm >> 3) & 7u) | ((rex & 4u) << 1));
  rm = (uint8_t)((modrm & 7u) | ((rex & 1u) << 3));
  *dstp = opcode == 0x8bu ? reg : rm;
  *srcp = opcode == 0x8bu ? rm : reg;
  *endp = j;
  return 1;
}

static int decode_x64_bit_rr(const uint8_t *mc, size_t len, size_t pos,
			     uint8_t opcode, uint8_t *valuep,
			     uint8_t *bitp, size_t *endp)
{
  size_t j = pos;
  uint8_t rex = 0, modrm;
  if (j < len && is_rex(mc[j]))
    rex = mc[j++];
  if (!(rex & 8u) || j + 3u > len || mc[j] != 0x0fu ||
      mc[j+1] != opcode)
    return 0;
  j += 2u;
  modrm = mc[j++];
  if ((modrm >> 6) != 3u)
    return 0;
  *bitp = (uint8_t)(((modrm >> 3) & 7u) | ((rex & 4u) << 1));
  *valuep = (uint8_t)((modrm & 7u) | ((rex & 1u) << 3));
  *endp = j;
  return 1;
}

static int decode_x64_arith1(const uint8_t *mc, size_t len, size_t pos,
			     uint8_t group, uint8_t reg, size_t *endp)
{
  size_t j = pos;
  uint8_t rex = 0, modrm, dst;
  if (j < len && is_rex(mc[j]))
    rex = mc[j++];
  if (j + 3u > len || mc[j++] != 0x83u)
    return 0;
  modrm = mc[j++];
  dst = (uint8_t)((modrm & 7u) | ((rex & 1u) << 3));
  if ((modrm >> 6) != 3u || ((modrm >> 3) & 7u) != group ||
      dst != reg || mc[j++] != 1u)
    return 0;
  *endp = j;
  return 1;
}

static int decode_x64_arith_imm(const uint8_t *mc, size_t len, size_t pos,
				 uint8_t group, uint8_t reg, uint8_t imm,
				 size_t *endp)
{
  size_t j = pos;
  uint8_t rex = 0, modrm, dst;
  if (j < len && is_rex(mc[j]))
    rex = mc[j++];
  if (j + 3u > len || mc[j++] != 0x83u)
    return 0;
  modrm = mc[j++];
  dst = (uint8_t)((modrm & 7u) | ((rex & 1u) << 3));
  if ((modrm >> 6) != 3u || ((modrm >> 3) & 7u) != group ||
	 dst != reg || mc[j++] != imm)
    return 0;
  *endp = j;
  return 1;
}

static int decode_x64_adjust1(const uint8_t *mc, size_t len, size_t pos,
			      uint8_t group, uint8_t reg, size_t *endp)
{
  size_t j = pos;
  uint8_t rex = 0, modrm, dst, base;
  int8_t wanted = group == XOg_ADD ? 1 : -1;
  if (decode_x64_arith1(mc, len, pos, group, reg, endp))
    return 1;
  if (j < len && is_rex(mc[j]))
    rex = mc[j++];
  if (j + 3u > len || mc[j++] != 0x8du)
    return 0;
  modrm = mc[j++];
  dst = (uint8_t)(((modrm >> 3) & 7u) | ((rex & 4u) << 1));
  base = (uint8_t)((modrm & 7u) | ((rex & 1u) << 3));
  if ((modrm >> 6) != 1u || dst != reg || base != reg ||
	(int8_t)mc[j++] != wanted)
    return 0;
  *endp = j;
  return 1;
}

static int decode_x64_jcc(const uint8_t *mc, size_t len, size_t pos,
			  uint8_t cc, size_t *endp, size_t *targetp)
{
  size_t end;
  int32_t rel;
  int64_t target;
  if (pos + 2u <= len && mc[pos] == (uint8_t)(0x70u + cc)) {
	end = pos + 2u;
	rel = (int8_t)mc[pos+1];
  } else if (pos + 6u <= len && mc[pos] == 0x0fu &&
      mc[pos+1] == (uint8_t)(0x80u + cc)) {
    end = pos + 6u;
    rel = (int32_t)((uint32_t)mc[pos+2] |
	((uint32_t)mc[pos+3] << 8) | ((uint32_t)mc[pos+4] << 16) |
	((uint32_t)mc[pos+5] << 24));
  } else {
    return 0;
  }
  target = (int64_t)end + rel;
  if (target < 0 || target >= (int64_t)len)
    return 0;
  *endp = end;
  *targetp = (size_t)target;
  return 1;
}

static int decode_root_bit_transition(const uint8_t *mc, size_t len,
				      size_t *posp, uint8_t value,
				      uint8_t bit, uint32_t *fromp,
				      uint32_t *top, size_t *impossiblep)
{
  size_t p = *posp, next, impossible;
  uint8_t gotvalue, gotbit;
  uint32_t from, to;
  if (decode_x64_bit_rr(mc, len, p, 0xabu, &gotvalue, &gotbit, &p)) {
    from = 0;
    if (gotvalue != value || gotbit != bit ||
	!decode_x64_jcc(mc, len, p, CC_B, &p, &impossible))
      return 0;
    if (decode_x64_bit_rr(mc, len, p, 0xb3u,
			  &gotvalue, &gotbit, &next)) {
      if (gotvalue != value || gotbit != bit)
	return 0;
      p = next;
      to = 0;
    } else {
      to = 1;
    }
  } else if (decode_x64_bit_rr(mc, len, p, 0xb3u,
				&gotvalue, &gotbit, &p)) {
    from = 1;
    if (gotvalue != value || gotbit != bit ||
	!decode_x64_jcc(mc, len, p, CC_AE, &p, &impossible))
      return 0;
    if (decode_x64_bit_rr(mc, len, p, 0xabu,
			  &gotvalue, &gotbit, &next)) {
      if (gotvalue != value || gotbit != bit)
	return 0;
      p = next;
      to = 1;
    } else {
      to = 0;
    }
  } else {
    return 0;
  }
  if (*impossiblep != (size_t)-1 && *impossiblep != impossible)
    return 0;
  *impossiblep = impossible;
  *posp = p;
  *fromp = from;
  *top = to;
  return 1;
}

static int decode_root_transition(const uint8_t *mc, size_t len,
				  const X64MemInsn *cas, uint32_t *fromp,
				  uint32_t *top, size_t *jne_endp,
				  size_t *impossiblep)
{
  size_t p, first_end, jne_end, impossible = (size_t)-1;
  uint8_t dst, src, value, bit;
  uint32_t lofrom, loto, hifrom, hito;
  if (!decode_backward_jne(mc, len, cas->end, 0, &p, &jne_end) ||
      !decode_x64_mov_rr(mc, len, p, &dst, &src, &p) ||
      dst != cas->reg || src != RID_RET)
    return 0;
  if (!decode_x64_bit_rr(mc, len, p, 0xabu, &value, &bit, &first_end) &&
      !decode_x64_bit_rr(mc, len, p, 0xb3u, &value, &bit, &first_end))
    return 0;
  if (first_end <= p || value != cas->reg ||
      !decode_root_bit_transition(mc, len, &p, value, bit,
				   &lofrom, &loto, &impossible) ||
      !decode_x64_adjust1(mc, len, p, XOg_ADD, bit, &p) ||
      !decode_root_bit_transition(mc, len, &p, value, bit,
				   &hifrom, &hito, &impossible) ||
      !decode_x64_adjust1(mc, len, p, XOg_SUB, bit, &p))
    return 0;
  /* DynASM's hand-written commit restores an unchanged low bit after the high
  ** transition, while the JIT template restores it before advancing to high.
  ** Accept either exact ordering and derive the same packed from/to state. */
  if (p != cas->start) {
    size_t next;
    uint8_t gotvalue, gotbit;
    uint8_t opcode = lofrom ? 0xabu : 0xb3u;
    if (loto == lofrom ||
	!decode_x64_bit_rr(mc, len, p, opcode,
			  &gotvalue, &gotbit, &next) ||
	gotvalue != value || gotbit != bit || next != cas->start)
      return 0;
    loto = lofrom;
    p = next;
  }
  if (p != cas->start)
    return 0;
  *fromp = lofrom | (hifrom << 1);
  *top = loto | (hito << 1);
  *jne_endp = jne_end;
  *impossiblep = impossible;
  return 1;
}

static size_t find_root_transition(const uint8_t *mc, size_t len,
				    size_t start, uint32_t wanted_from,
				    uint32_t wanted_to, X64MemInsn *found,
				    size_t *impossiblep)
{
  while (start < len) {
    X64MemInsn cas;
    size_t pos = find_locked_cmpxchg(mc, len, start,
	(int32_t)offsetof(GCArena, root), -1, &cas);
    size_t jne_end, impossible;
    uint32_t from, to;
    if (pos == (size_t)-1)
      return pos;
    if (decode_root_transition(mc, len, &cas, &from, &to, &jne_end,
			       &impossible) &&
	from == wanted_from && to == wanted_to) {
      if (found)
	*found = cas;
      if (impossiblep)
	*impossiblep = impossible;
      return pos;
    }
    start = pos + 1u;
  }
  return (size_t)-1;
}

static int decode_packed_transition_body(const uint8_t *mc, size_t len,
					 const X64MemInsn *cas, size_t p,
					 uint32_t lane_bits, uint32_t *fromp,
					 uint32_t *top, size_t *impossiblep)
{
  size_t next, impossible = (size_t)-1;
  uint8_t dst, src, value, bit;
  uint32_t from = 0, to = 0, j;
  if (lane_bits < 2u || lane_bits > 4u ||
      !decode_x64_mov_rr(mc, len, p, &dst, &src, &p) ||
      dst != cas->reg || src != RID_RET)
    return 0;
  for (j = 0; j < lane_bits; j++) {
    uint32_t bitfrom, bitto;
    if (!decode_x64_bit_rr(mc, len, p, 0xabu, &value, &bit, &next) &&
	!decode_x64_bit_rr(mc, len, p, 0xb3u, &value, &bit, &next))
      return 0;
    if (value != cas->reg ||
	!decode_root_bit_transition(mc, len, &p, value, bit,
				   &bitfrom, &bitto, &impossible))
      return 0;
    from |= bitfrom << j;
    to |= bitto << j;
    if (j + 1u < lane_bits &&
	!decode_x64_adjust1(mc, len, p, XOg_ADD, bit, &p))
      return 0;
  }
  if (!decode_x64_arith_imm(mc, len, p, XOg_SUB, bit,
			    (uint8_t)(lane_bits-1u), &p) || p != cas->start)
    return 0;
  *fromp = from;
  *top = to;
  *impossiblep = impossible;
  return 1;
}

static int decode_packed_transition(const uint8_t *mc, size_t len,
				    const X64MemInsn *cas,
				    uint32_t lane_bits, uint32_t *fromp,
				    uint32_t *top, size_t *impossiblep)
{
  size_t p, jne_end;
  if (!decode_backward_jne(mc, len, cas->end, 0, &p, &jne_end))
    return 0;
  if (decode_packed_transition_body(mc, len, cas, p, lane_bits,
				    fromp, top, impossiblep))
    return 1;
  /* Commit CAS failure redispatches C/M/L from the refreshed RAX sample, so
  ** its JNE target precedes the exact C->L arm. Locate that arm locally. */
  p = cas->start > 192u ? cas->start - 192u : 0u;
  for (; p < cas->start; p++)
    if (decode_packed_transition_body(mc, len, cas, p, lane_bits,
				      fromp, top, impossiblep))
      return 1;
  return 0;
}

static size_t find_lifetime_transition(const uint8_t *mc, size_t len,
				       size_t start, uint32_t wanted_from,
				       uint32_t wanted_to, X64MemInsn *found,
				       size_t *impossiblep)
{
  while (start < len) {
    X64MemInsn cas;
    size_t pos = find_locked_cmpxchg(mc, len, start,
	(int32_t)offsetof(GCArena, lifetime), -1, &cas);
    size_t impossible;
    uint32_t from, to;
    if (pos == (size_t)-1)
      return pos;
    if (decode_packed_transition(mc, len, &cas, 4u, &from, &to,
				 &impossible) &&
	from == wanted_from && to == wanted_to) {
      if (found)
	*found = cas;
      if (impossiblep)
	*impossiblep = impossible;
      return pos;
    }
    start = pos + 1u;
  }
  return (size_t)-1;
}

static int decode_pending_retry(const uint8_t *mc, size_t len, size_t floor,
				 const X64MemInsn *cas, X64MemInsn *tail,
				 size_t *jne_endp)
{
  X64MemInsn link;
  size_t target, jne_end, i, scan;
  if (!decode_backward_jne(mc, len, cas->end, floor, &target, &jne_end) ||
      !decode_x64_mov_store(mc, len, target, tail) ||
      tail->reg != RID_RET || tail->indexed ||
      tail->disp != (int32_t)offsetof(GChead, nextgc) ||
      tail->base == cas->reg)
    return 0;

  /* The retry target is the tail rewrite `uv->nextgc = RAX`. Tie that base
  ** back to the stable `fn->nextgc = uv` instruction immediately before it,
  ** without depending on the allocator's choice of fn/uv registers. */
  scan = target > 15u ? target - 15u : floor;
  if (scan < floor)
    scan = floor;
  for (i = scan; i < target; i++) {
    if (decode_x64_mov_store(mc, len, i, &link) && link.end == target &&
	!link.indexed && link.base == cas->reg && link.reg == tail->base &&
	link.disp == (int32_t)offsetof(GChead, nextgc)) {
      *jne_endp = jne_end;
      return 1;
    }
  }
  return 0;
}

static size_t find_rel_call_target(const uint8_t *mc, size_t len,
				   const void *target)
{
  size_t i;
  uintptr_t want = (uintptr_t)target;
  for (i = 0; i + 5u <= len; i++) {
    int32_t rel;
    uintptr_t got;
    if (mc[i] != 0xe8u)
      continue;
    rel = (int32_t)((uint32_t)mc[i+1] | ((uint32_t)mc[i+2] << 8) |
	((uint32_t)mc[i+3] << 16) | ((uint32_t)mc[i+4] << 24));
    got = (uintptr_t)(mc + i + 5u) + rel;
    if (got == want)
      return i;
  }
  return (size_t)-1;
}

static int decode_rel_jmp_target(const uint8_t *mc, size_t len, size_t pos,
				 size_t *targetp, size_t *endp)
{
  size_t end;
  int32_t rel;
  int64_t target;
  if (pos + 2u <= len && mc[pos] == 0xebu) {
    rel = (int8_t)mc[pos+1];
    end = pos + 2u;
  } else if (pos + 5u <= len && mc[pos] == 0xe9u) {
    rel = (int32_t)((uint32_t)mc[pos+1] |
	((uint32_t)mc[pos+2] << 8) | ((uint32_t)mc[pos+3] << 16) |
	((uint32_t)mc[pos+4] << 24));
    end = pos + 5u;
  } else {
    return 0;
  }
  target = (int64_t)end + rel;
  if (target < 0 || target >= (int64_t)len)
    return 0;
  *targetp = (size_t)target;
  *endp = end;
  return 1;
}

static void assert_vm_tnew_membership_order(void)
{
  const uint8_t *mc = (const uint8_t *)lj_vm_asm_begin + lj_bc_ofs[BC_TNEW];
  size_t len = (size_t)(lj_bc_ofs[BC_TDUP] - lj_bc_ofs[BC_TNEW]);
  size_t life_claim, claim, life_rollback, gct, ready, block;
  size_t hint1, pending, hint2, commit;
  size_t life_bad = (size_t)-1, rollback_bad = (size_t)-1;
  size_t claim_bad = (size_t)-1, commit_bad = (size_t)-1;
  size_t retry_target, retry_end;
  size_t cold_call, cold_jmp = (size_t)-1, cold_restart = (size_t)-1;
  X64MemInsn insn;
  size_t i;

  assert(lj_bc_ofs[BC_TDUP] > lj_bc_ofs[BC_TNEW]);
  life_claim = find_lifetime_transition(mc, len, 0,
	LJ_ARENA_LIFETIME_FREE, LJ_ARENA_LIFETIME_CONSTRUCT,
	&insn, &life_bad);
  claim = find_root_transition(mc, len, 0, LJ_ARENA_ROOT_NONE,
			       LJ_ARENA_ROOT_LINKING, &insn, &claim_bad);
  life_rollback = find_lifetime_transition(mc, len, claim,
	LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_FREE,
	&insn, &rollback_bad);
  gct = find_gct_store(mc, len, (uint8_t)~LJ_TTAB);
  ready = find_bitmap_op(mc, len, 0, 0, 0xabu,
			  (uint32_t)offsetof(GCArena, ready));
  block = find_bitmap_op(mc, len, 0, 0, 0xabu,
			  (uint32_t)offsetof(GCArena, block));
  hint1 = find_i32_store(mc, len, block,
	(uint32_t)offsetof(global_State, gcroot_pending_hint), 1u);
  pending = find_locked_cmpxchg(mc, len, block,
	(int32_t)DISPATCH_TG(gcroot_pending), RID_DISPATCH, &insn);
  assert(pending != (size_t)-1);
  assert(decode_backward_jne(mc, len, insn.end, block,
			     &retry_target, &retry_end));
  hint2 = find_i32_store(mc, len, retry_end,
	(uint32_t)offsetof(global_State, gcroot_pending_hint), 1u);
  commit = find_root_transition(mc, len, retry_end,
				LJ_ARENA_ROOT_LINKING,
				LJ_ARENA_ROOT_MEMBER, &insn, &commit_bad);
  cold_call = find_rel_call_target(mc, len, (const void *)lj_gc_step_fixtop);
  if (cold_call != (size_t)-1) {
    size_t stop = cold_call + 48u < len ? cold_call + 48u : len;
    for (i = cold_call + 5u; i < stop; i++) {
      size_t end;
      if (decode_rel_jmp_target(mc, len, i, &cold_restart, &end)) {
	cold_jmp = i;
	break;
      }
    }
  }

  assert(life_claim != (size_t)-1 && claim != (size_t)-1 &&
	 life_rollback != (size_t)-1 && gct != (size_t)-1);
  assert(ready != (size_t)-1 && block != (size_t)-1);
  assert(hint1 != (size_t)-1 && hint2 != (size_t)-1);
  assert(commit != (size_t)-1);
  assert(claim < claim_bad && claim_bad <= life_rollback);
  assert(life_bad == rollback_bad && rollback_bad == commit_bad &&
	 commit_bad + 3u <= len);
  assert(mc[commit_bad] == 0xccu && mc[commit_bad+1u] == 0xebu &&
	 mc[commit_bad+2u] == 0xfdu);
  assert(cold_call != (size_t)-1 && cold_jmp != (size_t)-1);
  /* The cold fixtop helper must restart at the original eligibility label
  ** (`test RDd,RDd`), never at either later membership CAS retry loop. */
  assert(cold_restart + 2u <= len);
  assert(mc[cold_restart] == 0x85u && mc[cold_restart+1] == 0xc0u);
  assert(cold_restart < life_claim && life_claim < claim);
  assert(claim < gct);
  assert(gct < ready && ready < block);
  assert(block < hint1 && hint1 < pending);
  assert(retry_target < pending && pending < retry_end);
  assert(retry_end <= hint2 && hint2 < commit);
}

static void assert_traced_fnew_publication_order(lua_State *L)
{
  int traceno;
  for (traceno = 1; traceno <= 256; traceno++) {
    size_t len, fn_header, uv_header, mark_set, mark_clear, first_mark;
    size_t ready, block;
    size_t pending_hint, pending_cas, pending_hint2, retry_jne_end = 0;
    size_t life_claim1, life_claim2, life_commit1, life_commit2;
    size_t root_claim1, root_claim2, root_commit1, root_commit2;
    size_t root_rollback;
    size_t life_claim1_bad = (size_t)-1, life_claim2_bad = (size_t)-1;
    size_t life_commit1_bad = (size_t)-1, life_commit2_bad = (size_t)-1;
    size_t claim1_bad = (size_t)-1, claim2_bad = (size_t)-1;
    size_t commit1_bad = (size_t)-1, commit2_bad = (size_t)-1;
    size_t rollback_bad = (size_t)-1;
    X64MemInsn pending_cmp, pending_tail;
    X64MemInsn root_insn;
    int pending_retry;
    const uint8_t *mc;
    lua_getglobal(L, "require");
    lua_pushliteral(L, "jit.util");
    ljt_lua_pcall(L, 1, 1, "load jit.util for FNEW mcode");
    lua_getfield(L, -1, "tracemc");
    lua_pushinteger(L, traceno);
    ljt_lua_pcall(L, 1, 1, "fetch FNEW trace mcode");
    lua_remove(L, -2);  /* jit.util table. */
    mc = (const uint8_t *)lua_tolstring(L, -1, &len);
    if (mc == NULL) {
      lua_pop(L, 1);
      continue;
    }
    mark_set = find_bitmap_op(mc, len, 0, 1, 0xabu,
			      (uint32_t)offsetof(GCArena, mark));
    block = find_bitmap_op(mc, len, 0, 0, 0xabu,
			   (uint32_t)offsetof(GCArena, block));
    ready = find_bitmap_op(mc, len, 0, 0, 0xabu,
			   (uint32_t)offsetof(GCArena, ready));
    if (mark_set == (size_t)-1 || block == (size_t)-1) {
      lua_pop(L, 1);
      continue;
    }
    fn_header = find_gct_store(mc, len, (uint8_t)~LJ_TFUNC);
    uv_header = find_gct_store(mc, len, (uint8_t)~LJ_TUPVAL);
    mark_clear = find_bitmap_op(mc, len, 0, 1, 0xb3u,
				(uint32_t)offsetof(GCArena, mark));
    pending_cas = find_locked_cmpxchg(mc, len, block,
				     (int32_t)DISPATCH_TG(gcroot_pending),
				     RID_DISPATCH, &pending_cmp);
    pending_retry = pending_cas != (size_t)-1 &&
      decode_pending_retry(mc, len, block, &pending_cmp, &pending_tail,
			   &retry_jne_end);
    pending_hint = !pending_retry ? (size_t)-1 :
      find_i32_store(mc, len, pending_tail.end,
	(uint32_t)offsetof(global_State, gcroot_pending_hint), 1u);
    pending_hint2 = !pending_retry ? (size_t)-1 :
      find_i32_store(mc, len, retry_jne_end,
	(uint32_t)offsetof(global_State, gcroot_pending_hint), 1u);
    life_claim1 = find_lifetime_transition(mc, len, 0,
	LJ_ARENA_LIFETIME_FREE, LJ_ARENA_LIFETIME_CONSTRUCT,
	&root_insn, &life_claim1_bad);
    life_claim2 = life_claim1 == (size_t)-1 ? (size_t)-1 :
      find_lifetime_transition(mc, len, life_claim1 + 1u,
	LJ_ARENA_LIFETIME_FREE, LJ_ARENA_LIFETIME_CONSTRUCT,
	&root_insn, &life_claim2_bad);
    root_claim1 = find_root_transition(mc, len, 0,
				       LJ_ARENA_ROOT_NONE,
				       LJ_ARENA_ROOT_LINKING, &root_insn,
				       &claim1_bad);
    root_claim2 = root_claim1 == (size_t)-1 ? (size_t)-1 :
      find_root_transition(mc, len, root_claim1 + 1u,
			   LJ_ARENA_ROOT_NONE,
			   LJ_ARENA_ROOT_LINKING, &root_insn, &claim2_bad);
    root_commit1 = !pending_retry ? (size_t)-1 :
      find_root_transition(mc, len, retry_jne_end,
			   LJ_ARENA_ROOT_LINKING,
			   LJ_ARENA_ROOT_MEMBER, &root_insn, &commit1_bad);
    root_commit2 = root_commit1 == (size_t)-1 ? (size_t)-1 :
      find_root_transition(mc, len, root_commit1 + 1u,
			   LJ_ARENA_ROOT_LINKING,
			   LJ_ARENA_ROOT_MEMBER, &root_insn, &commit2_bad);
    life_commit1 = !pending_retry ? (size_t)-1 :
      find_lifetime_transition(mc, len, retry_jne_end,
	LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_LIVE,
	&root_insn, &life_commit1_bad);
    life_commit2 = life_commit1 == (size_t)-1 ? (size_t)-1 :
      find_lifetime_transition(mc, len, life_commit1 + 1u,
	LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_LIVE,
	&root_insn, &life_commit2_bad);
    root_rollback = find_root_transition(mc, len, 0,
					 LJ_ARENA_ROOT_LINKING,
					 LJ_ARENA_ROOT_NONE,
					 &root_insn, &rollback_bad);
    assert(fn_header != (size_t)-1 && uv_header != (size_t)-1);
    assert(mark_clear != (size_t)-1);
    assert(ready != (size_t)-1);
    assert(pending_hint != (size_t)-1);
    assert(pending_cas != (size_t)-1);
    assert(pending_retry);
    assert(pending_hint2 != (size_t)-1);
    assert(life_claim1 != (size_t)-1 && life_claim2 != (size_t)-1);
    assert(root_claim1 != (size_t)-1 && root_claim2 != (size_t)-1);
    assert(life_commit1 != (size_t)-1 && life_commit2 != (size_t)-1);
    assert(root_commit1 != (size_t)-1 && root_commit2 != (size_t)-1);
    assert(root_rollback != (size_t)-1);
    first_mark = mark_set < mark_clear ? mark_set : mark_clear;
    /* x64 emits backwards: this guards the order in executable mcode, not the
    ** visually misleading order of emit_* calls in lj_asm_x86.h. */
    assert(fn_header < first_mark);
    assert(uv_header < first_mark);
    /* Both exact starts are LINKING before any header/bitmap discovery. The
    ** decoded BTS/BTR/Jcc sequences above prove the emitted state transforms,
    ** rather than merely counting locked instructions at the root offset. */
    assert(life_claim1 < root_claim1 && root_claim1 < life_claim2);
    assert(life_claim2 < root_claim2);
    assert(root_claim2 < fn_header && root_claim2 < uv_header);
    assert(first_mark < ready);
    assert(ready < block);
    assert(block < pending_tail.start);
    assert(pending_tail.end <= pending_hint);
    assert(pending_hint - pending_tail.end <= 16u);
    assert(pending_hint < pending_cas);
    assert(pending_cmp.end < retry_jne_end);
    assert(retry_jne_end <= pending_hint2);
    assert(pending_hint2 - retry_jne_end <= 32u);
    assert(pending_hint2 < life_commit1 && life_commit1 < root_commit1);
    assert(root_commit1 < life_commit2 && life_commit2 < root_commit2);
    /* The pre-visibility pair rollback is present but out of the normal
    ** success path; its physical placement is allocator-dependent. */
    /* Every validation edge except the second claim is terminal. The second
    ** claim enters the first claim's exact rollback block before that same
    ** terminal INT3 loop. */
    assert(life_claim1_bad != (size_t)-1 &&
	   life_claim2_bad != (size_t)-1 && claim1_bad != (size_t)-1 &&
	   claim2_bad != (size_t)-1);
    assert(life_commit1_bad == commit1_bad &&
	   life_commit2_bad == commit1_bad &&
	   commit1_bad == commit2_bad && commit1_bad == rollback_bad);
    assert(commit1_bad + 3u <= len && mc[commit1_bad] == 0xccu &&
	   mc[commit1_bad+1u] == 0xebu && mc[commit1_bad+2u] == 0xfdu);
    assert(claim2_bad < len && root_rollback < len);
    lua_pop(L, 1);
    return;
  }
  assert(0 && "missing traced inline FNEW bitmap publication");
}

static void reset_gc1num_bump_counters(void)
{
  lj_func_test_reset_gc1num_bump_fast_calls();
  lj_func_test_reset_gc1num_bump_fallback_calls();
}

static uint32_t gc1num_bump_helper_calls(void)
{
  return lj_func_test_gc1num_bump_fast_calls() +
	 lj_func_test_gc1num_bump_fallback_calls();
}

static void assert_arena_root_member(const void *p)
{
  GCArena *a = lj_arena_of(p);
  uint32_t cell = lj_arena_cellof(p);
  assert(cell >= LJ_AFIRST_CELL && cell < LJ_ARENA_CELLS);
  assert(lj_arena_lifetime_state_acq(a, cell) == LJ_ARENA_LIFETIME_LIVE);
  assert(lj_arena_root_state_acq(a, cell) == LJ_ARENA_ROOT_MEMBER);
}

static int packed_lane_transition(uint32_t state, uint32_t lane_bits,
				  uint32_t from, uint32_t to,
				  uint32_t *resultp)
{
  uint32_t mask = (1u << lane_bits) - 1u;
  if (state > mask || from > mask || to > mask || state != from)
    return 0;
  *resultp = (state & ~mask) | to;
  return 1;
}

static void assert_lifetime_state_matrix(void)
{
  uint32_t state;
  for (state = 0; state < 16u; state++) {
    uint32_t result = 0xffu;
    int claim = packed_lane_transition(state, 4u,
	LJ_ARENA_LIFETIME_FREE, LJ_ARENA_LIFETIME_CONSTRUCT, &result);
    int normal_commit = packed_lane_transition(state, 4u,
	LJ_ARENA_LIFETIME_CONSTRUCT, LJ_ARENA_LIFETIME_LIVE, &result);
    int recovery_commit = packed_lane_transition(state, 4u,
	LJ_ARENA_LIFETIME_MUTATING, LJ_ARENA_LIFETIME_MUTATING, &result);
    int completed_commit = packed_lane_transition(state, 4u,
	LJ_ARENA_LIFETIME_LIVE, LJ_ARENA_LIFETIME_LIVE, &result);
    /* These are the three emitted dispatch contracts. In particular, upper
    ** nibble states DESTRUCT/RESCUE and 6..15 can never masquerade as the old
    ** two-bit C/M encodings. */
    assert(claim == (state == 0u));
    assert(normal_commit == (state == 2u));
    assert(recovery_commit == (state == 3u));
    assert(completed_commit == (state == 1u));
    assert((claim + normal_commit + recovery_commit + completed_commit) <= 1);
    if (claim)
      assert(result == LJ_ARENA_LIFETIME_CONSTRUCT);
    else if (normal_commit || completed_commit)
      assert(result == LJ_ARENA_LIFETIME_LIVE);
    else if (recovery_commit)
      assert(result == LJ_ARENA_LIFETIME_MUTATING);
    else
      assert(result == 0xffu);
    if (state >= LJ_ARENA_LIFETIME_DESTRUCT)
      assert(!claim && !normal_commit && !recovery_commit &&
	     !completed_commit);
  }
}

static void assert_arena_root_range_state(const GCArena *a, uint32_t cell,
					  uint32_t ncells, uint32_t state)
{
  uint32_t i;
  assert(a != NULL && ncells != 0);
  assert(cell >= LJ_AFIRST_CELL && ncells <= LJ_ARENA_CELLS - cell);
  for (i = 0; i < ncells; i++)
    assert(lj_arena_root_state_acq(a, cell + i) == state);
}

static int assert_fnew_root_layout(GCfunc *fn)
{
  GCupval *uv = func_uv_acq(&fn->l, 0);
  GCArena *a = lj_arena_of(fn);
  uint32_t fncell = lj_arena_cellof(fn);
  uint32_t uvcell = lj_arena_cellof(uv);
  uint32_t fncells = lj_arena_ncells(sizeLfunc(1));
  uint32_t uvcells = lj_arena_ncells(sizeof(GCupval));
  assert_arena_root_member(fn);
  assert_arena_root_member(uv);
  assert(lj_arena_of(uv) == a && uvcell == fncell + fncells);
  if (fncells > 1u)
    assert_arena_root_range_state(a, fncell + 1u, fncells - 1u,
					  LJ_ARENA_ROOT_NONE);
  if (uvcells > 1u)
    assert_arena_root_range_state(a, uvcell + 1u, uvcells - 1u,
					  LJ_ARENA_ROOT_NONE);
  return fncell / LJ_ARENA_ROOT_CELLS_PER_WORD !=
	 uvcell / LJ_ARENA_ROOT_CELLS_PER_WORD;
}

static void prime_traversable_bump_window(TGState *tg)
{
  TGAlloc *alloc = &tg->alloc;
  GCArena *a = lj_arena_map(&tg->prng, LJ_AF_TRAVERSABLE);
  assert(a != NULL);
  lj_arena_owner_rel(a, lj_arena_alloc_owner_acq(alloc));
  lj_arena_next_rel(a, alloc->owned[LJ_ARENAK_TRAVERSABLE]);
  alloc->owned[LJ_ARENAK_TRAVERSABLE] = a;
  alloc->bump[LJ_ARENAK_TRAVERSABLE].a = a;
  alloc->bump[LJ_ARENAK_TRAVERSABLE].cell = LJ_AFIRST_CELL;
  alloc->bump[LJ_ARENAK_TRAVERSABLE].end = LJ_ARENA_CELLS;
  assert(lj_arena_alloc_register_existing(alloc));
}

static void test_vm_tnew_root_member(lua_State *L, global_State *g,
				     TGState *tg)
{
  GCArena *a;
  uint32_t cell, ncells = lj_arena_ncells(sizeof(GCtab));
  GCtab *t;
  ljt_lua_loadstring(L, "return {}\n");
  (void)lj_gc_flush_root_pending(g);
  prime_traversable_bump_window(tg);
  a = tg->alloc.bump[LJ_ARENAK_TRAVERSABLE].a;
  cell = tg->alloc.bump[LJ_ARENAK_TRAVERSABLE].cell;
  /* The direct VM is a consumer of the allocator-owned reservation token: the
  ** exact start and its undiscovered interiors must arrive NONE(00). */
  assert_arena_root_range_state(a, cell, ncells, LJ_ARENA_ROOT_NONE);
  /* total >= 0 deterministically takes BC_TNEW's fixtop cold edge. After the
  ** helper, the handler must reload RD/G, restart eligibility, and finish the
  ** same inline allocation with committed membership. */
  lj_gc_threshold_store(g, 0);
  ljt_lua_pcall(L, 0, 1, "x64 cold-fixtop TNEW membership commit");
  lj_gc_threshold_store(g, UINT64_MAX / 2u);
  assert(tvistab(L->top - 1));
  t = tabV(L->top - 1);
  assert(lj_arena_of(t) == a && lj_arena_cellof(t) == cell);
  assert_arena_root_member(t);
  if (ncells > 1u)
    assert_arena_root_range_state(a, cell + 1u, ncells - 1u,
					  LJ_ARENA_ROOT_NONE);
  assert(lj_gc_flush_root_pending(g) >= 1u);
  lua_pop(L, 1);
}

static void test_vm_tnew_branch_targets(lua_State *L, global_State *g)
{
  uint32_t old_allocf_arena = la_load32_acq(&g->allocf_arena);

  /* RD!=0 must retain BC_TNEW's original >6 generic-table target. */
  ljt_lua_loadstring(L, "return { 11, 22, 33 }\n");
  ljt_lua_pcall(L, 0, 1, "x64 nonempty TNEW branch target");
  assert(lua_istable(L, -1));
  assert(lua_objlen(L, -1) == 3u);
  lua_rawgeti(L, -1, 3);
  assert(lua_tointeger(L, -1) == 33);
  lua_pop(L, 2);

  /* A failed fast-path eligibility sample must retain >7, not enter any
  ** membership label inserted between the predicate and the slow helper. */
  assert(old_allocf_arena != 0);
  la_store32_rel(&g->allocf_arena, 0);
  ljt_lua_loadstring(L, "return {}\n");
  ljt_lua_pcall(L, 0, 1, "x64 TNEW eligibility fallback target");
  la_store32_rel(&g->allocf_arena, old_allocf_arena);
  assert(lua_istable(L, -1));
  lua_pop(L, 1);
}

static void suppress_new_trace_recording(lua_State *L, global_State *g)
{
  run_script(L, "jit.opt.start('hotloop=1000000', 'hotexit=1000000')\n",
	     "suppress fresh trace recording");
  lj_dispatch_init_hotcount(g);
}

static GCproto *first_child_proto(GCproto *pt)
{
  MSize i;
  assert((pt->flags & PROTO_CHILD) != 0);
  for (i = 0; i < pt->sizekgc; i++) {
    GCobj *o = proto_kgc(pt, ~(ptrdiff_t)i);
    if (o->gch.gct == ~LJ_TPROTO)
      return gco2pt(o);
  }
  assert(0 && "missing child proto");
  return NULL;
}

static GCfunc *top_lfunc(lua_State *L)
{
  GCfunc *fn;
  assert(tvisfunc(L->top - 1));
  fn = funcV(L->top - 1);
  assert(isluafunc(fn));
  return fn;
}

static void test_traced_behavior(lua_State *L)
{
  uint32_t fallback0;
  const char *code =
    "local util = require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local t = {}\n"
    "for i = 1, 100 do\n"
    "  local x = i\n"
    "  t[i] = function()\n"
    "    x = x + 1\n"
    "    return x\n"
    "  end\n"
    "end\n"
    "assert(util.traceinfo(1), 'numeric FNEW loop did not trace')\n"
    "assert(t[1]() == 2)\n"
    "assert(t[2]() == 3)\n"
    "assert(t[100]() == 101)\n"
    "assert(t[1]() == 3)\n"
    "assert(debug.upvalueid(t[1], 1) ~= debug.upvalueid(t[2], 1))\n"
    "local name = debug.setupvalue(t[1], 1, 50)\n"
    "assert(name == 'x', name)\n"
    "assert(t[1]() == 51)\n"
    "assert(t[2]() == 4)\n";

  lj_func_test_reset_gc1num_bump_fallback_calls();
  fallback0 = lj_func_test_gc1num_bump_fallback_calls();

  run_script(L, code, "numeric FNEW traced behavior");

  assert(lj_func_test_gc1num_bump_fallback_calls() == fallback0);
}

static void test_traced_immutable_numeric_inline(lua_State *L, global_State *g)
{
  TGState *tg = L2TG(L);
  uint32_t helper0, helper1;
  const char *setup =
    "require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "__fnew_hint_t = {}\n";
  const char *warm =
    "local util = package.loaded['jit.util']\n"
    "function __fnew_hint_run(offset)\n"
    "  local t = assert(__fnew_hint_t)\n"
    "  for i = 1, 100 do\n"
    "    local x = i + offset\n"
    "    t[i] = function()\n"
    "      return x\n"
    "    end\n"
    "  end\n"
    "  return t[1](), t[100]()\n"
    "end\n"
    "local a, b = __fnew_hint_run(0.5)\n"
    "assert(a == 1.5 and b == 100.5)\n"
    "assert(util.traceinfo(1), 'immutable numeric FNEW loop did not trace')\n"
    "assert(debug.upvalueid(__fnew_hint_t[1], 1) ~= "
    "debug.upvalueid(__fnew_hint_t[2], 1))\n";

  run_script(L, setup, "immutable numeric FNEW traced inline setup");
  run_script(L, warm, "immutable numeric FNEW traced inline warmup");
  assert_traced_fnew_publication_order(L);
  (void)lj_gc_flush_root_pending(g);
  lj_gc_threshold_store(g, UINT64_MAX / 2u);
  lj_gc2_hard_store(g, UINT64_MAX / 2u);
  lj_gc2_trigger_store(g, UINT64_MAX / 2u);
  la_store64_rel(&tg->local_total, 0);
  lj_gcroot_pending_hint_rel(g, 0);
  reset_gc1num_bump_counters();
  helper0 = gc1num_bump_helper_calls();

  lua_getglobal(L, "__fnew_hint_run");
  assert(lua_isfunction(L, -1));
  lua_pushnumber(L, 1000.5);
  ljt_lua_pcall(L, 1, 2, "immutable numeric FNEW traced inline rerun");
  assert(lua_tonumber(L, -2) == 1001.5);
  assert(lua_tonumber(L, -1) == 1100.5);
  lua_pop(L, 2);

  helper1 = gc1num_bump_helper_calls();
  assert(helper1 == helper0);
  lua_getglobal(L, "__fnew_hint_t");
  assert(lua_istable(L, -1));
  lua_rawgeti(L, -1, 1);
  assert(tvisfunc(L->top - 1) && isluafunc(funcV(L->top - 1)));
  (void)assert_fnew_root_layout(funcV(L->top - 1));
  lua_pop(L, 1);
  {
    int i, found_cross_word = 0;
    for (i = 1; i <= 100 && !found_cross_word; i++) {
      lua_rawgeti(L, -1, i);
      assert(tvisfunc(L->top - 1) && isluafunc(funcV(L->top - 1)));
      found_cross_word = assert_fnew_root_layout(funcV(L->top - 1));
      lua_pop(L, 1);
    }
    /* Exercise the FNEW pair path whose exact starts live in different packed
    ** root words; each CAS must derive its own word without touching either
    ** allocation's NONE interiors. */
    assert(found_cross_word);
  }
  lua_pop(L, 1);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(lj_gc_flush_root_pending(g) > 0);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  lua_pushnil(L);
  lua_setglobal(L, "__fnew_hint_t");
  lua_pushnil(L);
  lua_setglobal(L, "__fnew_hint_run");
}

static void test_traced_active_black_inline(lua_State *L, global_State *g,
					    TGState *tg)
{
  uint32_t old_mark_active = lj_tg_mark_active_acq(tg);
  uint8_t old_alloc_black = lj_tg_alloc_black_acq(tg);
  uint32_t helper0, helper1;
  GCobj *pending0;
  const char *setup =
    "require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n";
  const char *warm =
    "local util = package.loaded['jit.util']\n"
    "function __fnew_active_run(n, offset)\n"
    "  local s = 0\n"
    "  for i = 1, n do\n"
    "    local x = i + offset\n"
    "    local f = function()\n"
    "      x = x + 1\n"
    "      return x\n"
    "    end\n"
    "    s = s + f()\n"
    "  end\n"
    "  return s\n"
    "end\n"
    "assert(__fnew_active_run(100, 0) == 5150)\n"
    "assert(util.traceinfo(1), 'numeric FNEW loop did not trace')\n";

  run_script(L, setup, "numeric FNEW traced active-black inline setup");
  run_script(L, warm, "numeric FNEW traced active-black inline warmup");
  suppress_new_trace_recording(L, g);
  (void)lj_gc_flush_root_pending(g);
  lj_gcroot_pending_hint_rel(g, 0);
  pending0 = lj_tg_gcroot_pending_acq(tg);
  assert(pending0 == NULL);
  reset_gc1num_bump_counters();
  helper0 = gc1num_bump_helper_calls();

  lj_gc_threshold_store(g, UINT64_MAX / 2u);
  lj_gc2_hard_store(g, UINT64_MAX / 2u);
  lj_gc2_trigger_store(g, UINT64_MAX / 2u);
  la_store64_rel(&tg->local_total, 0);
  prime_traversable_bump_window(tg);

  lua_getglobal(L, "__fnew_active_run");
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, 100);
  lua_pushnumber(L, 1000);
  (void)lj_gc_flush_root_pending(g);
  lj_gcroot_pending_hint_rel(g, 0);
  pending0 = lj_tg_gcroot_pending_acq(tg);
  assert(pending0 == NULL);
  lj_tg_mark_active_rel(tg, 1);
  lj_tg_alloc_black_rel(tg, 1);
  ljt_lua_pcall(L, 2, 1, "numeric FNEW traced active-black inline rerun");
  assert(lua_tonumber(L, -1) == 105150);
  lua_pop(L, 1);

  lj_tg_alloc_black_rel(tg, old_alloc_black);
  lj_tg_mark_active_rel(tg, old_mark_active);

  helper1 = gc1num_bump_helper_calls();
  assert(helper1 == helper0);
  assert(lj_tg_gcroot_pending_acq(tg) != pending0);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(lj_gc_flush_root_pending(g) > 0);
  assert(lj_gcroot_pending_hint_acq(g) == 0);
  lua_pushnil(L);
  lua_setglobal(L, "__fnew_active_run");
}

static void test_traced_mark_active_white_fallback(lua_State *L,
						   global_State *g,
						   TGState *tg)
{
  uint32_t old_mark_active = lj_tg_mark_active_acq(tg);
  uint8_t old_alloc_black = lj_tg_alloc_black_acq(tg);
  uint32_t helper0, helper1;
  const char *code =
    "local util = require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "local t = {}\n"
    "for i = 1, 100 do\n"
    "  local x = i\n"
    "  t[i] = function()\n"
    "    x = x + 1\n"
    "    return x\n"
    "  end\n"
    "end\n"
    "assert(util.traceinfo(1), 'numeric FNEW loop did not trace')\n"
    "assert(t[1]() == 2)\n"
    "assert(t[2]() == 3)\n"
    "assert(t[100]() == 101)\n"
    "assert(debug.upvalueid(t[1], 1) ~= debug.upvalueid(t[2], 1))\n";

  reset_gc1num_bump_counters();
  helper0 = gc1num_bump_helper_calls();

  lj_gc_threshold_store(g, UINT64_MAX / 2u);
  lj_gc2_hard_store(g, UINT64_MAX / 2u);
  lj_gc2_trigger_store(g, UINT64_MAX / 2u);
  lj_tg_mark_active_rel(tg, 1);
  lj_tg_alloc_black_rel(tg, 0);

  run_script(L, code, "numeric FNEW traced active-white fallback");

  lj_tg_alloc_black_rel(tg, old_alloc_black);
  lj_tg_mark_active_rel(tg, old_mark_active);

  helper1 = gc1num_bump_helper_calls();
  assert(helper1 > helper0);
}

static void test_traced_alloc_black_inline(lua_State *L, global_State *g,
					   TGState *tg)
{
  uint32_t old_mark_active = lj_tg_mark_active_acq(tg);
  uint8_t old_alloc_black = lj_tg_alloc_black_acq(tg);
  uint32_t helper0, helper1;
  const char *setup =
    "require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n";
  const char *warm =
    "local util = package.loaded['jit.util']\n"
    "function __fnew_allocblack_run(n, offset)\n"
    "  local s = 0\n"
    "  for i = 1, n do\n"
    "    local x = i + offset\n"
    "    local f = function()\n"
    "      x = x + 1\n"
    "      return x\n"
    "    end\n"
    "    s = s + f()\n"
    "  end\n"
    "  return s\n"
    "end\n"
    "assert(__fnew_allocblack_run(100, 0) == 5150)\n"
    "assert(util.traceinfo(1), 'numeric FNEW loop did not trace')\n";

  run_script(L, setup, "numeric FNEW traced alloc-black inline setup");
  run_script(L, warm, "numeric FNEW traced alloc-black inline warmup");
  suppress_new_trace_recording(L, g);
  (void)lj_gc_flush_root_pending(g);
  lj_gcroot_pending_hint_rel(g, 0);
  reset_gc1num_bump_counters();
  helper0 = gc1num_bump_helper_calls();

  lj_gc_threshold_store(g, UINT64_MAX / 2u);
  lj_gc2_hard_store(g, UINT64_MAX / 2u);
  lj_gc2_trigger_store(g, UINT64_MAX / 2u);
  la_store64_rel(&tg->local_total, 0);
  prime_traversable_bump_window(tg);
  lj_tg_mark_active_rel(tg, 0);
  lj_tg_alloc_black_rel(tg, 1);

  lua_getglobal(L, "__fnew_allocblack_run");
  assert(lua_isfunction(L, -1));
  lua_pushinteger(L, 100);
  lua_pushnumber(L, 1000);
  ljt_lua_pcall(L, 2, 1, "numeric FNEW traced alloc-black inline rerun");
  assert(lua_tonumber(L, -1) == 105150);
  lua_pop(L, 1);

  lj_tg_alloc_black_rel(tg, old_alloc_black);
  lj_tg_mark_active_rel(tg, old_mark_active);

  helper1 = gc1num_bump_helper_calls();
  assert(helper1 == helper0);
  lua_pushnil(L);
  lua_setglobal(L, "__fnew_allocblack_run");
}

static void test_traced_post_sweep_bump_refill(lua_State *L)
{
  uint32_t fallback0, fallback1;
  const char *setup =
    "local util = require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1')\n"
    "function __fnew_refill_run(n)\n"
    "  local s = 0\n"
    "  for i = 1, n do\n"
    "    local x = i\n"
    "    local f = function()\n"
    "      x = x + 1\n"
    "      return x\n"
    "    end\n"
    "    s = s + f()\n"
    "  end\n"
    "  return s\n"
    "end\n"
    "local n = 20000\n"
    "assert(__fnew_refill_run(n) == n * (n + 3) / 2)\n"
    "assert(util.traceinfo(1), 'numeric FNEW refill loop did not trace')\n"
    "collectgarbage('collect')\n";
  const char *rerun =
    "local n = 20000\n"
    "assert(__fnew_refill_run(n) == n * (n + 3) / 2)\n";

  run_script(L, setup, "numeric FNEW post-sweep refill setup");

  lj_func_test_reset_gc1num_bump_fast_calls();
  lj_func_test_reset_gc1num_bump_fallback_calls();
  fallback0 = lj_func_test_gc1num_bump_fallback_calls();

  run_script(L, rerun, "numeric FNEW post-sweep refill rerun");

  fallback1 = lj_func_test_gc1num_bump_fallback_calls();
  /* A semantic construction lease may preserve a reusable inline bump window
  ** through this collection. Either the trace keeps allocating inline or its
  ** first miss refills through the fast helper; neither may reach fallback. */
  assert(fallback1 == fallback0);
  lua_pushnil(L);
  lua_setglobal(L, "__fnew_refill_run");
}

static void test_traced_gcvalue_promotion(lua_State *L)
{
  const char *code =
    "local util = require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1', '-sink')\n"
    "local keep\n"
    "local function run(n)\n"
    "  local x = { value = 0 }\n"
    "  for i = 1, n do\n"
    "    x = { value = i }\n"
    "    local function f() return x end\n"
    "    keep = f\n"
    "  end\n"
    "  return keep().value\n"
    "end\n"
    "assert(run(2000) == 2000)\n"
    "assert(util.traceinfo(1), 'GC-valued cell promotion did not trace')\n"
    "collectgarbage('collect')\n"
    "assert(run(2000) == 2000)\n";
  run_script(L, code, "traced GC-valued local-cell promotion");
}

static void load_one_upvalue_fixture(lua_State *L, GCfunc **parentp,
				     GCproto **childp, int32_t *slotnop)
{
  GCfunc *parent;
  GCproto *child;
  uint32_t uvdesc;

  assert(luaL_loadstring(L,
    "return function()\n"
    "  local x = 0\n"
    "  return function()\n"
    "    x = x + 1\n"
    "    return x\n"
    "  end\n"
    "end\n") == LUA_OK);
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
  parent = top_lfunc(L);
  child = first_child_proto(funcproto(parent));
  assert(child->sizeuv == 1);
  assert(proto_celluv(child));
  uvdesc = proto_uv(child)[0];
  assert((uvdesc & PROTO_UV_LOCAL) != 0);
  *slotnop = (int32_t)(uvdesc & 0xffu);
  *parentp = parent;
  *childp = child;
}

static void assert_one_upvalue_result(GCfunc *fn, TValue *slot, int32_t value)
{
  GCupval *uv;
  assert(lj_funcL_nupvalues(&fn->l) == 1);
  uv = func_uv_acq(&fn->l, 0);
  assert(uv->closed);
  assert(uvval(uv) == &uv->tv);
  assert(tvisnumber(&uv->tv));
  assert((int32_t)numberVnum(&uv->tv) == value);
  assert(tvisgcv(slot) && gcV(slot) == obj2gco(uv));
}

static void assert_nil_closed_cell(GCupval *uv)
{
  assert(uv != NULL);
  assert(uv->closed);
  assert(uvval(uv) == &uv->tv);
  assert(tvisnil(&uv->tv));
  assert(!lj_uv_immutable(uv));
}

static void quiet_gc_for_bump(global_State *g, TGState *tg)
{
  lj_gc_threshold_store(g, UINT64_MAX / 2u);
  lj_gc2_hard_store(g, UINT64_MAX / 2u);
  lj_gc2_trigger_store(g, UINT64_MAX / 2u);
  la_store64_rel(&tg->local_total, 0);
}

static void test_uvcell_bump_direct(lua_State *L, global_State *g, TGState *tg)
{
  uint32_t bump0, bump1, bump2;
  TValue slots[8];
  GCupval *uv;

  quiet_gc_for_bump(g, tg);

  lj_func_test_reset_uvcell_bump_calls();
  bump0 = lj_func_test_uvcell_bump_calls();
  uv = lj_func_newuvcell(L);
  assert_nil_closed_cell(uv);
  bump1 = lj_func_test_uvcell_bump_calls();
  assert(bump1 > bump0);
  assert(lj_tg_local_total_acq(tg) > 0);

  setnilV(&slots[3]);
  uv = lj_func_newuvcell_forjit(L, slots, 3);
  assert_nil_closed_cell(uv);
  assert(tvisgcv(&slots[3]) && gcV(&slots[3]) == obj2gco(uv));
  bump2 = lj_func_test_uvcell_bump_calls();
  assert(bump2 > bump1);
}

static void assert_gc1num_bump_blocked(lua_State *L, TValue *slots,
				       GCproto *child, GCfuncL *parent,
				       int32_t slotno, int32_t value)
{
  uint32_t fast0 = lj_func_test_gc1num_bump_fast_calls();
  uint32_t fallback0 = lj_func_test_gc1num_bump_fallback_calls();
  GCfunc *fn;

  setnumV(&slots[slotno], value);
  fn = lj_func_newL_gc1num_forjit(L, slots, child, parent, slotno, value);
  assert_one_upvalue_result(fn, &slots[slotno], value);
  assert(lj_func_test_gc1num_bump_fast_calls() == fast0);
  assert(lj_func_test_gc1num_bump_fallback_calls() > fallback0);
}

static void load_no_upvalue_fixture(lua_State *L, GCfunc **parentp,
				    GCproto **childp)
{
  GCfunc *parent;
  GCproto *child;

  assert(luaL_loadstring(L,
    "return function()\n"
    "  return function()\n"
    "    return 42\n"
    "  end\n"
    "end\n") == LUA_OK);
  assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
  parent = top_lfunc(L);
  child = first_child_proto(funcproto(parent));
  assert(child->sizeuv == 0);
  *parentp = parent;
  *childp = child;
}

static void test_bump_allocator_gate_direct(lua_State *L, global_State *g,
					    TGState *tg)
{
  uint32_t old_workers = gc2_n_workers_acq(g);
  uint32_t old_allocf_arena = la_load32_acq(&g->allocf_arena);
  uint32_t fast0;
  TValue slots[256];
  GCfunc *parent, *fn;
  GCproto *child;
  GCupval *uv;
  int32_t slotno;

  assert(mt_entering_acq(g) == 0);
  assert(old_workers == 0);
  assert(old_allocf_arena != 0);

  load_one_upvalue_fixture(L, &parent, &child, &slotno);
  quiet_gc_for_bump(g, tg);
  lj_func_test_reset_gc1num_bump_fast_calls();
  lj_func_test_reset_gc1num_bump_fallback_calls();

  assert(mt_entering_add_rlx(g, 1) == 0);
  assert_gc1num_bump_blocked(L, slots, child, &parent->l, slotno, 11);
  assert(mt_entering_sub_acqrel(g, 1) == 1);
  mt_entering_futex_wake(g, 0x7fffffff);

  gc2_n_workers_rel(g, 1);
  assert_gc1num_bump_blocked(L, slots, child, &parent->l, slotno, 22);
  gc2_n_workers_rel(g, old_workers);

  la_store32_rel(&g->allocf_arena, 0);
  assert_gc1num_bump_blocked(L, slots, child, &parent->l, slotno, 33);
  la_store32_rel(&g->allocf_arena, old_allocf_arena);
  lua_pop(L, 1);

  quiet_gc_for_bump(g, tg);
  lj_func_test_reset_uvcell_bump_calls();
  fast0 = lj_func_test_uvcell_bump_calls();
  assert(mt_entering_add_rlx(g, 1) == 0);
  uv = lj_func_newuvcell(L);
  assert_nil_closed_cell(uv);
  assert(lj_func_test_uvcell_bump_calls() == fast0);
  assert(mt_entering_sub_acqrel(g, 1) == 1);
  mt_entering_futex_wake(g, 0x7fffffff);

  load_no_upvalue_fixture(L, &parent, &child);
  quiet_gc_for_bump(g, tg);
  lj_func_test_reset_gc0_bump_trace_calls();
  fast0 = lj_func_test_gc0_bump_trace_calls();
  assert(mt_entering_add_rlx(g, 1) == 0);
  fn = lj_func_newL_gc_forjit(L, NULL, child, &parent->l);
  assert(isluafunc(fn));
  assert(lj_funcL_nupvalues(&fn->l) == 0);
  assert(lj_func_test_gc0_bump_trace_calls() == fast0);
  assert(mt_entering_sub_acqrel(g, 1) == 1);
  mt_entering_futex_wake(g, 0x7fffffff);
  lua_pop(L, 1);
}

static void test_interpreter_numeric_fast_path(lua_State *L)
{
  uint32_t interp0;
  const char *code =
    "jit.off()\n"
    "local t = {}\n"
    "for i = 1, 40 do\n"
    "  local x = i + 0.25\n"
    "  t[i] = function()\n"
    "    x = x + 1\n"
    "    return x\n"
    "  end\n"
    "end\n"
    "assert(t[1]() == 2.25)\n"
    "assert(t[2]() == 3.25)\n"
    "assert(t[40]() == 41.25)\n"
    "assert(t[1]() == 3.25)\n"
    "assert(debug.upvalueid(t[1], 1) ~= debug.upvalueid(t[2], 1))\n"
    "local name = debug.setupvalue(t[1], 1, 50.5)\n"
    "assert(name == 'x', name)\n"
    "assert(t[1]() == 51.5)\n"
    "assert(t[2]() == 4.25)\n"
    "local r = {}\n"
    "for i = 1, 40 do\n"
    "  local x = i + 0.5\n"
    "  r[i] = function() return x end\n"
    "end\n"
    "assert(r[1]() == 1.5)\n"
    "assert(r[40]() == 40.5)\n"
    "local a, b\n"
    "do\n"
    "  local x = 10.25\n"
    "  a = function() x = x + 1; return x end\n"
    "  b = function() x = x + 1; return x end\n"
    "end\n"
    "assert(debug.upvalueid(a, 1) == debug.upvalueid(b, 1))\n"
    "assert(a() == 11.25)\n"
    "assert(b() == 12.25)\n"
    "jit.on()\n";

  lj_func_test_reset_gc1num_bump_interp_calls();
  interp0 = lj_func_test_gc1num_bump_interp_calls();
  run_script(L, code, "interpreter numeric FNEW fast path");
  assert(lj_func_test_gc1num_bump_interp_calls() > interp0);
}

static void test_interpreter_generic_oneuv_chain(lua_State *L)
{
  uint32_t chain0, chain1;
  const char *code =
    "jit.off()\n"
    "local s = {}\n"
    "for i = 1, 40 do\n"
    "  local x = 'v' .. i\n"
    "  s[i] = function()\n"
    "    return x\n"
    "  end\n"
    "end\n"
    "assert(s[1]() == 'v1')\n"
    "assert(s[40]() == 'v40')\n"
    "assert(debug.upvalueid(s[1], 1) ~= debug.upvalueid(s[2], 1))\n"
    "local name = debug.setupvalue(s[1], 1, 'changed')\n"
    "assert(name == 'x', name)\n"
    "assert(s[1]() == 'changed')\n"
    "assert(s[2]() == 'v2')\n"
    "local a, b\n"
    "do\n"
    "  local x = { n = 10 }\n"
    "  a = function() x = { n = x.n + 1 }; return x.n end\n"
    "  b = function() x = { n = x.n + 1 }; return x.n end\n"
    "end\n"
    "assert(debug.upvalueid(a, 1) == debug.upvalueid(b, 1))\n"
    "assert(a() == 11)\n"
    "assert(b() == 12)\n"
    "jit.on()\n";

  lj_func_test_reset_gc1uv_chain_calls();
  chain0 = lj_func_test_gc1uv_chain_calls();
  run_script(L, code, "interpreter generic one-upvalue FNEW chain");
  chain1 = lj_func_test_gc1uv_chain_calls();
  assert(chain1 > chain0);
}

static void test_interpreter_multiuv_afterfn(lua_State *L)
{
  uint32_t after0, after1;
  const char *code =
    "jit.off()\n"
    "local t = {}\n"
    "for i = 1, 40 do\n"
    "  local a = 'a' .. i\n"
    "  local b = { n = i }\n"
    "  t[i] = function()\n"
    "    a = a .. '!'\n"
    "    b = { n = b.n + 1 }\n"
    "    return a, b.n\n"
    "  end\n"
    "end\n"
    "local a1, b1 = t[1](); assert(a1 == 'a1!' and b1 == 2)\n"
    "local a2, b2 = t[2](); assert(a2 == 'a2!' and b2 == 3)\n"
    "assert(debug.upvalueid(t[1], 1) ~= debug.upvalueid(t[2], 1))\n"
    "assert(debug.upvalueid(t[1], 2) ~= debug.upvalueid(t[2], 2))\n"
    "local name = debug.setupvalue(t[1], 1, 'z')\n"
    "assert(name == 'a', name)\n"
    "a1, b1 = t[1](); assert(a1 == 'z!' and b1 == 3)\n"
    "a2, b2 = t[2](); assert(a2 == 'a2!!' and b2 == 4)\n"
    "jit.on()\n";

  lj_func_test_reset_uv_afterfn_calls();
  after0 = lj_func_test_uv_afterfn_calls();
  run_script(L, code, "interpreter multi-upvalue FNEW after-function links");
  after1 = lj_func_test_uv_afterfn_calls();
  assert(after1 > after0);
}

static void test_interpreter_no_upvalue_fast_path(lua_State *L)
{
  uint32_t fast0, fast1;
  const char *code =
    "jit.off()\n"
    "local t = {}\n"
    "for i = 1, 80 do\n"
    "  t[i] = function() return 42 end\n"
    "end\n"
    "assert(t[1]() == 42)\n"
    "assert(t[80]() == 42)\n"
    "assert(t[1] ~= t[2])\n"
    "assert(debug.getupvalue(t[1], 1) == nil)\n"
    "jit.on()\n";

  lj_func_test_reset_gc0_bump_interp_calls();
  fast0 = lj_func_test_gc0_bump_interp_calls();
  run_script(L, code, "interpreter no-upvalue FNEW fast path");
  fast1 = lj_func_test_gc0_bump_interp_calls();
  assert(fast1 > fast0);
}

static void test_traced_no_upvalue_fast_path(lua_State *L)
{
  const char *code =
    "local util = require'jit.util'\n"
    "jit.flush()\n"
    "jit.opt.start('hotloop=1', 'hotexit=1', '-sink')\n"
    "local t = {}\n"
    "for i = 1, 120 do\n"
    "  t[i] = function() return 42 end\n"
    "end\n"
    "assert(util.traceinfo(1), 'no-upvalue FNEW loop did not trace')\n"
    "assert(t[1]() == 42)\n"
    "assert(t[120]() == 42)\n"
    "assert(t[1] ~= t[2])\n"
    "assert(debug.getupvalue(t[1], 1) == nil)\n";

  run_script(L, code, "traced no-upvalue FNEW fast path");
}

static void test_no_upvalue_forjit_fast_direct(lua_State *L, global_State *g,
						TGState *tg)
{
  GCfunc *parent, *fn;
  GCproto *child;
  uint32_t fast0;

  load_no_upvalue_fixture(L, &parent, &child);
  quiet_gc_for_bump(g, tg);
  prime_traversable_bump_window(tg);
  lj_func_test_reset_gc0_bump_trace_calls();
  fast0 = lj_func_test_gc0_bump_trace_calls();
  fn = lj_func_newL_gc_forjit(L, NULL, child, &parent->l);
  assert(isluafunc(fn) && lj_funcL_nupvalues(&fn->l) == 0);
  assert(lj_func_test_gc0_bump_trace_calls() > fast0);
  lua_pop(L, 1);
}

static void test_accounting_fast_direct(lua_State *L, global_State *g,
					TGState *tg)
{
  uint32_t fast0, fallback0;
  TValue slots[256];
  GCfunc *parent, *fn;
  GCproto *child;
  int32_t slotno;
  UNUSED(g);

  lj_func_test_reset_gc1num_bump_fast_calls();
  lj_func_test_reset_gc1num_bump_fallback_calls();
  fast0 = lj_func_test_gc1num_bump_fast_calls();
  fallback0 = lj_func_test_gc1num_bump_fallback_calls();

  load_one_upvalue_fixture(L, &parent, &child, &slotno);
  la_store64_rel(&tg->local_total, 0);
  setnumV(&slots[slotno], 123);

  fn = lj_func_newL_gc1num_forjit(L, slots, child, &parent->l, slotno, 123);
  assert_one_upvalue_result(fn, &slots[slotno], 123);
  assert(lj_func_test_gc1num_bump_fast_calls() > fast0);
  assert(lj_func_test_gc1num_bump_fallback_calls() == fallback0);
  assert(lj_tg_local_total_acq(tg) > 0);
  lua_pop(L, 1);
}

static void test_active_black_direct_publishes_exact(lua_State *L,
						     global_State *g,
						     TGState *tg)
{
  uint32_t old_mark_active = lj_tg_mark_active_acq(tg);
  uint8_t old_alloc_black = lj_tg_alloc_black_acq(tg);
  uint32_t fast0;
  TValue slots[256];
  GCfunc *parent, *fn;
  GCproto *child;
  GCupval *uv;
  GCobj *pending0;
  int32_t slotno;

  lj_func_test_reset_gc1num_bump_fast_calls();
  fast0 = lj_func_test_gc1num_bump_fast_calls();

  load_one_upvalue_fixture(L, &parent, &child, &slotno);
  (void)lj_gc_flush_root_pending(g);
  lj_gcroot_pending_hint_rel(g, 0);
  pending0 = lj_tg_gcroot_pending_acq(tg);
  assert(pending0 == NULL);

  lj_gc_threshold_store(g, UINT64_MAX / 2u);
  lj_gc2_hard_store(g, UINT64_MAX / 2u);
  lj_gc2_trigger_store(g, UINT64_MAX / 2u);
  la_store64_rel(&tg->local_total, 0);
  lj_tg_mark_active_rel(tg, 1);
  lj_tg_alloc_black_rel(tg, 1);

  setnumV(&slots[slotno], 321);
  fn = lj_func_newL_gc1num_forjit(L, slots, child, &parent->l, slotno, 321);
  assert_one_upvalue_result(fn, &slots[slotno], 321);
  uv = func_uv_acq(&fn->l, 0);
  assert(lj_func_test_gc1num_bump_fast_calls() > fast0);
  assert(lj_funcL_nupvalues(&fn->l) == 1u);
  assert(lj_tg_gcroot_pending_acq(tg) == obj2gco(fn));
  assert(lj_obj_gcw_acq(obj2gco(fn)) == obj2gco(uv));
  assert(lj_obj_gcw_acq(obj2gco(uv)) == pending0);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(lj_gc_flush_root_pending(g) >= 2u);
  assert(lj_gcroot_pending_hint_acq(g) == 0);

  lj_tg_alloc_black_rel(tg, old_alloc_black);
  lj_tg_mark_active_rel(tg, old_mark_active);
  lua_pop(L, 1);
}

static void test_active_black_direct_keeps_exact_cells(
  lua_State *L, global_State *g, TGState *tg)
{
  uint32_t old_mark_active = lj_tg_mark_active_acq(tg);
  uint8_t old_alloc_black = lj_tg_alloc_black_acq(tg);
  uint32_t fast0, fallback0;
  TValue slots[256];
  GCfunc *parent, *fn;
  GCproto *child;
  GCupval *uv;
  GCobj *pending0;
  GCArena *a;
  uint32_t fncell, uvcell;
  int32_t slotno;

  lj_func_test_reset_gc1num_bump_fast_calls();
  lj_func_test_reset_gc1num_bump_fallback_calls();
  fast0 = lj_func_test_gc1num_bump_fast_calls();
  fallback0 = lj_func_test_gc1num_bump_fallback_calls();

  load_one_upvalue_fixture(L, &parent, &child, &slotno);
  lua_gc(L, LUA_GCCOLLECT, 0);
  (void)lj_gc_flush_root_pending(g);
  lj_gcroot_pending_hint_rel(g, 0);
  pending0 = lj_tg_gcroot_pending_acq(tg);
  assert(pending0 == NULL);

  quiet_gc_for_bump(g, tg);
  lj_tg_mark_active_rel(tg, 1);
  lj_tg_alloc_black_rel(tg, 1);

  setnumV(&slots[slotno], 654);
  fn = lj_func_newL_gc1num_forjit(L, slots, child, &parent->l, slotno, 654);
  assert_one_upvalue_result(fn, &slots[slotno], 654);
  uv = func_uv_acq(&fn->l, 0);
  assert(lj_func_test_gc1num_bump_fast_calls() > fast0);
  assert(lj_func_test_gc1num_bump_fallback_calls() == fallback0);
  assert(lj_funcL_nupvalues(&fn->l) == 1u);
  assert(lj_tg_gcroot_pending_acq(tg) == obj2gco(fn));
  assert(lj_obj_gcw_acq(obj2gco(fn)) == obj2gco(uv));
  assert(lj_obj_gcw_acq(obj2gco(uv)) == pending0);
  assert(lj_gcroot_pending_hint_acq(g) != 0);
  assert(lj_gc_flush_root_pending(g) >= 2u);
  assert(lj_gcroot_pending_hint_acq(g) == 0);

  a = lj_arena_of(fn);
  assert(a == lj_arena_of(uv));
  fncell = lj_arena_cellof(fn);
  uvcell = lj_arena_cellof(uv);
  assert(lj_arena_bm_get(a->block, fncell));
  assert(lj_arena_bm_get(a->block, uvcell));
  assert(lj_arena_bm_get(a->mark, fncell));
  assert(lj_arena_bm_get(a->mark, uvcell));
  lj_tg_alloc_black_rel(tg, old_alloc_black);
  lj_tg_mark_active_rel(tg, old_mark_active);
  lua_pop(L, 1);
}

static void test_accounting_checkpoint(lua_State *L, global_State *g,
				       TGState *tg)
{
  uint32_t fast0, fallback0;
  TValue slots[256];
  GCfunc *parent, *fn;
  GCproto *child;
  int32_t slotno;

  lj_func_test_reset_gc1num_bump_fast_calls();
  lj_func_test_reset_gc1num_bump_fallback_calls();
  fast0 = lj_func_test_gc1num_bump_fast_calls();
  fallback0 = lj_func_test_gc1num_bump_fallback_calls();

  load_one_upvalue_fixture(L, &parent, &child, &slotno);
  setnumV(&slots[slotno], 123);

  lj_gc_threshold_store(g, lj_gc_total_load(g) + 4u * LJ_GC2_ACCT_FLUSH);
  lj_gc2_hard_store(g, UINT64_MAX / 2u);
  lj_gc2_trigger_store(g, UINT64_MAX / 2u);
  la_store64_rel(&tg->local_total, LJ_GC2_ACCT_FLUSH - 1u);
  fn = lj_func_newL_gc1num_forjit(L, slots, child, &parent->l, slotno, 123);
  assert_one_upvalue_result(fn, &slots[slotno], 123);
  /* A would-flush bump runs the accounting/assist checkpoint before reserving
  ** CONSTRUCT lanes. Once the checkpoint returns, exact pair publication stays
  ** safepoint-free and need not pay the generic allocator fallback. */
  assert(lj_func_test_gc1num_bump_fallback_calls() == fallback0);
  assert(lj_func_test_gc1num_bump_fast_calls() > fast0);
  assert(lj_tg_local_total_acq(tg) < LJ_GC2_ACCT_FLUSH);
  lua_pop(L, 1);
}

int main(void)
{
  lua_State *L = luaL_newstate();
  global_State *g;
  TGState *tg;

  assert(L != NULL);
  assert_lifetime_state_matrix();
  assert_vm_tnew_membership_order();
  luaL_openlibs(L);
  g = G(L);
  tg = L2TG(L);

  test_vm_tnew_root_member(L, g, tg);
  test_vm_tnew_branch_targets(L, g);
  if (getenv("LJ_TEST_X64_PUBLICATION_ONLY") != NULL) {
    /* Keep an allocation/GC-independent runner for emitted-mcode audits. This
    ** is also useful while a separate collector tranche is under repair. */
    test_traced_active_black_inline(L, g, tg);
    test_traced_immutable_numeric_inline(L, g);
    puts("t-jit-fnew-bump x64 publication OK");
    fflush(stdout);
    _Exit(0);
  }
  test_uvcell_bump_direct(L, g, tg);
  test_bump_allocator_gate_direct(L, g, tg);
  test_interpreter_numeric_fast_path(L);
  test_accounting_fast_direct(L, g, tg);
  test_active_black_direct_publishes_exact(L, g, tg);
  test_active_black_direct_keeps_exact_cells(L, g, tg);
  test_accounting_checkpoint(L, g, tg);
  test_interpreter_generic_oneuv_chain(L);
  test_interpreter_multiuv_afterfn(L);
  test_interpreter_no_upvalue_fast_path(L);
  test_no_upvalue_forjit_fast_direct(L, g, tg);
  test_traced_active_black_inline(L, g, tg);
  test_traced_immutable_numeric_inline(L, g);
  if (getenv("LJ_TEST_TRACED_FNEW") != NULL) {
    test_traced_behavior(L);
    test_traced_mark_active_white_fallback(L, g, tg);
    test_traced_alloc_black_inline(L, g, tg);
    test_traced_post_sweep_bump_refill(L);
    test_traced_no_upvalue_fast_path(L);
    test_traced_gcvalue_promotion(L);
  }

  lua_close(L);
  puts("t-jit-fnew-bump OK");
  return 0;
}
