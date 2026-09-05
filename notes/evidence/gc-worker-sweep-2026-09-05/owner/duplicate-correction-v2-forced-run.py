from pathlib import Path
import subprocess,json,time,os,sys,hashlib,re
r=Path(__file__).resolve().parent
v=sys.argv[1];src=r/v/'src';env=dict(os.environ);env['LUA_PATH']=str(src/'?.lua')+';;'
asan='-asan' in v
if asan:env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
configs=[('t-safepoint-local-native-duplicate',[[]])]
results=[];identities={}
def run(argv,label):
 st=time.monotonic()
 with (r/(v+'-duplicate-correction-v2-forced-'+label+'.stdout')).open('w') as out,(r/(v+'-duplicate-correction-v2-forced-'+label+'.stderr')).open('w') as err:
  try:
   p=subprocess.run(argv,cwd=r,env=env,stdout=out,stderr=err,timeout=45);res=dict(exit_code=p.returncode)
  except subprocess.TimeoutExpired:res=dict(timeout=True)
 res.update(argv=argv,label=label,cwd=str(r),seconds=time.monotonic()-st,LUA_PATH=env['LUA_PATH'],ASAN_OPTIONS=env.get('ASAN_OPTIONS'))
 results.append(res);(r/(v+'-duplicate-correction-v2-forced-results.json')).write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(res),flush=True)
 return res.get('exit_code')==0
passed=True
for name,cases in configs:
 f=r/'duplicate-correction-v2-forced'/(name+'.c');b=r/(v+'-duplicate-correction-v2-forced-'+name)
 cmd=['clang' if asan else 'cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-DLUA_USE_ASSERT','-DLUA_USE_APICHECK','-DLJ_GC2_TEST_HELPERS','-I'+str(src),str(f),str(src/'libluajit.a'),'-lm','-ldl','-pthread','-Wl,-E','-o',str(b)]
 if asan:cmd[1:1]=['-fsanitize=address','-fno-omit-frame-pointer']
 cmd+=['-Wl,--wrap='+n for n in sorted(set(re.findall(r'__wrap_([a-zA-Z0-9_]+)',f.read_text())))]
 ok=run(cmd,name+'-compile');passed &= ok
 if not ok:continue
 for p in [f,b,src/'libluajit.a']:
  identities[str(p)]=dict(sha256=hashlib.sha256(p.read_bytes()).hexdigest(),bytes=p.stat().st_size)
 (r/(v+'-duplicate-correction-v2-forced-inputs.json')).write_text(json.dumps(identities,indent=2)+'\n')
 for i,args in enumerate(cases):passed &= run([str(b),*args],name+'-'+str(i))
raise SystemExit(0 if passed else 1)
