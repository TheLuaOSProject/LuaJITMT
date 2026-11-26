from pathlib import Path
import subprocess,sys
p=Path(__file__).resolve().parent
mode=sys.argv[1]
phases=['plain']
if not mode.startswith(('noffi','nojit','nobuf')):
 phases+=['smoke','public']
 if not mode.startswith('gcc'):phases+=['progress','lease','arena']
 if mode.startswith(('assert','asan')):phases+=['traverse']
 if not mode.startswith('tsan'):phases+=['matrix']
failed=[]
for phase in phases:
 code=subprocess.run([sys.executable,str(p/'validate.py'),mode,phase]).returncode
 if code:failed.append((phase,code))
code=subprocess.run([sys.executable,str(p/'extra.py'),mode]).returncode
if code:failed.append(('extra',code))
print(mode,'FAILURES',failed,flush=True)
sys.exit(bool(failed))
