from pathlib import Path
import subprocess,json,time,sys,os,hashlib
r=Path(__file__).resolve().parent
v=sys.argv[1];cases=sys.argv[2:] or ['0-0','4096-0','262144-0','262144-1','262144-2'];src=r/v/'src'
label=v+os.environ.get('EOF_RUN_SUFFIX','')
b=r/(label+'-eof');asan=v.endswith('-asan')
env=dict(os.environ);env['LUA_PATH']=str(src/'?.lua')+';;'
if asan:env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
results=[]
def run(cmd,case):
 st=time.monotonic()
 with (r/(label+'-'+case+'.stdout')).open('w') as out,(r/(label+'-'+case+'.stderr')).open('w') as err:
  try:p=subprocess.run(cmd,cwd=r,env=env,stdout=out,stderr=err,timeout=50);d={'exit_code':p.returncode}
  except subprocess.TimeoutExpired:d={'timeout':True}
 d.update(argv=cmd,case=case,cwd=str(r),seconds=time.monotonic()-st,LUA_PATH=env['LUA_PATH'],ASAN_OPTIONS=env.get('ASAN_OPTIONS'))
 results.append(d);(r/(label+'-results.json')).write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(d),flush=True)
 return d.get('exit_code')==0
cmd=['clang' if asan else 'cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-I'+str(src),str(r/'t-pending-root-eof.c'),str(src/'libluajit.a'),'-lm','-ldl','-pthread','-Wl,-E','-Wl,--wrap=lj_gc_sweep_gc2_unmarked','-o',str(b)]
if asan:cmd[1:1]=['-DLUA_USE_ASSERT','-DLUA_USE_APICHECK','-fsanitize=address','-fno-omit-frame-pointer']
if not run(cmd,'compile'):raise SystemExit(1)
(r/(label+'-inputs.json')).write_text(json.dumps({str(p):{'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'bytes':p.stat().st_size} for p in [r/'t-pending-root-eof.c',b,src/'libluajit.a']},indent=2)+'\n')
passed=True
for case in cases:passed &= run([str(b),*case.split('-')],case)
raise SystemExit(0 if passed else 1)
