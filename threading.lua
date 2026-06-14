---@meta threading

---@alias threading.timeout_error "timeout"
---@alias threading.recv_status true|false|"timeout"

---@class threading.thread
local threading_thread = {}

---Wait for this thread to finish.
---
---Returns `nil, "timeout"` when a timeout is supplied and the thread is still
---running. Otherwise returns the child call status followed by the child
---function's return values, or by the error object when the child failed.
---@param timeout? number seconds to wait; omitted blocks indefinitely.
---@return boolean|nil ok
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

---@class threading.mutex
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

---@class threading.channel
local threading_channel = {}

---Send a value to the channel.
---
---Errors if the channel is closed.
---@param value any
---@param timeout? number seconds to wait; omitted blocks indefinitely.
---@return true|nil ok
---@return threading.timeout_error|nil err
function threading_channel:send(value, timeout) end

---Receive a value from the channel.
---
---Returns `value, true` on success, `nil, false` when the channel is closed, or
---`nil, "timeout"` when a timeout is supplied and no value is available.
---@param timeout? number seconds to wait; omitted blocks indefinitely.
---@return any value
---@return threading.recv_status status
function threading_channel:recv(timeout) end

---Inspect the next value without removing it from the channel.
---
---Returns `value, true` on success, `nil, false` when the channel is closed, or
---`nil, "timeout"` when no value is immediately available.
---@return any value
---@return threading.recv_status status
function threading_channel:peek() end

---Close the channel.
function threading_channel:close() end

---@return "threading.channel"
---@nodiscard
function threading_channel:__tostring() end

---@class threadinglib
local threading = threading or package.loaded["threading"] or {}

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

---@param capacity? integer buffered slot count; defaults to 0.
---@return threading.channel channel
---@nodiscard
function threading.channel(capacity) end

return threading
