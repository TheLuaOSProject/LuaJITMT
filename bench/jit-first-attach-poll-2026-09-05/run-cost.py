from pathlib import Path
import subprocess,time,json,hashlib,os,statistics,platform
r=Path(__file__).resolve().parent
out=r/'cost';out.mkdir(exist_ok=True)
records=[]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
meta={'date':'2026-09-05','cpu':30,'rounds':7,'samples_per_process':5,'clock':'os.clock CPU seconds','GC':'enabled, unchanged defaults','host':platform.uname()._asdict(),'benchmark_sha256':sha(r/'bench.lua'),'binaries':{k:sha(r/k/'src/luajit') for k in ['base-normal','fix-normal']}}
(out/'metadata.json').write_text(json.dumps(meta,indent=2)+'\n')
for rnd in range(7):
 for case in ['numeric','ffi_struct','table_read']:
  order=['base-normal','fix-normal'] if rnd%2==0 else ['fix-normal','base-normal']
  for kind in order:
   tree=r/kind;cmd=['taskset','-c','30',str(tree/'src/luajit'),str(r/'bench.lua'),case];env=os.environ.copy();env['LUA_PATH']=str(tree/'src/?.lua')+';'+str(tree/'src/?/init.lua')+';;';t=time.monotonic()
   try:
    p=subprocess.run(cmd,cwd=tree,env=env,capture_output=True,text=True,timeout=30)
    d={'round':rnd+1,'case':case,'variant':kind,'command':cmd,'LUA_PATH':env['LUA_PATH'],'exit':p.returncode,'seconds':time.monotonic()-t,'stdout':p.stdout,'stderr':p.stderr}
    if p.returncode==0:
     fields=p.stdout.strip().split();assert fields[0]==case;d.update(iterations=int(fields[1]),best_seconds=float(fields[2]),samples=[float(x) for x in fields[3].split(',')],native_exits=int(fields[4]))
   except subprocess.TimeoutExpired as e:
    d={'round':rnd+1,'case':case,'variant':kind,'command':cmd,'exit':None,'status':'timeout','stdout':str(e.stdout),'stderr':str(e.stderr)}
   records.append(d);(out/'raw.json').write_text(json.dumps(records,indent=2)+'\n');print(case,kind,rnd+1,d['exit'],d.get('best_seconds'),flush=True)
   assert d['exit']==0,d
summary=[]
for case in ['numeric','ffi_struct','table_read']:
 pairs=[]
 for rnd in range(1,8):
  a=next(d for d in records if d['round']==rnd and d['case']==case and d['variant']=='base-normal');b=next(d for d in records if d['round']==rnd and d['case']==case and d['variant']=='fix-normal');pairs.append(b['best_seconds']/a['best_seconds'])
 def median(kind):return statistics.median(d['best_seconds']/d['iterations']*1e9 for d in records if d['case']==case and d['variant']==kind)
 summary.append({'case':case,'baseline_ns_per_iteration':median('base-normal'),'fix_ns_per_iteration':median('fix-normal'),'paired_ratio_median':statistics.median(pairs),'paired_ratio_range':[min(pairs),max(pairs)],'pair_ratios':pairs})
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary),flush=True)
