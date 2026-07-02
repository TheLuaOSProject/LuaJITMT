local build = require("suite_build")
local runtime = require("suite_runtime")
local cellops = require("suite_cell_ops")
local checks = require("suite_assert")
local utils = require("suite_utils")

local run_luajit = runtime.luajit
local run_stock = runtime.run_stock
local build_and_run_c = build.compile_and_run_c
local run_c_fixture_specs = build.run_c_fixture_specs
local build_and_run_luajit_script = runtime.build_and_run_luajit_script
local shell_quote = utils.shell_quote

local function assert_profile_dumpstack_claim_cleanup(t)
  local awk_api = [=[
BEGIN { infn = 0; claim = 0; protect = 0; drop = 0; rethrow = 0 }
/^LUA_API const char \*luaJIT_profile_dumpstack\(lua_State \*L,/ {
  infn = 1
  next
}
infn && /^}/ { infn = 0; next }
infn && /lj_state_tryclaim/ { claim = NR }
infn && /lj_vm_cpcall/ { protect = NR }
infn && /lj_state_dropclaim/ { drop = NR }
infn && /lj_err_throw/ {
  rethrow = NR
  if (!drop || drop > NR) {
    print "profile dumpstack rethrows before dropping state claim"
    exit 1
  }
}
END {
  if (!claim || !protect || !drop || !rethrow || claim > protect || protect > drop) {
    print "profile dumpstack claim cleanup boundary missing"
    exit 1
  }
}
]=]
  local awk_cp = [=[
BEGIN { infn = 0; reset = 0; dump = 0; len = 0 }
/^static TValue \*profile_dumpstack_cp\(lua_State \*L,/ {
  infn = 1
  next
}
infn && /^}/ { infn = 0; next }
infn && /lj_buf_reset/ { reset = NR }
infn && /lj_debug_dumpstack/ { dump = NR }
infn && /sbuflen/ { len = NR }
END {
  if (!reset || !dump || !len || reset > dump || dump > len) {
    print "profile dumpstack protected body missing"
    exit 1
  }
}
]=]
  utils.capture_command("cd " .. shell_quote(t.root) ..
                        " && awk " .. shell_quote(awk_api) ..
                        " src/lj_profile.c")
  utils.capture_command("cd " .. shell_quote(t.root) ..
                        " && awk " .. shell_quote(awk_cp) ..
                        " src/lj_profile.c")
end

