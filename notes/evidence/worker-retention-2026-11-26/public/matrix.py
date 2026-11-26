from pathlib import Path
import json,subprocess,sys,time
p=Path(__file__).resolve().parent
plans=[]
for mode in ['off','on']:
 for pool in [0,2]:
  for workers in [1,2,4]:
   plans.append([mode,str(workers),str(pool),'8','5','20000','0','4','0'])
for mode in ['off','on']:
 for pool in [0,2]:
  for workers in [1,4]:
   plans.append([mode,str(workers),str(pool),'12','5','20000','1','0','6'])
(p/'matrix-plan.json').write_text(json.dumps(plans,indent=2)+'\n')
rows=[]
for args in plans:
 stem='-'.join(args);cmd=[sys.executable,str(p/'run.py'),*args]
 print('RUN',stem,flush=True);t=time.monotonic()
 with (p/(stem+'.controller.log')).open('w') as out:
  r=subprocess.run(cmd,stdout=out,stderr=subprocess.STDOUT,timeout=135)
 row=dict(stem=stem,command=cmd,exit=r.returncode,seconds=time.monotonic()-t)
 rows.append(row);(p/'matrix-results.json').write_text(json.dumps(rows,indent=2)+'\n')
 print('DONE',stem,r.returncode,flush=True)
 if r.returncode:
  print((p/stem/'stderr').read_text(),flush=True)
  raise SystemExit(r.returncode)
