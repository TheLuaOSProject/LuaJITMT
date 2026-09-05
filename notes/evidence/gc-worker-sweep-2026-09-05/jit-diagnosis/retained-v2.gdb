set pagination off
set confirm off
set print elements 32
set disable-randomization on
set debuginfod enabled off
set environment RETENTION_JIT 1
unset environment ASAN_OPTIONS
set environment LUA_PATH /tmp/lj-gc-pending-root-design-20260905-blju2qsh/baseline/src/?.lua;/tmp/lj-gc-pending-root-design-20260905-blju2qsh/baseline/tests/lib/?.lua;;
python
OUTFILE='/tmp/lj-jit-sweep-diagnosis-20260905-jjdidw9u/retained-v2.jsonl'
exec(open('/tmp/lj-jit-sweep-diagnosis-20260905-jjdidw9u/retained-v2.py').read())
end
run 0 0 0 /tmp/lj-jit-sweep-diagnosis-20260905-jjdidw9u/peer-control.lua
