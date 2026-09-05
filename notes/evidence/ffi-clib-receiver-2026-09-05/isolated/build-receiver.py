from pathlib import Path
import hashlib,json,os,subprocess,sys,time
p=Path(__file__).resolve().parent
kind=sys.argv[1];tree=p/kind
flags=['-DLUA_USE_ASSERT','-DLJ_FUNC_TEST_HELPERS','-DLJ_GC2_TEST_HELPERS','-DLJ_TAB_TEST_HELPERS','-DLJ_ARENA_TEST_HELPERS','-DLJ_TRACE_TEST_HELPERS','-DLJ_XSAVE_TEST_HELPERS']
cmd=['taskset','-c','16-31','make','-C',str(tree/'src'),'-j4']
if kind!='candidate':cmd+=['BUILDMODE=static','CCDEBUG=-g','TARGET_STRIP=:','XCFLAGS='+' '.join(flags)]
if kind=='asan':cmd+=['CC=clang','CCOPT=-O1','TARGET_CFLAGS=-fsanitize=address -fno-omit-frame-pointer','TARGET_LDFLAGS=-fsanitize=address']
st=time.monotonic();r=subprocess.run(cmd,cwd=tree,capture_output=True,text=True,timeout=240)
(p/(kind+'-receiver-build.json')).write_text(json.dumps(dict(command=cmd,cwd=str(tree),seconds=time.monotonic()-st,exit=r.returncode,stdout=r.stdout,stderr=r.stderr),indent=2)+'\n')
(p/(kind+'-receiver-binaries.json')).write_text(json.dumps({f:hashlib.sha256((tree/'src'/f).read_bytes()).hexdigest() for f in ['luajit','libluajit.a','lj_crecord.o']},indent=2)+'\n')
print(kind,r.returncode,r.stderr)
assert r.returncode==0
