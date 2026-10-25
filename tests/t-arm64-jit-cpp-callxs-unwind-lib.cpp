/* C++ frame between generated CALLXS and an erroring Lua FFI callback. */
#include <assert.h>
typedef int (*LuaCallback)(int);
typedef int (*NativeObserver)(int);
static LuaCallback active_callback;
static NativeObserver native_observer;
static unsigned bridge_calls;
static unsigned cxx_unwinds;
static unsigned cxx_destructors;

struct CallFrame {
  ~CallFrame() { cxx_destructors++; }
};

extern "C" int arm64_cpp_call_bridge(int value)
{
  CallFrame frame;
  bridge_calls++;
  int generated = native_observer && native_observer(active_callback != 0);
  /* The first loop iteration may run in the interpreter. Trigger the error
     only after the observer proves a live generated native-call frame. */
  if (!active_callback || !generated) return value;
  try {
    return active_callback(value);
  } catch (...) {
    cxx_unwinds++;
    throw;
  }
}

/* Five outgoing stack arguments exceed the VM's fixed spill area. Retain
   native C++ frames across the callback and check the carriers on return. */
extern "C" int arm64_cpp_stack_bridge(int a, int b, int c, int d, int e,
    int f, int g, int h, int i, int j, int k, int l, int value)
{
  volatile int args[] = { a, b, c, d, e, f, g, h, i, j, k, l, value };
  for (unsigned n = 0; n < 12; n++) assert(args[n] == 101*(int)(n+1));
  int result = arm64_cpp_call_bridge(args[12]);
  for (unsigned n = 0; n < 12; n++) assert(args[n] == 101*(int)(n+1));
  assert(args[12] == value);
  return result;
}

extern "C" void arm64_cpp_install_callback(LuaCallback callback)
{
  active_callback = callback;
}

extern "C" void arm64_cpp_install_observer(NativeObserver observer)
{
  native_observer = observer;
}

extern "C" unsigned arm64_cpp_bridge_calls(void) { return bridge_calls; }
extern "C" unsigned arm64_cpp_unwinds(void) { return cxx_unwinds; }
extern "C" unsigned arm64_cpp_destructors(void) { return cxx_destructors; }
