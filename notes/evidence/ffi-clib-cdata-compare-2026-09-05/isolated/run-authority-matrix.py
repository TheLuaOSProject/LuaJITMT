from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import json,subprocess
p=Path(__file__).resolve().parent
v=p/'validation'
def run(variant):
    rows=[]
    cpu={'candidate':'24','strict':'25','asan':'26'}[variant]
    if variant!='candidate':
        for script in ['run-final.py','run-supplement.py']:
            cmd=['taskset','-c',cpu,'python3',str(v/script),variant,'compile']
            r=subprocess.run(cmd,cwd=v,capture_output=True,text=True)
            rows.append({'command':cmd,'cwd':str(v),'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr})
            assert r.returncode==0,r.stderr
    for script,suite in [('run-final.py','full'),('run-final.py','gc'),('run-final.py','wrapper'),('run-final.py','gc-wrapper'),('run-supplement.py','full')]:
        cmd=['taskset','-c',cpu,'python3',str(v/script),variant,suite]
        r=subprocess.run(cmd,cwd=v,capture_output=True,text=True)
        rows.append({'command':cmd,'cwd':str(v),'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr})
        print(variant,suite,r.returncode,r.stdout[-800:],r.stderr,flush=True)
        assert r.returncode==0
    (v/(variant+'-matrix-driver.json')).write_text(json.dumps(rows,indent=2)+'\n')
with ThreadPoolExecutor(max_workers=3) as pool:list(pool.map(run,['candidate','strict','asan']))
