from pathlib import Path
import subprocess,json
p=Path(__file__).parent;rows=[]
for variant in ['calloc-normal','tail-normal']:
 cmd=['taskset','-c','0-15','gcc','-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-mcx16','-I'+str(p/variant/'src'),str(p/'huge-cost.c'),str(p/variant/'src/libluajit.a'),'-lm','-ldl','-pthread','-Wl,--wrap=mmap','-Wl,--wrap=mmap64','-Wl,--wrap=munmap','-Wl,--wrap=calloc','-Wl,--wrap=free','-o',str(p/(variant+'-cost'))]
 if variant=='tail-normal':cmd+=['-DTAIL_W']
 q=subprocess.run(cmd,capture_output=True,text=True);rows.append({'command':cmd,'exit':q.returncode,'stdout':q.stdout,'stderr':q.stderr});print(variant,q.returncode,q.stderr)
(p/'compile-cost.json').write_text(json.dumps(rows,indent=2)+'\n');raise SystemExit(any(r['exit'] for r in rows))