local function assert_api_debug_claim_cleanup(t)
  local function awk(script, path)
    utils.capture_command("cd " .. shell_quote(t.root) ..
                          " && awk " .. shell_quote(script) ..
                          " " .. shell_quote(path))
  end

  awk([=[
BEGIN { infn = 0; cpcall = 0; copy = 0; pub = 0; drop_from = 0; drop_to = 0 }
/^LUA_API void lua_xmove\(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_vm_cpcall/ { cpcall = NR }
infn && /copyTV/ { copy = NR }
infn && /lj_state_stack_pubtv/ { pub = NR }
infn && /lj_state_dropclaim\(&fromclaim\)/ { drop_from = NR }
infn && /lj_state_dropclaim\(&toclaim\)/ { drop_to = NR }
END {
  if (cpcall || !copy || !pub || !drop_from || !drop_to ||
      copy > pub || pub > drop_from || pub > drop_to) {
    print "lua_xmove must directly copy/publish before dropping claims"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
BEGIN { infn = 0; grow = 0; drop = 0; err = 0 }
/^static void api_checkstack1_claimed\(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_state_cpgrowstack/ { grow = NR }
infn && /lj_state_dropresumeclaim\(claim\)/ { drop = NR }
infn && /lj_err_callermsg/ { err = NR }
END {
  if (!grow || !drop || !err || grow > drop || drop > err) {
    print "api_checkstack1_claimed must drop resume claim before stack error"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
function reset(name, resume, grow, pub) {
  fun = name; want_resume = resume; want_grow = grow; want_pub = pub
  started = 0; depth = 0; claim = 0; access = 0; cpgrow = 0; pubrange = 0
  pubtv = 0; drop = 0; claim_kind = ""
}
function finish() {
  if (fun) {
    if (!claim || !access || !drop || claim > access || access > drop ||
	(want_resume && claim_kind != "resume") ||
	(!want_resume && claim_kind != "try") ||
	(want_grow && (!cpgrow || claim > cpgrow || cpgrow > drop)) ||
	(want_pub == "range" && (!pubrange || pubrange > drop)) ||
	(want_pub == "tv" && (!pubtv || pubtv > drop))) {
      print fun " stack owner-claim boundary missing"
      exit 1
    }
    seen++
  }
  fun = ""
}
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN { fun = ""; seen = 0; claim_kind = "" }
/^LUA_API int lua_gettop\(lua_State \*L\)/ { finish(); reset("lua_gettop", 0, 0, "none"); next }
/^LUA_API int lua_checkstack\(lua_State \*L,/ { finish(); reset("lua_checkstack", 1, 1, "none"); next }
/^LUA_API void lua_settop\(lua_State \*L,/ { finish(); reset("lua_settop", 1, 1, "range"); next }
/^LUA_API void lua_remove\(lua_State \*L,/ { finish(); reset("lua_remove", 0, 0, "range"); next }
/^LUA_API void lua_insert\(lua_State \*L,/ { finish(); reset("lua_insert", 0, 0, "range"); next }
/^LUA_API void lua_pushvalue\(lua_State \*L,/ { finish(); reset("lua_pushvalue", 1, 1, "tv"); next }
/^LUA_API void lua_replace\(lua_State \*L,/ { finish(); reset("lua_replace", 0, 0, "none"); next }
/^LUA_API void lua_copy\(lua_State \*L,/ { finish(); reset("lua_copy", 0, 0, "none"); next }
/^LUA_API void lua_pushnil\(lua_State \*L\)/ { finish(); reset("lua_pushnil", 1, 1, "tv"); next }
/^LUA_API void lua_pushnumber\(lua_State \*L,/ { finish(); reset("lua_pushnumber", 1, 1, "tv"); next }
/^LUA_API void lua_pushinteger\(lua_State \*L,/ { finish(); reset("lua_pushinteger", 1, 1, "tv"); next }
/^LUA_API void lua_pushboolean\(lua_State \*L,/ { finish(); reset("lua_pushboolean", 1, 1, "tv"); next }
/^LUA_API int lua_pushthread\(lua_State \*L\)/ { finish(); reset("lua_pushthread", 1, 1, "tv"); next }
/^LUA_API void lua_pushlstring\(lua_State \*L,/ { finish(); reset("lua_pushlstring", 1, 1, "tv"); next }
/^LUA_API void lua_pushstring\(lua_State \*L,/ { finish(); reset("lua_pushstring", 1, 1, "tv"); next }
/^LUA_API const char \*lua_pushvfstring\(lua_State \*L,/ { finish(); reset("lua_pushvfstring", 1, 1, "tv"); next }
/^LUA_API void lua_pushlightuserdata\(lua_State \*L,/ { finish(); reset("lua_pushlightuserdata", 1, 1, "tv"); next }
/^LUA_API void lua_createtable\(lua_State \*L,/ { finish(); reset("lua_createtable", 1, 1, "tv"); next }
/^LUA_API void \*lua_newuserdata\(lua_State \*L,/ { finish(); reset("lua_newuserdata", 1, 1, "tv"); next }
/^LUA_API lua_State \*lua_newthread\(lua_State \*L\)/ { finish(); reset("lua_newthread", 1, 1, "tv"); next }
fun {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/lj_state_resumeclaim/) { claim = NR; claim_kind = "resume" }
  if (/lj_state_tryclaim/) { claim = NR; claim_kind = "try" }
  if (/L->top|L->base|index2adr|copyTV/) {
    if (!access) access = NR
  }
  if (/lj_state_cpgrowstack|api_checkstack1_claimed/) cpgrow = NR
  if (/lj_state_stack_pubrange/) pubrange = NR
  if (/lj_state_stack_pubtv/) pubtv = NR
  if (/lj_state_dropresumeclaim|lj_state_dropclaim/) drop = NR
  if (started && depth == 0) finish()
}
END {
  finish()
  if (seen != 20) {
    print "missing public stack API owner-claim guard coverage"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN {
  infn = 0; started = 0; depth = 0
  pre = 0; pre_drop = 0; fmt = 0; claim = 0; grow = 0
  store = 0; publish = 0; drop = 0
}
/^LUA_API const char \*lua_pushvfstring\(lua_State \*L,/ { infn = 1; next }
infn {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/api_checkclaim\(L, &preclaim\)/) pre = NR
  if (/lj_state_dropclaim\(&preclaim\)/) pre_drop = NR
  if (/lj_strfmt_vstr/) fmt = NR
  if (/lj_state_resumeclaim/) claim = NR
  if (/api_checkstack1_claimed/) grow = NR
  if (/setstrV/) store = NR
  if (/lj_state_stack_pubtv/) publish = NR
  if (/lj_state_dropresumeclaim/) drop = NR
  if (started && depth == 0) infn = 0
}
END {
  if (!pre || !pre_drop || !fmt || !claim || !grow || !store || !publish ||
      !drop || pre > pre_drop || pre_drop > fmt || fmt > claim ||
      claim > grow || grow > store || store > publish || publish > drop) {
    print "lua_pushvfstring must format before claimed publication"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN { infn = 0; started = 0; depth = 0; delegate = 0 }
/^LUA_API const char \*lua_pushfstring\(lua_State \*L,/ { infn = 1; next }
infn {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/lua_pushvfstring/) delegate = NR
  if (/lj_strfmt_pushvf|lj_strfmt_vstr|lj_gc_check/) {
    print "lua_pushfstring must delegate through lua_pushvfstring"
    exit 1
  }
  if (started && depth == 0) infn = 0
}
END {
  if (!delegate) {
    print "lua_pushfstring must call lua_pushvfstring"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN {
  infn = 0; started = 0; depth = 0
  pre = 0; env = 0; pre_drop = 0; alloc = 0; claim = 0; grow = 0
  store = 0; publish = 0; drop = 0
}
/^LUA_API lua_State \*lua_newthread\(lua_State \*L\)/ { infn = 1; next }
infn {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/api_checkclaim\(L, &preclaim\)/) pre = NR
  if (/getcurrenv/) env = NR
  if (/lj_state_dropclaim\(&preclaim\)/) pre_drop = NR
  if (/lj_state_new_withenv/) alloc = NR
  if (/lj_state_resumeclaim/) claim = NR
  if (/api_checkstack1_claimed/) grow = NR
  if (/setthreadV/) store = NR
  if (/lj_state_stack_pubtv/) publish = NR
  if (/lj_state_dropresumeclaim/) drop = NR
  if (started && depth == 0) infn = 0
}
END {
  if (!pre || !env || !pre_drop || !alloc || !claim || !grow || !store ||
      !publish || !drop || pre > env || env > pre_drop || pre_drop > alloc ||
      alloc > claim || claim > grow || grow > store || store > publish ||
      publish > drop) {
    print "lua_newthread must snapshot env and allocate before claimed publication"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN {
  infn = 0; started = 0; depth = 0
  pre = 0; slot1 = 0; env = 0; pre_drop = 0; alloc = 0; claim = 0
  slot2 = 0; zerogrow = 0; grow = 0; copy = 0; store = 0
  publish = 0; drop = 0
}
/^LUA_API void lua_pushcclosure\(lua_State \*L,/ { infn = 1; next }
infn {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/api_checkclaim\(L, &preclaim\)/) pre = NR
  if (/lj_checkapi_slot\(n\)/) slot1 = NR
  if (/getcurrenv/) env = NR
  if (/lj_state_dropclaim\(&preclaim\)/) pre_drop = NR
  if (/lj_func_newC/) alloc = NR
  if (/lj_state_resumeclaim/) claim = NR
  if (/lj_checkapi_slot\(nup\)/) slot2 = NR
  if (/nup == 0/) zerogrow = NR
  if (/api_checkstack1_claimed/) grow = NR
  if (/copyTVrel\(L, &fn->c.upvalue/) copy = NR
  if (/setfuncV/) store = NR
  if (/lj_state_stack_pubtv/) publish = NR
  if (/lj_state_dropresumeclaim/) drop = NR
  if (started && depth == 0) infn = 0
}
END {
  if (!pre || !slot1 || !env || !pre_drop || !alloc || !claim || !slot2 ||
      !zerogrow || !grow || !copy || !store || !publish || !drop ||
      pre > slot1 || slot1 > env || env > pre_drop || pre_drop > alloc ||
      alloc > claim || claim > slot2 || slot2 > zerogrow ||
      zerogrow > grow || grow > copy || copy > store || store > publish ||
      publish > drop) {
    print "lua_pushcclosure must snapshot env, allocate unclaimed, and publish claimed"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN {
  infn = 0; started = 0; depth = 0
  pre = 0; env = 0; pre_drop = 0; alloc = 0; claim = 0; grow = 0
  store = 0; publish = 0; drop = 0
}
/^LUA_API void \*lua_newuserdata\(lua_State \*L,/ { infn = 1; next }
infn {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/api_checkclaim\(L, &preclaim\)/) pre = NR
  if (/getcurrenv/) env = NR
  if (/lj_state_dropclaim\(&preclaim\)/) pre_drop = NR
  if (/lj_udata_new/) alloc = NR
  if (/lj_state_resumeclaim/) claim = NR
  if (/api_checkstack1_claimed/) grow = NR
  if (/setudataV/) store = NR
  if (/lj_state_stack_pubtv/) publish = NR
  if (/lj_state_dropresumeclaim/) drop = NR
  if (started && depth == 0) infn = 0
}
END {
  if (!pre || !env || !pre_drop || !alloc || !claim || !grow || !store ||
      !publish || !drop || pre > env || env > pre_drop || pre_drop > alloc ||
      alloc > claim || claim > grow || grow > store || store > publish ||
      publish > drop) {
    print "lua_newuserdata must snapshot env and allocate before claimed publication"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN {
  infn = 0; started = 0; depth = 0
  pre = 0; pre_drop = 0; alloc = 0; claim = 0; grow = 0
  store = 0; publish = 0; drop = 0
}
/^LUA_API void lua_createtable\(lua_State \*L,/ { infn = 1; next }
infn {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/api_checkclaim\(L, &preclaim\)/) pre = NR
  if (/lj_state_dropclaim\(&preclaim\)/) pre_drop = NR
  if (/lj_tab_new_ah/) alloc = NR
  if (/lj_state_resumeclaim/) claim = NR
  if (/api_checkstack1_claimed/) grow = NR
  if (/settabV/) store = NR
  if (/lj_state_stack_pubtv/) publish = NR
  if (/lj_state_dropresumeclaim/) drop = NR
  if (started && depth == 0) infn = 0
}
END {
  if (!pre || !pre_drop || !alloc || !claim || !grow || !store ||
      !publish || !drop || pre > pre_drop || pre_drop > alloc ||
      alloc > claim || claim > grow || grow > store || store > publish ||
      publish > drop) {
    print "lua_createtable must allocate before claimed stack publication"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN {
  infn = 0; started = 0; depth = 0
  pre = 0; pre_drop = 0; intern = 0; claim = 0; grow = 0
  store = 0; publish = 0; drop = 0
}
/^LUA_API void lua_pushlightuserdata\(lua_State \*L,/ { infn = 1; next }
infn {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/api_checkclaim\(L, &preclaim\)/) pre = NR
  if (/lj_state_dropclaim\(&preclaim\)/) pre_drop = NR
  if (/lj_lightud_intern/) intern = NR
  if (/lj_state_resumeclaim/) claim = NR
  if (/api_checkstack1_claimed/) grow = NR
  if (/setrawlightudV/) store = NR
  if (/lj_state_stack_pubtv/) publish = NR
  if (/lj_state_dropresumeclaim/) drop = NR
  if (started && depth == 0) infn = 0
}
END {
  if (!pre || !pre_drop || !intern || !claim || !grow || !store ||
      !publish || !drop || pre > pre_drop || pre_drop > intern ||
      intern > claim || claim > grow || grow > store || store > publish ||
      publish > drop) {
    print "lua_pushlightuserdata must intern before claimed stack publication"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
function reset(name, ctor) {
  fun = name; want_ctor = ctor
  started = 0; depth = 0; pre = 0; pre_drop = 0; ctorline = 0
  claim = 0; grow = 0; publish = 0; drop = 0
}
function finish() {
  if (fun) {
    if (!pre || !pre_drop || !ctorline || !claim || !grow || !publish ||
	!drop || pre > pre_drop || pre_drop > ctorline ||
	ctorline > claim || claim > grow || grow > publish || publish > drop) {
      print fun " must allocate string before claimed stack publication"
      exit 1
    }
    seen++
  }
  fun = ""
}
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN { fun = ""; seen = 0 }
/^LUA_API void lua_pushlstring\(lua_State \*L,/ { finish(); reset("lua_pushlstring", "lj_str_new\\("); next }
/^LUA_API void lua_pushstring\(lua_State \*L,/ { finish(); reset("lua_pushstring", "lj_str_newz"); next }
fun {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/api_checkclaim\(L, &preclaim\)/) pre = NR
  if (/lj_state_dropclaim\(&preclaim\)/) pre_drop = NR
  if ($0 ~ want_ctor) ctorline = NR
  if (/lj_state_resumeclaim/) claim = NR
  if (/api_checkstack1_claimed/) grow = NR
  if (/lj_state_stack_pubtv/) publish = NR
  if (/lj_state_dropresumeclaim/) drop = NR
  if (started && depth == 0) finish()
}
END {
  finish()
  if (seen != 2) {
    print "missing public string push allocation guard coverage"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN {
  infn = 0; started = 0; depth = 0
  stackcopy = 0; publish = 0; registry = 0; upvalue = 0
}
/^static void copy_slot\(lua_State \*L,/ { infn = 1; next }
infn {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/lj_registry_store_rel/) registry = NR
  if (/index2adr_cupvalue_store_rel/) upvalue = NR
  if (/copyTV\(L, o, f\)/) stackcopy = NR
  if (/lj_state_stack_pubtv\(L, L, o\)/) publish = NR
  if (started && depth == 0) infn = 0
}
END {
  if (!registry || !upvalue || !stackcopy || !publish || stackcopy > publish) {
    print "copy_slot must preserve registry/upvalue and publish stack stores"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
BEGIN { infn = 0; claim = 0; read = 0; drop = 0 }
/^LUA_API int lua_isyieldable\(lua_State \*L\)/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_state_tryclaim/ { claim = NR }
infn && /cframe_canyield\(L->cframe\)/ { read = NR }
infn && /lj_state_dropclaim\(&claim\)/ { drop = NR }
END {
  if (!claim || !read || !drop || claim > read || read > drop) {
    print "lua_isyieldable owner-claim boundary missing"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
function reset(name) {
  fun = name; started = 0; depth = 0; claim = 0; access = 0; drop = 0
}
function finish() {
  if (fun) {
    if (!claim || !access || !drop || claim > access || access > drop) {
      print fun " getter owner-claim boundary missing"
      exit 1
    }
    seen++
  }
  fun = ""
}
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN { fun = ""; seen = 0 }
/^LUA_API int lua_type\(lua_State \*L,/ { finish(); reset("lua_type"); next }
/^LUA_API int lua_iscfunction\(lua_State \*L,/ { finish(); reset("lua_iscfunction"); next }
/^LUA_API int lua_isnumber\(lua_State \*L,/ { finish(); reset("lua_isnumber"); next }
/^LUA_API int lua_isstring\(lua_State \*L,/ { finish(); reset("lua_isstring"); next }
/^LUA_API int lua_isuserdata\(lua_State \*L,/ { finish(); reset("lua_isuserdata"); next }
/^LUA_API int lua_rawequal\(lua_State \*L,/ { finish(); reset("lua_rawequal"); next }
/^LUA_API lua_Number lua_tonumber\(lua_State \*L,/ { finish(); reset("lua_tonumber"); next }
/^LUA_API lua_Number lua_tonumberx\(lua_State \*L,/ { finish(); reset("lua_tonumberx"); next }
/^LUA_API lua_Integer lua_tointeger\(lua_State \*L,/ { finish(); reset("lua_tointeger"); next }
/^LUA_API lua_Integer lua_tointegerx\(lua_State \*L,/ { finish(); reset("lua_tointegerx"); next }
/^LUA_API int lua_toboolean\(lua_State \*L,/ { finish(); reset("lua_toboolean"); next }
/^LUA_API lua_CFunction lua_tocfunction\(lua_State \*L,/ { finish(); reset("lua_tocfunction"); next }
/^LUA_API void \*lua_touserdata\(lua_State \*L,/ { finish(); reset("lua_touserdata"); next }
/^LUA_API lua_State \*lua_tothread\(lua_State \*L,/ { finish(); reset("lua_tothread"); next }
/^LUA_API const void \*lua_topointer\(lua_State \*L,/ { finish(); reset("lua_topointer"); next }
fun {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/api_checkclaim/) claim = NR
  if (/index2adr_read/ && !access) access = NR
  if (/lj_state_dropclaim\(&claim\)/) drop = NR
  if (started && depth == 0) finish()
}
END {
  finish()
  if (seen != 15) {
    print "missing public getter API owner-claim guard coverage"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN {
  infn = 0; started = 0; depth = 0
  upstore = 0; stackstore = 0; publish = 0
}
/^static void index2adr_storestr\(lua_State \*L,/ { infn = 1; next }
infn {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/index2adr_cupvalue_store_rel/) upstore = NR
  if (/setstrV\(L, o, s\)/) stackstore = NR
  if (/lj_state_stack_pubtv/) publish = NR
  if (started && depth == 0) infn = 0
}
END {
  if (!upstore || !stackstore || !publish || stackstore > publish) {
    print "index2adr_storestr must publish converted stack strings"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN {
  infn = 0; started = 0; depth = 0
  claim1 = 0; access1 = 0; drop1 = 0; errstate = 0; format = 0
  claim2 = 0; access2 = 0; store = 0; drop2 = 0
}
/^static GCstr \*api_tolstring_claimed\(lua_State \*L,/ { infn = 1; next }
infn {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/api_checkclaim\(L, &claim\)/) {
    if (!claim1) claim1 = NR
    else if (format && !claim2) claim2 = NR
  }
  if (/index2adr_read/) {
    if (!access1) access1 = NR
    else if (format && !access2) access2 = NR
  }
  if (/lj_state_dropclaim\(&claim\)/) {
    if (!format && !drop1) drop1 = NR
    else if (claim2 && !drop2) drop2 = NR
  }
  if (/api_errstate/) errstate = NR
  if (/lj_strfmt_number/) format = NR
  if (/index2adr_storestr/) store = NR
  if (started && depth == 0) infn = 0
}
END {
  if (!claim1 || !access1 || !drop1 || !errstate || !format || !claim2 ||
      !access2 || !store || !drop2 || claim1 > access1 || access1 > drop1 ||
      drop1 > errstate || errstate > format || format > claim2 ||
      claim2 > access2 || access2 > store || store > drop2) {
    print "api_tolstring_claimed must allocate outside target claim and republish"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
function reset(name, want_error) {
  fun = name; want_err = want_error
  started = 0; depth = 0; helper = 0; err = 0; raw = 0
}
function finish() {
  if (fun) {
    if (!helper || raw || (want_err && (!err || helper > err))) {
      print fun " string conversion helper boundary missing"
      exit 1
    }
    seen++
  }
  fun = ""
}
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN { fun = ""; seen = 0 }
/^LUA_API const char \*lua_tolstring\(lua_State \*L,/ { finish(); reset("lua_tolstring", 0); next }
/^LUALIB_API const char \*luaL_checklstring\(lua_State \*L,/ { finish(); reset("luaL_checklstring", 1); next }
/^LUALIB_API const char \*luaL_optlstring\(lua_State \*L,/ { finish(); reset("luaL_optlstring", 1); next }
fun {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/api_tolstring_claimed/) helper = NR
  if (/lj_err_argt/) err = NR
  if (/index2adr_read|lj_strfmt_number/) raw = NR
  if (started && depth == 0) finish()
}
END {
  finish()
  if (seen != 3) {
    print "missing public string conversion helper guard coverage"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
BEGIN { infn = 0; claim = 0; access = 0; drop = 0; helper = 0 }
/^LUA_API size_t lua_objlen\(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /api_checkclaim\(L, &claim\)/ { claim = NR }
infn && /index2adr_read/ { access = NR }
infn && /lj_state_dropclaim\(&claim\)/ && !drop { drop = NR }
infn && /api_tolstring_claimed/ { helper = NR }
END {
  if (!claim || !access || !drop || !helper ||
      claim > access || access > drop || drop > helper) {
    print "lua_objlen must drop state claim before numeric string allocation"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
function reset(name) {
  fun = name; started = 0; depth = 0; claim = 0; access = 0; drop = 0; err = 0
}
function finish() {
  if (fun) {
    if (!claim || !access || !drop || !err ||
	claim > access || access > drop || drop > err) {
      print fun " aux owner-claim/error boundary missing"
      exit 1
    }
    seen++
  }
  fun = ""
}
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN { fun = ""; seen = 0 }
/^LUALIB_API void luaL_checkany\(lua_State \*L,/ { finish(); reset("luaL_checkany"); next }
/^LUALIB_API lua_Number luaL_checknumber\(lua_State \*L,/ { finish(); reset("luaL_checknumber"); next }
/^LUALIB_API lua_Number luaL_optnumber\(lua_State \*L,/ { finish(); reset("luaL_optnumber"); next }
/^LUALIB_API lua_Integer luaL_checkinteger\(lua_State \*L,/ { finish(); reset("luaL_checkinteger"); next }
/^LUALIB_API lua_Integer luaL_optinteger\(lua_State \*L,/ { finish(); reset("luaL_optinteger"); next }
fun {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/api_checkclaim/) claim = NR
  if (/index2adr_read/ && !access) access = NR
  if (/lj_state_dropclaim\(&claim\)/) drop = NR
  if (/lj_err_arg/) err = NR
  if (started && depth == 0) finish()
}
END {
  finish()
  if (seen != 5) {
    print "missing public aux API owner-claim guard coverage"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
function reset(name, resume, grow) {
  fun = name; want_resume = resume; want_grow = grow
  started = 0; depth = 0; claim = 0; access = 0; growcall = 0
  pub = 0; drop = 0; claim_kind = ""
}
function finish() {
  if (fun) {
    if (!claim || !access || !pub || !drop ||
	claim > access || access > pub || pub > drop ||
	(want_resume && claim_kind != "resume") ||
	(!want_resume && claim_kind != "try") ||
	(want_grow && (!growcall || claim > growcall || growcall > drop))) {
      print fun " raw object getter owner-claim boundary missing"
      exit 1
    }
    seen++
  }
  fun = ""
}
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN { fun = ""; seen = 0; claim_kind = "" }
/^LUA_API void lua_rawget\(lua_State \*L,/ { finish(); reset("lua_rawget", 0, 0); next }
/^LUA_API void lua_rawgeti\(lua_State \*L,/ { finish(); reset("lua_rawgeti", 1, 1); next }
/^LUA_API int lua_getmetatable\(lua_State \*L,/ { finish(); reset("lua_getmetatable", 1, 1); next }
/^LUA_API void lua_getfenv\(lua_State \*L,/ { finish(); reset("lua_getfenv", 1, 1); next }
/^LUA_API int lua_next\(lua_State \*L,/ { finish(); reset("lua_next", 1, 1); next }
fun {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (!claim && /lj_state_resumeclaim/) { claim = NR; claim_kind = "resume" }
  if (!claim && /api_checkclaim/) { claim = NR; claim_kind = "try" }
  if (/index2adr_read|index2adr_check_read/) {
    if (!access) access = NR
  }
  if (/api_checkstack1_claimed/) growcall = NR
  if (/lj_state_stack_pubtv/) pub = NR
  if (/lj_state_dropresumeclaim\(&claim\)|lj_state_dropclaim\(&claim\)/) drop = NR
  if (started && depth == 0) finish()
}
END {
  finish()
  if (seen != 5) {
    print "missing public raw object getter owner-claim guard coverage"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
function reset(name, resume, grow, pub) {
  fun = name; want_resume = resume; want_grow = grow; want_pub = pub
  started = 0; depth = 0; claim = 0; access = 0; growcall = 0
  pubtv = 0; drop = 0; claim_kind = ""
}
function finish() {
  if (fun) {
    if (!claim || !access || !drop || claim > access || access > drop ||
	(want_resume && claim_kind != "resume") ||
	(!want_resume && claim_kind != "try") ||
	(want_grow && (!growcall || access > growcall || growcall > drop)) ||
	(want_pub && (!pubtv || growcall > pubtv || pubtv > drop))) {
      print fun " upvalue owner-claim boundary missing"
      exit 1
    }
    seen++
  }
  fun = ""
}
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN { fun = ""; seen = 0; claim_kind = "" }
/^LUA_API const char \*lua_getupvalue\(lua_State \*L,/ { finish(); reset("lua_getupvalue", 1, 1, 1); next }
/^LUA_API void \*lua_upvalueid\(lua_State \*L,/ { finish(); reset("lua_upvalueid", 0, 0, 0); next }
fun {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (!claim && /lj_state_resumeclaim/) { claim = NR; claim_kind = "resume" }
  if (!claim && /api_checkclaim/) { claim = NR; claim_kind = "try" }
  if (/index2adr_read/) {
    if (!access) access = NR
  }
  if (/api_checkstack1_claimed/) growcall = NR
  if (/lj_state_stack_pubtv/) pubtv = NR
  if (/lj_state_dropresumeclaim\(&claim\)|lj_state_dropclaim\(&claim\)/) drop = NR
  if (started && depth == 0) finish()
}
END {
  finish()
  if (seen != 2) {
    print "missing public upvalue API owner-claim guard coverage"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
BEGIN {
  infn = 0; claim1 = 0; access1 = 0; drop1 = 0; key = 0
  claim2 = 0; access2 = 0; drop2 = 0
}
/^LUALIB_API void \*luaL_testudata\(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /api_checkclaim/ {
  if (!claim1) claim1 = NR
  else if (!claim2) claim2 = NR
}
infn && /index2adr_read/ {
  if (!access1) access1 = NR
  else if (!access2) access2 = NR
}
infn && /lj_state_dropclaim\(&claim\)/ {
  if (!drop1) drop1 = NR
  else if (!drop2) drop2 = NR
}
infn && /lj_str_newz/ { key = NR }
END {
  if (!claim1 || !access1 || !drop1 || !key ||
      !claim2 || !access2 || !drop2 ||
      !(claim1 < access1 && access1 < drop1 && drop1 < key &&
	key < claim2 && claim2 < access2 && access2 < drop2)) {
    print "luaL_testudata two-claim owner boundary missing"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
BEGIN { infn = 0; test = 0; err = 0 }
/^LUALIB_API void \*luaL_checkudata\(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /luaL_testudata/ { test = NR }
infn && /lj_err_argtype/ { err = NR }
END {
  if (!test || !err || test > err) {
    print "luaL_checkudata must delegate to luaL_testudata before errors"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
BEGIN {
  infn = 0; preclaim = 0; predrop = 0; key = 0; setslot = 0
  alloc = 0; cas = 0; regpub = 0; resume = 0; check = 0
  stackpub = 0; drop = 0
}
/^LUALIB_API int luaL_newmetatable\(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /api_checkclaim\(L, &preclaim\)/ { preclaim = NR }
infn && /lj_state_dropclaim\(&preclaim\)/ { predrop = NR }
infn && /lj_str_newz\(errL,/ { key = NR }
infn && /lj_tab_setstr\(errL,/ { setslot = NR }
infn && /lj_tab_new\(errL,/ { alloc = NR }
infn && /lj_tab_trysetnil_cas_keyed\(errL,/ { cas = NR }
infn && /lj_gc_pubtab\(errL,/ { regpub = NR }
infn && /lj_state_resumeclaim/ { resume = NR }
infn && /api_checkstack1_claimed/ { check = NR }
infn && /lj_state_stack_pubtv/ { stackpub = NR }
infn && /lj_state_dropresumeclaim/ { drop = NR }
END {
  if (!preclaim || !predrop || !key || !setslot || !alloc || !cas ||
      !regpub || !resume || !check || !stackpub || !drop ||
      preclaim > predrop || predrop > key || key > setslot ||
      setslot > alloc || alloc > cas || cas > regpub ||
      regpub > resume || resume > check || check > stackpub ||
      stackpub > drop) {
    print "luaL_newmetatable owner-claim boundary missing"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
BEGIN { infn = 0; rel = 0; drop = 0; pub = 0 }
/^LUA_API int lua_setfenv\(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_state_env_rel/ { rel = NR }
infn && rel && /lj_state_dropclaim\(&thclaim\)/ && !drop { drop = NR }
infn && /lj_gc_pubobjobj\(L, obj2gco\(L1\), t\)/ { pub = NR }
END {
  if (!rel || !drop || !pub || !(rel < drop && drop < pub)) {
    print "lua_setfenv thread-env publication must happen after claim drop"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
BEGIN { infn = 0; errstr = 0; inc = 0 }
/^static TValue \*api_resume_invalid_cp\(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_err_str/ { errstr = NR }
infn && /incr_top/ { inc = NR }
END {
  if (!errstr || !inc || errstr > inc) {
    print "api_resume_invalid_cp no longer builds invalid-resume error"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
BEGIN { infn = 0; claim = 0; cpcall = 0; cleanup = 0; gccheck = 0; drop = 0 }
/^LUA_API int lua_loadx\(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_state_resumeclaim/ { claim = NR }
infn && /lj_vm_cpcall/ { cpcall = NR }
infn && /lj_lex_cleanup/ { cleanup = NR }
infn && /lj_gc_check/ { gccheck = NR }
infn && /lj_state_dropresumeclaim/ { drop = NR }
END {
  if (!claim || !cpcall || !cleanup || !gccheck || !drop ||
      claim > cpcall || cpcall > cleanup || cleanup > gccheck ||
      gccheck > drop) {
    print "lua_loadx must hold a resume claim across parser cpcall cleanup"
    exit 1
  }
}
]=], "src/lj_load.c")

  awk([=[
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN { infn = 0; depth = 0; pop = 0; remove = 0; rawtop = 0 }
/^LUALIB_API int luaL_loadfilex\(lua_State \*L,/ { infn = 1; depth = 1; next }
infn {
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/load_pop1_claimed\(L\)/) pop++
  if (/load_remove_chunkname_claimed\(L\)/) remove = NR
  if (/L->top/) rawtop = NR
  if (depth == 0) infn = 0
}
END {
  if (rawtop || pop < 2 || !remove) {
    print "luaL_loadfilex must use internal claimed stack helpers"
    exit 1
  }
}
]=], "src/lj_load.c")

  awk([=[
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN {
  infn = 0; depth = 0
  claim = 0; check = 0; read = 0; dump = 0; drop = 0
}
/^LUA_API int lua_dump\(lua_State \*L,/ { infn = 1; depth = 1; next }
infn {
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/lj_state_resumeclaim/) claim = NR
  if (/lj_checkapi/) check = NR
  if (/L->top-1/) read = NR
  if (/lj_bcwrite/) dump = NR
  if (/lj_state_dropresumeclaim/) drop = NR
  if (depth == 0) infn = 0
}
END {
  if (!claim || !check || !read || !dump || !drop ||
      claim > check || check > read || read > dump || dump > drop) {
    print "lua_dump owner-claim boundary missing"
    exit 1
  }
}
]=], "src/lj_load.c")

  awk([=[
function reset(name) {
  fun = name; started = 0; depth = 0
  boolpush = 0; nilpush = 0; strpush = 0; intpush = 0; rawtop = 0
}
function finish() {
  if (fun) {
    if (rawtop || !boolpush || !nilpush || !strpush || !intpush) {
      print fun " must use owner-claiming public stack pushes"
      exit 1
    }
    seen++
  }
  fun = ""
}
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN { fun = ""; seen = 0 }
/^LUALIB_API int luaL_fileresult\(lua_State \*L,/ {
  finish(); reset("luaL_fileresult"); next
}
/^LUALIB_API int luaL_execresult\(lua_State \*L,/ {
  finish(); reset("luaL_execresult"); next
}
fun {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/lua_pushboolean/) boolpush = NR
  if (/lua_pushnil/) nilpush = NR
  if (/lua_pushfstring|lua_pushliteral/) strpush = NR
  if (/lua_pushinteger/) intpush = NR
  if (/L->top/) rawtop = NR
  if (started && depth == 0) finish()
}
END {
  finish()
  if (seen != 2) {
    print "missing aux result owner-claim guard coverage"
    exit 1
  }
}
]=], "src/lib_aux.c")

  awk([=[
BEGIN {
  infn = 0; claim = 0; loc = 0; drop = 0; pushf = 0; pushlit = 0
}
/^LUALIB_API void luaL_where\(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_state_resumeclaim/ { claim = NR }
infn && /lj_debug_getloc_claimed/ { loc = NR }
infn && /lj_state_dropresumeclaim/ { drop = NR }
infn && /lua_pushfstring/ { pushf = NR }
infn && /lua_pushliteral/ { pushlit = NR }
END {
  if (!claim || !loc || !drop || !pushf || !pushlit ||
      claim > loc || loc > drop || drop > pushf || drop > pushlit) {
    print "luaL_where owner-claim boundary missing"
    exit 1
  }
}
]=], "src/lj_err.c")

  awk([=[
BEGIN {
  infn = 0; frame = 0; line = 0; shortname = 0
}
/^int lj_debug_getloc_claimed\(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_debug_frame/ { frame = NR }
infn && /debug_frameline/ { line = NR }
infn && /lj_debug_shortname/ { shortname = NR }
END {
  if (!frame || !line || !shortname || frame > line || line > shortname) {
    print "lj_debug_getloc_claimed helper missing"
    exit 1
  }
}
]=], "src/lj_debug.c")

  awk([=[
BEGIN {
  infn = 0; claim = 0; pcall = 0; call = 0; drop_err = 0; drop_call = 0; rethrow = 0
}
/^LUA_API void lua_call\(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_state_resumeclaim/ { claim = NR }
infn && /lj_vm_pcall/ { pcall = NR }
infn && /lj_vm_call/ { call = NR }
infn && /lj_state_dropresumeclaim/ {
  if (call) drop_call = NR
  else drop_err = NR
}
infn && /lj_err_throw/ {
  rethrow = NR
  if (!drop_err || drop_err > NR) {
    print "lua_call ownerless error rethrows before dropping resume claim"
    exit 1
  }
}
END {
  if (!claim || !pcall || !call || !drop_err || !drop_call || !rethrow ||
      claim > pcall || claim > call || pcall > drop_err ||
      drop_err > rethrow || call > drop_call) {
    print "lua_call resume-claim VM entry boundary missing"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
BEGIN { infn = 0; claim = 0; pcall = 0; drop = 0 }
/^LUA_API int lua_pcall\(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_state_resumeclaim/ { claim = NR }
infn && /lj_vm_pcall/ { pcall = NR }
infn && /lj_state_dropresumeclaim/ { drop = NR }
END {
  if (!claim || !pcall || !drop || claim > pcall || pcall > drop) {
    print "lua_pcall must hold a resume claim across VM pcall"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
BEGIN { infn = 0; claim = 0; cpcall = 0; drop = 0 }
/^LUA_API int lua_cpcall\(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_state_resumeclaim/ { claim = NR }
infn && /lj_vm_cpcall/ { cpcall = NR }
infn && /lj_state_dropresumeclaim/ { drop = NR }
END {
  if (!claim || !cpcall || !drop || claim > cpcall || cpcall > drop) {
    print "lua_cpcall must hold a resume claim across VM cpcall"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
BEGIN { infn = 0; pcall = 0; drop = 0; rethrow = 0; rawcall = 0 }
/^static void api_vm_call_claimed\(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_vm_pcall/ { pcall = NR }
infn && /lj_vm_call/ { rawcall = NR }
infn && /lj_state_dropresumeclaim/ { drop = NR }
infn && /lj_err_throw/ {
  rethrow = NR
  if (!drop || drop > NR) {
    print "api_vm_call_claimed rethrows before dropping resume claim"
    exit 1
  }
}
END {
  if (!pcall || !rawcall || !drop || !rethrow || pcall > drop ||
      drop > rethrow) {
    print "api_vm_call_claimed cleanup boundary missing"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
function reset(name) {
  fun = name; started = 0; depth = 0
  claim = 0; slot = 0; table = 0; top = 0; store = 0; barrier = 0
  pop = 0; drop = 0
}
function finish() {
  if (fun) {
    if (!claim || !slot || !table || !top || !store || !barrier ||
	!pop || !drop || claim > slot || slot > table || table > top ||
	top > store || store > barrier || barrier > pop || pop > drop) {
      print fun " raw setter owner-claim boundary missing"
      exit 1
    }
    seen++
  }
  fun = ""
}
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN { fun = ""; seen = 0 }
/^LUA_API void lua_rawset\(lua_State \*L,/ { finish(); reset("lua_rawset"); next }
/^LUA_API void lua_rawseti\(lua_State \*L,/ { finish(); reset("lua_rawseti"); next }
fun {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/lj_state_resumeclaim/) claim = NR
  if (/lj_checkapi_slot/) slot = NR
  if (/index2adr_read/) table = NR
  if (/L->top-/) top = NR
  if (/lj_tab_trystoretv_cas_keyed/) store = NR
  if (/lj_gc_pubtab/) barrier = NR
  if (/L->top =/) pop = NR
  if (/lj_state_dropresumeclaim/) drop = NR
  if (started && depth == 0) finish()
}
END {
  finish()
  if (seen != 2) {
    print "missing raw setter owner-claim guard coverage"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
function reset(name) {
  fun = name; started = 0; depth = 0
  claim = 0; access = 0; top = 0; mutate = 0; pop = 0; drop = 0
}
function finish() {
  if (fun) {
    if (!claim || !access || !top || !mutate || !pop || !drop ||
	claim > access || access > top || top > mutate ||
	mutate > pop || pop > drop) {
      print fun " object setter owner-claim boundary missing"
      exit 1
    }
    seen++
  }
  fun = ""
}
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN { fun = ""; seen = 0 }
/^LUA_API int lua_setmetatable\(lua_State \*L,/ {
  finish(); reset("lua_setmetatable"); next
}
/^LUA_API int lua_setfenv\(lua_State \*L,/ {
  finish(); reset("lua_setfenv"); next
}
fun {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/lj_state_resumeclaim/) claim = NR
  if (/index2adr_check_read/) access = NR
  if (/L->top-1/) top = NR
  if (/lj_tab_metatable_rel|lj_udata_metatable_rel|lj_basemt_|lj_func_env_rel|lj_udata_env_rel|lj_state_env_rel/)
    mutate = NR
  if (/L->top--/) pop = NR
  if (/lj_state_dropresumeclaim/) drop = NR
  if (started && depth == 0) finish()
}
END {
  finish()
  if (seen != 2) {
    print "missing object setter owner-claim guard coverage"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
BEGIN {
  infn = 0; claim = 0; access = 0; top = 0; uvname = 0
  pop = 0; copy = 0; drop = 0
}
/^LUA_API const char \*lua_setupvalue\(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_state_resumeclaim/ { claim = NR }
infn && /index2adr_read/ { access = NR }
infn && /L->top-1/ { top = NR }
infn && /lj_debug_uvnamev/ { uvname = NR }
infn && /L->top--/ { pop = NR }
infn && /copyTVrel/ { copy = NR }
infn && /lj_state_dropresumeclaim/ { drop = NR }
END {
  if (!claim || !access || !top || !uvname || !pop || !copy || !drop ||
      claim > access || access > top || top > uvname || uvname > pop ||
      pop > copy || copy > drop) {
    print "lua_setupvalue owner-claim boundary missing"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
BEGIN {
  infn = 0; claim = 0; read1 = 0; read2 = 0
  flush = 0; store = 0; pub = 0; drop = 0
}
/^LUA_API void lua_upvaluejoin\(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_state_resumeclaim/ { claim = NR }
infn && /index2adr_read/ {
  if (!read1) read1 = NR
  else if (!read2) read2 = NR
}
infn && /api_trace_flush_mutation_claimed/ { flush = NR }
infn && /setgcrefrel/ { store = NR }
infn && /lj_gc_pubobjobj/ { pub = NR }
infn && /lj_state_dropresumeclaim/ { drop = NR }
END {
  if (!claim || !read1 || !read2 || !flush || !store || !pub || !drop ||
      claim > read1 || read1 > read2 || read2 > flush || flush > store ||
      store > pub || pub > drop) {
    print "lua_upvaluejoin owner-claim boundary missing"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
function reset(name) {
  fun = name; started = 0; depth = 0; claim = 0; call = 0; drop = 0
}
function finish() {
  if (fun) {
    if (!claim || !call || !drop || claim > call || call > drop) {
      print fun " must claim state around public metamethod VM entry"
      exit 1
    }
    seen++
  }
  fun = ""
}
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN { fun = ""; seen = 0 }
/^LUA_API int lua_equal\(lua_State \*L,/ { finish(); reset("lua_equal"); next }
/^LUA_API int lua_lessthan\(lua_State \*L,/ { finish(); reset("lua_lessthan"); next }
/^LUA_API void lua_concat\(lua_State \*L,/ { finish(); reset("lua_concat"); next }
/^LUA_API void lua_gettable\(lua_State \*L,/ { finish(); reset("lua_gettable"); next }
/^LUA_API void lua_getfield\(lua_State \*L,/ { finish(); reset("lua_getfield"); next }
/^LUA_API void lua_settable\(lua_State \*L,/ { finish(); reset("lua_settable"); next }
/^LUA_API void lua_setfield\(lua_State \*L,/ { finish(); reset("lua_setfield"); next }
fun {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/lj_state_resumeclaim/) claim = NR
  if (/api_vm_call_claimed/) call = NR
  if (/lj_state_dropresumeclaim/) drop = NR
  if (started && depth == 0) finish()
}
END {
  finish()
  if (seen != 7) {
    print "missing public metamethod VM entry guard coverage"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN {
  infn = 0; started = 0; depth = 0
  pre = 0; pre_drop = 0; key = 0; claim = 0; helper = 0; drop = 0
}
/^LUALIB_API int luaL_getmetafield\(lua_State \*L,/ { infn = 1; next }
infn {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/api_checkclaim\(L, &preclaim\)/) pre = NR
  if (/lj_state_dropclaim\(&preclaim\)/) pre_drop = NR
  if (/lj_str_newz\(api_errstate\(L\), field\)/) key = NR
  if (/lj_state_resumeclaim/) claim = NR
  if (/api_getmetafield_key_claimed/) helper = NR
  if (/lj_state_dropresumeclaim\(&claim\)/) drop = NR
  if (started && depth == 0) infn = 0
}
END {
  if (!pre || !pre_drop || !key || !claim || !helper || !drop ||
      pre > pre_drop || pre_drop > key || key > claim || claim > helper ||
      helper > drop) {
    print "luaL_getmetafield must pre-intern key and use claimed helper"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN {
  infn = 0; started = 0; depth = 0
  access = 0; lookup = 0; grow = 0; publish = 0
}
/^static int api_getmetafield_key_claimed\(lua_State \*L,/ { infn = 1; next }
infn {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/index2adr_read/) access = NR
  if (/lj_tab_getstr/) lookup = NR
  if (/api_checkstack1_claimed/) grow = NR
  if (/lj_state_stack_pubtv/) publish = NR
  if (started && depth == 0) infn = 0
}
END {
  if (!access || !lookup || !grow || !publish ||
      access > lookup || lookup > grow || grow > publish) {
    print "api_getmetafield_key_claimed must read, lookup, grow, then publish"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
function count_char(text, ch,    n, i) {
  n = 0
  for (i = 1; i <= length(text); i++)
    if (substr(text, i, 1) == ch) n++
  return n
}
BEGIN {
  infn = 0; started = 0; depth = 0
  pre = 0; pre_drop = 0; key = 0; claim = 0; helper = 0; call = 0; drop = 0
}
/^LUALIB_API int luaL_callmeta\(lua_State \*L,/ { infn = 1; next }
infn {
  if (index($0, "{")) started = 1
  depth += count_char($0, "{")
  depth -= count_char($0, "}")
  if (/api_checkclaim\(L, &preclaim\)/) pre = NR
  if (/lj_state_dropclaim\(&preclaim\)/) pre_drop = NR
  if (/lj_str_newz\(api_errstate\(L\), field\)/) key = NR
  if (/lj_state_resumeclaim/) claim = NR
  if (/api_getmetafield_key_claimed/) helper = NR
  if (/api_vm_call_claimed/) call = NR
  if (/lj_state_dropresumeclaim\(&claim\)/ && !drop) drop = NR
  if (started && depth == 0) infn = 0
}
END {
  if (!pre || !pre_drop || !key || !claim || !helper || !call || !drop ||
      pre > pre_drop || pre_drop > key || key > claim || claim > helper ||
      helper > call || call > drop) {
    print "luaL_callmeta must pre-intern key and hold claim across VM call"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
BEGIN { infn = 0; cpcall = 0; drop = 0; rethrow = 0 }
/^LUA_API int lua_resume\(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /api_resume_invalid_cp/ && /lj_vm_cpcall/ { cpcall = NR }
infn && /lj_state_dropresumeclaim\(&claim\)/ { drop = NR }
infn && /lj_err_throw/ {
  rethrow = NR
  if (!drop || drop > NR) {
    print "lua_resume invalid-resume error rethrows before dropping claim"
    exit 1
  }
}
END {
  if (!cpcall || !drop || !rethrow || cpcall > drop) {
    print "lua_resume invalid-resume protected boundary missing"
    exit 1
  }
}
]=], "src/lj_api.c")

  awk([=[
BEGIN { infn = 0; grow = 0; drop = 0; err = 0 }
/^LUA_API const char \*lua_getlocal\(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_state_cpgrowstack/ { grow = NR }
infn && grow && /lj_state_dropclaim\(&claim\)/ { drop = NR }
infn && grow && /lj_err_callermsg/ {
  err = NR
  if (!drop || drop > NR) {
    print "lua_getlocal stack-growth error reports before dropping claim"
    exit 1
  }
}
END {
  if (!grow || !drop || !err || grow > drop || drop > err) {
    print "lua_getlocal protected stack-growth cleanup missing"
    exit 1
  }
}
]=], "src/lj_debug.c")

  awk([=[
BEGIN { infn = 0; drop = 0; pub = 0 }
/^LUA_API const char \*lua_setlocal\(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_state_dropclaim\(&claim\)/ { drop = NR }
infn && /lj_gc_pubuv/ { pub = NR }
END {
  if (!drop || !pub || drop > pub) {
    print "lua_setlocal must publish upvalue after dropping state claim"
    exit 1
  }
}
]=], "src/lj_debug.c")

  awk([=[
BEGIN { infn = 0; claim = 0; claimed = 0; active = 0; cpcall = 0; drop = 0; rethrow = 0 }
/^LUA_API int lua_getinfo\(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_state_resumeclaim/ { claim = NR }
infn && /lj_debug_getinfo_claimed/ { claimed = NR }
infn && /lj_debug_pushactivelines/ { active = NR }
infn && /lj_vm_cpcall/ { cpcall = NR }
infn && /lj_state_dropresumeclaim\(&claim\)/ { drop = NR }
infn && /lj_err_throw/ {
  rethrow = NR
  if (!drop || drop > NR) {
    print "lua_getinfo rethrows before dropping resume claim"
    exit 1
  }
}
END {
  if (!claim || !claimed || !active || !cpcall || !drop || !rethrow ||
      claim > claimed || claim > cpcall || cpcall > drop) {
    print "lua_getinfo resume-claim cleanup boundary missing"
    exit 1
  }
}
]=], "src/lj_debug.c")

  awk([=[
BEGIN { infn = 0; stack = 0; info = 0; drop = 0; badcp = 0 }
/^LUALIB_API void luaL_traceback \(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_vm_cpcall/ { badcp = NR }
infn && /lj_debug_getstack_claimed/ { stack = NR }
infn && /lj_debug_getinfo_claimed/ { info = NR }
infn && info && /lj_state_dropclaim\(&claim\)/ { drop = NR }
END {
  if (badcp || !stack || !info || !drop || stack > info || info > drop) {
    print "luaL_traceback must use claimed debug helpers without foreign cpcall"
    exit 1
  }
}
]=], "src/lj_debug.c")

  awk([=[
BEGIN { infn = 0; pub = 0 }
/^static void debug_pushfunc_root\(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_state_stack_pubtv/ { pub = NR }
END {
  if (!pub) {
    print "debug_pushfunc_root must release-publish caller stack roots"
    exit 1
  }
}
]=], "src/lib_debug.c")

  awk([=[
BEGIN {
  infn = 0; stack = 0; info = 0; root = 0; active = 0; drop = 0; table = 0; bad = 0
}
/^LJLIB_CF\(debug_getinfo\)/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_vm_cpcall|lua_xmove|lua_getstack|lua_getinfo/ { bad = NR }
infn && /lj_debug_getstack_claimed/ { stack = NR }
infn && /lj_debug_getinfo_claimed/ { info = NR }
infn && /debug_pushfunc_root/ { root = NR }
infn && /lj_debug_pushactivelines/ { active = NR }
infn && /lj_state_dropclaim\(&claim\)/ { drop = NR }
infn && /lua_createtable/ { table = NR }
END {
  if (bad || !info || !root || !active || !drop || !table ||
      (stack && stack > info) || info > root || root > drop || drop > active ||
      active > table) {
    print "debug.getinfo must root claimed-state results before claim drop"
    exit 1
  }
}
]=], "src/lib_debug.c")

  awk([=[
BEGIN { infn = 0; stack = 0; local = 0; root = 0; drop = 0; push = 0; bad = 0 }
/^LJLIB_CF\(debug_getlocal\)/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_vm_cpcall|lua_xmove/ { bad = NR }
infn && /lj_debug_getstack_claimed/ { stack = NR }
infn && /lj_debug_getlocal_claimed/ { local = NR }
infn && /debug_pushfunc_root/ { root = NR }
infn && /lj_state_dropclaim\(&claim\)/ { drop = NR }
infn && /lua_pushstring/ { push = NR }
END {
  if (bad || !stack || !local || !root || !drop || !push ||
      stack > local || local > root || root > drop || drop > push) {
    print "debug.getlocal must copy/root claimed-state local before claim drop"
    exit 1
  }
}
]=], "src/lib_debug.c")

  awk([=[
BEGIN { infn = 0; local = 0; root = 0; drop = 0; pub = 0; push = 0; bad = 0 }
/^LJLIB_CF\(debug_setlocal\)/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_vm_cpcall|lua_setlocal|restorestack/ { bad = NR }
infn && /lj_debug_setlocal_claimed/ { local = NR }
infn && /debug_pushfunc_root/ { root = NR }
infn && /lj_state_dropclaim\(&claim\)/ { drop = NR }
infn && /lj_gc_pubuv/ { pub = NR }
infn && /lua_pushstring/ { push = NR }
END {
  if (bad || !local || !drop || !push || local > drop || drop > push ||
      (root && (local > root || root > drop)) ||
      (pub && (drop > pub || pub > push))) {
    print "debug.setlocal must update claimed-state local before claim drop"
    exit 1
  }
}
]=], "src/lib_debug.c")

  awk([=[
BEGIN { infn = 0; str = 0; pub = 0 }
/^static TValue \*jit_profile_callback_setup_cp\(lua_State \*L,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_str_new/ { str = NR }
infn && /lj_state_stack_pubtv/ { pub = NR }
END {
  if (!str || !pub) {
    print "jit_profile_callback_setup_cp no longer protects callback setup"
    exit 1
  }
}
]=], "src/lib_jit.c")

  awk([=[
BEGIN { infn = 0; claim = 0; cpcall = 0; drop = 0; rethrow = 0 }
/^static void jit_profile_callback\(lua_State \*L2,/ { infn = 1; next }
infn && /^}/ { infn = 0; next }
infn && /lj_state_resumeclaim/ { claim = NR }
infn && /jit_profile_callback_setup_cp/ && /lj_vm_cpcall/ { cpcall = NR }
infn && /lj_state_dropresumeclaim\(&claim\)/ { drop = NR }
infn && /lj_err_throw/ {
  rethrow = NR
  if (!drop || drop > NR) {
    print "jit_profile_callback rethrows before dropping resume claim"
    exit 1
  }
}
END {
  if (!claim || !cpcall || !drop || !rethrow || claim > cpcall ||
      cpcall > drop) {
    print "jit_profile_callback protected setup boundary missing"
    exit 1
  }
}
]=], "src/lib_jit.c")
end

local function api_debug_claim_cleanup_smoke()
  return [=[
local co = coroutine.create(function()
  local target = "before"
  coroutine.yield("ready")
  return target
end)

local ok, msg = coroutine.resume(co)
assert(ok and msg == "ready")

local info = debug.getinfo(co, 1, "flnSuL")
assert(type(info) == "table")
assert(type(info.func) == "function")
assert(type(info.activelines) == "table")

local slot
for i = 1, 20 do
  local name, value = debug.getlocal(co, 1, i)
  if not name then break end
  if name == "target" then
    assert(value == "before")
    slot = i
    break
  end
end
assert(slot, "suspended coroutine local not found")

local name = debug.setlocal(co, 1, slot, "after")
assert(name == "target")

ok, msg = coroutine.resume(co)
assert(ok and msg == "after")
assert(coroutine.status(co) == "dead")

local function self_info()
  return debug.getinfo(1, "flnSuL")
end
info = self_info()
assert(type(info) == "table" and type(info.func) == "function")
assert(type(info.activelines) == "table")

print("api-debug-claim-cleanup-smoke OK")
]=]
end

local function table_value_smoke()
  return [=[
local util = require("jit.util")
local linfo = util.funcinfo(function() return 1 end)
assert(linfo.proto ~= nil and linfo.upvalues ~= nil)
local cinfo = util.funcinfo(print)
assert(cinfo.addr ~= nil and cinfo.upvalues ~= nil)
jit.flush()
jit.opt.start("hotloop=1")
local function hot(n)
  local s = 0
  for i = 1, n do s = s + i end
  return s
end
for _ = 1, 5 do hot(20) end
local traced
for tr = 1, 32 do
  local info = util.traceinfo(tr)
  if info then
    assert(type(info.nins) == "number" and type(info.linktype) == "string")
    traced = tr
    break
  end
end
assert(traced)
local snap
for sn = 0, 32 do
  snap = util.tracesnap(traced, sn)
  if snap then break end
end
assert(snap and type(snap[0]) == "number" and type(snap[1]) == "number")
local lines = debug.getinfo(function()
  local x = 1
  return x
end, "L").activelines
assert(type(lines) == "table")
local saw_line = false
for line, active in pairs(lines) do
  if type(line) == "number" and active == true then
    saw_line = true
    break
  end
end
assert(saw_line)
local t = { 1, 2, 3 }
t.name = "table-value-publish"
assert(t[3] == 3 and t.name == "table-value-publish")
assert(("table-value-publish"):sub(1, 5) == "table")
local function event_cb() end
jit.attach(event_cb, "bc")
jit.attach(event_cb)
do
  local ffi = require("ffi")
  local x = 1LL
  assert(type(x) == "cdata" and tonumber(x) == 1 and ffi.typeof(x))
end
do
  local buffer = require("string.buffer")
  local mt = {}
  local dict = { "key", "hello", "key", false }
  local dict_mt = { mt, mt, false }
  local b = buffer.new({ dict = dict, metatable = dict_mt })
  b:encode(setmetatable({ key = "hello" }, mt))
  local out = b:decode()
  assert(out.key == "hello" and getmetatable(out) == mt)
end
]=]
end

local function tset_nil_smoke()
  return [=[
local mt = {
  __newindex = function(t, k, v) rawset(t, "hit", tostring(k) .. ":" .. tostring(v)) end
}
local t = setmetatable({ a = 1 }, mt)
t.a = 2
assert(t.a == 2 and t.hit == nil)
t.b = 3
assert(t.hit == "b:3")
local a = { 1, 2 }
a[1] = 10
local k = 2
a[k] = 20
assert(a[1] == 10 and a[2] == 20)
local b = { 1, nil, 3 }
b[2] = 22
local bk = 2
b[bk] = 23
assert(b[1] == 1 and b[2] == 23 and b[3] == 3)
local function many() return 1, 2, 3 end
local m = { many() }
assert(m[1] == 1 and m[2] == 2 and m[3] == 3)
local function spread(n)
  local r = {}
  for i = 1, n do r[i] = i end
  return unpack(r, 1, n)
end
local big = { spread(96) }
assert(#big == 96 and big[1] == 1 and big[96] == 96)
local s = { spread(96) }
s[64] = 640
local kk = 70
s[kk] = 700
for i = 80, 82 do s[i] = i * 10 end
assert(s[64] == 640 and s[70] == 700 and s[82] == 820)
]=]
end

local function jit_trace_publish_smoke()
  return [=[
local util = require"jit.util"
local trace_count = require"jit_harness".trace_count

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function f(n)
  local s = 0
  for i = 1, n do s = s + i end
  return s
end
for _ = 1, 40 do
  assert(f(200) == 20100)
end
assert(trace_count(200) > 0, "no root trace was published")
jit.flush()
assert(trace_count(200) == 0, "trace slots were not cleared")

jit.flush()
jit.opt.start("hotloop=1")
local function f1(a)
  if a > 0 then
    local b = f1(a - 1)
    return function()
      if type(b) == "function" then return a + b() end
      return a + b
    end
  end
  return a
end
local function f2(a) return f1(a)() end
for _ = 1, 41 do
  assert(f2(4) + f2(4) == 20)
end

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
local function side(n, flip)
  local s = 0
  for i = 1, n do
    if flip and i % 3 == 0 then s = s + i else s = s - 1 end
  end
  return s
end
local function expect(n, flip)
  local s = 0
  for i = 1, n do
    if flip and i % 3 == 0 then s = s + i else s = s - 1 end
  end
  return s
end
for _ = 1, 60 do
  assert(side(90, false) == expect(90, false))
end
local before = trace_count(200)
for _ = 1, 120 do
  assert(side(90, true) == expect(90, true))
end
assert(trace_count(200) > before, "no side trace was published")
local after_side = trace_count(200)
for _ = 1, 120 do
  assert(side(90, true) == expect(90, true))
end
assert(util.traceinfo(1), "missing root trace 1")
jit.flush(1)
assert(not util.traceinfo(1), "scoped root flush did not clear root slot")
assert(trace_count(200) < after_side, "scoped root flush did not retire any slots")
jit.flush()
assert(trace_count(200) == 0, "full flush after scoped root flush left traces")
for _ = 1, 20 do
  assert(side(90, true) == expect(90, true))
end
print("jit-trace-publish-smoke OK")
]=]
end

local function hookmask_atomic_smoke()
  return [=[
local hits = { count = 0, line = 0, call = 0, ["return"] = 0 }
local phase = 0

local function hook(ev)
  hits[ev] = (hits[ev] or 0) + 1
  if ev == "count" and phase == 0 then
    phase = 1
    debug.sethook(hook, "crl", 0)
    local fn, mask, count = debug.gethook()
    assert(fn == hook, "hook function was not preserved")
    assert(mask:find("c", 1, true), "call hook bit missing")
    assert(mask:find("r", 1, true), "return hook bit missing")
    assert(mask:find("l", 1, true), "line hook bit missing")
    assert(count == 0, "count hook was not disabled")
  end
end

local function inner(n)
  local s = 0
  for i = 1, n do
    s = s + i
  end
  return s
end

debug.sethook(hook, "", 1)
assert(inner(12) == 78)
assert(inner(12) == 78)
debug.sethook()
assert(hits.count == 1, "count hook did not transition once")
assert(hits.line > 0, "line hook did not run after mask update")
assert(hits.call > 0, "call hook did not run after mask update")
assert(hits["return"] > 0, "return hook did not run after mask update")

local ok, profile = pcall(require, "jit.profile")
if ok then
  profile.start("i1", function() end)
  local s = 0
  for i = 1, 200000 do
    s = s + i
  end
  profile.stop()
  assert(s > 0)
end

print("hookmask-atomic-smoke OK")
]=]
end

local function hook_state_atomic_smoke()
  return [=[
local first_hits = 0
local second_hits = 0

local function second(ev)
  assert(ev == "count")
  second_hits = second_hits + 1
end

local function first(ev)
  assert(ev == "count")
  first_hits = first_hits + 1
  debug.sethook(second, "", 3)
  local fn, mask, count = debug.gethook()
  assert(fn == second, "hook function did not update")
  assert(mask == "", "count-only mask should not expose event bits")
  assert(count == 3, "hook count start did not update")
end

debug.sethook(first, "", 1)
local sum = 0
for i = 1, 80 do
  sum = sum + i
end
assert(sum == 3240)
debug.sethook()

assert(first_hits == 1, "first hook should run once before replacement")
assert(second_hits > 0, "replacement count hook did not run")
local fn, mask, count = debug.gethook()
assert(fn == nil and mask == "" and count == 0, "hook clear did not publish")

print("hook-state-atomic-smoke OK")
]=]
end

local function assert_panic_callback_atomic_accessors(t)
  local obj_guard = [=[
BEGIN {
  load = 0; store = 0; xchg = 0
  loadimpl = 0; storeimpl = 0; xchgimpl = 0
}
/panicf_load\(global_State \*g\)/ { load = 1 }
/panicf_store\(global_State \*g, lua_CFunction panicf\)/ { store = 1 }
/panicf_xchg\(global_State \*g,/ { xchg = 1 }
/la_loadfunc_acq\(&g->panic\)/ { loadimpl = 1 }
/la_storefunc_rel\(&g->panic, panicf\)/ { storeimpl = 1 }
/la_xchgfunc_acqrel\(&g->panic, panicf\)/ { xchgimpl = 1 }
END {
  if (!load || !store || !xchg || !loadimpl || !storeimpl || !xchgimpl) {
    print "panic callback atomic accessor definitions missing"
    exit 1
  }
}
]=]
  local raw_guard = [=[
{
  if ($0 !~ /la_loadfunc_acq\(&g->panic\)/ &&
      $0 !~ /la_storefunc_rel\(&g->panic, panicf\)/ &&
      $0 !~ /la_xchgfunc_acqrel\(&g->panic, panicf\)/) {
    print "raw panic callback access: " $0
    bad = 1
  }
}
END { exit bad }
]=]
  local atpanic_guard = [=[
BEGIN { infn = 0; xchg = 0 }
/^LUA_API lua_CFunction lua_atpanic\(lua_State \*L, lua_CFunction panicf\)/ {
  infn = 1
  next
}
infn && /panicf_xchg\(G\(L\), panicf\)/ { xchg = 1 }
infn && /^}/ {
  if (!xchg) {
    print "lua_atpanic must atomically exchange panic callback"
    exit 1
  }
  infn = 0
}
END {
  if (!xchg) {
    print "lua_atpanic guard did not find atomic exchange"
    exit 1
  }
}
]=]
  utils.capture_command("cd " .. shell_quote(t.root) ..
                        " && awk " .. shell_quote(obj_guard) ..
                        " src/lj_obj.h")
  utils.capture_command("cd " .. shell_quote(t.root) ..
                        " && rg -n -- " .. shell_quote("->panic") ..
                        " src/*.c src/*.h | awk " ..
                        shell_quote(raw_guard))
  utils.capture_command("cd " .. shell_quote(t.root) ..
                        " && awk " .. shell_quote(atpanic_guard) ..
                        " src/lj_err.c")
end

local function assert_wrapcf_func_atomic_publication(t)
  local obj_guard = [=[
BEGIN { store = 0; impl = 0 }
/wrapf_store\(global_State \*g, lua_CFunction wrapf\)/ { store = 1 }
/la_storefunc_rel\(&g->wrapf, wrapf\)/ { impl = 1 }
END {
  if (!store || !impl) {
    print "wrapf release-store accessor missing"
    exit 1
  }
}
]=]
  local raw_guard = [=[
{
  if ($0 !~ /la_storefunc_rel\(&g->wrapf, wrapf\)/) {
    print "raw C wrapper callback access: " $0
    bad = 1
  }
}
END { exit bad }
]=]
  local mode_guard = [=[
BEGIN { infn = 0; claim = 0; slot = 0; store = 0; drop = 0; publish = 0 }
/^int luaJIT_setmode\(lua_State \*L, int idx, int mode\)/ {
  infn = 1
  next
}
infn && /setmode_checkclaim\(L, &claim\)/ { claim = NR }
infn && /setmode_stack_slot\(L, idx\)/ { slot = NR }
infn && /wrapf_store\(g, \(lua_CFunction\)lightudV\(g, tv\)\)/ { store = NR }
infn && /lj_state_dropclaim\(&claim\)/ {
  if (store && !drop) drop = NR
}
infn && /bc_publish_op\(&g->bc_cfunc_ext, BC_FUNCCW\)/ { publish = NR }
infn && /^}/ {
  if (!claim || !slot || !store || !drop || !publish ||
      claim > slot || slot > store || store > drop || drop > publish) {
    print "WRAPCFUNC must claim stack slot before wrapper/BC publication"
    exit 1
  }
  infn = 0
}
END {
  if (!claim || !slot || !store || !drop || !publish) {
    print "WRAPCFUNC publication guard did not find full claimed path"
    exit 1
  }
}
]=]
  utils.capture_command("cd " .. shell_quote(t.root) ..
                        " && awk " .. shell_quote(obj_guard) ..
                        " src/lj_obj.h")
  utils.capture_command("cd " .. shell_quote(t.root) ..
                        " && rg -n -- " .. shell_quote("->wrapf") ..
                        " src/*.c src/*.h | awk " ..
                        shell_quote(raw_guard))
  utils.capture_command("cd " .. shell_quote(t.root) ..
                        " && awk " .. shell_quote(mode_guard) ..
                        " src/lj_dispatch.c")
  local func_guard = [=[
BEGIN {
  infn = 0; inmode = 0; token = 0; claim = 0; slot = 0
  mutate = 0; drop = 0; flush = 0; release = 0
}
/^int luaJIT_setmode\(lua_State \*L, int idx, int mode\)/ {
  infn = 1
  next
}
infn && /case LUAJIT_MODE_FUNC:/ { inmode = 1 }
infn && inmode && /lj_jit_token_acquire_wait\(G2J\(g\)\)/ { token = NR }
infn && inmode && /setmode_checkclaim\(L, &claim\)/ { claim = NR }
infn && inmode && /setmode_stack_slot\(L, idx\)/ { slot = NR }
infn && inmode && /setptmode\(g, pt, mode\)/ { mutate = NR }
infn && inmode && /lj_state_dropclaim\(&claim\)/ {
  if (mutate && !drop) drop = NR
}
infn && inmode && /lj_trace_flushscope_hs\(g, flushed\)/ { flush = NR }
infn && inmode && /lj_jit_token_release\(G2J\(g\)\)/ {
  if (drop && !release) release = NR
}
infn && inmode && /case LUAJIT_MODE_TRACE:/ {
  if (!token || !claim || !slot || !mutate || !drop || !flush || !release ||
      token > claim || claim > slot || slot > mutate || mutate > drop ||
      drop > flush || flush > release) {
    print "FUNC setmode must claim stack/prototype mutation before trace flush"
    exit 1
  }
  inmode = 0
}
END {
  if (inmode || !release) {
    print "FUNC setmode guard did not find full claimed path"
    exit 1
  }
}
]=]
  utils.capture_command("cd " .. shell_quote(t.root) ..
                        " && awk " .. shell_quote(func_guard) ..
                        " src/lj_dispatch.c")
end

local function assert_closed_upvalue_publish_order(t)
  local raw_guard = [=[
/lj_mem_newgco\(L, sizeof\(GCupval\)\)/ {
  print "closed upvalues must not use immediate-link lj_mem_newgco"
  exit 1
}
]=]
  local publish_guard = [=[
BEGIN { infn = 0; white = 0; link = 0 }
/^static void func_publishuv\(global_State \*g, GCupval \*uv\)/ {
  infn = 1
  next
}
infn && /^}/ { infn = 0; next }
infn && /newwhite\(g, uv\)/ { white = NR }
infn && /lj_gc_linkobj_new\(g, obj2gco\(uv\)\)/ { link = NR }
END {
  if (!white || !link || white > link) {
    print "func_publishuv must mark white before root publication"
    exit 1
  }
}
]=]
  local closed_guard = [=[
BEGIN { infn = 0; alloc = 0; gct = 0; closed = 0; tv = 0; v = 0; meta = 0; publish = 0 }
/^static GCupval \*func_newuvclosed\(lua_State \*L\)/ {
  infn = 1
  next
}
infn && /^}/ { infn = 0; next }
infn && /lj_mem_newgco_unlinked\(L, sizeof\(GCupval\)\)/ { alloc = NR }
infn && /uv->gct = ~LJ_TUPVAL/ { gct = NR }
infn && /uv->closed = 1/ { closed = NR }
infn && /setnilV\(&uv->tv\)/ { tv = NR }
infn && /setmref\(uv->v, &uv->tv\)/ { v = NR }
infn && /uv->dhash = 0/ { meta = NR }
infn && /func_publishuv\(g, uv\)/ { publish = NR }
END {
  if (!alloc || !gct || !closed || !tv || !v || !meta || !publish ||
      alloc > gct || gct > closed || closed > tv || tv > v ||
      v > meta || meta > publish) {
    print "func_newuvclosed must initialize closed upvalue before root publication"
    exit 1
  }
}
]=]
  local snap_guard = [=[
BEGIN { infn = 0; alloc = 0; gct = 0; closed = 0; copy = 0; v = 0; meta = 0; white = 0; pub = 0; link = 0 }
/^static GCupval \*func_snapshotuv\(lua_State \*L, const TValue \*slot\)/ {
  infn = 1
  next
}
infn && /^}/ { infn = 0; next }
infn && /lj_mem_newgco_unlinked\(L, sizeof\(GCupval\)\)/ { alloc = NR }
infn && /uv->gct = ~LJ_TUPVAL/ { gct = NR }
infn && /uv->closed = 1/ { closed = NR }
infn && /copyTVrel\(L, &uv->tv, slot\)/ { copy = NR }
infn && /setmref\(uv->v, &uv->tv\)/ { v = NR }
infn && /uv->dhash = 0/ { meta = NR }
infn && /newwhite\(g, uv\)/ { white = NR }
infn && /lj_gc_pubobjtv\(L, uv, &uv->tv\)/ { pub = NR }
infn && /lj_gc_linkobj_new\(g, obj2gco\(uv\)\)/ { link = NR }
END {
  if (!alloc || !gct || !closed || !copy || !v || !meta || !white || !pub || !link ||
      alloc > gct || gct > closed || closed > copy || copy > v ||
      v > meta || meta > white || white > pub || pub > link) {
    print "func_snapshotuv must initialize/barrier closed upvalue before root publication"
    exit 1
  }
}
]=]
  utils.capture_command("cd " .. shell_quote(t.root) ..
                        " && awk " .. shell_quote(raw_guard) ..
                        " src/lj_func.c")
  utils.capture_command("cd " .. shell_quote(t.root) ..
                        " && awk " .. shell_quote(publish_guard) ..
                        " src/lj_func.c")
  utils.capture_command("cd " .. shell_quote(t.root) ..
                        " && awk " .. shell_quote(closed_guard) ..
                        " src/lj_func.c")
  utils.capture_command("cd " .. shell_quote(t.root) ..
                        " && awk " .. shell_quote(snap_guard) ..
                        " src/lj_func.c")
end

local function gc_total_atomic_smoke()
  return [=[
local th = require"threading"

local function stats_total()
  local stats = th.gcstats()
  assert(type(stats.total_bytes) == "number", "missing total_bytes")
  assert(type(stats.total_kbytes) == "number", "missing total_kbytes")
  assert(stats.total_bytes >= stats.total_kbytes * 1024)
  return stats.total_bytes
end

local before = stats_total()
for round = 1, 4 do
  local workers = {}
  for id = 1, 4 do
    workers[id] = th.spawn(function(worker)
      local keep = {}
      for i = 1, 1500 do
	keep[i] = { worker, i, tostring(i) }
      end
      collectgarbage("collect")
      return #keep, th.gcstats().total_bytes
    end, id)
  end

  local total = 0
  for id = 1, 4 do
    local ok, n, worker_total = workers[id]:join(30)
    assert(ok == true, "worker failed")
    assert(n == 1500, "worker allocation count mismatch")
    assert(type(worker_total) == "number" and worker_total > 0)
    total = total + n
  end
  assert(total == 6000, "worker total mismatch")
end

collectgarbage("collect")
local after = stats_total()
assert(before > 0 and after > 0)

print("gc-total-atomic-smoke OK")
]=]
end

local function gc2_pacing_atomic_smoke()
  return [=[
local th = require"threading"

local function pacing_stats()
  local stats = th.gcstats()
  for _, name in ipairs({
    "alloc_since_trigger", "cycle_alloc_bytes", "trigger_bytes", "hard_bytes"
  }) do
    assert(type(stats[name]) == "number", "missing " .. name)
    assert(stats[name] >= 0, "negative " .. name)
  end
  assert(stats.trigger_bytes > 0, "missing GC2 trigger pacing")
  assert(stats.hard_bytes >= stats.trigger_bytes, "hard limit below trigger")
  return stats
end

local before = pacing_stats()
local workers = {}
for id = 1, 4 do
  workers[id] = th.spawn(function(worker)
    local keep = {}
    for i = 1, 500 do
      keep[i] = { worker, i, tostring(i) }
    end
    return #keep, th.gcstats().alloc_since_trigger
  end, id)
end

local total = 0
for id = 1, 4 do
  local ok, n, alloc = workers[id]:join(30)
  assert(ok == true, "worker failed")
  assert(n == 500, "worker allocation count mismatch")
  assert(type(alloc) == "number" and alloc >= 0)
  total = total + n
end

local after = pacing_stats()
assert(total == 2000)
assert(after.hard_bytes >= before.trigger_bytes)

print("gc2-pacing-atomic-smoke OK")
]=]
end

local function assert_gc_pacing_controls_atomic(t)
  local helper_guard = [=[
BEGIN {
  pause_load = 0; pause_store = 0; pause_xchg = 0
  step_load = 0; step_store = 0; step_xchg = 0
  pause_load_impl = 0; pause_store_impl = 0; pause_xchg_impl = 0
  step_load_impl = 0; step_store_impl = 0; step_xchg_impl = 0
}
/lj_gc_pause_load\(global_State \*g\)/ { pause_load = 1 }
/lj_gc_pause_store\(global_State \*g, MSize pause\)/ { pause_store = 1 }
/lj_gc_pause_xchg\(global_State \*g, MSize pause\)/ { pause_xchg = 1 }
/lj_gc_stepmul_load\(global_State \*g\)/ { step_load = 1 }
/lj_gc_stepmul_store\(global_State \*g, MSize stepmul\)/ { step_store = 1 }
/lj_gc_stepmul_xchg\(global_State \*g, MSize stepmul\)/ { step_xchg = 1 }
/la_load32_acq\(&g->gc.pause\)/ { pause_load_impl = 1 }
/la_store32_rel\(&g->gc.pause, \(uint32_t\)pause\)/ { pause_store_impl = 1 }
/la_xchg32_acqrel\(&g->gc.pause, \(uint32_t\)pause\)/ { pause_xchg_impl = 1 }
/la_load32_acq\(&g->gc.stepmul\)/ { step_load_impl = 1 }
/la_store32_rel\(&g->gc.stepmul, \(uint32_t\)stepmul\)/ { step_store_impl = 1 }
/la_xchg32_acqrel\(&g->gc.stepmul, \(uint32_t\)stepmul\)/ { step_xchg_impl = 1 }
END {
  if (!pause_load || !pause_store || !pause_xchg ||
      !step_load || !step_store || !step_xchg ||
      !pause_load_impl || !pause_store_impl || !pause_xchg_impl ||
      !step_load_impl || !step_store_impl || !step_xchg_impl) {
    print "GC pacing-control atomic accessor definitions missing"
    exit 1
  }
}
]=]
  local raw_guard = [=[
{
  if ($0 !~ /src\/lj_gc\.h:.*la_(load32_acq|store32_rel|xchg32_acqrel)\(&g->gc\.(pause|stepmul)/) {
    print "raw GC pacing-control access: " $0
    bad = 1
  }
}
END { exit bad }
]=]
  local api_guard = [=[
BEGIN { pause_xchg = 0; step_xchg = 0; restart_load = 0 }
/lj_gc_pause_load\(g\)/ { restart_load = 1 }
/lj_gc_pause_xchg\(g, \(MSize\)data\)/ { pause_xchg = 1 }
/lj_gc_stepmul_xchg\(g, \(MSize\)data\)/ { step_xchg = 1 }
END {
  if (!restart_load || !pause_xchg || !step_xchg) {
    print "lua_gc pacing-control atomic use missing"
    exit 1
  }
}
]=]
  utils.capture_command("cd " .. shell_quote(t.root) ..
                        " && awk " .. shell_quote(helper_guard) ..
                        " src/lj_gc.h")
  utils.capture_command("cd " .. shell_quote(t.root) ..
                        " && rg -n -- " ..
                        shell_quote("g->gc\\.(pause|stepmul)") ..
                        " src/*.c src/*.h | awk " ..
                        shell_quote(raw_guard))
  utils.capture_command("cd " .. shell_quote(t.root) ..
                        " && awk " .. shell_quote(api_guard) ..
                        " src/lj_api.c")
end

local function proto_kgc_acq_smoke()
  return [=[
local util = require("jit.util")

local function parent(t)
  local function child()
    return "proto-kgc-acq-child"
  end
  return t.proto_kgc_acq_marker, child
end

local saw_string = false
local saw_child = false
for i = -64, 64 do
  local k = util.funck(parent, i)
  if k == "proto_kgc_acq_marker" then saw_string = true end
  if type(k) == "proto" then saw_child = true end
end
assert(saw_string, "jit.util.funck did not expose string KGC")
assert(saw_child, "jit.util.funck did not expose child proto KGC")

local function field_name()
  local t = {}
  return t.proto_kgc_acq_marker + 1
end
local ok, err = pcall(field_name)
assert(not ok and tostring(err):match("field 'proto_kgc_acq_marker'"),
       tostring(err))

local function global_name()
  return proto_kgc_acq_global_missing + 1
end
ok, err = pcall(global_name)
assert(not ok and tostring(err):match("global 'proto_kgc_acq_global_missing'"),
       tostring(err))

jit.off(parent, true)
jit.on(parent, true)
print("proto-kgc-acq-smoke OK")
]=]
end

local function proto_chunkname_acq_smoke()
  return [=[
local util = require("jit.util")

local src = [[
return function()
  local t = {}
  return t.proto_chunkname_acq_field + 1
end
]]
local fn = assert(loadstring(src, "@proto_chunkname_acq_src"))()
local jinfo = util.funcinfo(fn)
assert(jinfo.source == "@proto_chunkname_acq_src", tostring(jinfo.source))
local dinfo = debug.getinfo(fn, "S")
assert(dinfo.source == "@proto_chunkname_acq_src", tostring(dinfo.source))
local ok, err = pcall(fn)
assert(not ok and tostring(err):match("proto_chunkname_acq_src"),
       tostring(err))

local dumped = string.dump(fn)
local fn2 = assert(loadstring(dumped))
dinfo = debug.getinfo(fn2, "S")
assert(dinfo.source == "@proto_chunkname_acq_src", tostring(dinfo.source))

jit.flush()
jit.opt.start("hotloop=1")
for _ = 1, 8 do pcall(fn2) end
print("proto-chunkname-acq-smoke OK")
]=]
end

local function proto_knum_acq_smoke()
  return [=[
local util = require("jit.util")
local ffi = require("ffi")

local function numeric_constants()
  return 123.25, -9876.5
end

local saw_a = false
local saw_b = false
for i = 0, 16 do
  local k = util.funck(numeric_constants, i)
  if k == 123.25 then saw_a = true end
  if k == -9876.5 then saw_b = true end
end
assert(saw_a and saw_b, "jit.util.funck did not expose numeric constants")

local dumped = string.dump(numeric_constants)
local loaded = assert(loadstring(dumped))
local a, b = loaded()
assert(a == 123.25 and b == -9876.5)

local ct = ffi.metatype("struct { int x; }", {
  __eq = function(lhs, rhs)
    if type(rhs) == "number" then return rhs == 123.25 end
    return lhs.x == rhs.x
  end
})
local c = ct(1)
assert(c == 123.25)
assert(not (c == 124.25))

print("proto-knum-acq-smoke OK")
]=]
end

return function(add)
  add({
    name = "m5_state_owner",
    description = "lua_State owner claim behavior",
    run = function(t)
      t:build({ clean = true, quiet = true })
      build_and_run_c(t, t:tmp("lj_t-state-owner"), "t-state-owner.c")
      print("M5 lua_State owner behavior passed")
    end
  })

  add({
    name = "m5_cell_ops",
    description = "local-cell bytecode and behavior guards",
    run = function(t)
      t:build({ clean = true, quiet = true })

      cellops.run_bytecode_guards(t, "lj_m5_cell_ops_bc")
      cellops.run_publication_behavior_guards(t)
      run_stock(t, { "test.lua", "--quiet", "lang/upvalue" })
      run_stock(t, { "misc/uclo.lua" })
      run_stock(t, { "test.lua", "--quiet", "opt/fwd/upval.lua" })
      run_stock(t, { "test.lua", "--quiet", "lang/goto.lua" })
      print("M5 local-cell opcode substrate guard passed")
    end
  })

  add({
    name = "m5_upvalue_publish_gc",
    description = "closed-upvalue GC object publication behavior",
    run = function(t)
      assert_closed_upvalue_publish_order(t)
      build_and_run_luajit_script(t, "t-threading-upvalue.lua", nil,
                                  { joff = true })
      build_and_run_c(t, t:tmp("lj_t-cclosure-upvalue-snapshot"),
                      "t-cclosure-upvalue-snapshot.c")
      build_and_run_c(t, t:tmp("lj_t-cclosure-upvalue-race"),
                      "t-cclosure-upvalue-race.c", { timeout = "20s" })
      print("M5 closed-upvalue GC publication behavior passed")
    end
  })

  add({
    name = "m5_stock_api_surface",
    description = "stock LuaJIT public C API surface behavior",
    run = function(t)
      build_and_run_c(t, t:tmp("lj_t-stock-api-surface"),
                      "t-stock-api-surface.c")
      print("M5 stock LuaJIT public C API surface passed")
    end
  })

  add({
    name = "m5_jit_trace_publish",
    description = "JIT trace-slot and trace-link publication guards",
    run = function(t)
      t:build({ quiet = true })
      run_c_fixture_specs(t, {
        { output = "lj_t-jit-tracevec", cfile = "t-jit-tracevec.c" },
        { output = "lj_t-jit-mcode-retire", cfile = "t-jit-mcode-retire.c" },
        { output = "lj_t-jit-trace-retire", cfile = "t-jit-trace-retire.c" }
      }, { timeout = "20s" })
      run_luajit(t, { "-e", jit_trace_publish_smoke() })
      print("M5 JIT trace publication guard passed")
    end
  })

  add({
    name = "m5_hookmask_atomic",
    description = "global hookmask atomic helper behavior",
    run = function(t)
      t:build({ quiet = true })
      run_luajit(t, { "-e", hookmask_atomic_smoke() })
      print("M5 hookmask atomic helper behavior passed")
    end
  })

  add({
    name = "m5_hook_state_atomic",
    description = "global hook function/count atomic helper behavior",
    run = function(t)
      t:build({ quiet = true })
      run_luajit(t, { "-e", hook_state_atomic_smoke() })
      print("M5 hook function/count atomic helper behavior passed")
    end
  })

  add({
    name = "m5_panic_callback_atomic",
    description = "global panic callback atomic exchange behavior",
    run = function(t)
      assert_panic_callback_atomic_accessors(t)
      t:build({ quiet = true })
      build_and_run_c(t, t:tmp("lj_t-panic-callback-atomic"),
                      "t-panic-callback-atomic.c", { timeout = "20s" })
      print("M5 panic callback atomic exchange behavior passed")
    end
  })

  add({
    name = "m5_wrapcf_func_publish",
    description = "C wrapper callback release-publication guard",
    run = function(t)
      assert_wrapcf_func_atomic_publication(t)
      t:build({ quiet = true })
      print("M5 C wrapper callback publication guard passed")
    end
  })

  add({
    name = "m5_api_debug_claim_cleanup",
    description = "API/debug/JIT state-claim cleanup boundaries",
    run = function(t)
      assert_api_debug_claim_cleanup(t)
      t:build({ quiet = true })
      run_luajit(t, { "-e", api_debug_claim_cleanup_smoke() })
      print("M5 API/debug/JIT claim cleanup boundaries passed")
    end
  })

  add({
    name = "m5_profile_stop_native",
    description = "jit.profile stop native-state STOPREQ behavior",
    run = function(t)
      assert_profile_dumpstack_claim_cleanup(t)
      t:build({ quiet = true })
      build_and_run_c(t, t:tmp("lj_t_profile_stop_native"),
                      "t-profile-stop-native.c", { build = false,
                                                   timeout = "20s" })
      print("M5 jit.profile stop native-state behavior passed")
    end
  })

  add({
    name = "m5_profile_blocked_tg_samples",
    description = "jit.profile sample delivery with another TG blocked",
    run = function(t)
      assert_profile_dumpstack_claim_cleanup(t)
      t:build({ quiet = true })
      build_and_run_luajit_script(t, "t-profile-blocked-tg.lua", nil,
                                  { build = false, joff = true,
                                    timeout = "20s" })
      print("M5 jit.profile blocked TG sample delivery passed")
    end
  })

  add({
    name = "m5_gc_total_atomic",
    description = "GC total atomic accounting helper behavior",
    run = function(t)
      t:build({ quiet = true })
      build_and_run_c(t, t:tmp("lj_t_gc_total_atomic"),
                      "t-gc-total-atomic.c", { build = false })
      run_luajit(t, { "-e", gc_total_atomic_smoke() })
      print("M5 GC total atomic accounting behavior passed")
    end
  })

  add({
    name = "m5_gc2_pacing_atomic",
    description = "GC/GC2 pacing control atomic helper behavior",
    run = function(t)
      t:build({ quiet = true })
      assert_gc_pacing_controls_atomic(t)
      build_and_run_c(t, t:tmp("lj_t_gc2_pacing_atomic"),
                      "t-gc2-pacing-atomic.c", { build = false })
      run_luajit(t, { "-e", gc2_pacing_atomic_smoke() })
      print("M5 GC/GC2 pacing control atomic helper behavior passed")
    end
  })

  add({
    name = "m5_proto_kgc_acq",
    description = "prototype KGC acquire-reader behavior",
    run = function(t)
      t:build({ quiet = true })
      run_luajit(t, { "-e", proto_kgc_acq_smoke() })
      print("M5 prototype KGC acquire-reader behavior passed")
    end
  })

  add({
    name = "m5_proto_chunkname_acq",
    description = "prototype chunkname acquire-reader behavior",
    run = function(t)
      t:build({ quiet = true })
      run_luajit(t, { "-e", proto_chunkname_acq_smoke() })
      print("M5 prototype chunkname acquire-reader behavior passed")
    end
  })

  add({
    name = "m5_proto_knum_acq",
    description = "prototype numeric constant acquire-reader behavior",
    run = function(t)
      t:build({ quiet = true })
      run_luajit(t, { "-e", proto_knum_acq_smoke() })
      print("M5 prototype numeric constant acquire-reader behavior passed")
    end
  })

  add({
    name = "m5_tab_array_publish",
    description = "table array publication and retirement guards",
    run = function(t)
      t:build({ clean = true, quiet = true })
      build_and_run_c(t, t:tmp("lj_t-tab-array-publish"),
                      "t-tab-array-publish.c", { timeout = "20s" })
      print("M5 table array publication tests passed")
    end
  })

  add({
    name = "m5_tab_colocated_resize",
    description = "colocated array resize freezes old inline slots",
    run = function(t)
      t:build({
        clean = true,
        jobs = false,
        quiet = true,
        xcflags = "-DLJ_TAB_TEST_HELPERS"
      })
      build_and_run_c(t, t:tmp("lj_t-tab-colocated-resize"),
                      "t-tab-colocated-resize.c", {
        cflags = "-DLJ_TAB_TEST_HELPERS",
        timeout = "20s"
      })
      print("M5 colocated array resize freeze guard passed")
    end
  })

  add({
    name = "m5_tab_cas_store",
    description = "table CAS store behavior",
    run = function(t)
      t:build({ clean = true, quiet = true })
      build_and_run_c(t, t:tmp("lj_t-tab-cas-store"),
                      "t-tab-cas-store.c", { timeout = "20s" })
      print("M5 table CAS store behavior passed")
    end
  })

  add({
    name = "m5_tab_value_publish",
    description = "C-side table value release-publication guards",
    run = function(t)
      t:build({ clean = true, quiet = true })
      run_luajit(t, { "-e", table_value_smoke() })
      build_and_run_c(t, t:tmp("lj_t-tab-cas-store-value"),
                      "t-tab-cas-store.c", { timeout = "20s" })
      print("M5 table value publication guard passed")
    end
  })

  add({
    name = "m5_x64_tset_nil_snapshot",
    description = "x64 TSET previous-value nil behavior",
    run = function(t)
      t:build({ clean = true, quiet = true })
      checks.assert_text_contains(
        "x64 TSETS existing string-key VM helper",
        t:read(t:path("src", "lj_vm.S")),
        "lj_tab_storetv_forvm_strhash",
        "generated VM")
      run_luajit(t, { "-joff", "-e", tset_nil_smoke() })
      build_and_run_c(t, t:tmp("lj_t-x64-tset-forward"),
                      "t-x64-tset-forward.c")
      print("M5 x64 TSET previous-value nil behavior passed")
    end
  })
end
