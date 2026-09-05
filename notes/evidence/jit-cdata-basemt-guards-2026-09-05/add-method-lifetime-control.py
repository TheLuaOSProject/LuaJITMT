from pathlib import Path
import difflib
r=Path(__file__).resolve().parent;p=r/'t-jit-cdata-basemt-guards.lua';s=p.read_text();(r/'t-jit-cdata-basemt-guards-v2.lua').write_text(s)
s=s.replace('local function onexit(trace)\n','local function make_unretained_call()\n  return function(ctype, ...) return oldcall(ctype, ...) end\nend\nlocal function onexit(trace)\n')
s=s.replace('  jit.opt.start("hotloop=1", "hotexit=1000")\n  exits = {}\n','  jit.opt.start("hotloop=1", "hotexit=1000")\n  local weakmethod\n  if mode == "methodlife" then\n    basemt.__call = make_unretained_call()\n    weakmethod = setmetatable({basemt.__call}, {__mode = "v"})\n  end\n  exits = {}\n')
s=s.replace('  elseif mode == "replace" then\n','  elseif mode == "methodlife" then\n    basemt.__call = replacement_call\n    collectgarbage("collect")\n    if jit.status() then\n      assert(weakmethod[1] ~= nil, "trace must retain the recorded method after replacement")\n    end\n  elseif mode == "replace" then\n')
s=s.replace('{"call", "index", "newindex", "missing", "nonfunction", "resize", "replace"}', '{"call", "index", "newindex", "missing", "nonfunction", "resize", "methodlife", "replace"}')
p.write_text(s);(r/'canonical/tests/t-jit-cdata-basemt-guards.lua').write_text(s)
patch=(r/'pre-mt-cdata-method-guards.patch').read_text();a=(r/'base-normal/tests/suites/m6_jit.lua').read_text();b=(r/'canonical/tests/suites/m6_jit.lua').read_text()
patch+=''.join(difflib.unified_diff(a.splitlines(True),b.splitlines(True),fromfile='a/tests/suites/m6_jit.lua',tofile='b/tests/suites/m6_jit.lua'))
patch+=''.join(difflib.unified_diff([],s.splitlines(True),fromfile='/dev/null',tofile='b/tests/t-jit-cdata-basemt-guards.lua'))
(r/'pre-mt-cdata-guards-review.patch').write_text(patch)
