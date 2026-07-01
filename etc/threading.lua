---@meta threading

---@alias threading.timeout_error "timeout"
---@alias threading.thread_fn fun(...: any): ...
---@alias threading.recv_status true|false|threading.timeout_error
---@alias threading.peek_status true|false
---@alias threading.send_result true|nil
---@alias threading.gcmode "incremental"|"generational"
---@alias threading.gcstats_latency_buckets table<integer, number>

---@class threading.gcstats
---@field [string] number|threading.gcstats_latency_buckets
---@field total_bytes number
---@field total_kbytes number
---@field phase integer
---@field generational integer
---@field cycle_minor_requested integer
---@field cycle_sweep_minor integer
---@field minor_sweep_enabled integer
---@field cycle_roots_minor integer
---@field minor_roots_enabled integer
---@field cycle_requests number
---@field cycle_starts number
---@field major_cycle_starts number
---@field minor_cycle_requests number
---@field minor_cycle_starts number
---@field minor_sweep_deferred number
---@field minor_sweep_arenas number
---@field minor_roots_deferred number
---@field major_root_scans number
---@field minor_root_scans number
---@field minor_survival_base_live number
---@field minor_survival_bytes number
---@field minor_survival_pct integer
---@field minor_survival_threshold_pct integer
---@field minor_survival_major_requests number
---@field remembered_barriers number
---@field remembered_pushed number
---@field remembered_overflows number
---@field remembered_filtered number
---@field remembered_drained number
---@field poll_ack_samples number
---@field poll_ack_latency_sum_ns number
---@field poll_ack_latency_max_ns number
---@field poll_ack_latency_buckets threading.gcstats_latency_buckets
---@field alloc_since_trigger number
---@field cycle_alloc_bytes number
---@field trigger_bytes number
---@field hard_bytes number
---@field assist_runs number
---@field assist_grey_drained number
---@field assist_ssb_converted number
---@field assist_weak_drained number
---@field worker_runs number
---@field worker_grey_drained number
---@field worker_ssb_converted number
---@field worker_weak_drained number
---@field worker_idle_declares number
---@field worker_busy_retries number
---@field worker_wakes number
---@field worker_parks number
---@field worker_async_progress number
---@field sweep_owner_runs number
---@field sweep_owner_arenas number
---@field sweep_owner_live_cells number
---@field sweep_live_updates number
---@field sweep_live_huge_bytes number
---@field live_estimate number
---@field weak_clear_tables number
---@field weak_clear_cleared number
---@field weak_bridge_skipped number
---@field weak_bridge_fallbacks number
---@field weak_bridge_backfills number
---@field weak_bridge_backfill_tables number
---@field weak_bridge_backfill_slots number
---@field weak_bridge_backfill_cleared number
---@field weak_keys_marked number
---@field weak_values_marked number
---@field finreg_cdata_sets number
---@field finreg_cdata_clears number
---@field finreg_cdata_queued number
---@field finreg_cdata_sweep_queued number
---@field finreg_cdata_pweak_queued number
---@field finreg_cdata_pweak_claimed number
---@field finreg_cdata_preclaim_overflow number
---@field finreg_cdata_preclaim_dispatched number
---@field finreg_cdata_order_seen number
---@field finreg_cdata_order_claimed number
---@field finreg_cdata_order_unlinked number
---@field finreg_cdata_order_queued number
---@field finreg_cdata_order_retired number
---@field finreg_cdata_order_tombstones number
---@field finreg_cdata_order_fallbacks number
---@field finreg_cdata_pending_order_hits number
---@field finreg_udata_sets number
---@field finreg_udata_clears number
---@field finreg_udata_queued number
---@field finreg_udata_registered number
---@field finreg_udata_retired_nodes number
---@field finreg_udata_discovered number
---@field finreg_udata_forgets number
---@field finalizer_queued number
---@field finalizer_dequeued number
---@field finalizer_mpsc_drained number
---@field finalizer_enters number
---@field finalizer_leaves number
---@field finalizer_sweep_blocks number
---@field finalizer_spawn_deferrals number
---@field finalizer_spawn_release_wakes number

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
---@return any ... child results, an error object, or the timeout reason.
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
---@return nil
function threading_mutex:lock() end

---@return boolean locked
---@nodiscard
function threading_mutex:trylock() end

---Release the mutex.
---
---Errors if the mutex is not currently locked.
---@return nil
function threading_mutex:unlock() end

---@return "threading.mutex"
---@nodiscard
function threading_mutex:__tostring() end

---@class threading.channel<T>: userdata
local threading_channel = {}

---Send a value to the channel.
---
---Errors if the channel is closed. If this is a rendezvous channel, success
---means a receiver has taken the value.
---@overload fun(self: threading.channel<T>, value: T): true
---@overload fun(self: threading.channel<T>, value: T, timeout: number): true
---@overload fun(self: threading.channel<T>, value: T, timeout: number): nil, threading.timeout_error
---@param value T
---@param timeout? number seconds to wait; omitted blocks indefinitely.
---@return threading.send_result ok
---@return threading.timeout_error? err
function threading_channel:send(value, timeout) end

---Receive a value from the channel.
---
---Returns `value, true` on success, `nil, false` when the channel is closed, or
---`nil, "timeout"` when a timeout is supplied and no value is available.
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
---@overload fun(self: threading.channel<T>): T, true
---@overload fun(self: threading.channel<T>): nil, false
---@return T|nil value
---@return threading.peek_status status
function threading_channel:peek() end

---Close the channel.
---@return nil
function threading_channel:close() end

---@return "threading.channel"
---@nodiscard
function threading_channel:__tostring() end

---@class threading
local threading = {}

---@return integer count
---@nodiscard
function threading.cpucount() end

---Return a monotonic wall-clock timestamp in seconds.
---@return number|nil seconds nil if the platform clock failed.
---@nodiscard
function threading.now() end

---Issue a cross-thread memory fence.
---@return nil
function threading.fence() end

---@overload fun()
---@param seconds? number seconds to sleep; omitted defaults to 0.
---@return nil
function threading.sleep(seconds) end

---Return GC2/lockless runtime telemetry used by tests and benchmarks.
---@return threading.gcstats stats
---@nodiscard
function threading.gcstats() end

---Query or set the number of parked GC2 worker threads.
---@param count? integer
---@return integer old_count
function threading.gcworkers(count) end

---Query or set the GC2 collection mode.
---@param mode? threading.gcmode
---@return threading.gcmode old_mode
function threading.gcmode(mode) end

---Spawn a new OS thread that calls `fn(...)`.
---@param fn threading.thread_fn
---@param ... any
---@return threading.thread thread
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
---@overload fun<T>(): threading.channel<T>
---@overload fun<T>(capacity: integer): threading.channel<T>
---@param capacity? integer buffered slot count; omitted defaults to 0.
---@return threading.channel<T> channel
function threading.channel(capacity) end

return threading
