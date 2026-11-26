from pathlib import Path
import subprocess,sys
p=Path(__file__).resolve().parent
mode=sys.argv[1]
phases=['smoke','public','progress','lease']
if mode=='combined-v1': phases.remove('progress'); phases.remove('lease')
if mode in ['assert-v1','asan-v1']:phases.append('traverse')
if mode!='tsan-v1':phases.append('matrix')
failed=[]
for phase in phases:
 code=subprocess.run([sys.executable,str(p/'validate.py'),mode,phase]).returncode
 if code: failed.append((phase,code))
code=subprocess.run([sys.executable,str(p/'extra.py'),mode]).returncode
if code:failed.append(('extra',code))
print(mode,'FAILURES',failed,flush=True)
sys.exit(bool(failed))
