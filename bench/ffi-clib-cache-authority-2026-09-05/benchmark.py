from pathlib import Path
import hashlib, json, os, resource, subprocess, time
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
p = Path(__file__).resolve().parent
repo = Path('/workspaces/lj-lockless')
trees = {'baseline':Path('/tmp/lj-udata-pure-receiver-combined-20260905-sn9vd57b/candidate'),
         'candidate':p.parent/'candidate'}
rows=[]
for workload in ['direct_clib', 'clib', 'call', 'ffi_struct', 'shared']:
    for pair in range(7):
        for variant in (['baseline','candidate'] if pair%2==0 else ['candidate','baseline']):
            tree=trees[variant]
            envadd={'LUA_PATH':str(tree/'src/?.lua')+';;', 'BENCH_SCALE':'1'}
            if workload=='direct_clib': fixture=p/'direct-cost.lua';args=[str(fixture),'clib']
            elif workload=='shared':
                fixture=p/'shared-cost.lua';args=[str(fixture),'2000000','helper' if variant=='candidate' else 'baseline']
            elif workload=='ffi_struct': fixture=repo/'aux/bench/bench.lua';args=[str(fixture),'ffi_struct']
            else: fixture=p/'cost.lua';args=[str(fixture),workload]
            cmd=['taskset','-c','31',str(tree/'src/luajit'),'-jon']+args
            start=time.monotonic()
            try:
                q=subprocess.run(cmd,cwd=repo,env={**os.environ,**envadd},capture_output=True,text=True,timeout=45)
                out={'exit':q.returncode,'stdout':q.stdout,'stderr':q.stderr}
            except subprocess.TimeoutExpired as e:
                out={'exit':None,'timeout':True,'stdout':(e.stdout or b'').decode(errors='replace'),'stderr':(e.stderr or b'').decode(errors='replace')}
            rows.append({'workload':workload,'pair':pair,'variant':variant,'command':cmd,'cwd':str(repo),
                         'environment':envadd,'seconds':time.monotonic()-start,
                         'fixture_sha256':hashlib.sha256(fixture.read_bytes()).hexdigest(),
                         'exe_sha256':hashlib.sha256((tree/'src/luajit').read_bytes()).hexdigest(),**out})
            (p/'cost-results.json').write_text(json.dumps(rows,indent=2)+'\n')
            assert out['exit']==0, rows[-1]
        print(workload, pair, 'passed',flush=True)
