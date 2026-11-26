from pathlib import Path
import hashlib,json,resource,subprocess,sys,time
p=Path(__file__).resolve().parent
binary,case=sys.argv[1:]
def limits():
 resource.setrlimit(resource.RLIMIT_CORE,(0,0))
 resource.setrlimit(resource.RLIMIT_AS,(1024*1024*1024,1024*1024*1024))
stem=binary+'-'+case
command=['taskset','-c','8',str(p/binary),case]
start=time.monotonic()
r=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=60,preexec_fn=limits)
u=resource.getrusage(resource.RUSAGE_CHILDREN)
record={'case':case,'command':command,'exit':r.returncode,'seconds':time.monotonic()-start,'user_seconds':u.ru_utime,'system_seconds':u.ru_stime,'maxrss_kib':u.ru_maxrss,'binary_sha256':hashlib.sha256((p/binary).read_bytes()).hexdigest()}
(p/(stem+'.log')).write_text(r.stdout)
(p/(stem+'.json')).write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record));print(r.stdout,end='')
