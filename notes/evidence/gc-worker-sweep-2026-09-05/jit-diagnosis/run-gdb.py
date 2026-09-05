import json,pathlib,resource,subprocess,time,sys,os
P=pathlib.Path(__file__).resolve().parent
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
name=sys.argv[1];cmd=['taskset','-c','0-15','gdb','-q','-nx','-batch','-x',str(P/(name+'.gdb')),str(P/'automatic-retention')]
start=time.monotonic()
with open(P/(name+'.stdout'),'wb') as fo,open(P/(name+'.stderr'),'wb') as fe:
 try:
  r=subprocess.run(cmd,cwd=P,stdout=fo,stderr=fe,timeout=50);status={'exit':r.returncode,'timed_out':False}
 except subprocess.TimeoutExpired:status={'exit':None,'timed_out':True}
status.update(command=cmd,cwd=str(P),seconds=time.monotonic()-start,external_timeout_seconds=50,internal_fixture_alarm_seconds=45,gdb_diagnostic=True)
(P/(name+'-run.json')).write_text(json.dumps(status,indent=2)+'\n');print(status)
