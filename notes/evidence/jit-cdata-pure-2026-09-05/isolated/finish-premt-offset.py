from pathlib import Path
import json,shutil,hashlib,subprocess,os,time,difflib
from concurrent.futures import ThreadPoolExecutor
r=Path('/tmp/lj-premt-cdata-hoist-20260905-oa96m15y')
for v in ['fix-normal','fix-assert']:
 p=r/v/'src/lj_opt_loop.c';s=p.read_text();s=s.replace('ir_kint64(ofs)->i64 >= (int64_t)sizeof(GCcdata)','ir_kint64(ofs)->u64 >= (uint64_t)sizeof(GCcdata)').replace('ir_kint64(ofs)->i64 <= INT32_MAX','ir_kint64(ofs)->u64 <= INT32_MAX');p.write_text(s)
paths=['src/lj_jit.h','src/lj_opt_loop.c','src/lj_opt_mem.c']
(r/'candidate.patch').write_text(''.join(''.join(difflib.unified_diff((r/'base-normal'/p).read_text().splitlines(True),(r/'fix-normal'/p).read_text().splitlines(True),fromfile='a/'+p,tofile='b/'+p)) for p in paths))
(r/'candidate-source.json').write_text(json.dumps({'base':'b4e26564542cb8bfa997a11c6a90e5e0017a2f79','sha256':{p:hashlib.sha256((r/'fix-normal'/p).read_bytes()).hexdigest() for p in paths}},indent=2)+'\n')
def run(v):
 cmd=['taskset','-c','0-15','make','-C',str(r/v/'src'),'-j4','BUILDMODE=static','CCDEBUG=-g','TARGET_STRIP=:']
 if v=='fix-assert':cmd+=['XCFLAGS=-DLUA_USE_ASSERT -DLJ_GC2_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLJ_TAB_TEST_HELPERS']
 z=subprocess.run(cmd,capture_output=True,text=True,timeout=90)
 for k in ['stdout','stderr']:(r/(v+'-offset-u64-build.'+k)).write_text(getattr(z,k))
 assert z.returncode==0,z.stderr
 env=os.environ.copy();env['LUA_PATH']=str(r/v/'src/?.lua')+';;'
 cmd=['taskset','-c','0-15',str(r/v/'src/luajit'),str(r/'positive.lua')]
 z=subprocess.run(cmd,capture_output=True,text=True,env=env,timeout=20)
 for k in ['stdout','stderr']:(r/(v+'-positive.'+k)).write_text(getattr(z,k))
 return dict(variant=v,command=cmd,exit=z.returncode,stderr=z.stderr)
results=list(ThreadPoolExecutor(2).map(run,['fix-normal','fix-assert']))
(r/'offset-u64-results.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results,indent=2))
