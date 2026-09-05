from pathlib import Path
import json,shutil,hashlib,subprocess,os,time
from concurrent.futures import ThreadPoolExecutor
r=Path('/tmp/lj-premt-cdata-hoist-20260905-oa96m15y')
snap=r/'initial-ineligible';snap.mkdir(exist_ok=True)
for f in ['candidate.patch','candidate-source.json','positive-results.json','build-results.json']:
 shutil.copy2(r/f,snap/f)
for v in ['base-normal','fix-normal','fix-assert']:
 for s in ['positive.stdout','positive.stderr']:
  shutil.copy2(r/(v+'-'+s),snap/(v+'-'+s))
for v in ['fix-normal','fix-assert']:
 p=r/v/'src/lj_opt_loop.c';s=p.read_text();old='''  return irt_iscdata(base->t) &&
    (base->o == IR_SLOAD || base->o == IR_KGC) &&
    ofs->o == IR_KINT && ofs->i >= (int32_t)sizeof(GCcdata);
''';new='''  if (!irt_iscdata(base->t) ||
      (base->o != IR_SLOAD && base->o != IR_KGC))
    return 0;
  /* x64 records native address offsets as KINT64. Keep both representations
  ** within the same positive int32 payload-offset range. */
  if (ofs->o == IR_KINT)
    return ofs->i >= (int32_t)sizeof(GCcdata);
  return ofs->o == IR_KINT64 &&
    ir_kint64(ofs)->i64 >= (int64_t)sizeof(GCcdata) &&
    ir_kint64(ofs)->i64 <= INT32_MAX;
''';assert old in s;s=s.replace(old,new);p.write_text(s)
paths=['src/lj_jit.h','src/lj_opt_loop.c','src/lj_opt_mem.c']
import difflib
(r/'candidate.patch').write_text(''.join(''.join(difflib.unified_diff((r/'base-normal'/p).read_text().splitlines(True),(r/'fix-normal'/p).read_text().splitlines(True),fromfile='a/'+p,tofile='b/'+p)) for p in paths))
(r/'candidate-source.json').write_text(json.dumps({'base':'b4e26564542cb8bfa997a11c6a90e5e0017a2f79','sha256':{p:hashlib.sha256((r/'fix-normal'/p).read_bytes()).hexdigest() for p in paths}},indent=2)+'\n')
def run(v):
 cmd=['taskset','-c','0-15','make','-C',str(r/v/'src'),'-j4','BUILDMODE=static','CCDEBUG=-g','TARGET_STRIP=:']
 if v=='fix-assert':cmd+=['XCFLAGS=-DLUA_USE_ASSERT -DLJ_GC2_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLJ_TAB_TEST_HELPERS']
 t=time.monotonic();z=subprocess.run(cmd,capture_output=True,text=True,timeout=90)
 for k in ['stdout','stderr']:(r/(v+'-offset-build.'+k)).write_text(getattr(z,k))
 return dict(variant=v,command=cmd,exit=z.returncode,seconds=time.monotonic()-t,stderr=z.stderr)
results=list(ThreadPoolExecutor(2).map(run,['fix-normal','fix-assert']))
(r/'offset-build-results.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results,indent=2))
