## GC2 worker parking slot helpers

GC2 parked worker bookkeeping uses helper accessors for the `worker_thread[]`
and `worker_tg[]` slots. The thread slots remain opaque pointers so `lj_obj.h`
does not need the platform thread type.

Worker initialization, start failure cleanup, TG slot release, worker stop, and
the worker scheduler fixture now use the helper surface. The M3 worker
scheduler notes document why raw production/fixture access to those parking slots.
