#include <assert.h>
#include <stdio.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_jit.h"
#include "lj_dispatch.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lib/lua_fixture_helpers.h"
static int armed,hit;
extern void *__real_lj_mem_realloc(lua_State *L,void *p,GCSize osz,GCSize nsz);
void *__wrap_lj_mem_realloc(lua_State *L,void *p,GCSize osz,GCSize nsz) {
 jit_State *J=G2J(G(L));
 if(armed && J->loop_cdata_fload) {armed=0;hit++;lj_err_mem(L);}
 return __real_lj_mem_realloc(L,p,osz,nsz);
}
int main(void) {
 lua_State *L=ljt_lua_newstate_openlibs();jit_State *J=G2J(G(L));int status;
 ljt_lua_dostring(L,
 "local ffi=require('ffi');p=ffi.new('struct {double x;double y;}',0,100)\n"
 "function run(p,n) local s=0;for i=0,n do if i>0 then p.x=i;s=s+p.x+p.y end end;return s end\n"
 "jit.opt.start('hotloop=1','hotexit=1000')\n");
 ljt_lua_loadstring(L,"return run(p,80)");
 armed=1;status=lua_pcall(L,0,1,0);
 assert(hit==1);assert(J->loop_cdata_fload==0);armed=0;
 printf("flag-error injected=%d status=%d flag=%u\n",hit,status,(unsigned)J->loop_cdata_fload);
 lua_settop(L,0);
 ljt_lua_dostring(L,"jit.flush();local util=require('jit.util');assert(run(p,80)==11240);assert(util.traceinfo(1))\n");
 assert(J->loop_cdata_fload==0);
 lua_close(L);return 0;
}
