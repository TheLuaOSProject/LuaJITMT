/*
** Error handling.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#define lj_err_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_err.h"
#include "lj_debug.h"
#include "lj_str.h"
#include "lj_func.h"
#include "lj_state.h"
#include "lj_frame.h"
#include "lj_ff.h"
#include "lj_trace.h"
#include "lj_tg.h"
#include "lj_thr.h"
#include "lj_ccallback.h"
#include "lj_vm.h"
#include "lj_strfmt.h"
#include "lj_oserr.h"

#if LJ_TARGET_WINDOWS
#if LJ_UNWIND_EXT
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#elif _WIN32_WINNT < 0x0602
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

typedef LJOSerrState ErrOSState;

#define err_os_save(err) lj_oserr_save((err))
#define err_os_restore(err) lj_oserr_restore((err))

#ifdef LJ_OSERR_TEST_UNWIND_CLOBBER
/* Test-only fault injection: model an unwinder changing platform TLS after a
** personality/SEH handler has returned but before the landing target runs. */
static LJ_AINLINE void err_os_test_unwind_clobber(void)
{
  errno = EILSEQ;
#if LJ_TARGET_WINDOWS
  SetLastError((DWORD)0x7eedu);
#endif
}
#else
#define err_os_test_unwind_clobber() ((void)0)
#endif

static LJ_AINLINE void *err_os_restore_ptr(const ErrOSState *err, void *p)
{
  err_os_restore(err);
  return p;
}

/* Carrier for the x64 external-unwind trampoline. The personality temporarily
** replaces callee-saved R12 with this pointer and saves the original value
** here. The
** trampoline restores all trace registers before entering the real target. */
#if LJ_TARGET_X64
typedef struct LJErrX64Carrier {
  uint64_t os;
  uintptr_t target;
  uintptr_t r12;
} LJErrX64Carrier;

LJ_STATIC_ASSERT(sizeof(LJErrX64Carrier) == 24u);
LJ_STATIC_ASSERT(offsetof(LJErrX64Carrier, os) == 0u);
LJ_STATIC_ASSERT(offsetof(LJErrX64Carrier, target) == 8u);
LJ_STATIC_ASSERT(offsetof(LJErrX64Carrier, r12) == 16u);

static LJ_AINLINE void err_x64_carrier_set(LJErrX64Carrier *carrier,
					   ASMFunction target,
					   uintptr_t r12,
					   const ErrOSState *err)
{
  carrier->os = lj_oserr_pack(err);
  carrier->target = (uintptr_t)lj_ptr_strip(target);
  carrier->r12 = r12;
}
#endif

/* Carrier for the ARM64 external-unwind trampoline. X28 temporarily points
** here; its original trace value is restored before the real landing target.
** X30 is the backend's fixed scratch register and carries the final return. */
#if LJ_TARGET_ARM64 && LJ_UNWIND_EXT && !LJ_ABI_WIN
typedef struct LJErrARM64Carrier {
  uint64_t os;
  uintptr_t target;
  uintptr_t x28;
} LJErrARM64Carrier;

LJ_STATIC_ASSERT(sizeof(LJErrARM64Carrier) == 24u);
LJ_STATIC_ASSERT(offsetof(LJErrARM64Carrier, os) == 0u);
LJ_STATIC_ASSERT(offsetof(LJErrARM64Carrier, target) == 8u);
LJ_STATIC_ASSERT(offsetof(LJErrARM64Carrier, x28) == 16u);

static LJ_AINLINE void err_arm64_carrier_set(LJErrARM64Carrier *carrier,
					      uintptr_t target,
					      uintptr_t x28,
					      const ErrOSState *err)
{
  carrier->os = lj_oserr_pack(err);
  carrier->target = target;
  carrier->x28 = x28;
}
#endif

#if LJ_TARGET_WINDOWS
#if defined(_MSC_VER)
#define LJ_ERR_TLS __declspec(thread)
#else
#define LJ_ERR_TLS __thread
#endif
#else
#define LJ_ERR_TLS LJ_TLS
#endif

/* Called only from assembler landing pads. This must finish with the Win32
** LastError store: the C return sequence itself is error-state transparent. */
void LJ_FASTCALL lj_err_restore_os(uint64_t packed)
{
  ErrOSState err;
  lj_oserr_unpack(&err, packed);
  err_os_restore(&err);
}

/*
** LuaJIT can either use internal or external frame unwinding:
**
** - Internal frame unwinding (INT) is free-standing and doesn't require
**   any OS or library support.
**
** - External frame unwinding (EXT) uses the system-provided unwind handler.
**
** Pros and Cons:
**
** - EXT requires unwind tables for *all* functions on the C stack between
**   the pcall/catch and the error/throw. C modules used by Lua code can
**   throw errors, so these need to have unwind tables, too. Transitively
**   this applies to all system libraries used by C modules -- at least
**   when they have callbacks which may throw an error.
**
** - INT is faster when actually throwing errors, but this happens rarely.
**   Setting up error handlers is zero-cost in any case.
**
** - INT needs to save *all* callee-saved registers when entering the
**   interpreter. EXT only needs to save those actually used inside the
**   interpreter. JIT-compiled code may need to save some more.
**
** - EXT provides full interoperability with C++ exceptions. You can throw
**   Lua errors or C++ exceptions through a mix of Lua frames and C++ frames.
**   C++ destructors are called as needed. C++ exceptions caught by pcall
**   are converted to the string "C++ exception". Lua errors can be caught
**   with catch (...) in C++.
**
** - INT has only limited support for automatically catching C++ exceptions
**   on POSIX systems using DWARF2 stack unwinding. Other systems may use
**   the wrapper function feature. Lua errors thrown through C++ frames
**   cannot be caught by C++ code and C++ destructors are not run.
**
** - EXT can handle errors from internal helper functions that are called
**   from JIT-compiled code (except for Windows/x86 and 32 bit ARM).
**   INT has no choice but to call the panic handler, if this happens.
**   Note: this is mainly relevant for out-of-memory errors.
**
** EXT is the default on all systems where the toolchain produces unwind
** tables by default (*). This is hard-coded and/or detected in src/Makefile.
** You can thwart the detection with: TARGET_XCFLAGS=-DLUAJIT_UNWIND_INTERNAL
**
** INT is the default on all other systems.
**
** EXT can be manually enabled for toolchains that are able to produce
** conforming unwind tables:
**   "TARGET_XCFLAGS=-funwind-tables -DLUAJIT_UNWIND_EXTERNAL"
** As explained above, *all* C code used directly or indirectly by LuaJIT
** must be compiled with -funwind-tables (or -fexceptions). C++ code must
** *not* be compiled with -fno-exceptions.
**
** If you're unsure whether error handling inside the VM works correctly,
** try running this and check whether it prints "OK":
**
**   luajit -e "print(select(2, load('OK')):match('OK'))"
**
** (*) Originally, toolchains only generated unwind tables for C++ code. For
** interoperability reasons, this can be manually enabled for plain C code,
** too (with -funwind-tables). With the introduction of the x64 architecture,
** the corresponding POSIX and Windows ABIs mandated unwind tables for all
** code. Over the following years most desktop and server platforms have
** enabled unwind tables by default on all architectures. OTOH mobile and
** embedded platforms do not consistently mandate unwind tables.
*/

/* -- Error messages ------------------------------------------------------ */

/* Error message strings. */
LJ_DATADEF const char *lj_err_allmsg =
#define ERRDEF(name, msg)	msg "\0"
#include "lj_errmsg.h"
;

static const char *err_strunknown(char *buf, size_t bufsz)
{
  static const char msg[] = "Unknown error";
  size_t len = sizeof(msg);
  if (bufsz == 0)
    return msg;
  if (len > bufsz)
    len = bufsz;
  memcpy(buf, msg, len-1);
  buf[len-1] = '\0';
  return buf;
}

/* Return a thread-local/reentrant OS error string when the target supports it. */
const char *lj_err_strerrno(int errnum, char *buf, size_t bufsz)
{
  if (bufsz == 0)
    return err_strunknown(buf, bufsz);
#if LJ_TARGET_POSIX
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
  {
    char *s = strerror_r(errnum, buf, bufsz);
    return s ? s : err_strunknown(buf, bufsz);
  }
#else
  if (strerror_r(errnum, buf, bufsz) == 0) {
    buf[bufsz-1] = '\0';
    return buf;
  }
  return err_strunknown(buf, bufsz);
#endif
#else
  {
    const char *s = strerror(errnum);
    return s ? s : err_strunknown(buf, bufsz);
  }
#endif
}

/* -- Internal frame unwinding -------------------------------------------- */

static LJ_AINLINE int err_frame_func_slot_valid(cTValue *frame)
{
#if LJ_FR2
  cTValue *func = frame-1;
  /*
  ** Trace-side and fast-function runtime errors can reach the formatter before
  ** L->base has been restored to a complete Lua frame. The function slot is the
  ** cheap invariant that distinguishes a real frame from argument/result cells:
  ** regular frames carry a function and dummy frames carry the thread object.
  */
  return (tvisfunc(func) || tvisthread(func)) && lj_tv_gcref_type_match(func);
#else
  UNUSED(frame);
  return 1;
#endif
}

/* Unwind Lua stack and move error message to new top. */
LJ_NOINLINE static void unwindstack(lua_State *L, TValue *top)
{
  lj_func_closeuv(L, top);
  if (top < L->top-1) {
    copyTV(L, top, L->top-1);
    L->top = top+1;
  }
  lj_state_relimitstack(L);
}

