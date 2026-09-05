from pathlib import Path
import tempfile,subprocess,tarfile,io,difflib,json,hashlib,concurrent.futures,time
r=Path(tempfile.mkdtemp(prefix='lj-premt-cdata-hoist-20260905-'));Path('/tmp/lj-premt-cdata-hoist-path').write_text(str(r)+'\n');repo=Path('/workspaces/lj-lockless');base='b4e26564542cb8bfa997a11c6a90e5e0017a2f79';archive=subprocess.check_output(['git','archive',base],cwd=repo)
for variant in ('base-normal','fix-normal','fix-assert'):
 p=r/variant;p.mkdir()
 with tarfile.open(fileobj=io.BytesIO(archive)) as a:a.extractall(p,filter='data')
source={p:(r/'base-normal'/p).read_text() for p in ['src/lj_jit.h','src/lj_opt_loop.c','src/lj_opt_mem.c']};new=dict(source)
new['src/lj_jit.h']=new['src/lj_jit.h'].replace('  uint8_t root_startins_pending;  /* Record captured root ITERN once. */\n','  uint8_t root_startins_pending;  /* Record captured root ITERN once. */\n  uint8_t loop_cdata_fload;  /* Token-private pure pre-MT unroll scope. */\n')
s=new['src/lj_opt_loop.c'].replace('#include "lj_profile.h"\n','#include "lj_profile.h"\n#include "lj_record.h"\n')
anchor='''#if LJ_TARGET_X64 && LJ_GC64
static LJ_AINLINE int loop_needs_xpoll(jit_State *J)
'''
helper='''#if LJ_TARGET_X64 && LJ_GC64
/* Only a direct scalar payload store is admitted. Pointer-based/indirect FFI
** stores and every Lua-object store remain outside this first optimization. */
static int loop_cdata_scalar_store(jit_State *J, IRIns *ir)
{
  IRIns *addr = IR(ir->op1), *base, *ofs;
  if (!(irt_isnum(ir->t) || irt_isint(ir->t)) || addr->o != IR_ADD)
    return 0;
  base = IR(addr->op1);
  ofs = IR(addr->op2);
  return irt_iscdata(base->t) &&
    (base->o == IR_SLOAD || base->o == IR_KGC) &&
    ofs->o == IR_KINT && ofs->i >= (int32_t)sizeof(GCcdata);
}

/* Classify the complete original body, including operations after a candidate
** lookup. The copied prefix alone cannot exclude a previous iteration's late
** mutation. No opaque/helper operation or allocation is presumed pure. */
static int loop_cdata_fload_pure(jit_State *J, IRRef end)
{
  IRRef ref;
  if (J->parent != 0 || J->cur.link != J->cur.traceno ||
      lj_record_mt_runtime_shared(J2G(J), J->L))
    return 0;
  for (ref = REF_FIRST; ref < end; ref++) {
    IRIns *ir = IR(ref);
    switch (ir->o) {
    case IR_NOP: case IR_SLOAD: case IR_FLOAD:
    case IR_HREFK: case IR_HLOAD: case IR_XLOAD:
    case IR_ADD: case IR_ADDOV: case IR_EQ: case IR_NE:
      break;
    case IR_LT: case IR_GE: case IR_LE: case IR_GT:
    case IR_ULT: case IR_UGE: case IR_ULE: case IR_UGT:
      if (!(irt_isnum(ir->t) || irt_isint(ir->t)))
        return 0;
      break;
    case IR_CONV:
      if (ir->op2 != IRCONV_NUM_INT &&
          ir->op2 != (IRCONV_INT_NUM|IRCONV_CHECK))
        return 0;
      break;
    case IR_XSTORE:
      if (!loop_cdata_scalar_store(J, ir))
        return 0;
      break;
    default:
      return 0;
    }
  }
  return 1;
}

static LJ_AINLINE int loop_needs_xpoll(jit_State *J)
'''
assert s.count(anchor)==1;s=s.replace(anchor,helper)
s=s.replace('''  emitir_raw(IRTG(IR_XPOLL, IRT_NIL), loop_needs_xpoll(J), 0);
#endif
''','''  {
    int needs_poll = loop_needs_xpoll(J);
    J->loop_cdata_fload = !needs_poll && loop_cdata_fload_pure(J, invar);
    emitir_raw(IRTG(IR_XPOLL, IRT_NIL), needs_poll, 0);
  }
#endif
''',1)
s=s.replace('''  errcode = lj_vm_cpcall(J->L, NULL, &lps, cploop_opt);
  lj_mem_freevec''','''  J->loop_cdata_fload = 0;
  errcode = lj_vm_cpcall(J->L, NULL, &lps, cploop_opt);
  /* The protected callback owns the flag on every success/error path. Never
  ** carry eligibility into loop retry, another trace, or later optimizer work. */
  J->loop_cdata_fload = 0;
  lj_mem_freevec''',1)
