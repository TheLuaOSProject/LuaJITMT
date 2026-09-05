from pathlib import Path
import subprocess, sys
p=Path(__file__).resolve().parent
variant=sys.argv[1]
assert variant in ['candidate','optimized','asan','baseline']
if variant=='baseline':
    cases=['next,dense','rooted,empty']
else:
    cases=[m+','+k for m in ['next','itern','rooted','cursor']
           for k in ['dense','sparse','empty','holes','zero','bool']
           if not (variant=='candidate' and k in ['dense','empty'])]
q=subprocess.run([sys.executable,str(p/'validation-driver.py'),variant,
    't-tab-scalar-next-progress.c','progress-'+variant+'-complete']+cases,cwd=p)
assert q.returncode==0
