-- Run the stock suite with every load/loadstring source compiled by an original
-- LuaJIT process, then imported as genuine v2 bytecode. Run from tests/stock/test:
-- LUAJIT_STOCK=/path/to/original/luajit ../../../src/luajit ../../../tools/ci/stock_bytecode_corpus.lua
-- FIFO compiler transport avoids launching a process for each generated case.
local original_load, original_loadstring = load, loadstring
local stock = os.getenv('LUAJIT_STOCK') or 'luajit'
local strip = false
local function quote(s) return "'" .. s:gsub("'", "'\\''") .. "'" end
local reqpath, respath = os.tmpname(), os.tmpname()
os.remove(reqpath); os.remove(respath)
assert(os.execute('mkfifo '..quote(reqpath)..' '..quote(respath)) == 0)
local server = 'while true do local line=io.read("*l"); if not line then break end; local source=io.read(tonumber(line)); local name=io.read("*l"); local f,e=loadstring(source,name); local result=f and string.dump(f,'..tostring(strip)..') or ("ERROR"..string.char(0)..e); io.write(#result,"\\n",result); io.flush() end'
assert(os.execute(quote(stock)..' -e '..quote(server)..' <'..quote(reqpath)..' >'..quote(respath)..' &') == 0)
local req, res = assert(io.open(reqpath,'w')), assert(io.open(respath,'r'))
os.remove(reqpath);os.remove(respath)
local function translate(source,name)
  if source:byte(1)==27 then return source end
  name=name or '=(stock-v2)'
  name=name:gsub('[\r\n]',' ')
  req:write(#source,'\n',source,name,'\n');req:flush()
  local size=assert(res:read('*l'),'stock compiler ended')
  local result=assert(res:read(tonumber(size)))
  if result:sub(1,6)=='ERROR\0' then return nil,result:sub(7) end
  assert(result:sub(1,4)=='\27LJ\2','LUAJIT_STOCK must be an original v2 compiler')
  return result
end
loadstring=function(s,name)
  if type(s)~='string' then return original_loadstring(s,name) end
  local b,e=translate(s,name);if not b then return nil,e end
  return original_loadstring(b,name)
end
load=function(reader,name,mode,env)
  local s
  if type(reader)=='function' then
    local chunks={}
    while true do local piece=reader();if piece==nil or piece=='' then break end;chunks[#chunks+1]=piece end
    s=table.concat(chunks)
  elseif type(reader)=='string' then s=reader
  else return original_load(reader,name,mode,env) end
  if mode=='b' or s:byte(1)==27 then return original_load(s,name,mode,env) end
  local b,e=translate(s,name);if not b then return nil,e end
  return original_load(b,name,'b',env)
end
if os.getenv('BCCELL_NOJIT') and jit then jit.off(); jit=nil end
assert(require('threading').spawn(function()return true end):join())
threading=nil; package.loaded.threading=nil
arg={'--quiet'}
dofile('test.lua')
