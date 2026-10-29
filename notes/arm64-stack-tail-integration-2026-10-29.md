# ARM64 stack-aggregate and tail-return integration

The next integration combines full-width integer stack aggregates
(`be1bd66b0`), aligned wide HFA stack groups (`23b375129`), and the bounded
captured-leaf tail-return root (`adeb447fd`) on the validated scalar ITERL
integration. The recorder merge preserves the existing ITERL tuple and RET0
classification, while allowing CALLT only through the separate FUNCF tail
shape. The redundant preprocessor branches around identical preflight calls
were removed without changing their behavior.

Independent review cleared the tail-frame caller-edge question: a Lua caller
entered by C pcall legitimately carries FRAME_C, which describes how it
returns to its own caller. The current child FRAME_LUA edge, prior Lua closure,
its prototype, and the exact source call authenticate this transition.
Requiring frame_islua(caller) would reject that valid path.

Combined full-gate and ordinary ARM64/x86-64 stock, imported-bytecode, topology,
aggregate, and strict GDBJIT compilation validation is pending. Evidence is
being collected under
`/Users/frityet/Scratch/ljmt-stack-tail-integrated-20261029/`.

This checkpoint does not establish full architecture parity. Nil-terminated
ITERL, wider side graphs, additional calls/returns, and broader FFI aggregate
ABI shapes remain separate work.
