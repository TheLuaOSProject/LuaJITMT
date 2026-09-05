from pathlib import Path
import subprocess,json,time,concurrent.futures,hashlib
p=Path(__file__).parent
flags='-DLJ_GC2_TEST_HELPERS -DLJ_ARENA_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLUA_USE_ASSERT'
def build(kind):
 root=p/kind
 cmd=['taskset','-c','0-15','make','-C',str(root/'src'),'-j8','BUILDMODE=static']
 if kind=='assert':cmd.append('XCFLAGS='+flags)
 start=time.monotonic()
 with (p/('build-'+kind+'.log')).open('w') as out:
  r=subprocess.run(cmd,stdout=out,stderr=subprocess.STDOUT)
 result={'kind':kind,'command':cmd,'exit':r.returncode,'seconds':time.monotonic()-start}
 if r.returncode==0: result['executable_sha256']=hashlib.sha256((root/'src/luajit').read_bytes()).hexdigest()
 (p/('build-'+kind+'.json')).write_text(json.dumps(result,indent=2)+'\n')
 print(json.dumps(result),flush=True)
 return r.returncode
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
 results=list(pool.map(build,['normal','assert']))
assert results==[0,0],results