/* Unwind until stop frame. Optionally cleanup frames. */
static void *err_unwind(lua_State *L, void *stopcf, int errcode)
{
  ErrOSState err;
  TValue *frame = L->base-1;
  void *cf = L->cframe;
  err_os_save(&err);
  while (cf) {
    int32_t nres = cframe_nres(cframe_raw(cf));
    if (nres < 0) {  /* C frame without Lua frame? */
      TValue *top = restorestack(L, -nres);
      if (frame < top) {  /* Frame reached? */
	if (errcode) {
	  L->base = frame+1;
	  L->cframe = cframe_prev(cf);
	  unwindstack(L, top);
	}
	return err_os_restore_ptr(&err, cf);
      }
    }
    if (frame <= tvref(L->stack)+LJ_FR2)
      break;
    if (!err_frame_func_slot_valid(frame))
      return err_os_restore_ptr(&err, NULL);
    switch (frame_typep(frame)) {
    case FRAME_LUA:  /* Lua frame. */
    case FRAME_LUAP:
      frame = frame_prevl(frame);
      break;
    case FRAME_C:  /* C frame. */
    unwind_c:
#if LJ_UNWIND_EXT
      if (errcode) {
	L->base = frame_prevd(frame) + 1;
	L->cframe = cframe_prev(cf);
	unwindstack(L, frame - LJ_FR2);
      } else if (cf != stopcf) {
	cf = cframe_prev(cf);
	frame = frame_prevd(frame);
	break;
      }
      return err_os_restore_ptr(&err, NULL);  /* Continue unwinding. */
#else
      UNUSED(stopcf);
      cf = cframe_prev(cf);
      frame = frame_prevd(frame);
      break;
#endif
    case FRAME_CP:  /* Protected C frame. */
      if (cframe_canyield(cf)) {  /* Resume? */
	if (errcode) {
	  hook_call_leave(G(L));  /* Assumes nobody uses coroutines inside hooks. */
	  L->cframe = NULL;
	  L->status = (uint8_t)errcode;
	}
	return err_os_restore_ptr(&err, cf);
      }
      if (errcode) {
	L->base = frame_prevd(frame) + 1;
	L->cframe = cframe_prev(cf);
	unwindstack(L, frame - LJ_FR2);
      }
      return err_os_restore_ptr(&err, cf);
    case FRAME_CONT:  /* Continuation frame. */
      if (frame_iscont_fficb(frame)) {
#if LJ_HASFFI
	if (errcode) {
	  /* callback_unwind() must see the original throw-edge pair when the
	  ** callback body owns it, not an errno value changed by stack walking. */
	  err_os_restore(&err);
	  lj_ccallback_unwind(L, frame);
	}
	if (errcode)
	  err_os_save(&err);  /* Callback frame selected its outgoing pair. */
#endif
	goto unwind_c;
      }
      /* fallthrough */
    case FRAME_VARG:  /* Vararg frame. */
      frame = frame_prevd(frame);
      break;
    case FRAME_PCALL:  /* FF pcall() frame. */
    case FRAME_PCALLH:  /* FF pcall() frame inside hook. */
      if (errcode) {
	global_State *g;
	if (errcode == LUA_YIELD) {
	  frame = frame_prevd(frame);
	  break;
	}
	/* A fast pcall is an in-VM catch boundary: an outer lua_pcall() can
	** therefore return success and never see anchors abandoned by this
	** error. Packed 64-bit delta frames carry their entry depth in the unused
	** upper word, so restore it before exposing the caught error to Lua.
	** Rollback only nils already-published slots and cannot allocate or throw. */
#if LJ_FRAME_PCALL_ROOT_ANCHOR
	/* The checkpoint came from the VM's physical DISPATCH TG. A coroutine's
	** tg_hint is migratable ownership metadata and is not authoritative for
	** this owner-private anchor stack during platform unwinding. */
	lj_tg_root_anchor_rollback(G2TG(G(L)),
				   frame_pcall_root_top(frame));
#endif
	g = G(L);
	lj_tg_setcur_L(g, L);
	if (frame_typep(frame) == FRAME_PCALL)
	  hook_call_leave(g);
	L->base = frame_prevd(frame) + 1;
	L->cframe = cf;
	unwindstack(L, L->base);
      }
      return err_os_restore_ptr(&err,
	(void *)((intptr_t)cf | CFRAME_UNWIND_FF));
    }
  }
  /* No C frame. */
  if (errcode) {
    lua_CFunction panic;
    L->base = tvref(L->stack)+1+LJ_FR2;
    L->cframe = NULL;
    unwindstack(L, L->base);
    panic = panicf_load(G(L));
    if (panic) {
      err_os_restore(&err);
      panic(L);
    }
    exit(EXIT_FAILURE);
  }
  return err_os_restore_ptr(&err, L);  /* Anything non-NULL will do. */
}

/* -- External frame unwinding -------------------------------------------- */

#if LJ_ABI_WIN

/*
** Someone in Redmond owes me several days of my life. A lot of this is
** undocumented or just plain wrong on MSDN. Some of it can be gathered
** from 3rd party docs or must be found by trial-and-error. They really
** don't want you to write your own language-specific exception handler
** or to interact gracefully with MSVC. :-(
*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#if LJ_TARGET_X86
typedef void *UndocumentedDispatcherContext;  /* Unused on x86. */
#else
/* Taken from: http://www.nynaeve.net/?p=99 */
typedef struct UndocumentedDispatcherContext {
  ULONG64 ControlPc;
  ULONG64 ImageBase;
  PRUNTIME_FUNCTION FunctionEntry;
  ULONG64 EstablisherFrame;
  ULONG64 TargetIp;
  PCONTEXT ContextRecord;
  void (*LanguageHandler)(void);
  PVOID HandlerData;
  PUNWIND_HISTORY_TABLE HistoryTable;
  ULONG ScopeIndex;
  ULONG Fill0;
} UndocumentedDispatcherContext;
#endif

/* Another wild guess. */
extern void __DestructExceptionObject(EXCEPTION_RECORD *rec, int nothrow);

#define LJ_MSVC_EXCODE		((DWORD)0xe06d7363)
#define LJ_GCC_EXCODE		((DWORD)0x20474343)

#define LJ_EXCODE		((DWORD)0xe24c4a00)
#define LJ_EXCODE_MAKE(c)	(LJ_EXCODE | (DWORD)(c))
#define LJ_EXCODE_CHECK(cl)	(((cl) ^ LJ_EXCODE) <= 0xff)
#define LJ_EXCODE_ERRCODE(cl)	((int)((cl) & 0xff))

static LJ_AINLINE EXCEPTION_RECORD *err_win_lua_record(
  EXCEPTION_RECORD *rec)
{
  if (LJ_EXCODE_CHECK(rec->ExceptionCode))
    return rec;
  if (rec->ExceptionCode == STATUS_LONGJUMP && rec->ExceptionRecord &&
      LJ_EXCODE_CHECK(rec->ExceptionRecord->ExceptionCode))
    return rec->ExceptionRecord;
  return NULL;
}

static LJ_AINLINE EXCEPTION_RECORD *err_win_os_enter(EXCEPTION_RECORD *rec,
						      ErrOSState *err)
{
  EXCEPTION_RECORD *lua_rec = err_win_lua_record(rec);
  if (lua_rec && lua_rec->NumberParameters >= 2) {
    err->errnum = (int)(uint32_t)lua_rec->ExceptionInformation[0];
    err->winerr = (uint32_t)lua_rec->ExceptionInformation[1];
  } else {
    err_os_save(err);
  }
  err_os_restore(err);
  return lua_rec;
}

static LJ_AINLINE int err_win_os_leave(EXCEPTION_RECORD *lua_rec,
				       CONTEXT *ctx, const ErrOSState *err,
				       int result)
{
  if (lua_rec) {
    lua_rec->NumberParameters = 2;
    lua_rec->ExceptionInformation[0] = (ULONG_PTR)(uint32_t)err->errnum;
    lua_rec->ExceptionInformation[1] = (ULONG_PTR)err->winerr;
  }
#if LJ_TARGET_X64
  /* RtlUnwindEx restores volatile registers from this target context. The
  ** dedicated C/FF landing labels consume R10 before running any helper. */
  ctx->R10 = (DWORD64)lj_oserr_pack(err);
#else
  UNUSED(ctx);
#endif
  err_os_restore(err);
  err_os_test_unwind_clobber();
  return result;
}

