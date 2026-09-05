from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import hashlib,json,os,shutil,subprocess,time
p=Path(__file__).resolve().parent
source=json.loads((p/'source-identity-v1.json').read_text())
flags='-DLUA_USE_ASSERT -DLJ_FUNC_TEST_HELPERS -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_ARENA_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLJ_XSAVE_TEST_HELPERS'
for variant in ['strict','asan']:
    for name,row in source['inputs'].items():
        f=p/'candidate'/name;assert hashlib.sha256(f.read_bytes()).hexdigest()==row['candidate_sha256']
        dest=p/variant/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(f,dest)
    for name in ['Makefile','.relver']:shutil.copyfile(p/'candidate'/name,p/variant/name)
def build(variant):
    cmd=['taskset','-c','16-19' if variant=='strict' else '20-23','make','-C',str(p/variant/'src'),'-j4','BUILDMODE=static','CCDEBUG=-g','TARGET_STRIP=:','XCFLAGS='+flags]
    if variant=='asan':cmd+=['CC=clang','CCOPT=-O1','TARGET_CFLAGS=-fsanitize=address -fno-omit-frame-pointer','TARGET_LDFLAGS=-fsanitize=address']
    start=time.monotonic();r=subprocess.run(cmd,cwd=p/variant,capture_output=True,text=True,timeout=240)
    after={name:hashlib.sha256((p/variant/name).read_bytes()).hexdigest() for name in source['inputs']}
    row={'command':cmd,'cwd':str(p/variant),'environment':{},'seconds':time.monotonic()-start,'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr,'source_identity_sha256':hashlib.sha256((p/'source-identity-v1.json').read_bytes()).hexdigest(),'runtime_input_hashes':after}
    if r.returncode==0:row['binaries']={name:hashlib.sha256((p/variant/'src'/name).read_bytes()).hexdigest() for name in ['luajit','libluajit.a','jit/vmdef.lua']}
    (p/(variant+'-build-v1.json')).write_text(json.dumps(row,indent=2)+'\n')
    assert all(after[name]==v['candidate_sha256'] for name,v in source['inputs'].items())
    print(variant,r.returncode,row['seconds'],r.stderr,flush=True)
    assert r.returncode==0
with ThreadPoolExecutor(max_workers=2) as pool:list(pool.map(build,['strict','asan']))
