from pathlib import Path
import hashlib,json,os,subprocess,time
p=Path(__file__).resolve().parent;tree=p/'releasehelpers-v2'
flags='-DLJ_FUNC_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_ARENA_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLJ_XSAVE_TEST_HELPERS'
cmd=['taskset','-c','16-19','make','-C',str(tree/'src'),'-j4','BUILDMODE=static','CCDEBUG=-g','TARGET_STRIP=:','XCFLAGS='+flags]
start=time.monotonic();q=subprocess.run(cmd,cwd=tree,capture_output=True,text=True,timeout=180)
sha=lambda f:hashlib.sha256(Path(f).read_bytes()).hexdigest()
row={'command':cmd,'cwd':str(tree),'seconds':time.monotonic()-start,'exit':q.returncode,'stdout':q.stdout,'stderr':q.stderr,'inputs_sha256':sha(p/'releasehelpers-v2-inputs.json')}
if q.returncode==0:
 row['binaries']={rel:sha(tree/rel) for rel in ['src/luajit','src/libluajit.a','src/jit/vmdef.lua']}
 inp=json.loads((p/'releasehelpers-v2-inputs.json').read_text())
 assert all(sha(tree/rel)==digest for rel,digest in inp['sources'].items())
out=p/'releasehelpers-v2-build.json';assert not out.exists();out.write_text(json.dumps(row,indent=2)+'\n')
print('build exit',q.returncode,'seconds',row['seconds'],flush=True)
assert q.returncode==0,row
