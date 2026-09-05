from pathlib import Path
import subprocess,json,time,hashlib,os,signal
out=Path(__file__).parent;rows=[]
flags=["-DLJ_GC2_TEST_HELPERS","-DLJ_TAB_TEST_HELPERS","-DLJ_FUNC_TEST_HELPERS","-DLJ_TRACE_TEST_HELPERS","-DLJ_ARENA_TEST_HELPERS","-DLUA_USE_ASSERT"]
def run(name,cmd,timeout=50,cwd=None,env=None):
 start=time.monotonic();p=subprocess.Popen(cmd,cwd=cwd,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
 try: stdout,stderr=p.communicate(timeout=timeout);status="complete"
 except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);stdout,stderr=p.communicate();status="timeout"
 row={"name":name,"command":cmd,"cwd":str(cwd) if cwd else None,"status":status,"rc":p.returncode,"elapsed":time.monotonic()-start,"stdout":stdout,"stderr":stderr};rows.append(row);(out/"related-results.json").write_text(json.dumps(rows,indent=2)+"\n");print(name,p.returncode,stdout,stderr,flush=True);return row
for variant in ["strict","normal"]:
 tree=out/variant;env=os.environ.copy();env["LUA_PATH"]=str(tree/"src/?.lua")+";;"
 for mode in ["-joff","-jon"]:run("stock-"+variant+mode,["taskset","-c","0-15",str(tree/"src/luajit"),mode,"test.lua","--quiet"],cwd=tree/"tests/stock/test",env=env)
tree=out/"strict"
for name,source,args in [("rollover",out/"t-wide-stamp.c",["all"]),("coalescing",out/"t-wide-stamp.c",["existing"]),("traverse",out/"traverse-adapter.c",[]),("recovery",tree/"tests/t-gc2-recovery.c",[]),("store-guard",tree/"tests/t-gc2-table-store-guard.c",[])]:
 exe=out/("related-"+name)
 cmd=["gcc","-std=gnu11","-O2","-g","-Wall","-Wextra","-Werror","-mcx16"]+flags+["-I"+str(tree/"src"),"-I"+str(tree/"tests"),str(source),str(tree/"src/libluajit.a"),"-lm","-ldl","-pthread","-o",str(exe)]
 row=run("compile-"+name,cmd)
 if not row["rc"]:run(name,["taskset","-c","0-15",str(exe)]+args)
