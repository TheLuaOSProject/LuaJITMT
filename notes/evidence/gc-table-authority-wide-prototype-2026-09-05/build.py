from pathlib import Path
import subprocess, json, time, sys, hashlib
out=Path(__file__).parent;kind=sys.argv[1];tree=out/kind
cmd=['taskset','-c','0-15','make','-C',str(tree/'src'),'-j4','BUILDMODE=static']
if 'helper' in kind:cmd+=['XCFLAGS=-DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLUA_USE_ASSERT']
with (out/('build-'+kind+'.log')).open('w') as log:
 t=time.monotonic();r=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT)
 row={'kind':kind,'command':cmd,'rc':r.returncode,'elapsed':time.monotonic()-t}
 if not r.returncode:
  row['binary_sha256']=hashlib.sha256((tree/'src/luajit').read_bytes()).hexdigest()
  row['archive_sha256']=hashlib.sha256((tree/'src/libluajit.a').read_bytes()).hexdigest()
 (out/('build-'+kind+'.json')).write_text(json.dumps(row,indent=2));print(row,flush=True)