/* Windows exception handler for interpreter frame. */
LJ_FUNCA int lj_err_unwind_win(EXCEPTION_RECORD *rec,
  void *f, CONTEXT *ctx, UndocumentedDispatcherContext *dispatch)
{
  ErrOSState err;
  EXCEPTION_RECORD *lua_rec = err_win_os_enter(rec, &err);
  void *cf2;
#if LJ_TARGET_X86
  void *cf = (char *)f - CFRAME_OFS_SEH;
#elif LJ_TARGET_ARM64
  void *cf = (char *)f - CFRAME_SIZE;
#else
  void *cf = f;
#endif
  lua_State *L = cframe_L(cf);
  int errcode = LJ_EXCODE_CHECK(rec->ExceptionCode) ?
		LJ_EXCODE_ERRCODE(rec->ExceptionCode) : LUA_ERRRUN;
  if ((rec->ExceptionFlags & 6)) {  /* EH_UNWINDING|EH_EXIT_UNWIND */
    if (rec->ExceptionCode == STATUS_LONGJUMP &&
	rec->ExceptionRecord &&
	LJ_EXCODE_CHECK(rec->ExceptionRecord->ExceptionCode)) {
      errcode = LJ_EXCODE_ERRCODE(rec->ExceptionRecord->ExceptionCode);
      if ((rec->ExceptionFlags & 0x20)) {  /* EH_TARGET_UNWIND */
	/* Unwinding is about to finish; revert the ExceptionCode so that
	** RtlRestoreContext does not try to restore from a _JUMP_BUFFER.
	*/
	rec->ExceptionCode = 0;
      }
    }
    /* Unwind internal frames. */
    err_os_restore(&err);
    cf2 = err_unwind(L, cf, errcode);
    err_os_save(&err);
#if LJ_HASFFI
    /* This handler is the last code which may inspect the callback L. A NULL
    ** result after callback_unwind marks the unprotected callback boundary;
    ** discharge its scoped auto-attach before SEH continues to the catcher. */
    if (cf2 == NULL)
      lj_ccallback_unwind_detach();
#endif
  } else {
    err_os_restore(&err);
    cf2 = err_unwind(L, cf, 0);
    err_os_save(&err);
    if (cf2) {  /* We catch it, so start unwinding the upper frames. */
#if !LJ_TARGET_X86
      EXCEPTION_RECORD rec2;
#endif
      if (rec->ExceptionCode == LJ_MSVC_EXCODE ||
	  rec->ExceptionCode == LJ_GCC_EXCODE) {
#if !LJ_TARGET_CYGWIN
	__DestructExceptionObject(rec, 1);
#endif
	setstrV(L, L->top++, lj_err_str(L, LJ_ERR_ERRCPP));
      } else if (!LJ_EXCODE_CHECK(rec->ExceptionCode)) {
	/* Don't catch access violations etc. */
	return err_win_os_leave(lua_rec, ctx, &err,
				1);  /* ExceptionContinueSearch */
      }
#if LJ_TARGET_X86
      UNUSED(ctx);
      UNUSED(dispatch);
      /* Call all handlers for all lower C frames (including ourselves) again
      ** with EH_UNWINDING set. Then call the specified function, passing cf
      ** and errcode.
      */
      (void)err_win_os_leave(lua_rec, ctx, &err, 0);
      lj_vm_rtlunwind(cf, (void *)rec,
	(cframe_unwind_ff(cf2) && errcode != LUA_YIELD) ?
	(void *)lj_vm_unwind_ff : (void *)lj_vm_unwind_c, errcode);
      /* lj_vm_rtlunwind does not return. */
#else
      if (LJ_EXCODE_CHECK(rec->ExceptionCode)) {
	/* For unwind purposes, wrap the EXCEPTION_RECORD in something that
	** looks like a longjmp, so that MSVC will execute C++ destructors in
	** the frames we unwind over. ExceptionInformation[0] should really
	** contain a _JUMP_BUFFER*, but hopefully nobody is looking too closely
	** at this point.
	*/
	rec2.ExceptionCode = STATUS_LONGJUMP;
	rec2.ExceptionRecord = rec;
	rec2.ExceptionAddress = 0;
	rec2.NumberParameters = 1;
	rec2.ExceptionInformation[0] = (ULONG_PTR)ctx;
	rec = &rec2;
      }
      /* Unwind the stack and call all handlers for all lower C frames
      ** (including ourselves) again with EH_UNWINDING set. Then set
      ** stack pointer = f, result = errcode and jump to the specified target.
      */
      (void)err_win_os_leave(lua_rec, ctx, &err, 0);
      RtlUnwindEx(f, (void *)((cframe_unwind_ff(cf2) && errcode != LUA_YIELD) ?
#if LJ_TARGET_X64
			      lj_vm_unwind_ff_eh_os :
			      lj_vm_unwind_c_eh_os),
#else
			      lj_vm_unwind_ff_eh :
			      lj_vm_unwind_c_eh),
#endif
		  rec, (void *)(uintptr_t)errcode, dispatch->ContextRecord,
		  dispatch->HistoryTable);
      /* RtlUnwindEx should never return. */
#endif
    }
  }
  return err_win_os_leave(lua_rec, ctx, &err,
			  1);  /* ExceptionContinueSearch */
}

#if LJ_UNWIND_JIT

#if LJ_TARGET_X64
#define CONTEXT_REG_PC	Rip
#elif LJ_TARGET_ARM64
#define CONTEXT_REG_PC	Pc
#else
#error "NYI: Windows arch-specific unwinder for JIT-compiled code"
#endif

#if LJ_TARGET_X64
static LJ_ERR_TLS LJErrX64Carrier err_win_jit_carrier;
#endif

/* Windows unwinder for JIT-compiled code. */
static void err_unwind_win_jit(global_State *g, int errcode,
			       const ErrOSState *err)
{
  CONTEXT ctx;
  UNWIND_HISTORY_TABLE hist;

  memset(&hist, 0, sizeof(hist));
  RtlCaptureContext(&ctx);
  while (1) {
    DWORD64 frame, base, addr = ctx.CONTEXT_REG_PC;
    void *hdata;
    PRUNTIME_FUNCTION func = RtlLookupFunctionEntry(addr, &base, &hist);
    if (!func) {  /* Found frame without .pdata: must be JIT-compiled code. */
      ExitNo exitno;
      uintptr_t stub = lj_trace_unwind(G2J(g), (uintptr_t)(addr - sizeof(MCode)), &exitno);
      if (stub) {  /* Jump to side exit to unwind the trace. */
#if LJ_TARGET_X64
	err_x64_carrier_set(&err_win_jit_carrier,
				(ASMFunction)(uintptr_t)stub,
				(uintptr_t)ctx.R12, err);
	ctx.R12 = (DWORD64)(uintptr_t)&err_win_jit_carrier;
	ctx.CONTEXT_REG_PC = (DWORD64)(uintptr_t)lj_vm_unwind_os_eh;
	lj_tg_jit_exitcode_rel(G2TG(g), errcode);
#else
	ctx.CONTEXT_REG_PC = stub;
	lj_tg_jit_exitcode_rel(G2TG(g), errcode);
#endif
	/* RtlRestoreContext itself may touch platform TLS. The x64 trampoline
	** performs the authoritative restore after context installation. */
	err_os_restore(err);
	RtlRestoreContext(&ctx, NULL);  /* Does not return. */
      }
      break;
    }
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, addr, func,
		     &ctx, &hdata, &frame, NULL);
    if (!addr) break;
  }
  /* Unwinding failed, if we end up here. */
}
#endif

/* Raise Windows exception. */
static void err_raise_ext(global_State *g, int errcode,
			  const ErrOSState *err)
{
  ULONG_PTR args[2];
#if LJ_UNWIND_JIT
  if (lj_tg_jit_base(g)) {
    err_unwind_win_jit(g, errcode, err);
    return;  /* Unwinding failed. */
  }
#elif LJ_HASJIT
  /* Cannot catch on-trace errors for Windows/x86 SEH. Unwind to interpreter. */
  lj_tg_setjit_base(g, NULL);
#endif
  UNUSED(g);
  args[0] = (ULONG_PTR)(uint32_t)err->errnum;
  args[1] = (ULONG_PTR)err->winerr;
  err_os_restore(err);
  RaiseException(LJ_EXCODE_MAKE(errcode), 1 /* EH_NONCONTINUABLE */, 2, args);
}

#elif !LJ_NO_UNWIND && (defined(__GNUC__) || defined(__clang__))

/*
** We have to use our own definitions instead of the mandatory (!) unwind.h,
** since various OS, distros and compilers mess up the header installation.
*/

typedef struct _Unwind_Context _Unwind_Context;

#define _URC_OK			0
#define _URC_FATAL_PHASE2_ERROR	2
#define _URC_FATAL_PHASE1_ERROR	3
#define _URC_HANDLER_FOUND	6
#define _URC_INSTALL_CONTEXT	7
#define _URC_CONTINUE_UNWIND	8
#define _URC_FAILURE		9

#define LJ_UEXCLASS		0x4c55414a49543200ULL	/* LUAJIT2\0 */
#define LJ_UEXCLASS_MAKE(c)	(LJ_UEXCLASS | (uint64_t)(c))
#define LJ_UEXCLASS_CHECK(cl)	(((cl) ^ LJ_UEXCLASS) <= 0xff)
#define LJ_UEXCLASS_ERRCODE(cl)	((int)((cl) & 0xff))

#if !LJ_TARGET_ARM

typedef struct _Unwind_Exception
{
  uint64_t exclass;
  void (*excleanup)(int, struct _Unwind_Exception *);
  uintptr_t p1, p2;
} __attribute__((__aligned__)) _Unwind_Exception;
#define UNWIND_EXCEPTION_TYPE	_Unwind_Exception

typedef struct LJErrUEx {
  UNWIND_EXCEPTION_TYPE ex;
  global_State *g;
  ErrOSState os;
#if LJ_TARGET_X64
  LJErrX64Carrier landing;
#elif LJ_TARGET_ARM64
  LJErrARM64Carrier landing;
#endif
} LJErrUEx;

/* JIT personality code historically addressed g as *(uex + 1). Keep that
** private ABI exact while extending only the tail of our exception object. */
LJ_STATIC_ASSERT(offsetof(LJErrUEx, ex) == 0u);
LJ_STATIC_ASSERT(offsetof(LJErrUEx, g) == sizeof(UNWIND_EXCEPTION_TYPE));
LJ_STATIC_ASSERT(offsetof(LJErrUEx, os) ==
		 sizeof(UNWIND_EXCEPTION_TYPE) + sizeof(global_State *));
#if LJ_TARGET_X64 || LJ_TARGET_ARM64
LJ_STATIC_ASSERT(offsetof(LJErrUEx, landing) ==
		 offsetof(LJErrUEx, os) + sizeof(ErrOSState));
