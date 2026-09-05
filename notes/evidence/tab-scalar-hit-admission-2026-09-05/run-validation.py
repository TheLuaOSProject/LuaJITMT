import pathlib,subprocess,time,json,os,sys
b=pathlib.Path(__file__).parent
records=[]
def run(name,command,cwd,timeout=120,env=None):
 t=time.monotonic()
 effective=os.environ.copy()
 if env:effective.update(env)
 with (b/(name+".stdout")).open("w") as out,(b/(name+".stderr")).open("w") as err:
  try:
   r=subprocess.run(command,cwd=cwd,stdout=out,stderr=err,env=effective,timeout=timeout)
   code=r.returncode
  except subprocess.TimeoutExpired:code="timeout"
 rec={"name":name,"command":command,"cwd":str(cwd),"exit":code,"seconds":time.monotonic()-t,"env":env}
 records.append(rec)
 (b/(sys.argv[1]+"-runs.json")).write_text(json.dumps(records,indent=2)+"\n")
 print(name,code,rec["seconds"],flush=True)
 if code not in (0,-14): print((b/(name+".stderr")).read_text()[-2400:],flush=True)
 return code
flags="-DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS -DLJ_ARENA_TEST_HELPERS -DLUA_USE_ASSERT"
variant=sys.argv[1];tree=b/variant
make=["taskset","-c","0-15","make","-j4","BUILDMODE=static","CCDEBUG=-g","TARGET_STRIP=:"]
if variant!="normal":make += ["XCFLAGS="+flags]
cc="cc";copt=["-g","-O1"]
if variant=="asan":
 cc="clang";copt += ["-fno-omit-frame-pointer","-fsanitize=address"]
 make += ["CC=clang","CCOPT=-O1 -fno-omit-frame-pointer -fsanitize=address","CCOPT_x86=","CCOPT_x64=","TARGET_LDFLAGS=-fsanitize=address","HOST_LDFLAGS=-fsanitize=address"]
if run(variant+"-build",make,tree,180,{"ASAN_OPTIONS":"detect_leaks=0"} if variant=="asan" else None):sys.exit(1)
if variant in ("asan","negative"):
 names=["t-tab-scalar-hit"] if variant=="negative" else ["t-tab-scalar-hit","t-tab-rooted-get-try","t-tab-rooted-len-try","t-tab-rooted-reader"]
 for name in names:
  target=b/(variant+"-"+name)
  cmd=[cc]+copt+flags.split()+["-I"+str(tree/"src"),str(tree/"tests"/(name+".c")),str(tree/"src/libluajit.a"),"-lm","-ldl","-pthread","-o",str(target)]
  if run(variant+"-"+name+"-compile",cmd,tree):sys.exit(1)
  cmd=["taskset","-c","0-15",str(target)]+(["paused-only"] if variant=="negative" else [])
  if run(variant+"-"+name,cmd,tree,50,{"ASAN_OPTIONS":"detect_leaks=1:abort_on_error=1"} if variant=="asan" else None) != (-14 if variant=="negative" else 0):sys.exit(1)
if variant=="normal":
 for mode in ["-joff","-jon"]:
  if run("stock"+mode,["taskset","-c","0-15",str(tree/"src/luajit"),mode,"test.lua"],tree/"tests/stock/test",120):sys.exit(1)
