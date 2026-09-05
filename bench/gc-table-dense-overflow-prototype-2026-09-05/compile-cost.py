from pathlib import Path
import subprocess,json
p=Path(__file__).parent;rows=[]
for name in ['base-normal','normal']:
 cmd=['taskset','-c','0-15','gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16','-I'+str(p/name/'src'),str(p/'cost.c'),str(p/name/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(p/(name+'-cost'))]
 if name=='normal':cmd+=['-DDENSE_W']
 q=subprocess.run(cmd,capture_output=True,text=True);rows.append({'command':cmd,'exit':q.returncode,'stdout':q.stdout,'stderr':q.stderr});print(name,q.returncode,q.stderr)
(p/'compile-cost.json').write_text(json.dumps(rows,indent=2)+'\n')

raise SystemExit(any(row['exit'] for row in rows))
