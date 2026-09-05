from pathlib import Path
import subprocess,json,time,hashlib,os,signal
out=Path(__file__).parent;rows=[]
flags=["-DLJ_GC2_TEST_HELPERS","-DLJ_TAB_TEST_HELPERS","-DLJ_FUNC_TEST_HELPERS","-DLJ_TRACE_TEST_HELPERS","-DLJ_ARENA_TEST_HELPERS","-DLUA_USE_ASSERT"]
def run(name,cmd,timeout=60,cwd=None,env=None):
 start=time.monotonic();p=subprocess.Popen(cmd,cwd=cwd,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
 try: stdout,stderr=p.communicate(timeout=timeout);status="complete"
 except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);stdout,stderr=p.communicate();status="timeout"
 row={"name":name,"command":cmd,"cwd":str(cwd) if cwd else None,"status":status,"rc":p.returncode,"elapsed":time.monotonic()-start,"stdout":stdout,"stderr":stderr};rows.append(row);(out/"emitted-results.json").write_text(json.dumps(rows,indent=2)+"\n");print({k:v for k,v in row.items() if k not in ["stdout","stderr","command"]},stdout,stderr,flush=True);return row
for variant in ["strict","control"]:
 tree=out/variant
 for test in ["tnew","fnew"]:
  exe=out/(variant+"-"+test)
  cmd=["gcc","-std=gnu11","-O2","-g","-Wall","-Wextra","-Werror","-mcx16"]+flags+["-I"+str(tree/"src"),"-I"+str(tree/"tests"),str(out/("t-wide-"+test+".c")),str(tree/"src/libluajit.a"),"-lm","-ldl","-pthread","-o",str(exe)]
  row=run("compile-"+variant+"-"+test,cmd);assert row["rc"]==0
  cases=[["1536"],["1537"]]
  if variant=="strict" and test=="tnew":cases += [["1536","1"],["1537","1"]]
  for args in cases:run(variant+"-"+test+"-"+"-".join(args),["taskset","-c","0-15",str(exe)]+args)
for test in ["tnew","fnew"]:run("strict-"+test+"-existing",["taskset","-c","0-15",str(out/("strict-"+test)),"existing"])
