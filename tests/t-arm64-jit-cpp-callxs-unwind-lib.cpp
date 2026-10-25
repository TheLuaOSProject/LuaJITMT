/* C++ frame between generated CALLXS and an erroring Lua FFI callback. */
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
