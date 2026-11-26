from pathlib import Path
import datetime,hashlib,json,resource,subprocess,sys,time
p=Path(__file__).resolve().parent
binary,case=sys.argv[1:]
def limits():
 resource.setrlimit(resource.RLIMIT_CORE,(0,0))
 resource.setrlimit(resource.RLIMIT_AS,(1024*1024*1024,1024*1024*1024))
stem=binary+'-'+case
command=['taskset','-c','8',str(p/binary),case]
utc_start=datetime.datetime.now(datetime.timezone.utc).isoformat();start=time.monotonic()
r=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=30,preexec_fn=limits)
u=resource.getrusage(resource.RUSAGE_CHILDREN)
record={'case':case,'command':command,'exit':r.returncode,'utc_start':utc_start,'utc_end':datetime.datetime.now(datetime.timezone.utc).isoformat(),'seconds':time.monotonic()-start,'user_seconds':u.ru_utime,'system_seconds':u.ru_stime,'maxrss_kib':u.ru_maxrss,'binary_sha256':hashlib.sha256((p/binary).read_bytes()).hexdigest()}
(p/(stem+'.stdout')).write_text(r.stdout);(p/(stem+'.stderr')).write_text(r.stderr)
(p/(stem+'.json')).write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record));print(r.stdout,end='');print(r.stderr,end='')