#endif

static void err_uex_cleanup(int reason, UNWIND_EXCEPTION_TYPE *uex)
{
  UNUSED(reason);
  UNUSED(uex);  /* TLS-owned exception storage is reused by the next throw. */
}

static LJ_AINLINE int err_uex_has_os(uint64_t uexclass,
				     const UNWIND_EXCEPTION_TYPE *uex)
{
  /* Another LuaJIT DSO may use the same exception class with the historical
  ** shorter {ex,g} object. The cleanup marker prevents an out-of-bounds tail
  ** access while preserving the established g-at-(uex+1) JIT ABI. */
  return LJ_UEXCLASS_CHECK(uexclass) && uex->excleanup == err_uex_cleanup;
}

static LJ_AINLINE void err_uex_os_enter(uint64_t uexclass,
					UNWIND_EXCEPTION_TYPE *uex,
					ErrOSState *err)
{
  if (err_uex_has_os(uexclass, uex))
    *err = ((LJErrUEx *)(void *)uex)->os;
  else
    err_os_save(err);
  err_os_restore(err);
}

static LJ_AINLINE int err_uex_os_leave(uint64_t uexclass,
				       UNWIND_EXCEPTION_TYPE *uex,
				       const ErrOSState *err, int result)
{
  if (err_uex_has_os(uexclass, uex))
    ((LJErrUEx *)(void *)uex)->os = *err;
  err_os_restore(err);
  err_os_test_unwind_clobber();
  return result;
}

extern uintptr_t _Unwind_GetCFA(_Unwind_Context *);
extern uintptr_t _Unwind_GetGR(_Unwind_Context *, int);
extern void _Unwind_SetGR(_Unwind_Context *, int, uintptr_t);
extern uintptr_t _Unwind_GetIP(_Unwind_Context *);
extern void _Unwind_SetIP(_Unwind_Context *, uintptr_t);
extern void _Unwind_DeleteException(_Unwind_Exception *);
extern int _Unwind_RaiseException(_Unwind_Exception *);

#if LJ_TARGET_OSX && LJ_TARGET_ARM64 && LJ_ABI_PAUTH
/* Apple's arm64e unwinder treats an IP installed through _Unwind_SetIP() as
** an authenticated return address for the current cursor SP. Passing a raw VM
** or JIT address makes unw_set_reg() authenticate it with key B and trap before
** the new context can be installed. */
#define LJ_ERR_UNWIND_SP_REG	(-2)  /* libunwind's UNW_REG_SP. */
static LJ_AINLINE uintptr_t err_unwind_sign_ip(_Unwind_Context *ctx,
						uintptr_t ip)
{
  return (uintptr_t)ptrauth_sign_unauthenticated((void *)ip,
	 ptrauth_key_return_address,
	 _Unwind_GetGR(ctx, LJ_ERR_UNWIND_SP_REG));
}
#else
#define err_unwind_sign_ip(ctx, ip) ((uintptr_t)(ip))
#endif

#if LJ_TARGET_ARM64
/* X28 is DWARF register 28. It is an ordinary callee-saved value register on
** AAPCS64, unlike Apple's reserved X18 or the return-address register X30. */
#define LJ_ERR_ARM64_CARRIER_REG	28
static LJ_AINLINE void err_uex_install_arm64(_Unwind_Context *ctx,
					      uint64_t uexclass,
					      UNWIND_EXCEPTION_TYPE *uex,
					      const ErrOSState *err,
					      uintptr_t target)
{
  LJErrUEx *ex;
  if (!err_uex_has_os(uexclass, uex)) {
    _Unwind_SetIP(ctx, err_unwind_sign_ip(ctx, target));
    return;
  }
  ex = (LJErrUEx *)(void *)uex;
  err_arm64_carrier_set(&ex->landing, err_unwind_sign_ip(ctx, target),
	_Unwind_GetGR(ctx, LJ_ERR_ARM64_CARRIER_REG), err);
  _Unwind_SetGR(ctx, LJ_ERR_ARM64_CARRIER_REG,
		(uintptr_t)&ex->landing);
  _Unwind_SetIP(ctx, err_unwind_sign_ip(ctx,
	(uintptr_t)lj_ptr_strip((ASMFunction)lj_vm_unwind_os_eh)));
}
#endif

#define _UA_SEARCH_PHASE	1
#define _UA_CLEANUP_PHASE	2
#define _UA_HANDLER_FRAME	4
#define _UA_FORCE_UNWIND	8

#if LJ_TARGET_X64
/* R12 is DWARF register 12 on SysV AMD64 and Darwin x86_64. The exception-owned
** carrier saves its trace value before installing the trampoline pointer. */
#define LJ_ERR_X64_CARRIER_REG	12
static LJ_AINLINE void err_uex_install_x64(_Unwind_Context *ctx,
					    uint64_t uexclass,
					    UNWIND_EXCEPTION_TYPE *uex,
					    const ErrOSState *err,
					    ASMFunction target)
{
  LJErrUEx *ex;
  if (!err_uex_has_os(uexclass, uex)) {
    _Unwind_SetIP(ctx, (uintptr_t)lj_ptr_strip(target));
    return;
  }
  ex = (LJErrUEx *)(void *)uex;
  err_x64_carrier_set(&ex->landing, target,
	_Unwind_GetGR(ctx, LJ_ERR_X64_CARRIER_REG), err);
  _Unwind_SetGR(ctx, LJ_ERR_X64_CARRIER_REG,
		(uintptr_t)&ex->landing);
  _Unwind_SetIP(ctx, (uintptr_t)lj_ptr_strip(lj_vm_unwind_os_eh));
}
#endif

/* DWARF2 personality handler referenced from interpreter .eh_frame. */
LJ_FUNCA int lj_err_unwind_dwarf(int version, int actions,
  uint64_t uexclass, _Unwind_Exception *uex, _Unwind_Context *ctx)
{
  ErrOSState err;
  void *cf;
  lua_State *L;
  err_uex_os_enter(uexclass, uex, &err);
  if (version != 1)
    return err_uex_os_leave(uexclass, uex, &err,
			    _URC_FATAL_PHASE1_ERROR);
  cf = (void *)_Unwind_GetCFA(ctx);
  L = cframe_L(cf);
  if ((actions & _UA_SEARCH_PHASE)) {
#if LJ_UNWIND_EXT
    err_os_restore(&err);
    if (err_unwind(L, cf, 0) == NULL) {
      err_os_save(&err);
      return err_uex_os_leave(uexclass, uex, &err,
			      _URC_CONTINUE_UNWIND);
    }
    err_os_save(&err);
#endif
    if (!LJ_UEXCLASS_CHECK(uexclass)) {
      setstrV(L, L->top++, lj_err_str(L, LJ_ERR_ERRCPP));
    }
    return err_uex_os_leave(uexclass, uex, &err, _URC_HANDLER_FOUND);
  }
  if ((actions & _UA_CLEANUP_PHASE)) {
    int errcode;
    if (LJ_UEXCLASS_CHECK(uexclass)) {
      errcode = LJ_UEXCLASS_ERRCODE(uexclass);
    } else {
      if ((actions & _UA_HANDLER_FRAME))
	_Unwind_DeleteException(uex);
      errcode = LUA_ERRRUN;
    }
#if LJ_UNWIND_EXT
    err_os_restore(&err);
    cf = err_unwind(L, cf, errcode);
    /* A callback continuation can deliberately select a different outgoing
    ** pair while cleanup walks the Lua frames. Carry that pair into every
    ** later personality invocation through the exception object itself. */
    err_os_save(&err);
    if ((actions & _UA_FORCE_UNWIND)) {
#if LJ_HASFFI
      if (cf == NULL)
	lj_ccallback_unwind_detach();
#endif
      return err_uex_os_leave(uexclass, uex, &err,
			      _URC_CONTINUE_UNWIND);
    } else if (cf) {
      ASMFunction ip;
      _Unwind_SetGR(ctx, LJ_TARGET_EHRETREG, errcode);
      ip = cframe_unwind_ff(cf) ? lj_vm_unwind_ff_eh : lj_vm_unwind_c_eh;
#if LJ_TARGET_X64
      err_uex_install_x64(ctx, uexclass, uex, &err, ip);
#elif LJ_TARGET_ARM64
      err_uex_install_arm64(ctx, uexclass, uex, &err,
	(uintptr_t)lj_ptr_strip(ip));
#else
      _Unwind_SetIP(ctx,
	err_unwind_sign_ip(ctx, (uintptr_t)lj_ptr_strip(ip)));
#endif
      return err_uex_os_leave(uexclass, uex, &err,
			      _URC_INSTALL_CONTEXT);
    }
#if LJ_TARGET_X86ORX64
    else if ((actions & _UA_HANDLER_FRAME)) {
      /* Workaround for ancient libgcc bug. Still present in RHEL 5.5. :-/
      ** Real fix: http://gcc.gnu.org/viewcvs/trunk/gcc/unwind-dw2.c?r1=121165&r2=124837&pathrev=153877&diff_format=h
      */
      _Unwind_SetGR(ctx, LJ_TARGET_EHRETREG, errcode);
#if LJ_TARGET_X64
      err_uex_install_x64(ctx, uexclass, uex, &err,
			  lj_vm_unwind_rethrow);
#else
      _Unwind_SetIP(ctx, (uintptr_t)lj_vm_unwind_rethrow);
#endif
      return err_uex_os_leave(uexclass, uex, &err,
			      _URC_INSTALL_CONTEXT);
    }
#endif
#else
    /* This is not the proper way to escape from the unwinder. We get away with
    ** it on non-x64 because the interpreter restores all callee-saved regs.
    */
    lj_err_throw(L, errcode);
#if LJ_TARGET_X64
#error "Broken build system -- only use the provided Makefiles!"
#endif
#endif
  }
#if LJ_HASFFI
  /* Only the actual CONTINUE_UNWIND edge is safe. INSTALL_CONTEXT landings use
  ** SAVE_L, so a pending auto-attach may be released only after all handler
  ** branches have declined. This is the last LuaJIT L/g access before the
  ** unwinder physically disposes the callback frame. */
  if ((actions & _UA_CLEANUP_PHASE) && cf == NULL)
    lj_ccallback_unwind_detach();
#endif
  return err_uex_os_leave(uexclass, uex, &err, _URC_CONTINUE_UNWIND);
}

