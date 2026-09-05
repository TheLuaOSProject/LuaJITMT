from pathlib import Path
import hashlib,json,os,resource,statistics,subprocess,sys,time

resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent
setup=json.loads((p.parent/'setup.json').read_text())
trees={'baseline':Path('/tmp/lj-worker-bridge-combined-20260905-bz9wysjp/candidate'),
       'candidate':p.parent/'candidate'}
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
pilot='--pilot' in sys.argv
output=p/('pilot.json' if pilot else 'results.json')
assert not output.exists()
def verify():
    for kind,tree in trees.items():
        for f,digest in setup['before_inputs' if kind=='baseline' else 'combined_inputs'].items():
            assert sha(tree/f)==digest,(kind,f)
verify()
rows=[]
for workload,mode in [('next','-joff'),('itern','-joff'),('next','-jon'),('itern','-jon')]:
    rounds=10000 if mode=='-joff' else 200000
    for pair in range(1 if pilot else 7):
        for kind in (['baseline','candidate'] if pair%2==0 else ['candidate','baseline']):
            tree=trees[kind]
            cmd=['taskset','-c','30',str(tree/'src/luajit'),mode,str(p/'iteration-cost.lua'),workload,str(rounds)]
            env=os.environ.copy();env.pop('ASAN_OPTIONS',None)
            env['LUA_PATH']=str(tree/'src/?.lua')+';;'
            start=time.monotonic()
            row=dict(workload=workload,mode=mode,pair=pair,variant=kind,command=cmd,cwd=str(p),
                environment={'LUA_PATH':env['LUA_PATH']},fixture_sha256=sha(p/'iteration-cost.lua'),
                exe_sha256=sha(tree/'src/luajit'),archive_sha256=sha(tree/'src/libluajit.a'),timeout_seconds=60)
            try:
                q=subprocess.run(cmd,cwd=p,env=env,capture_output=True,text=True,timeout=60)
                row.update(exit=q.returncode,stdout=q.stdout,stderr=q.stderr)
            except subprocess.TimeoutExpired as e:
                row.update(exit=124,timed_out=True,stdout=(e.stdout or b'').decode(errors='replace'),stderr=(e.stderr or b'').decode(errors='replace'))
            row['seconds']=time.monotonic()-start
            rows.append(row);output.write_text(json.dumps(rows,indent=2)+'\n')
            print(workload,mode,pair,kind,row['exit'],flush=True)
            assert row['exit']==0,(workload,mode,kind,row['stderr'])
            parsed=[json.loads(x) for x in row['stdout'].splitlines()]
            assert len(parsed)==6 and all(x['visits']==rounds*32 for x in parsed[:5])
            row['median_ns']=statistics.median(x['ns'] for x in parsed[:5])
            row['jit_observation']=parsed[-1]
            output.write_text(json.dumps(rows,indent=2)+'\n')
verify()
if not pilot:
    summary=[]
    for workload,mode in [('next','-joff'),('itern','-joff'),('next','-jon'),('itern','-jon')]:
        selected=[x for x in rows if x['workload']==workload and x['mode']==mode]
        a=[x['median_ns'] for x in selected if x['variant']=='baseline']
        b=[x['median_ns'] for x in selected if x['variant']=='candidate']
        ratios=[b[i]/a[i] for i in range(7)]
        summary.append(dict(workload=workload,mode=mode,baseline_median_ns=statistics.median(a),
            candidate_median_ns=statistics.median(b),baseline_range=[min(a),max(a)],candidate_range=[min(b),max(b)],
            median_paired_percent=(statistics.median(ratios)-1)*100,paired_ratios=ratios))
    (p/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
