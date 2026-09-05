from pathlib import Path
import json,subprocess,time,os,sys,hashlib,resource
P=Path(__file__).resolve().parent;v=sys.argv[1]
S=Path('/tmp/lj-reclaim-fair-pass-20260905-kw8kfdam/candidate') if v=='assert' else P/'asan'
A=P/(v+'-accepted-fixture-checks');A.mkdir(exist_ok=True)
flags='-DLUA_USE_ASSERT -DLUA_USE_APICHECK -DLJ_GC2_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_FUNC_TEST_HELPERS -DLJ_UDATA_TEST_HELPERS -DLJ_STR_TEST_HELPERS'.split()
if v.startswith('arena'):flags+=['-DLJ_ARENA_TEST_HELPERS']
if v.endswith('asan'):flags+=['-fsanitize=address','-fno-omit-frame-pointer']
cc='clang' if v.endswith('asan') else 'cc'
env=os.environ.copy();env['LUA_PATH']=str(S/'src/?.lua')+';'+str(S/'tests/lib/?.lua')+';;'
if v.endswith('asan'):env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
resource.setrlimit(resource.RLIMIT_CORE,(0,0));rows=[];ids={}
def identify(q):
 q=Path(q);ids[str(q)]=dict(sha256=hashlib.sha256(q.read_bytes()).hexdigest(),bytes=q.stat().st_size)
 (A/'identities.json').write_text(json.dumps(ids,indent=2)+'\n')
def run(name,argv,kind):
 start=time.monotonic();timed=False
 with (A/(name+'.stdout')).open('wb') as out,(A/(name+'.stderr')).open('wb') as err:
  try:r=subprocess.run(argv,cwd=S,env=env,stdout=out,stderr=err,timeout=60);code=r.returncode
  except subprocess.TimeoutExpired:timed=True;code=124
 row=dict(name=name,kind=kind,argv=argv,cwd=str(S),LUA_PATH=env['LUA_PATH'],ASAN_OPTIONS=env.get('ASAN_OPTIONS'),timeout_seconds=60,exit_code=code,timed_out=timed,seconds=time.monotonic()-start,stdout=name+'.stdout',stderr=name+'.stderr');rows.append(row)
 (A/'results.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps({k:row[k] for k in ['name','kind','exit_code','timed_out','seconds']}),flush=True)
 return code
identify(S/'src/libluajit.a');specs=json.loads((P/'specs.json').read_text())
chosen=set(sys.argv[2:])
for spec in specs:
 name=spec['name']
 if name not in chosen:continue
 q=P/'accepted-fixtures/tests'/(name+'.c');identify(q);exe=A/name
 argv=[cc,'-std=gnu11','-O1' if v.endswith('asan') else '-O2','-g','-Wall','-Wextra','-Werror','-I'+str(S/'src'),'-I'+str(S/'tests'),*flags,str(q),str(S/'src/libluajit.a'),'-Wl,-E','-lm','-ldl','-pthread',*['-Wl,--wrap='+w for w in spec['wraps']],'-o',str(exe)]
 if run(name+'-compile',argv,'compile'):continue
 identify(exe)
 for mode in spec['modes']:run(name+('-'+'-'.join(mode) if mode else ''),[str(exe),*mode],'runtime')
