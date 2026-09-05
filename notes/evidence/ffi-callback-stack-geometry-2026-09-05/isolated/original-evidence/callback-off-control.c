#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "lj_obj.h"
#include "lj_ccall.h"
#include "lj_tg.h"
#include "lib/lua_fixture_helpers.h"
static TGState *testtg;
static int enabled;
static uint32_t generated_calls,generated_callbacks,suspended_callbacks;
typedef int32_t (*Callback)(int32_t);
static int32_t invoke(Callback callback,int32_t value) {
 LJFFINativeFrameSnapshot s;
 int native=lj_ffi_native_frame_snapshot(testtg,&s)==LJ_FFI_NATIVE_FRAME_SNAPSHOT_STABLE && s.depth &&
  lj_ffi_native_frame_func_acq(&s.frame[s.depth-1])==(void*)invoke &&
  (lj_ffi_native_frame_flags_acq(&s.frame[s.depth-1])&LJ_FFI_NATIVE_FRAME_F_ACTIVE);
 if(native)generated_calls++;
 if(enabled && native)fprintf(stderr,"generated invoke value=%d depth=%u flags=%u\n",value,s.depth,lj_ffi_native_frame_flags_acq(&s.frame[s.depth-1]));
 if(enabled) {if(native)generated_callbacks++;return callback(value);}
 return value+1;
}
static int enable(lua_State *L) {(void)L;enabled=1;return 0;}
static int mark_callback(lua_State *L) {
 LJFFINativeFrameSnapshot s;(void)L;
 if(lj_ffi_native_frame_snapshot(testtg,&s)==LJ_FFI_NATIVE_FRAME_SNAPSHOT_STABLE && s.depth &&
    lj_ffi_native_frame_func_acq(&s.frame[s.depth-1])==(void*)invoke &&
    (lj_ffi_native_frame_flags_acq(&s.frame[s.depth-1])&LJ_FFI_NATIVE_FRAME_F_SUSPENDED)) {suspended_callbacks++;fprintf(stderr,"suspended callback count=%u\n",suspended_callbacks);}
 return 0;
}
int main(int argc, char **argv) {
 (void)argv;
 lua_State *L=ljt_lua_newstate_openlibs();testtg=L2TG(L);
 lua_pushboolean(L,argc>1);lua_setglobal(L,"interpret_control");
 lua_pushlightuserdata(L,(void*)invoke);lua_setglobal(L,"invoke_ptr");
 lua_pushcfunction(L,enable);lua_setglobal(L,"enable_callback");
 lua_pushcfunction(L,mark_callback);lua_setglobal(L,"mark_callback");
 ljt_lua_dostring(L,
  "local ffi,util,vmdef,bit=require('ffi'),require('jit.util'),require('jit.vmdef'),require('bit')\n"
  "local p=ffi.new('struct {double x;double y;}',0,1)\n"
  "local fn=ffi.cast('int(*)(int(*)(int),int)',invoke_ptr)\n"
  "local function cb_body(i) mark_callback();return i+1 end;jit.off(cb_body,true);local cb=ffi.cast('int(*)(int)',cb_body)\n"
  "local function run(p,n,fn,cb) local s=0;for i=0,n do if i>0 then p.x=i;s=s+p.x+p.y+fn(cb,i) end end;return s end\n"
  "local exits=0;local function onexit() exits=exits+1 end;jit.off(onexit,true)\n"
  "local function check()\n"
  " if os.getenv('DUMP') then require('jit.dump').start('is',os.getenv('DUMP')) end\n"
  " if interpret_control then jit.off(run,true) end\n"
  " jit.opt.start('hotloop=1','hotexit=1000');jit.attach(onexit,'texit');assert(run(p,80,fn,cb)==6640);if not interpret_control then assert(exits>0) end\n"
  " local post,call=0,0;if not interpret_control then local t=assert(util.traceinfo(1));local loop=0\n"
  " for r=1,t.nins do local _,ot,a,b=util.traceir(1,r);if ot then local op=vmdef.irnames:sub(6*bit.rshift(ot,8)+1,6*bit.rshift(ot,8)+6);if op=='LOOP  ' then loop=r end;if loop>0 and op=='FLOAD ' and a==-1 and b==276 then post=post+1 end;if op=='CALLXS' then call=call+1 end end end\n"
  " assert(loop>0 and post>0 and call>0,'callback-capable CALLXS must keep repeated cdata proof') end\n"
  " enable_callback();exits=0;local after=run(p,80,fn,cb);print('callback-after',after,exits);io.stdout:flush();assert(after==6640,'callback result');if not interpret_control then assert(exits>0,'callback native exits') end;print('callback-negative','postroot',post,'CALLXS',call,'after-exits',exits)\n"
  " jit.attach(onexit);cb:free()\n"
  "end;jit.off(check,true);check()\n");
 if(argc==1) {assert(generated_calls>0);assert(generated_callbacks>0);assert(suspended_callbacks>0);}
 else {assert(generated_calls==0 && generated_callbacks==0 && suspended_callbacks==0);}
 printf("callback-negative generated=%u generated-callbacks=%u suspended=%u\n",generated_calls,generated_callbacks,suspended_callbacks);
 lua_close(L);return 0;
}