#if LJ_UNWIND_EXT && defined(LUA_USE_ASSERT)
struct dwarf_eh_bases { void *tbase, *dbase, *func; };
extern const void *_Unwind_Find_FDE(void *pc, struct dwarf_eh_bases *bases);

/* Verify that external error handling actually has a chance to work. */
void lj_err_verify(void)
{
#if !LJ_TARGET_OSX
  /* Check disabled on MacOS due to brilliant software engineering at Apple. */
  struct dwarf_eh_bases ehb;
  lj_assertX(_Unwind_Find_FDE((void *)lj_err_throw, &ehb), "broken build: external frame unwinding enabled, but missing -funwind-tables");
#endif
  /* Check disabled, because of broken Fedora/ARM64. See #722.
  lj_assertX(_Unwind_Find_FDE((void *)_Unwind_RaiseException, &ehb), "broken build: external frame unwinding enabled, but system libraries have no unwind tables");
  */
}
#endif

#if LJ_UNWIND_JIT
#if LJ_TARGET_OSX && LJ_TARGET_ARM64 && LJ_ABI_PAUTH && \
    defined(LJ_ERR_UNWIND_TEST_HELPERS)
/* Test-only arm64e landing override. It still uses the production JIT CIE,
** registered FDE and personality. Only the trace lookup/exit-stub tail is
** replaced so a focused fixture can raise a real LuaJIT exception through a
** tiny synthetic mcode frame without constructing a fake GCtrace. */
typedef struct ErrArm64eUnwindTest {
  uintptr_t start;
  uintptr_t end;
  uintptr_t landing;
  uint32_t searches;
  uint32_t cleanups;
  uint32_t installs;
  uint32_t armed;
} ErrArm64eUnwindTest;

static LJ_TLS ErrArm64eUnwindTest err_arm64e_unwind_test;

LJ_FUNCA void lj_err_test_arm64e_unwind_arm(uintptr_t start, uintptr_t end,
					     uintptr_t landing)
{
  ErrArm64eUnwindTest *t = &err_arm64e_unwind_test;
  lj_assertX(start < landing && landing < end,
	     "invalid arm64e JIT unwind test range");
  lj_assertX(!t->armed, "nested arm64e JIT unwind test");
  memset(t, 0, sizeof(*t));
  t->start = start;
  t->end = end;
  t->landing = landing;
  t->armed = 1;
}

LJ_FUNCA uint32_t lj_err_test_arm64e_unwind_disarm(void)
{
  ErrArm64eUnwindTest *t = &err_arm64e_unwind_test;
  uint32_t result;
  lj_assertX(t->armed, "inactive arm64e JIT unwind test");
  lj_assertX(t->searches <= 0xff && t->cleanups <= 0xff &&
	     t->installs <= 0xff, "arm64e JIT unwind test counter overflow");
  result = (t->searches << 16) | (t->cleanups << 8) | t->installs;
  memset(t, 0, sizeof(*t));
  return result;
}
#endif

/* DWARF2 personality handler for JIT-compiled code. */
static int err_unwind_jit(int version, int actions,
  uint64_t uexclass, _Unwind_Exception *uex, _Unwind_Context *ctx)
{
  ErrOSState err;
  err_uex_os_enter(uexclass, uex, &err);
  /* NYI: FFI C++ exception interoperability. */
  if (version != 1 || !LJ_UEXCLASS_CHECK(uexclass))
    return err_uex_os_leave(uexclass, uex, &err,
			    _URC_FATAL_PHASE1_ERROR);
#if LJ_TARGET_OSX && LJ_TARGET_ARM64 && LJ_ABI_PAUTH && \
    defined(LJ_ERR_UNWIND_TEST_HELPERS)
  {
    ErrArm64eUnwindTest *t = &err_arm64e_unwind_test;
    uintptr_t ip = _Unwind_GetIP(ctx);
    if (t->armed && ip > t->start && ip <= t->end) {
      if ((actions & _UA_SEARCH_PHASE)) {
	t->searches++;
	return err_uex_os_leave(uexclass, uex, &err, _URC_HANDLER_FOUND);
      }
      if ((actions & _UA_CLEANUP_PHASE)) {
	t->cleanups++;
	_Unwind_SetGR(ctx, 0, 0x4a17u);
	err_uex_install_arm64(ctx, uexclass, uex, &err, t->landing);
	t->installs++;
	return err_uex_os_leave(uexclass, uex, &err,
				_URC_INSTALL_CONTEXT);
      }
    }
  }
#endif
  if ((actions & _UA_SEARCH_PHASE)) {
    return err_uex_os_leave(uexclass, uex, &err, _URC_HANDLER_FOUND);
  }
  if ((actions & _UA_CLEANUP_PHASE)) {
    global_State *g = ((LJErrUEx *)(void *)uex)->g;
    ExitNo exitno;
    uintptr_t addr = _Unwind_GetIP(ctx);  /* Return address _after_ call. */
    uintptr_t stub = lj_trace_unwind(G2J(g), addr - sizeof(MCode), &exitno);
    lj_assertG(lj_tg_jit_base(g), "unexpected throw across mcode frame");
    if (stub) {  /* Jump to side exit to unwind the trace. */
#if LJ_TARGET_X64 || LJ_TARGET_ARM64
      lj_tg_jit_exitcode_rel(G2TG(g), LJ_UEXCLASS_ERRCODE(uexclass));
#else
      G2J(g)->exitcode = LJ_UEXCLASS_ERRCODE(uexclass);
#endif
#ifdef LJ_TARGET_MIPS
      _Unwind_SetGR(ctx, 4, stub);
      _Unwind_SetGR(ctx, 5, exitno);
      _Unwind_SetIP(ctx, (uintptr_t)(void *)lj_vm_unwind_stub);
#elif LJ_TARGET_X64
      err_uex_install_x64(ctx, uexclass, uex, &err,
			  (ASMFunction)(uintptr_t)stub);
#elif LJ_TARGET_ARM64
      err_uex_install_arm64(ctx, uexclass, uex, &err, stub);
#else
      _Unwind_SetIP(ctx, err_unwind_sign_ip(ctx, stub));
#endif
      return err_uex_os_leave(uexclass, uex, &err,
			      _URC_INSTALL_CONTEXT);
    }
    return err_uex_os_leave(uexclass, uex, &err,
			    _URC_FATAL_PHASE2_ERROR);
  }
  return err_uex_os_leave(uexclass, uex, &err,
			  _URC_FATAL_PHASE1_ERROR);
}

/* DWARF2 template frame info for JIT-compiled code.
**
** After copying the template to the start of the mcode segment,
** the frame handler function and the code size is patched.
** The frame handler always installs a new context to jump to the exit,
** so don't bother to add any unwind opcodes.
*/
static const uint8_t err_frame_jit_template[] = {
#if LJ_BE
  0,0,0,
#endif
  LJ_64 ? 0x1c : 0x14,  /* CIE length. */
#if LJ_LE
  0,0,0,
#endif
  0,0,0,0, 1, 'z','P','R',0,  /* CIE mark, CIE version, augmentation. */
  1, LJ_64 ? 0x78 : 0x7c, LJ_TARGET_EHRAREG,  /* Code/data align, RA. */
#if LJ_64
  10, 0, 0,0,0,0,0,0,0,0, 0x1b,  /* Aug. data ABS handler, PCREL|SDATA4 code. */
  0,0,0,0,0,  /* Alignment. */
#else
  6, 0, 0,0,0,0, 0x1b,  /* Aug. data ABS handler, PCREL|SDATA4 code. */
  0,  /* Alignment. */
#endif
#if LJ_BE
  0,0,0,
#endif
  LJ_64 ? 0x14 : 0x10,  /* FDE length. */
  0,0,0,
  LJ_64 ? 0x24 : 0x1c,  /* CIE offset. */
  0,0,0,
  LJ_64 ? 0x14 : 0x10,  /* Code offset. After Final FDE. */
#if LJ_LE
  0,0,0,
#endif
  0,0,0,0, 0, 0,0,0, /* Code size, augmentation length, alignment. */
#if LJ_64
  0,0,0,0,  /* Alignment. */
#endif
  0,0,0,0  /* Final FDE. */
};

#define ERR_FRAME_JIT_OFS_HANDLER	0x12
#define ERR_FRAME_JIT_OFS_FDE		(LJ_64 ? 0x20 : 0x18)
#define ERR_FRAME_JIT_OFS_CODE_SIZE	(LJ_64 ? 0x2c : 0x24)
#if LJ_TARGET_OSX
#define ERR_FRAME_JIT_OFS_REGISTER	ERR_FRAME_JIT_OFS_FDE
#else
#define ERR_FRAME_JIT_OFS_REGISTER	0
#endif

extern void __register_frame(const void *);
extern void __deregister_frame(const void *);

