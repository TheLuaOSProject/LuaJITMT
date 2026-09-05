from pathlib import Path
import hashlib,json,os,subprocess,time

r=Path(__file__).resolve().parent
src=r/'debug/src'
fixture=r/'debug-observed-v3-retention'
cmd=['cc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-I'+str(src),
     str(r/'t-string-retention-observed.c'),str(src/'libluajit.a'),
     '-lm','-ldl','-pthread','-Wl,-E','-o',str(fixture)]
with (r/'gdb-v3-compile.stdout').open('w') as o,(r/'gdb-v3-compile.stderr').open('w') as e:
    c=subprocess.run(cmd,cwd=r,stdout=o,stderr=e,timeout=50)
assert c.returncode==0
inputs={p.name:dict(sha256=hashlib.sha256(p.read_bytes()).hexdigest(),bytes=p.stat().st_size)
        for p in [fixture,src/'libluajit.a',r/'sweep-probe.h',r/'t-string-retention-observed.c',
                  r/'peer-control.lua',r/'gdb-sweep.py']}
(r/'gdb-v3-inputs.json').write_text(json.dumps({'compile':cmd,'identities':inputs},indent=2)+'\n')
results=[]
for case in ['0-0-0','0-0-2','0-1-0','0-1-2']:
    env=dict(os.environ,LUA_PATH=str(src/'?.lua')+';;',RETENTION_JIT='0',
             SWEEP_GDB_OUT=str(r/('gdb-v3-'+case)))
    argv=['gdb','-q','-nx','-batch','-x',str(r/'gdb-sweep.py'),'--args',str(fixture),
          *case.split('-'),str(r/'peer-control.lua')]
    st=time.monotonic()
    with (r/('gdb-v3-'+case+'.stdout')).open('w') as o,(r/('gdb-v3-'+case+'.stderr')).open('w') as e:
        try:
            c=subprocess.run(argv,cwd=r,env=env,stdout=o,stderr=e,timeout=55)
            result={'exit_code':c.returncode}
        except subprocess.TimeoutExpired:
            result={'timeout':True}
    result.update(case=case,argv=argv,seconds=time.monotonic()-st,
                  environment={k:env[k] for k in ['LUA_PATH','RETENTION_JIT','SWEEP_GDB_OUT']})
    results.append(result)
    (r/'gdb-v3-results.json').write_text(json.dumps(results,indent=2)+'\n')
    print(json.dumps(result),flush=True)
