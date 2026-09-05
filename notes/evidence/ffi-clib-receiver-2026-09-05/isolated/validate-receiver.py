from pathlib import Path
import hashlib,json,os,subprocess,sys,time
p=Path(__file__).resolve().parent
up=Path('/tmp/lj-special-udata-method-review-20260905-djl20ksd')
kind=sys.argv[1];tree=(up/'baseline' if kind=='baseline' else up/'candidate' if kind=='method-only' else p/kind)
exe=tree/'src/luajit'
rows=[]
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def run(name,cmd,extra={}):
 env=dict(os.environ,LUA_PATH=f'{tree}/src/?.lua;{tree}/tests/lib/?.lua;;')
 if kind=='asan':env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1'
 else:env.pop('ASAN_OPTIONS',None)
 st=time.monotonic()
 try:
  r=subprocess.run(cmd,cwd=tree,env=env,text=True,capture_output=True,timeout=30);code=r.returncode;stdout=r.stdout;stderr=r.stderr
 except subprocess.TimeoutExpired as e:
  code='timeout';stdout=e.stdout;stderr=e.stderr
 row=dict(name=name,command=cmd,cwd=str(tree),environment={k:env[k] for k in ('LUA_PATH','ASAN_OPTIONS') if k in env},seconds=time.monotonic()-st,exit=code,stdout=stdout,stderr=stderr,**extra)
 rows.append(row);(p/(kind+'-receiver-results.json')).write_text(json.dumps(rows,indent=2)+'\n')
 print(kind,name,code,stdout,stderr,flush=True)
 return code
for value in [11,29]:
 path=p/f'namespace-{kind}-{value}.so'
 assert run(f'library-{value}',['cc','-shared','-fPIC','-O2',f'-DNAMESPACE_VALUE={value}',str(p/'namespace-symbol-lib.c'),'-o',str(path)],{'library_source_sha256':sha(p/'namespace-symbol-lib.c')})==0
libs=[p/f'namespace-{kind}-{value}.so' for value in [11,29]]
for mode in ['-joff','-jon']:
 for case in ['index-other','index-type','newindex-other','newindex-type','index-life','newindex-life','index-side-other','index-side-type','newindex-side-other','newindex-side-type']:
  fixture=p/'captured-clib-receiver.lua'
  run(case+mode,[str(exe),mode,str(fixture),case,*map(str,libs)],{'fixture_sha256':sha(fixture),'exe_sha256':sha(exe),'library_sha256':[sha(lib) for lib in libs],'record_sha256':sha(tree/'src/lj_record.c'),'crecord_sha256':sha(tree/'src/lj_crecord.c')})
