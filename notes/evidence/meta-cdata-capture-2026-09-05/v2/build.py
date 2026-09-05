from pathlib import Path
import subprocess,time,json,hashlib
p=Path(__file__).parent
for kind in ['normal','assert']:
 cmd=['taskset','-c','0-15','make','-C',str(p/kind/'src'),'-j8','BUILDMODE=static']
 if kind=='assert':cmd+=['XCFLAGS=-DLJ_FUNC_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_ARENA_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLUA_USE_ASSERT']
 start=time.monotonic()
 with (p/('build-'+kind+'.log')).open('w') as out:r=subprocess.run(cmd,stdout=out,stderr=subprocess.STDOUT,timeout=120)
 rec={'kind':kind,'command':cmd,'exit':r.returncode,'seconds':time.monotonic()-start,'binary_sha256':hashlib.sha256((p/kind/'src/luajit').read_bytes()).hexdigest() if r.returncode==0 else None}
 (p/('build-'+kind+'.json')).write_text(json.dumps(rec,indent=2)+'\n');print(json.dumps(rec),flush=True);assert r.returncode==0
