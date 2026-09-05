from pathlib import Path
import json,subprocess,time,os,sys,hashlib,resource
P=Path(__file__).resolve().parent
F=Path('/tmp/lj-reclaim-fair-pass-20260905-kw8kfdam')
v=sys.argv[1]; S=F/'candidate' if v=='assert' else P/'asan';A=P/v;A.mkdir(exist_ok=True)
flags='-DLUA_USE_ASSERT -DLUA_USE_APICHECK -DLJ_GC2_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_FUNC_TEST_HELPERS -DLJ_UDATA_TEST_HELPERS -DLJ_STR_TEST_HELPERS'.split()
if v=='asan':flags+=['-fsanitize=address','-fno-omit-frame-pointer']
cc='clang' if v=='asan' else 'cc'
env=os.environ.copy();env['LUA_PATH']=str(S/'src/?.lua')+';'+str(S/'tests/lib/?.lua')+';;'
if v=='asan':env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
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
identify(S/'src/libluajit.a');identify(S/'src/luajit')
specs=json.loads((P/'specs.json').read_text())
if v=='asan':
 specs.append(dict(name='t-func-construction-anchor',source='tests/t-func-construction-anchor.c',wraps=[],modes=[[]]))
 specs.extend([dict(name='t-owner-defer',source=str(F/'acceptance/v2/t-owner-defer.c'),wraps=[],modes=[['publish'],['cancel'],['automatic']]),dict(name='t-mixed-owner',source=str(F/'acceptance/mixed-v2/t-mixed-owner.c'),wraps=[],modes=[['0','1'],['2','1']]),dict(name='t-fair-owner',source=str(F/'acceptance/fair-v1/t-fair-owner.c'),wraps=[],modes=[['0','1','1'],['0','3','1'],['0','3','64'],['2','3','64'],['0','3','1','detach']])])
for spec in specs:
 name=spec['name'];q=Path(spec['source']);q=q if q.is_absolute() else S/q
 identify(q);exe=A/name
 argv=[cc,'-std=gnu11','-O1' if v=='asan' else '-O2','-g','-Wall','-Wextra','-Werror','-I'+str(S/'src'),'-I'+str(S/'tests'),*flags,str(q),str(S/'src/libluajit.a'),'-Wl,-E','-lm','-ldl','-pthread',*['-Wl,--wrap='+w for w in spec['wraps']],'-o',str(exe)]
 if run(name+'-compile',argv,'compile'):continue
 identify(exe)
 for mode in spec['modes']:run(name+('-'+'-'.join(mode) if mode else ''),[str(exe),*mode],'runtime')
for name in ['t-gc-active-thread-roots','t-gc-workers','t-gc2-finalizer-peer-collect']:
 q=S/'tests'/(name+'.lua');identify(q)
 for off in [True,False]:run(name+('-joff' if off else '-jit'),[str(S/'src/luajit'),*(['-joff'] if off else []),str(q)],'runtime')
print(json.dumps(dict(variant=v,runtimes=sum(r['kind']=='runtime' for r in rows),failures=[r['name'] for r in rows if r['exit_code']!=0])),flush=True)
