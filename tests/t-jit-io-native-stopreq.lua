local path = os.getenv("LJ_JIT_IO_STOPREQ_OUT") or "/tmp/lj-jit-io-stopreq.out"
local f = assert(io.open(path, "w"))
assert(f:setvbuf("no"))
local vmdef = require("jit.vmdef")

jit.off()
for _, name in pairs(vmdef.ircall) do
  assert(name ~= "fputc" and name ~= "fwrite" and name ~= "fflush",
         "raw stdio IR call target still exists: " .. tostring(name))
end
jit.on()

jit.flush()
jit.opt.start("hotloop=1", "hotexit=1")
for i = 1, 200 do f:write("x", i, "\n") end

jit.flush()
for i = 1, 200 do f:flush() end

io.output(f)

jit.flush()
for i = 1, 200 do io.write("y", i, "\n") end

jit.flush()
for i = 1, 200 do io.flush() end

f:close()
local r = assert(io.open(path, "r"))
assert(r:read(0) == "")
local data = r:read("*a")
r:close()
assert(data:match("x200\n"), "file:write output missing")
assert(data:match("y200\n"), "io.write output missing")
os.remove(path)

print("t-jit-io-native-stopreq OK")
