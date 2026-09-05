from pathlib import Path
import json,hashlib,subprocess,os,time,sys
r=Path(__file__).resolve().parent;v=sys.argv[1];s=r/v/'src';label=v+'-boundary-overlaps';rows=[];ids={};env=dict(os.environ,LUA_PATH=str(s/'?.lua')+';;')
if v=='asan':env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
flags=['-DLUA_USE_ASSERT','-DLUA_USE_APICHECK'] if v in ['strict','asan'] else []
if v=='asan':flags+=['-fsanitize=address','-fno-omit-frame-pointer']
fixtures=['t-stop-first-attach','t-stop-first-attach-restart-v2','t-auto-restart-numeric-max']
if v!='asan':fixtures+=['t-restart-first-attach']
for f in fixtures:
 e=r/(label+'-'+f);src=r/(f+'.c');cmd=['clang' if v=='asan' else 'cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror',*flags,'-I'+str(s),str(src),str(s/'libluajit.a'),'-lm','-ldl','-pthread','-Wl,-E']
 if 'attach' in f:cmd+=['-Wl,--wrap=lj_vm_cpcall']
 cmd+=['-o',str(e)]
 for tag,argv in [('compile',cmd),('run',[str(e)])]:
  n=label+'-'+f+'-'+tag;st=time.monotonic()
  with (r/(n+'.stdout')).open('w') as out,(r/(n+'.stderr')).open('w') as err:
   try:x=subprocess.run(argv,cwd=r,env=env,stdout=out,stderr=err,timeout=25);code=x.returncode
   except subprocess.TimeoutExpired:code='timeout'
  rows.append(dict(name=n,argv=argv,exit_code=code,seconds=time.monotonic()-st,cwd=str(r),LUA_PATH=env['LUA_PATH'],ASAN_OPTIONS=env.get('ASAN_OPTIONS'),stdout=n+'.stdout',stderr=n+'.stderr'));(r/(label+'-results.json')).write_text(json.dumps(rows,indent=2)+'\n');print(v,f,tag,code,flush=True)
  if tag=='compile' and code:break
 for p in [e,src,s/'libluajit.a']:
  if p.exists():ids[str(p)]=dict(sha256=hashlib.sha256(p.read_bytes()).hexdigest(),bytes=p.stat().st_size)
(r/(label+'-identities.json')).write_text(json.dumps(ids,indent=2)+'\n')
