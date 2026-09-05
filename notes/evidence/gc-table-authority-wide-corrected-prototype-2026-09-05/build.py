from pathlib import Path
import subprocess,json,time,hashlib,sys
out=Path(__file__).parent; kind=sys.argv[1]; tree=out/kind
flags="-DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_FUNC_TEST_HELPERS -DLJ_TRACE_TEST_HELPERS -DLJ_ARENA_TEST_HELPERS -DLUA_USE_ASSERT"
cmd=["taskset","-c","0-15","make","-C",str(tree/"src"),"-j4","BUILDMODE=static"]
if kind!="normal": cmd.append("XCFLAGS="+flags)
t=time.monotonic()
with (out/("build-"+kind+".log")).open("w") as f:r=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT)
row={"command":cmd,"rc":r.returncode,"elapsed":time.monotonic()-t,"binaries":{}}
if not r.returncode:
 for name in ["src/luajit","src/libluajit.a","src/lj_vm.o","src/lj_asm.o"]:row["binaries"][name]=hashlib.sha256((tree/name).read_bytes()).hexdigest()
(out/("build-"+kind+".json")).write_text(json.dumps(row,indent=2)+"\n")
print(row,flush=True)