uint8_t *lj_err_register_mcode(void *base, size_t sz, uint8_t *info,
			       uint8_t *winfo)
{
  ASMFunction handler = (ASMFunction)err_unwind_jit;
  memcpy(winfo, err_frame_jit_template, sizeof(err_frame_jit_template));
#if LJ_ABI_PAUTH
#if LJ_TARGET_ARM64
  handler = ptrauth_auth_and_resign(handler,
    ptrauth_key_function_pointer, 0,
    ptrauth_key_process_independent_code, info + ERR_FRAME_JIT_OFS_HANDLER);
#else
#error "missing pointer authentication support for this architecture"
#endif
#endif
  memcpy(winfo + ERR_FRAME_JIT_OFS_HANDLER, &handler, sizeof(handler));
  *(uint32_t *)(winfo + ERR_FRAME_JIT_OFS_CODE_SIZE) =
    (uint32_t)(sz - sizeof(err_frame_jit_template) - (info - (uint8_t *)base));
  __register_frame(info + ERR_FRAME_JIT_OFS_REGISTER);
#ifdef LUA_USE_ASSERT
#if !(LJ_TARGET_OSX && LJ_TARGET_ARM64 && LJ_ABI_PAUTH)
  {
    struct dwarf_eh_bases ehb;
    lj_assertX(_Unwind_Find_FDE(info + sizeof(err_frame_jit_template)+1, &ehb),
	       "bad JIT unwind table registration");
  }
#else
  /* On arm64e, macOS implements _Unwind_Find_FDE() by putting the caller's
  ** PC into a live cursor with unw_set_reg(). The hardened setter authenticates
  ** that value as a key-B return address using a cursor-private SP, so neither
  ** a raw code address nor a normal signed function pointer can be supplied by
  ** this caller without a PAC failure. Actual exception traversal is the only
  ** viable registration verifier on this ABI. */
#endif
#endif
  return info + sizeof(err_frame_jit_template);
}

void lj_err_deregister_mcode(void *base, size_t sz, uint8_t *info)
{
  UNUSED(base); UNUSED(sz);
  __deregister_frame(info + ERR_FRAME_JIT_OFS_REGISTER);
}
#endif

#else /* LJ_TARGET_ARM */

#define _US_VIRTUAL_UNWIND_FRAME	0
#define _US_UNWIND_FRAME_STARTING	1
#define _US_ACTION_MASK			3
#define _US_FORCE_UNWIND		8

typedef struct _Unwind_Control_Block _Unwind_Control_Block;
#define UNWIND_EXCEPTION_TYPE	_Unwind_Control_Block

struct _Unwind_Control_Block {
  uint64_t exclass;
  uint32_t misc[20];
};

typedef struct LJErrUEx {
  UNWIND_EXCEPTION_TYPE ex;
  global_State *g;
  ErrOSState os;
} LJErrUEx;

LJ_STATIC_ASSERT(offsetof(LJErrUEx, g) == sizeof(UNWIND_EXCEPTION_TYPE));
LJ_STATIC_ASSERT(offsetof(LJErrUEx, os) ==
		 sizeof(UNWIND_EXCEPTION_TYPE) + sizeof(global_State *));

static LJ_AINLINE void err_uex_os_enter(uint64_t uexclass,
					UNWIND_EXCEPTION_TYPE *uex,
					ErrOSState *err)
{
  if (LJ_UEXCLASS_CHECK(uexclass))
    *err = ((LJErrUEx *)(void *)uex)->os;
  else
    err_os_save(err);
  err_os_restore(err);
}

static LJ_AINLINE int err_uex_os_leave(uint64_t uexclass,
				       UNWIND_EXCEPTION_TYPE *uex,
				       const ErrOSState *err, int result)
{
  if (LJ_UEXCLASS_CHECK(uexclass))
    ((LJErrUEx *)(void *)uex)->os = *err;
  err_os_restore(err);
  err_os_test_unwind_clobber();
  return result;
}

extern int _Unwind_RaiseException(_Unwind_Control_Block *);
extern int __gnu_unwind_frame(_Unwind_Control_Block *, _Unwind_Context *);
extern int _Unwind_VRS_Set(_Unwind_Context *, int, uint32_t, int, void *);
extern int _Unwind_VRS_Get(_Unwind_Context *, int, uint32_t, int, void *);

static inline uint32_t _Unwind_GetGR(_Unwind_Context *ctx, int r)
{
  uint32_t v;
  _Unwind_VRS_Get(ctx, 0, r, 0, &v);
  return v;
}

static inline void _Unwind_SetGR(_Unwind_Context *ctx, int r, uint32_t v)
{
  _Unwind_VRS_Set(ctx, 0, r, 0, &v);
}

extern void lj_vm_unwind_ext(void);

/* ARM unwinder personality handler referenced from interpreter .ARM.extab. */
LJ_FUNCA int lj_err_unwind_arm(int state, _Unwind_Control_Block *ucb,
			       _Unwind_Context *ctx)
{
  ErrOSState err;
  void *cf = (void *)_Unwind_GetGR(ctx, 13);
  lua_State *L = cframe_L(cf);
  int errcode;

  err_uex_os_enter(ucb->exclass, ucb, &err);

  switch ((state & _US_ACTION_MASK)) {
  case _US_VIRTUAL_UNWIND_FRAME:
    if ((state & _US_FORCE_UNWIND)) break;
    return err_uex_os_leave(ucb->exclass, ucb, &err,
			    _URC_HANDLER_FOUND);
  case _US_UNWIND_FRAME_STARTING:
    if (LJ_UEXCLASS_CHECK(ucb->exclass)) {
      errcode = LJ_UEXCLASS_ERRCODE(ucb->exclass);
    } else {
      errcode = LUA_ERRRUN;
      setstrV(L, L->top++, lj_err_str(L, LJ_ERR_ERRCPP));
    }
    err_os_restore(&err);
    cf = err_unwind(L, cf, errcode);
    err_os_save(&err);
#if LJ_HASFFI
    if (cf == NULL)
      lj_ccallback_unwind_detach();
#endif
    if ((state & _US_FORCE_UNWIND) || cf == NULL) break;
    _Unwind_SetGR(ctx, 15, (uint32_t)lj_vm_unwind_ext);
    _Unwind_SetGR(ctx, 0, (uint32_t)ucb);
    _Unwind_SetGR(ctx, 1, (uint32_t)errcode);
    _Unwind_SetGR(ctx, 2, cframe_unwind_ff(cf) ?
			    (uint32_t)lj_vm_unwind_ff_eh :
			    (uint32_t)lj_vm_unwind_c_eh);
    return err_uex_os_leave(ucb->exclass, ucb, &err,
			    _URC_INSTALL_CONTEXT);
  default:
    return err_uex_os_leave(ucb->exclass, ucb, &err, _URC_FAILURE);
  }
  if (__gnu_unwind_frame(ucb, ctx) != _URC_OK)
    return err_uex_os_leave(ucb->exclass, ucb, &err, _URC_FAILURE);
#ifdef LUA_USE_ASSERT
  /* We should never get here unless this is a forced unwind aka backtrace. */
  if (_Unwind_GetGR(ctx, 0) == 0xff33aa77) {
    _Unwind_SetGR(ctx, 0, 0xff33aa88);
  }
#endif
  return err_uex_os_leave(ucb->exclass, ucb, &err,
			  _URC_CONTINUE_UNWIND);
}

#if LJ_UNWIND_EXT && defined(LUA_USE_ASSERT)
typedef int (*_Unwind_Trace_Fn)(_Unwind_Context *, void *);
extern int _Unwind_Backtrace(_Unwind_Trace_Fn, void *);

static int err_verify_bt(_Unwind_Context *ctx, int *got)
{
  if (_Unwind_GetGR(ctx, 0) == 0xff33aa88) { *got = 2; }
  else if (*got == 0) { *got = 1; _Unwind_SetGR(ctx, 0, 0xff33aa77); }
  return _URC_OK;
}

/* Verify that external error handling actually has a chance to work. */
void lj_err_verify(void)
{
  int got = 0;
  _Unwind_Backtrace((_Unwind_Trace_Fn)err_verify_bt, &got);
  lj_assertX(got == 2, "broken build: external frame unwinding enabled, but missing -funwind-tables");
}
#endif

/*
** Note: LJ_UNWIND_JIT is not implemented for 32 bit ARM.
**
** The quirky ARM unwind API doesn't have __register_frame().
** A potential workaround might involve _Unwind_Backtrace.
** But most 32 bit ARM targets don't qualify for LJ_UNWIND_EXT, anyway,
** since they are built without unwind tables by default.
*/

#endif /* LJ_TARGET_ARM */


#if LJ_UNWIND_EXT
#if LJ_TARGET_WINDOWS
static DWORD err_uex_key = FLS_OUT_OF_INDEXES;
static INIT_ONCE err_uex_once = INIT_ONCE_STATIC_INIT;

static VOID CALLBACK err_uex_free(PVOID ptr)
{
  if (ptr)
    HeapFree(GetProcessHeap(), 0, ptr);
}

static BOOL CALLBACK err_uex_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
  DWORD key;
  UNUSED(once);
  UNUSED(param);
  UNUSED(ctx);
  key = FlsAlloc(err_uex_free);
  if (key == FLS_OUT_OF_INDEXES)
    return FALSE;
  err_uex_key = key;
  return TRUE;
}

