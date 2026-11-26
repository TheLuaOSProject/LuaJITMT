from pathlib import Path
import subprocess,time,hashlib,json,os,resource
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
p=Path(__file__).resolve().parent
rows=[]
for mode,tree in [('old',Path('/tmp/lj-buffer-headroom-20261126-y6qc0l6f/combined-v1')),('candidate',p/'gcc-final')]:
 cmd=['gcc','-O2','-g','-Wall','-Wextra','-Werror','-mcx16','-I'+str(tree/'src'),str(p/'live-growth.c'),str(tree/'src/libluajit.a'),'-lm','-ldl','-pthread','-o',str(p/('live-growth-final-'+mode))]
 done=subprocess.run(cmd,capture_output=True,text=True);assert done.returncode==0,done.stderr
 rows.append(dict(build=mode,command=cmd,binary_sha256=hashlib.sha256((p/('live-growth-final-'+mode)).read_bytes()).hexdigest(),library_sha256=hashlib.sha256((tree/'src/libluajit.a').read_bytes()).hexdigest()))
for count in [16,128,1024,4096]:
 for rep in range(5):
  for mode in (['old','candidate'] if rep%2==0 else ['candidate','old']):
   cmd=['taskset','-c','8',str(p/('live-growth-final-'+mode)),str(count)]
   done=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
   row=dict(mode=mode,count=count,rep=rep,command=cmd,exit=done.returncode,stdout=done.stdout,stderr=done.stderr);rows.append(row);print(row,flush=True)
   (p/'live-growth-final.json').write_text(json.dumps(rows,indent=2)+'\n')
