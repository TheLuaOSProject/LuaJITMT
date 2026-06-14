---@meta threading

---@alias threading.timeout_error "timeout"
---@alias threading.recv_status true|false|"timeout"
---@alias threading.peek_status true|false

---@class threading.thread: userdata
local threading_thread = {}

---Wait for this thread to finish.
---
---Returns `true, ...` when the child function returns, `false, err` when the
---child function errors, or `nil, "timeout"` when a timeout is supplied and the
---thread is still running.
---@overload fun(self: threading.thread): true, ...
---@overload fun(self: threading.thread): false, any
---@overload fun(self: threading.thread, timeout: number): true, ...
---@overload fun(self: threading.thread, timeout: number): false, any
---@overload fun(self: threading.thread, timeout: number): nil, threading.timeout_error
---@param timeout? number seconds to wait; omitted blocks indefinitely.
---@return true|false|nil ok true for child success, false for child error, nil on timeout.
---@return any ...
function threading_thread:join(timeout) end

---@return integer id
---@nodiscard
function threading_thread:id() end

---@return boolean running
---@nodiscard
function threading_thread:running() end

---@return "threading.thread"
---@nodiscard
function threading_thread:__tostring() end

---@class threading.mutex: userdata
local threading_mutex = {}

---Block until the mutex is acquired.
function threading_mutex:lock() end

---@return boolean locked
---@nodiscard
function threading_mutex:trylock() end

---Release the mutex.
---
---Errors if the mutex is not currently locked.
function threading_mutex:unlock() end

---@return "threading.mutex"
---@nodiscard
function threading_mutex:__tostring() end

---@generic T
---@class threading.channel<T>: userdata
---@field send fun(self: threading.channel<T>, value: T, timeout?: number): true|nil, threading.timeout_error|nil
---@field recv fun(self: threading.channel<T>, timeout?: number): T|nil, threading.recv_status
---@field peek fun(self: threading.channel<T>): T|nil, threading.peek_status
---@field close fun(self: threading.channel<T>)
local threading_channel = {}

---Send a value to the channel.
---
---Errors if the channel is closed. If this is a rendezvous channel, success
---means a receiver has taken the value.
---@generic T
---@overload fun(self: threading.channel<T>, value: T): true
---@overload fun(self: threading.channel<T>, value: T, timeout: number): true
---@overload fun(self: threading.channel<T>, value: T, timeout: number): nil, threading.timeout_error
---@param value T
---@param timeout? number seconds to wait; omitted blocks indefinitely.
---@return true|nil ok
---@return threading.timeout_error|nil err
function threading_channel:send(value, timeout) end

---Receive a value from the channel.
---
---Returns `value, true` on success, `nil, false` when the channel is closed, or
---`nil, "timeout"` when a timeout is supplied and no value is available.
---@generic T
---@overload fun(self: threading.channel<T>): T, true
---@overload fun(self: threading.channel<T>): nil, false
---@overload fun(self: threading.channel<T>, timeout: number): T, true
---@overload fun(self: threading.channel<T>, timeout: number): nil, false
---@overload fun(self: threading.channel<T>, timeout: number): nil, threading.timeout_error
---@param timeout? number seconds to wait; omitted blocks indefinitely.
---@return T|nil value
---@return threading.recv_status status
function threading_channel:recv(timeout) end

---Inspect the next value without removing it from the channel.
---
---Returns `value, true` on success, or `nil, false` when the channel is empty
---or closed.
---@generic T
---@overload fun(self: threading.channel<T>): T, true
---@overload fun(self: threading.channel<T>): nil, false
---@return T|nil value
---@return threading.peek_status status
function threading_channel:peek() end

---Close the channel.
function threading_channel:close() end

---@return "threading.channel"
---@nodiscard
function threading_channel:__tostring() end

---@class threadinglib
local threading = {}

---@return integer count
---@nodiscard
function threading.cpucount() end

---Issue a cross-thread memory fence.
function threading.fence() end

---@param seconds? number seconds to sleep; defaults to 0.
function threading.sleep(seconds) end

---Spawn a new OS thread that calls `fn(...)`.
---@param fn fun(...):...
---@param ... any
---@return threading.thread thread
---@nodiscard
function threading.spawn(fn, ...) end

---@return threading.thread thread
---@nodiscard
function threading.current() end

---@return threading.mutex mutex
---@nodiscard
function threading.mutex() end

---Create a channel.
---
---Capacity 0 creates a rendezvous channel. Buffered channels have at least the
---requested capacity.
---@generic T
---@param capacity? integer buffered slot count; defaults to 0.
---@return threading.channel<T> channel
---@nodiscard
function threading.channel(capacity) end

return threading