static LJErrUEx *err_uex_get(void)
{
  LJErrUEx *uex;
  if (!InitOnceExecuteOnce(&err_uex_once, err_uex_init, NULL, NULL))
    return NULL;
  uex = (LJErrUEx *)FlsGetValue(err_uex_key);
  if (!uex) {
    uex = (LJErrUEx *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
				sizeof(*uex));
    if (!uex || !FlsSetValue(err_uex_key, uex)) {
      if (uex)
	HeapFree(GetProcessHeap(), 0, uex);
      return NULL;
    }
  }
  return uex;
}
#else
static LJ_TLS LJErrUEx static_uex;
#endif

/* Raise external exception. */
static void err_raise_ext(global_State *g, int errcode,
			  const ErrOSState *err)
{
#if LJ_TARGET_WINDOWS
  LJErrUEx *uex = err_uex_get();
  if (!uex)
    abort();
  memset(uex, 0, sizeof(*uex));
  uex->ex.exclass = LJ_UEXCLASS_MAKE(errcode);
#if !LJ_TARGET_ARM
  uex->ex.excleanup = err_uex_cleanup;
#endif
  uex->g = g;
  uex->os = *err;
  err_os_restore(err);
  _Unwind_RaiseException(&uex->ex);
  err_os_restore(err);
#else
  memset(&static_uex, 0, sizeof(static_uex));
  static_uex.ex.exclass = LJ_UEXCLASS_MAKE(errcode);
#if !LJ_TARGET_ARM
  static_uex.ex.excleanup = err_uex_cleanup;
#endif
  static_uex.g = g;
  static_uex.os = *err;
  err_os_restore(err);
  _Unwind_RaiseException(&static_uex.ex);
  err_os_restore(err);
#endif
}

#endif

#endif

/* -- Error handling ------------------------------------------------------ */

/* Throw error. Find catch frame, unwind stack and continue. */
LJ_NOINLINE void LJ_FASTCALL lj_err_throw(lua_State *L, int errcode)
{
  ErrOSState err;
  global_State *g;
  /* Message construction, trace abort and platform unwinding are internal
  ** bookkeeping. In particular, a caught STOPREQ immediately following an
  ** FFI call must still expose the foreign errno/LastError pair. No pending
  ** state is published: the pair remains in this error-path C frame. */
  err_os_save(&err);
  g = G(L);
  lj_trace_abort(g);
  L->status = LUA_OK;
#if LJ_UNWIND_EXT
  err_os_restore(&err);
  err_raise_ext(g, errcode, &err);
  /*
  ** A return from this function signals a corrupt C stack that cannot be
  ** unwound. We have no choice but to call the panic function and exit.
  **
  ** Usually this is caused by a C function without unwind information.
  ** This may happen if you've manually enabled LUAJIT_UNWIND_EXTERNAL
  ** and forgot to recompile *every* non-C++ file with -funwind-tables.
  */
  {
    lua_CFunction panic = panicf_load(g);
    if (panic) {
      err_os_restore(&err);
      panic(L);
    }
  }
#else
#if LJ_HASJIT
  lj_tg_setjit_base(g, NULL);
#endif
  {
    void *cf;
    /* Restore before the stack walk, then retain any deliberate callback
    ** frame restoration performed by err_unwind(). */
    err_os_restore(&err);
    cf = err_unwind(L, NULL, errcode);
    if (cf == NULL) {
#if LJ_HASFFI
      lj_ccallback_unwind_detach();
#endif
      exit(EXIT_FAILURE);
    }
    if (cframe_unwind_ff(cf))
      lj_vm_unwind_ff(cframe_raw(cf));
    else
      lj_vm_unwind_c(cframe_raw(cf), errcode);
  }
#endif
  exit(EXIT_FAILURE);
}

/* Return string object for error message. */
LJ_NOINLINE GCstr *lj_err_str(lua_State *L, ErrMsg em)
{
  return lj_str_newz(L, err2msg(em));
}

LJ_NORET LJ_NOINLINE static void lj_err_err(lua_State *L)
{
  ErrOSState err;
  err_os_save(&err);
  setstrV(L, L->top++, lj_err_str(L, LJ_ERR_ERRERR));
  err_os_restore(&err);
  lj_err_throw(L, LUA_ERRERR);
}

/* Out-of-memory error. */
LJ_NOINLINE void lj_err_mem(lua_State *L)
{
  ErrOSState err;
  err_os_save(&err);
  if (L->status == LUA_ERRERR) {
    err_os_restore(&err);
    lj_err_err(L);
  }
  if (L->status == LUA_ERRERR+1) {  /* Don't touch stack during state setup. */
    err_os_restore(&err);
    lj_vm_unwind_c(L->cframe, LUA_ERRMEM);
  }
  if (LJ_HASJIT) {
    TValue *base = lj_tg_jit_base(G(L));
    if (base) L->base = base;
  }
  if (curr_funcisL(L)) {
    L->top = curr_topL(L);
    if (LJ_UNLIKELY(L->top > tvref(L->maxstack))) {
      /* The current Lua frame violates the stack. Replace it with a dummy. */
      L->top = L->base;
      setframe_gc(L->base - 1 - LJ_FR2, obj2gco(L), LJ_TTHREAD);
    }
  }
  setstrV(L, L->top++, lj_err_str(L, LJ_ERR_ERRMEM));
  /* In particular, a cdata nothrow allocation restores the foreign pair
  ** before entering here. Error-string lookup and stack repair must not
  ** replace that pair at the actual nonlocal throw edge. */
  err_os_restore(&err);
  lj_err_throw(L, LUA_ERRMEM);
}

/* Find error function for runtime errors. Requires an extra stack traversal. */
static ptrdiff_t finderrfunc(lua_State *L)
{
  cTValue *frame = L->base-1, *bot = tvref(L->stack)+LJ_FR2;
  void *cf = L->cframe;
  while (frame > bot && cf) {
    while (cframe_nres(cframe_raw(cf)) < 0) {  /* cframe without frame? */
      if (frame >= restorestack(L, -cframe_nres(cf)))
	break;
      if (cframe_errfunc(cf) >= 0)  /* Error handler not inherited (-1)? */
	return cframe_errfunc(cf);
      cf = cframe_prev(cf);  /* Else unwind cframe and continue searching. */
      if (cf == NULL)
	return 0;
    }
    if (!err_frame_func_slot_valid(frame))
      return 0;
    switch (frame_typep(frame)) {
    case FRAME_LUA:
    case FRAME_LUAP:
      frame = frame_prevl(frame);
      break;
    case FRAME_C:
      cf = cframe_prev(cf);
      /* fallthrough */
    case FRAME_VARG:
      frame = frame_prevd(frame);
      break;
    case FRAME_CONT:
      if (frame_iscont_fficb(frame))
	cf = cframe_prev(cf);
      frame = frame_prevd(frame);
      break;
    case FRAME_CP:
      if (cframe_canyield(cf)) return 0;
      if (cframe_errfunc(cf) >= 0)
	return cframe_errfunc(cf);
      cf = cframe_prev(cf);
      frame = frame_prevd(frame);
      break;
    case FRAME_PCALL:
    case FRAME_PCALLH:
      if (frame_func(frame_prevd(frame))->c.ffid == FF_xpcall)
	return savestack(L, frame_prevd(frame)+1);  /* xpcall's errorfunc. */
      return 0;
    default:
      lj_assertL(0, "bad frame type");
      return 0;
    }
  }
  return 0;
}

/* Runtime error. */
LJ_NOINLINE void LJ_FASTCALL lj_err_run(lua_State *L)
{
  ErrOSState err;
  ptrdiff_t ef;
  err_os_save(&err);
  ef = (LJ_HASJIT && lj_tg_jit_base(G(L))) ? 0 : finderrfunc(L);
  if (ef) {
    TValue *errfunc, *top;
    lj_state_checkstack(L, LUA_MINSTACK * 2);  /* Might raise new error. */
    lj_trace_abort(G(L));
    errfunc = restorestack(L, ef);
    top = L->top;
    if (!tvisfunc(errfunc) || L->status == LUA_ERRERR) {
      setstrV(L, top-1, lj_err_str(L, LJ_ERR_ERRERR));
      lj_err_throw(L, LUA_ERRERR);
    }
    L->status = LUA_ERRERR;
    copyTV(L, top+LJ_FR2, top-1);
    copyTV(L, top-1, errfunc);
    if (LJ_FR2) setnilV(top++);
    L->top = top+1;
    /* Let an xpcall handler observe the state at the original error edge.
    ** If it explicitly changes errno/LastError, preserve its outgoing pair. */
    err_os_restore(&err);
    lj_vm_call(L, top, 1+1);  /* Stack: |errfunc|msg| -> |msg| */
    err_os_save(&err);
  }
  err_os_restore(&err);
  lj_err_throw(L, LUA_ERRRUN);
}

/* Stack overflow error. */
void LJ_FASTCALL lj_err_stkov(lua_State *L)
{
  if (L->status == LUA_ERRERR)
    lj_err_err(L);
  lj_debug_addloc(L, err2msg(LJ_ERR_STKOV), L->base-1, NULL);
  lj_err_run(L);
}

#if LJ_HASJIT
/* Rethrow error after doing a trace exit. */
LJ_NOINLINE void LJ_FASTCALL lj_err_trace(lua_State *L, int errcode)
{
  if (errcode == LUA_ERRRUN)
    lj_err_run(L);
  else
    lj_err_throw(L, errcode);
}
#endif

/* Formatted runtime error message. */
LJ_NORET LJ_NOINLINE static void err_msgv(lua_State *L, ErrMsg em, ...)
{
  const char *msg;
  va_list argp;
  va_start(argp, em);
  if (LJ_HASJIT) {
    TValue *base = lj_tg_jit_base(G(L));
    if (base) L->base = base;
  }
  if (curr_funcisL(L)) L->top = curr_topL(L);
  msg = lj_strfmt_pushvf(L, err2msg(em), argp);
  va_end(argp);
  lj_debug_addloc(L, msg, L->base-1, NULL);
  lj_err_run(L);
}

