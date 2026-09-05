from pathlib import Path
import subprocess, os, signal, json, time
P=Path(__file__).resolve().parent;T=P/'canonical';rows=[]
env=os.environ.copy();env['LJ_TEST_ROOT']=str(T);env['JOBS']='4'
runner='/tmp/lj-dense-huge-tail-20260905-djqhqfe8/tail-normal/src/luajit'
for name in ['m2_arena_huge_tail','m3_gc2_sweep_table_coalescing','m5_x64_tnew_empty_inline','m6_jit_fnew_bump']:
 cmd=['taskset','-c','0-15','stdbuf','-oL','-eL',runner,str(T/'tools/test.lua'),name]
 start=time.monotonic();q=subprocess.Popen(cmd,env=env,cwd=T,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
 try:out,err=q.communicate(timeout=300);status='complete'
 except subprocess.TimeoutExpired:os.killpg(q.pid,signal.SIGKILL);out,err=q.communicate();status='timeout'
 rows.append({'name':name,'command':cmd,'cwd':str(T),'environment':{'LJ_TEST_ROOT':str(T),'JOBS':'4'},'exit':q.returncode,'status':status,'seconds':time.monotonic()-start,'stdout':out,'stderr':err});(P/'canonical-results.json').write_text(json.dumps(rows,indent=2)+'\n');print(name,q.returncode,flush=True)
 if q.returncode:print(out[-3000:],err[-3000:],flush=True)
print('Canonical registrations complete',flush=True)
