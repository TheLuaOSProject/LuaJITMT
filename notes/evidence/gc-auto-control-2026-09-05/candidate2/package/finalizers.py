from pathlib import Path
import json,hashlib,subprocess,os,time,sys,itertools
r=Path(__file__).resolve().parent;v=sys.argv[1];s=r/v/'src';label=v+'-finalizers';e=r/(label+'-fixture');env=dict(os.environ,LUA_PATH=str(s/'?.lua')+';;');rows=[]
if v=='asan':env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
flags=['-DLUA_USE_ASSERT','-DLUA_USE_APICHECK'] if v in ['strict','asan','helpers'] else []
if v=='asan':flags+=['-fsanitize=address','-fno-omit-frame-pointer']
cmd=['clang' if v=='asan' else 'cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror',*flags,'-I'+str(s),str(r/'t-auto-finalizer-controls.c'),str(s/'libluajit.a'),'-lm','-ldl','-pthread','-Wl,-E','-o',str(e)]
def run(argv,case):
 n=label+'-'+case;st=time.monotonic()
 with (r/(n+'.stdout')).open('w') as out,(r/(n+'.stderr')).open('w') as err:
  try:x=subprocess.run(argv,cwd=r,env=env,stdout=out,stderr=err,timeout=25);code=x.returncode
  except subprocess.TimeoutExpired:code='timeout'
 row=dict(case=case,argv=argv,exit_code=code,seconds=time.monotonic()-st,cwd=str(r),LUA_PATH=env['LUA_PATH'],ASAN_OPTIONS=env.get('ASAN_OPTIONS'),stdout=n+'.stdout',stderr=n+'.stderr');rows.append(row);(r/(label+'-results.json')).write_text(json.dumps(rows,indent=2)+'\n');print(v,case,code,flush=True);return code
if run(cmd,'compile'):raise SystemExit(1)
(r/(label+'-identities.json')).write_text(json.dumps({str(p):dict(sha256=hashlib.sha256(p.read_bytes()).hexdigest(),bytes=p.stat().st_size) for p in [e,r/'t-auto-finalizer-controls.c',s/'libluajit.a']},indent=2)+'\n')
for case in itertools.product(range(2),repeat=4):run([str(e),*[str(x) for x in case],'0' if v=='control' else '1'],'-'.join(str(x) for x in case))
raise SystemExit(0 if all(x['exit_code']==0 for x in rows) else 1)