/* Non-vararg variant for better calling conventions. */
LJ_NOINLINE void lj_err_msg(lua_State *L, ErrMsg em)
{
  err_msgv(L, em);
}

/* Lexer error. */
LJ_NOINLINE void lj_err_lex(lua_State *L, GCstr *src, const char *tok,
			    BCLine line, ErrMsg em, va_list argp)
{
  char buff[LUA_IDSIZE];
  const char *msg;
  lj_debug_shortname(buff, src, line);
  msg = lj_strfmt_pushvf(L, err2msg(em), argp);
  msg = lj_strfmt_pushf(L, "%s:%d: %s", buff, line, msg);
  if (tok)
    lj_strfmt_pushf(L, err2msg(LJ_ERR_XNEAR), msg, tok);
  lj_err_throw(L, LUA_ERRSYNTAX);
}

/* Typecheck error for operands. */
LJ_NOINLINE void lj_err_optype(lua_State *L, cTValue *o, ErrMsg opm)
{
  const char *tname = lj_typename(o);
  const char *opname = err2msg(opm);
  if (curr_funcisL(L)) {
    GCproto *pt = curr_proto(L);
    const BCIns *pc = cframe_Lpc(L) - 1;
    const char *oname = NULL;
    const char *kind = lj_debug_slotname(pt, pc, (BCReg)(o-L->base), &oname);
    if (kind)
      err_msgv(L, LJ_ERR_BADOPRT, opname, kind, oname, tname);
  }
  err_msgv(L, LJ_ERR_BADOPRV, opname, tname);
}

/* Typecheck error for ordered comparisons. */
LJ_NOINLINE void lj_err_comp(lua_State *L, cTValue *o1, cTValue *o2)
{
  const char *t1 = lj_typename(o1);
  const char *t2 = lj_typename(o2);
  err_msgv(L, t1 == t2 ? LJ_ERR_BADCMPV : LJ_ERR_BADCMPT, t1, t2);
  /* This assumes the two "boolean" entries are commoned by the C compiler. */
}

/* Typecheck error for __call. */
LJ_NOINLINE void lj_err_optype_call(lua_State *L, TValue *o)
{
  /* Gross hack if lua_[p]call or pcall/xpcall fail for a non-callable object:
  ** L->base still points to the caller. So add a dummy frame with L instead
  ** of a function. See lua_getstack().
  */
  const BCIns *pc = cframe_Lpc(L);
  if (((ptrdiff_t)pc & FRAME_TYPE) != FRAME_LUA) {
    const char *tname = lj_typename(o);
    setframe_gc(o, obj2gco(L), LJ_TTHREAD);
    if (LJ_FR2) o++;
    setframe_pc(o, pc);
    L->top = L->base = o+1;
    err_msgv(L, LJ_ERR_BADCALL, tname);
  }
  lj_err_optype(L, o, LJ_ERR_OPCALL);
}

/* Error in context of caller. */
LJ_NOINLINE void lj_err_callermsg(lua_State *L, const char *msg)
{
  ErrOSState err;
  TValue *frame = NULL, *pframe = NULL;
  err_os_save(&err);
  if (!(LJ_HASJIT && lj_tg_jit_base(G(L)))) {
    frame = L->base-1;
    if (frame_islua(frame)) {
      pframe = frame_prevl(frame);
    } else if (frame_iscont(frame)) {
      if (frame_iscont_fficb(frame)) {
	pframe = frame;
	frame = NULL;
      } else {
	pframe = frame_prevd(frame);
#if LJ_HASFFI
	/* Remove frame for FFI metamethods. */
	if (frame_func(frame)->c.ffid >= FF_ffi_meta___index &&
	    frame_func(frame)->c.ffid <= FF_ffi_meta___tostring) {
	  L->base = pframe+1;
	  L->top = frame;
	  setcframe_pc(cframe_raw(L->cframe), frame_contpc(frame));
	}
#endif
      }
    } else if (frame > tvref(L->stack)+LJ_FR2 &&
	       err_frame_func_slot_valid(frame)) {
      /*
      ** Fast functions and helper-backed C frames may leave L->base on the
      ** active C frame while formatting their own argument error. Stock error
      ** objects still carry the Lua caller location, so use the predecessor as
      ** the optional source-decoration frame after validating the frame slot.
      */
      pframe = frame_prevd(frame);
    }
  }
  lj_debug_addloc(L, msg, pframe, frame);
  err_os_restore(&err);
  lj_err_run(L);
}

/* Formatted error in context of caller. */
LJ_NOINLINE void lj_err_callerv(lua_State *L, ErrMsg em, ...)
{
  const char *msg;
  va_list argp;
  va_start(argp, em);
  msg = lj_strfmt_pushvf(L, err2msg(em), argp);
  va_end(argp);
  lj_err_callermsg(L, msg);
}

/* Error in context of caller. */
LJ_NOINLINE void lj_err_caller(lua_State *L, ErrMsg em)
{
  lj_err_callermsg(L, err2msg(em));
}

/* Argument error message. */
LJ_NORET LJ_NOINLINE static void err_argmsg(lua_State *L, int narg,
					    const char *msg)
{
  const char *fname = "?";
  const char *ftype = lj_debug_funcname(L, L->base - 1, &fname);
  if (narg < 0 && narg > LUA_REGISTRYINDEX)
    narg = (int)(L->top - L->base) + narg + 1;
  if (ftype && ftype[3] == 'h' && --narg == 0)  /* Check for "method". */
    msg = lj_strfmt_pushf(L, err2msg(LJ_ERR_BADSELF), fname, msg);
  else
    msg = lj_strfmt_pushf(L, err2msg(LJ_ERR_BADARG), narg, fname, msg);
  lj_err_callermsg(L, msg);
}

/* Formatted argument error. */
LJ_NOINLINE void lj_err_argv(lua_State *L, int narg, ErrMsg em, ...)
{
  const char *msg;
  va_list argp;
  va_start(argp, em);
  msg = lj_strfmt_pushvf(L, err2msg(em), argp);
  va_end(argp);
  err_argmsg(L, narg, msg);
}

/* Argument error. */
LJ_NOINLINE void lj_err_arg(lua_State *L, int narg, ErrMsg em)
{
  err_argmsg(L, narg, err2msg(em));
}

/* Typecheck error for arguments. */
LJ_NOINLINE void lj_err_argtype(lua_State *L, int narg, const char *xname)
{
  const char *tname, *msg;
  if (narg <= LUA_REGISTRYINDEX) {
    if (narg >= LUA_GLOBALSINDEX) {
      tname = lj_obj_itypename[~LJ_TTAB];
    } else {
      GCfunc *fn = curr_func(L);
      int idx = LUA_GLOBALSINDEX - narg;
      if ((uint32_t)idx <= lj_funcC_nupvalues(&fn->c)) {
	TValue tv;
	lj_tv_load_acq(&tv, &fn->c.upvalue[idx-1]);
	tname = lj_typename(&tv);
      } else {
	tname = lj_obj_typename[0];
      }
    }
  } else {
    TValue *o = narg < 0 ? L->top + narg : L->base + narg-1;
    tname = o < L->top ? lj_typename(o) : lj_obj_typename[0];
  }
  msg = lj_strfmt_pushf(L, err2msg(LJ_ERR_BADTYPE), xname, tname);
  err_argmsg(L, narg, msg);
}

/* Typecheck error for arguments. */
LJ_NOINLINE void lj_err_argt(lua_State *L, int narg, int tt)
{
  lj_err_argtype(L, narg, lj_obj_typename[tt+1]);
}

/* -- Public error handling API ------------------------------------------- */

LUA_API lua_CFunction lua_atpanic(lua_State *L, lua_CFunction panicf)
{
  return panicf_xchg(G(L), panicf);
}

/* Forwarders for the public API (C calling convention and no LJ_NORET). */
static lua_State *err_errstate(lua_State *L)
{
  lua_State *cur = lj_tg_cur_L(G(L));
  return cur && G(cur) == G(L) ? cur : L;
}

LUA_API int lua_error(lua_State *L)
{
  lj_err_run(L);
  return 0;  /* unreachable */
}

LUALIB_API int luaL_argerror(lua_State *L, int narg, const char *msg)
{
  err_argmsg(L, narg, msg);
  return 0;  /* unreachable */
}

LUALIB_API int luaL_typerror(lua_State *L, int narg, const char *xname)
{
  lj_err_argtype(L, narg, xname);
  return 0;  /* unreachable */
}

LUALIB_API void luaL_where(lua_State *L, int level)
{
  LJStateClaim claim;
  char loc[LUA_IDSIZE];
  int line;
  int have_loc;
  if (!lj_state_resumeclaim(L, lj_thr_current_id(G(L)), &claim))
    lj_err_callermsg(err_errstate(L), "thread busy");
  have_loc = lj_debug_getloc_claimed(L, level, loc, &line);
  lj_state_dropresumeclaim(&claim);
  if (have_loc)
    lua_pushfstring(L, "%s:%d: ", loc, line);
  else
    lua_pushliteral(L, "");
}

LUALIB_API int luaL_error(lua_State *L, const char *fmt, ...)
{
  const char *msg;
  va_list argp;
  va_start(argp, fmt);
  msg = lj_strfmt_pushvf(L, fmt, argp);
  va_end(argp);
  lj_err_callermsg(L, msg);
  return 0;  /* unreachable */
}