new['src/lj_opt_loop.c']=s
s=new['src/lj_opt_mem.c'].replace('#include "lj_dispatch.h"\n','#include "lj_dispatch.h"\n#include "lj_record.h"\n')
anchor='''static LJ_AINLINE IRRef fload_alias_limit(jit_State *J, IRRef lim, IRRef fid)
'''
helper='''/* Reuse only the cdata base-root/node chain while the protected root-loop
** unroller has classified the whole body as pure. Entry guards, XBAR, ordinary
** table alias checks, and the unconditional GC phase poll remain unchanged. */
static int fload_cdata_unroll_invariant(jit_State *J, IRRef obj, IRRef fid)
{
#if LJ_TARGET_X64 && LJ_GC64
  const IRRef basefid = (IRRef)(GG_OFS(g.gcroot[GCROOT_BASEMT+~LJ_TCDATA]) >> 2);
  IRRef poll = J->chain[IR_XPOLL];
  if (!J->loop_cdata_fload || !J->chain[IR_LOOP] ||
      poll <= J->chain[IR_LOOP] || IR(poll)->op1 != 0 ||
      J->parent != 0 || lj_record_mt_runtime_shared(J2G(J), J->L))
    return 0;
  if (obj == REF_NIL && fid == basefid && irt_istab(fins->t))
    return 1;
  if (fid == IRFL_TAB_NODE && !irref_isk(obj)) {
    IRIns *base = IR(obj);
    return base->o == IR_FLOAD && irt_istab(base->t) &&
      base->op1 == REF_NIL && base->op2 == basefid;
  }
#else
  UNUSED(J); UNUSED(obj); UNUSED(fid);
#endif
  return 0;
}

static LJ_AINLINE IRRef fload_alias_limit(jit_State *J, IRRef lim, IRRef fid)
'''
assert s.count(anchor)==1;s=s.replace(anchor,helper)
s=s.replace('''  if ((fid != IRFL_TAB_META || mt_active_or_entering_acq(J2G(J))) &&
      J->chain[IR_XPOLL] > lim)
''','''  if ((fid != IRFL_TAB_META || mt_active_or_entering_acq(J2G(J))) &&
      !fload_cdata_unroll_invariant(J, fins->op1, fid) &&
      J->chain[IR_XPOLL] > lim)
''',1)
new['src/lj_opt_mem.c']=s
for variant in ('fix-normal','fix-assert'):
 for f,s in new.items():(r/variant/f).write_text(s)
patch=''.join(''.join(difflib.unified_diff(source[f].splitlines(True),new[f].splitlines(True),fromfile='a/'+f,tofile='b/'+f)) for f in source)
(r/'candidate.patch').write_text(patch);(r/'create-script.py').write_bytes(Path(__file__).read_bytes())
(r/'candidate-source.json').write_text(json.dumps({'base':base,'files':{f:hashlib.sha256(s.encode()).hexdigest() for f,s in new.items()}},indent=2)+'\n')
print(r,flush=True)
def build(v):
 tree=r/v;cmd=['taskset','-c','0-15','make','-C',str(tree/'src'),'-j4','BUILDMODE=static','CCDEBUG=-g','TARGET_STRIP=:']
 if v=='fix-assert':cmd+=['XCFLAGS=-DLUA_USE_ASSERT -DLJ_GC2_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLJ_TAB_TEST_HELPERS']
 t=time.monotonic();p=subprocess.run(cmd,cwd=tree,capture_output=True,text=True,timeout=120);(r/(v+'-build.stdout')).write_text(p.stdout);(r/(v+'-build.stderr')).write_text(p.stderr);print(v,p.returncode,round(time.monotonic()-t,3),flush=True)
 return {'variant':v,'command':cmd,'cwd':str(tree),'exit':p.returncode,'seconds':time.monotonic()-t,'runtime_sha256':hashlib.sha256((tree/'src/luajit').read_bytes()).hexdigest() if (tree/'src/luajit').exists() else None}
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as ex:res=list(ex.map(build,('base-normal','fix-normal','fix-assert')))
(r/'build-results.json').write_text(json.dumps(res,indent=2)+'\n');assert all(x['exit']==0 for x in res)
