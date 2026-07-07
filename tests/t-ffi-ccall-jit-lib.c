/*
** Shared library for traced FFI C-call native-state probes.
*/

#include <stdint.h>

#include "lib/test_sleep.h"

static int lj_m7_ccall_jit_values[4] = { 11, 22, 33, 44 };
static int lj_m7_ccall_jit_void_count;

int lj_m7_ccall_jit_sleep_i32(int ms)
{
  if (ms > 0)
    sleep_ns((long)ms * 1000000L);
  return ms + 7;
}

int lj_m7_ccall_jit_add2_i32(int a, int b)
{
  return a + b + 3;
}

int64_t lj_m7_ccall_jit_i64_0(void)
{
  return 17;
}

int64_t lj_m7_ccall_jit_i64_i32(int32_t a)
{
  return INT64_C(0x100000000) + (int64_t)a;
}

int64_t lj_m7_ccall_jit_i64_ptr(int *p)
{
  return INT64_C(0x100000000) + (int64_t)*p;
}

int64_t lj_m7_ccall_jit_i64_i32_ptr(int32_t offset, int *p)
{
  return INT64_C(0x100000000) + (int64_t)p[offset];
}

int64_t lj_m7_ccall_jit_i64_i64(int64_t a)
{
  return a + 1;
}

int64_t lj_m7_ccall_jit_i64_i64_i64(int64_t a, int64_t b)
{
  return a + b + 3;
}

int64_t lj_m7_ccall_jit_i64_u64(uint64_t a)
{
  return a > UINT64_MAX - UINT64_C(16) ? -12 : (int64_t)a;
}

int64_t lj_m7_ccall_jit_i64_i64_u64(int64_t a, uint64_t b)
{
  return a + (int64_t)(b & UINT64_C(255)) + 5;
}

int64_t lj_m7_ccall_jit_i64_u64_i64(uint64_t a, int64_t b)
{
  return (int64_t)(a & UINT64_C(255)) + b + 3;
}

int64_t lj_m7_ccall_jit_i64_i32_ptr_u64(int32_t bias, int *p, uint64_t n)
{
  return INT64_C(0x100000000) + (int64_t)bias + (int64_t)p[n & 3u] +
	 (int64_t)(n & UINT64_C(1023));
}

int64_t lj_m7_ccall_jit_i64_i32_ptr_u64_i64(int32_t fd, int *p, uint64_t n,
					    int64_t offset)
{
  return INT64_C(0x100000000) + (int64_t)fd + (int64_t)p[n & 3u] +
	 (int64_t)(n & UINT64_C(1023)) +
	 (int64_t)((uint64_t)offset & UINT64_C(1023));
}

int64_t lj_m7_ccall_jit_i64_i32_i64_i32(int32_t fd, int64_t offset,
					int32_t whence)
{
  return INT64_C(0x100000000) + (int64_t)fd + offset + (int64_t)whence;
}

int32_t lj_m7_ccall_jit_i32_i32_ptr_u32(int32_t bias, int *p, uint32_t n)
{
  return bias + p[n & 3u] + (int32_t)(n & 1023u);
}

uint32_t lj_m7_ccall_jit_u32_i32_ptr_u32(int32_t bias, int *p, uint32_t n)
{
  return UINT32_C(0x80000000) + (uint32_t)bias + (uint32_t)p[n & 3u] +
	 (n & 1023u);
}

uint32_t lj_m7_ccall_jit_u32_u32_ptr_i32_u32(uint32_t count, int *handles,
					     int32_t wait_all,
					     uint32_t timeout)
{
  uint64_t acc = (uint64_t)count + (uint64_t)(uint32_t)handles[timeout & 3u] +
		 (uint64_t)(uint32_t)wait_all + (uint64_t)timeout;
  return UINT32_C(0x80000000) + (uint32_t)(acc & UINT64_C(1023));
}

uint32_t lj_m7_ccall_jit_u32_u32_ptr_i32_u32_i32(uint32_t count,
						 int *handles,
						 int32_t wait_all,
						 uint32_t timeout,
						 int32_t alertable)
{
  uint64_t acc = (uint64_t)count + (uint64_t)(uint32_t)handles[timeout & 3u] +
		 (uint64_t)(uint32_t)wait_all + (uint64_t)timeout +
		 (uint64_t)(uint32_t)alertable;
  return UINT32_C(0x80000000) + (uint32_t)(acc & UINT64_C(1023));
}

int32_t lj_m7_ccall_jit_i32_ptr_i32_u64(int *p, int32_t bias, uint64_t n)
{
  return p[n & 3u] + bias + (int32_t)(n & UINT64_C(1023));
}

uint32_t lj_m7_ccall_jit_u32_ptr_i32_u64(int *p, int32_t bias, uint64_t n)
{
  return UINT32_C(0x80000000) + (uint32_t)p[n & 3u] + (uint32_t)bias +
	 (uint32_t)(n & UINT64_C(1023));
}

int64_t lj_m7_ccall_jit_i64_ptr_i32_u64(int *p, int32_t bias, uint64_t n)
{
  return INT64_C(0x100000000) + (int64_t)p[n & 3u] + (int64_t)bias +
	 (int64_t)(n & UINT64_C(1023));
}

uint64_t lj_m7_ccall_jit_u64_ptr_i32_u64(int *p, int32_t bias, uint64_t n)
{
  return UINT64_C(0x100000000) + (uint64_t)p[n & 3u] + (uint64_t)bias +
	 (uint64_t)(n & UINT64_C(1023));
}

int *lj_m7_ccall_jit_ptr_ptr_i32_u64(int *dst, int32_t bias, uint64_t n)
{
  uint64_t i = n & 3u;
  dst[i] = bias + (int32_t)(n & UINT64_C(15));
  return dst + i;
}

void lj_m7_ccall_jit_void_ptr_i32_u64(int *dst, int32_t bias, uint64_t n)
{
  uint64_t i = n & 3u;
  dst[i] += bias + (int32_t)(n & UINT64_C(15));
}

int32_t lj_m7_ccall_jit_i32_ptr_i32_u32(int *p, int32_t bias, uint32_t n)
{
  return p[n & 3u] + bias + (int32_t)(n & 1023u);
}

uint32_t lj_m7_ccall_jit_u32_ptr_i32_u32(int *p, int32_t bias, uint32_t n)
{
  return UINT32_C(0x80000000) + (uint32_t)p[n & 3u] + (uint32_t)bias +
	 (n & 1023u);
}

int64_t lj_m7_ccall_jit_i64_ptr_i32_u32(int *p, int32_t bias, uint32_t n)
{
  return INT64_C(0x100000000) + (int64_t)p[n & 3u] + (int64_t)bias +
	 (int64_t)n;
}

uint64_t lj_m7_ccall_jit_u64_ptr_i32_u32(int *p, int32_t bias, uint32_t n)
{
  return UINT64_C(0x100000000) + (uint64_t)p[n & 3u] + (uint64_t)bias +
	 (uint64_t)n;
}

int *lj_m7_ccall_jit_ptr_ptr_i32_u32(int *dst, int32_t bias, uint32_t n)
{
  uint32_t i = n & 3u;
  dst[i] = bias + (int)(n & 15u);
  return dst + i;
}

void lj_m7_ccall_jit_void_ptr_i32_u32(int *dst, int32_t bias, uint32_t n)
{
  uint32_t i = n & 3u;
  dst[i] += bias + (int)(n & 15u);
}

int32_t lj_m7_ccall_jit_i32_ptr_u64_i32(int *p, uint64_t n, int32_t bias)
{
  return p[n & 3u] + bias + (int32_t)(n & UINT64_C(1023));
}

uint32_t lj_m7_ccall_jit_u32_ptr_u64_i32(int *p, uint64_t n, int32_t bias)
{
  return UINT32_C(0x80000000) + (uint32_t)p[n & 3u] + (uint32_t)bias +
	 (uint32_t)(n & UINT64_C(1023));
}

int64_t lj_m7_ccall_jit_i64_ptr_u64_i32(int *p, uint64_t n, int32_t bias)
{
  return INT64_C(0x100000000) + (int64_t)p[n & 3u] + (int64_t)bias +
	 (int64_t)(n & UINT64_C(1023));
}

uint64_t lj_m7_ccall_jit_u64_ptr_u64_i32(int *p, uint64_t n, int32_t bias)
{
  return UINT64_C(0x100000000) + (uint64_t)p[n & 3u] + (uint64_t)bias +
	 (uint64_t)(n & UINT64_C(1023));
}

int *lj_m7_ccall_jit_ptr_ptr_u64_i32(int *dst, uint64_t n, int32_t bias)
{
  uint64_t i = n & 3u;
  dst[i] = bias + (int32_t)(n & UINT64_C(15));
  return dst + i;
}

void lj_m7_ccall_jit_void_ptr_u64_i32(int *dst, uint64_t n, int32_t bias)
{
  uint64_t i = n & 3u;
  dst[i] += bias + (int32_t)(n & UINT64_C(15));
}

int32_t lj_m7_ccall_jit_i32_ptr_u64_u32(int *p, uint64_t n, uint32_t flags)
{
  return p[n & 3u] + (int32_t)(n & UINT64_C(1023)) +
	 (int32_t)(flags & 255u) + (int32_t)(flags >> 28);
}

int32_t lj_m7_ccall_jit_i32_ptr_u64_ptr(int *a, uint64_t n, int *b)
{
  return a[n & 3u] + b[(n + UINT64_C(1)) & 3u] +
	 (int32_t)(n & UINT64_C(1023));
}

uint32_t lj_m7_ccall_jit_u32_ptr_u64_ptr(int *a, uint64_t n, int *b)
{
  return UINT32_C(0x80000000) + (uint32_t)a[n & 3u] +
	 (uint32_t)b[(n + UINT64_C(1)) & 3u] +
	 (uint32_t)(n & UINT64_C(1023));
}

int64_t lj_m7_ccall_jit_i64_ptr_u64_ptr(int *a, uint64_t n, int *b)
{
  return INT64_C(0x100000000) + (int64_t)a[n & 3u] +
	 (int64_t)b[(n + UINT64_C(1)) & 3u] +
	 (int64_t)(n & UINT64_C(1023));
}

uint64_t lj_m7_ccall_jit_u64_ptr_u64_ptr(int *a, uint64_t n, int *b)
{
  return UINT64_C(0x100000000) + (uint64_t)a[n & 3u] +
	 (uint64_t)b[(n + UINT64_C(1)) & 3u] +
	 (uint64_t)(n & UINT64_C(1023));
}

int *lj_m7_ccall_jit_ptr_ptr_u64_ptr(int *dst, uint64_t n, int *src)
{
  uint64_t i = n & 3u;
  dst[i] = src[(n + UINT64_C(1)) & 3u] + (int)(n & UINT64_C(15));
  return dst + i;
}

void lj_m7_ccall_jit_void_ptr_u64_ptr(int *dst, uint64_t n, int *src)
{
  uint64_t i = n & 3u;
  dst[i] += src[(n + UINT64_C(1)) & 3u] + (int)(n & UINT64_C(15));
}

int32_t lj_m7_ccall_jit_i32_ptr_u64_u32_ptr(int *src, uint64_t n,
					    uint32_t flags, int *dst)
{
  uint32_t i = flags & 3u;
  dst[i] = src[n & 3u] + (int)(flags & 15u);
  return dst[i] + (int32_t)(n & UINT64_C(1023)) + (int32_t)(flags >> 28);
}

int32_t lj_m7_ccall_jit_i32_ptr_u32_u64_ptr(int *port, uint32_t bytes,
					    uint64_t key, int *overlapped)
{
  uint32_t i = bytes & 3u;
  overlapped[i] = port[key & 3u] + (int)(bytes & 15u) +
		  (int)(key & UINT64_C(15));
  return overlapped[i] + (int32_t)(bytes >> 28) +
	 (int32_t)(key & UINT64_C(1023));
}

int *lj_m7_ccall_jit_ptr_ptr_ptr_u64_u32(int *dst, int *port, uint64_t key,
					 uint32_t threads)
{
  uint64_t i = (key + (uint64_t)threads) & 3u;
  dst[i] = port[(key + UINT64_C(1)) & 3u] + (int)(key & UINT64_C(15)) +
	   (int)(threads & 15u);
  return dst + i;
}

int *lj_m7_ccall_jit_ptr_ptr_ptr_u32_u32(int *security, int *name,
					 uint32_t flags, uint32_t access)
{
  return security + (((uint64_t)(uint32_t)(name ? *name : 0) +
		      (uint64_t)flags + (uint64_t)access) & 3u);
}

int *lj_m7_ccall_jit_ptr_ptr_u64_u32_u32(int *base, uint64_t n,
					 uint32_t alloc_type,
					 uint32_t protect)
{
  return base + ((n + (uint64_t)alloc_type + (uint64_t)protect) & 3u);
}

int *lj_m7_ccall_jit_ptr_ptr_u64_i32_i32_i32_i64(int *base, uint64_t n,
						 int32_t prot, int32_t flags,
						 int32_t fd, int64_t offset)
{
  return base + ((n + (uint64_t)(uint32_t)prot +
		  (uint64_t)(uint32_t)flags + (uint64_t)(uint32_t)fd +
		  (uint64_t)offset) & 3u);
}

int *lj_m7_ccall_jit_ptr_ptr_u64_u64_i32(int *base, uint64_t old_size,
					 uint64_t new_size, int32_t flags)
{
  return base + ((old_size + new_size + (uint64_t)(uint32_t)flags) & 3u);
}

int *lj_m7_ccall_jit_ptr_ptr_u32_u32_u32_u64(int *base, uint32_t access,
					     uint32_t off_hi, uint32_t off_lo,
					     uint64_t bytes)
{
  return base + (((uint64_t)access + (uint64_t)off_hi + (uint64_t)off_lo +
		  bytes) & 3u);
}

int *lj_m7_ccall_jit_ptr_ptr_u32_u32_u32_u64_ptr(int *base, uint32_t access,
						 uint32_t off_hi,
						 uint32_t off_lo,
						 uint64_t bytes,
						 int *desired)
{
  return base + (((uint64_t)access + (uint64_t)off_hi + (uint64_t)off_lo +
		  bytes + (uint64_t)(uint32_t)(desired ? *desired : 0)) & 3u);
}

int *lj_m7_ccall_jit_ptr_ptr_u32_u32_ptr_u32_u32_ptr(int *name,
						     uint32_t access,
						     uint32_t share,
						     int *security,
						     uint32_t disposition,
						     uint32_t flags,
						     int *template_handle)
{
  return name + (((uint64_t)access + (uint64_t)share +
		  (uint64_t)(uint32_t)(security ? *security : 0) +
		  (uint64_t)disposition + (uint64_t)flags +
		  (uint64_t)(uint32_t)(template_handle ? *template_handle : 0)) &
		 3u);
}

int *lj_m7_ccall_jit_ptr_ptr_ptr_u32_u32_u32_ptr(int *handle, int *security,
						 uint32_t protect,
						 uint32_t max_hi,
						 uint32_t max_lo, int *name)
{
  return handle + (((uint64_t)(uint32_t)(security ? *security : 0) +
		    (uint64_t)protect + (uint64_t)max_hi + (uint64_t)max_lo +
		    (uint64_t)(uint32_t)(name ? *name : 0)) & 3u);
}

int *lj_m7_ccall_jit_ptr_ptr_u64_ptr_ptr_u32_ptr(int *security,
						 uint64_t stack_size,
						 int *start, int *param,
						 uint32_t flags,
						 int *thread_id)
{
  return security + ((stack_size + (uint64_t)(uint32_t)(start ? *start : 0) +
		      (uint64_t)(uint32_t)(param ? *param : 0) +
		      (uint64_t)flags +
		      (uint64_t)(uint32_t)(thread_id ? *thread_id : 0)) & 3u);
}

int *lj_m7_ccall_jit_ptr_ptr_i32_i32_ptr(int *security, int32_t manual,
					 int32_t initial, int *name)
{
  return security + (((uint64_t)(uint32_t)manual +
		      (uint64_t)(uint32_t)initial +
		      (uint64_t)(uint32_t)(name ? *name : 0)) & 3u);
}

int *lj_m7_ccall_jit_ptr_ptr_i32_i32_ptr_u32_u32(int *security,
						 int32_t initial,
						 int32_t maximum, int *name,
						 uint32_t flags,
						 uint32_t access)
{
  return security + (((uint64_t)(uint32_t)initial +
		      (uint64_t)(uint32_t)maximum +
		      (uint64_t)(uint32_t)(name ? *name : 0) +
		      (uint64_t)flags + (uint64_t)access) & 3u);
}

int *lj_m7_ccall_jit_ptr_ptr_i32_ptr(int *security, int32_t inherit,
				     int *name)
{
  return security + (((uint64_t)(uint32_t)inherit +
		      (uint64_t)(uint32_t)(name ? *name : 0)) & 3u);
}

int *lj_m7_ccall_jit_ptr_u32_i32_ptr(uint32_t access, int32_t inherit,
				     int *name)
{
  int *base = name ? name : lj_m7_ccall_jit_values;
  return base + (((uint64_t)access + (uint64_t)(uint32_t)inherit) & 3u);
}

int32_t lj_m7_ccall_jit_i32_ptr_u32_u32(int *handle, uint32_t mask,
					uint32_t flags)
{
  return (int32_t)(((uint64_t)(uint32_t)(handle ? handle[0] : 0) +
		    (uint64_t)mask + (uint64_t)flags) &
		   UINT64_C(1023)) + 19;
}

uint32_t lj_m7_ccall_jit_u32_ptr_u32(int *handle, uint32_t timeout)
{
  return UINT32_C(0x80000000) + (uint32_t)handle[timeout & 3u] +
	 (timeout & 1023u);
}

int32_t lj_m7_ccall_jit_i32_ptr_ptr_ptr_ptr_u32_i32_u32(int *src_proc,
							int *src_handle,
							int *target_proc,
							int *target_handle,
							uint32_t access,
							int32_t inherit,
							uint32_t options)
{
  uint64_t acc = (uint64_t)(uint32_t)(src_proc ? src_proc[0] : 0) +
		 (uint64_t)(uint32_t)(src_handle ? src_handle[1] : 0) +
		 (uint64_t)(uint32_t)(target_proc ? target_proc[2] : 0) +
		 (uint64_t)(uint32_t)(target_handle ? target_handle[3] : 0) +
		 (uint64_t)access + (uint64_t)(uint32_t)inherit +
		 (uint64_t)options;
  return (int32_t)((acc & UINT64_C(1023)) + UINT64_C(13));
}

int32_t lj_m7_ccall_jit_i32_ptr_ptr_u64(int *a, int *b, uint64_t n)
{
  return a[n & 3u] + b[(n + UINT64_C(1)) & 3u] +
	 (int32_t)(n & UINT64_C(1023));
}

int32_t lj_m7_ccall_jit_i32_ptr_ptr_u64_u32(int *addr, int *compare,
					    uint64_t size,
					    uint32_t timeout)
{
  uint32_t i = (uint32_t)((size + (uint64_t)timeout) & UINT64_C(3));
  return addr[i] + compare[(i + 1u) & 3u] +
	 (int32_t)(size & UINT64_C(1023)) + (int32_t)(timeout & 1023u) +
	 (int32_t)(size >> 60) + (int32_t)(timeout >> 28);
}

uint32_t lj_m7_ccall_jit_u32_ptr_ptr_u64(int *a, int *b, uint64_t n)
{
  return UINT32_C(0x80000000) + (uint32_t)a[n & 3u] +
	 (uint32_t)b[(n + UINT64_C(1)) & 3u] +
	 (uint32_t)(n & UINT64_C(1023));
}

int32_t lj_m7_ccall_jit_i32_ptr_ptr_u32(int *a, int *b, uint32_t n)
{
  return a[n & 3u] + b[(n + 1u) & 3u] + (int32_t)(n & 1023u);
}

int32_t lj_m7_ccall_jit_i32_ptr_ptr_u32_u32(int *cond, int *lock,
					    uint32_t timeout,
					    uint32_t flags)
{
  uint32_t i = (timeout + flags) & 3u;
  return cond[i] + lock[(i + 1u) & 3u] + (int32_t)(timeout & 15u) +
	 (int32_t)(flags & 15u) + (int32_t)(timeout >> 28) +
	 (int32_t)(flags >> 28);
}

int32_t lj_m7_ccall_jit_i32_ptr_ptr_ptr_ptr_u32(int *port, int *bytes,
						int *key, int *overlapped,
						uint32_t timeout)
{
  uint32_t i = timeout & 3u;
  bytes[i] = port[(timeout + 1u) & 3u] + (int)(timeout & 15u);
  key[i] = bytes[i] + 3;
  overlapped[i] = key[i] + 5;
  return bytes[i] + key[i] + overlapped[i] + (int32_t)(timeout >> 28);
}

uint32_t lj_m7_ccall_jit_u32_ptr_ptr_u32(int *a, int *b, uint32_t n)
{
  return UINT32_C(0x80000000) + (uint32_t)a[n & 3u] +
	 (uint32_t)b[(n + 1u) & 3u] + (n & 1023u);
}

uint32_t lj_m7_ccall_jit_u32_ptr_ptr_u32_i32(int *signal, int *wait,
					     uint32_t timeout,
					     int32_t alertable)
{
  uint64_t acc = (uint64_t)(uint32_t)signal[timeout & 3u] +
		 (uint64_t)(uint32_t)wait[(timeout + 1u) & 3u] +
		 (uint64_t)timeout + (uint64_t)(uint32_t)alertable;
  return UINT32_C(0x80000000) + (uint32_t)(acc & UINT64_C(1023));
}

int32_t lj_m7_ccall_jit_i32_ptr_ptr_u32_ptr_ptr(int *handle, int *buf,
						uint32_t n, int *out,
						int *overlapped)
{
  uint32_t i = n & 3u;
  if (out)
    out[i] = buf[i] + (int)(n & 15u);
  return handle[i] + buf[(i + 1u) & 3u] + (out ? out[i] : 0) +
	 (overlapped ? overlapped[(i + 2u) & 3u] : 0);
}

int32_t lj_m7_ccall_jit_i32_ptr_ptr_u32_ptr_u32_i32(int *port, int *entries,
						    uint32_t count,
						    int *removed,
						    uint32_t timeout,
						    int32_t alertable)
{
  uint32_t i = (count + timeout) & 3u;
  entries[i] = port[timeout & 3u] + (int)(count & 15u) +
	       (int)(timeout & 15u);
  removed[i] = entries[i] + (int)((uint32_t)alertable & 15u);
  return entries[i] + removed[i] + (int32_t)(count >> 28) +
	 (int32_t)(timeout >> 28) + alertable;
}

int32_t lj_m7_ccall_jit_i32_ptr_u32_ptr_u32_ptr_u32_ptr_ptr(
  int *handle, uint32_t code, int *inbuf, uint32_t insize, int *outbuf,
  uint32_t outsize, int *bytes_ret, int *overlapped)
{
  uint32_t i = (code + insize + outsize) & 3u;
  if (outbuf)
    outbuf[i] = handle[i] + (int)(code & 15u);
  if (bytes_ret)
    bytes_ret[i] = inbuf[i] + (int)(outsize & 15u);
  return handle[i] + inbuf[(i + 1u) & 3u] + (outbuf ? outbuf[i] : 0) +
	 (bytes_ret ? bytes_ret[i] : 0) +
	 (overlapped ? overlapped[(i + 2u) & 3u] : 0) +
	 (int)(code >> 28) + (int)(insize >> 28);
}

int32_t lj_m7_ccall_jit_i32_ptr_ptr_ptr_i32(int *handle, int *overlapped,
					    int *out, int32_t wait)
{
  if (out)
    out[1] = overlapped[2] + wait;
  return handle[0] + overlapped[1] + (out ? out[1] : 0) + wait;
}

int32_t lj_m7_ccall_jit_i32_ptr_ptr_ptr_u32_i32(int *handle,
						int *overlapped, int *out,
						uint32_t timeout,
						int32_t alertable)
{
  uint32_t i = timeout & 3u;
  if (out)
    out[i] = overlapped[(i + 1u) & 3u] + (int)(timeout & 15u) + alertable;
  return handle[i] + overlapped[(i + 2u) & 3u] + (out ? out[i] : 0) +
	 (int32_t)(timeout >> 28) + alertable;
}

int32_t lj_m7_ccall_jit_i32_ptr_ptr_i32(int *a, int *b, int32_t n)
{
  uint32_t u = (uint32_t)n;
  return a[u & 3u] + b[(u + 1u) & 3u] + n;
}

uint32_t lj_m7_ccall_jit_u32_ptr_ptr_i32(int *a, int *b, int32_t n)
{
  uint32_t u = (uint32_t)n;
  return UINT32_C(0x80000000) + (uint32_t)a[u & 3u] +
	 (uint32_t)b[(u + 1u) & 3u] + (uint32_t)n;
}

int64_t lj_m7_ccall_jit_i64_ptr_ptr_i32(const char *p, char **endp,
					int32_t base)
{
  if (endp)
    *endp = (char *)p + 1;
  return INT64_C(0x100000000) + (int64_t)(unsigned char)p[0] +
	 (int64_t)base + 1;
}

uint64_t lj_m7_ccall_jit_u64_ptr_ptr_i32(int *a, int *b, int32_t n)
{
  uint32_t u = (uint32_t)n;
  return UINT64_C(0x100000000) + (uint64_t)a[u & 3u] +
	 (uint64_t)b[(u + 1u) & 3u] + (uint64_t)(uint32_t)n;
}

int64_t lj_m7_ccall_jit_i64_ptr_ptr_u32(int *a, int *b, uint32_t n)
{
  return INT64_C(0x100000000) + (int64_t)a[n & 3u] +
	 (int64_t)b[(n + 1u) & 3u] + (int64_t)n;
}

uint64_t lj_m7_ccall_jit_u64_ptr_ptr_u32(int *a, int *b, uint32_t n)
{
  return UINT64_C(0x100000000) + (uint64_t)a[n & 3u] +
	 (uint64_t)b[(n + 1u) & 3u] + (uint64_t)n;
}

int64_t lj_m7_ccall_jit_i64_ptr_ptr_u64(int *a, int *b, uint64_t n)
{
  return INT64_C(0x100000000) + (int64_t)a[n & 3u] +
	 (int64_t)b[(n + UINT64_C(1)) & 3u] +
	 (int64_t)(n & UINT64_C(1023));
}

uint64_t lj_m7_ccall_jit_u64_ptr_ptr_u64(int *a, int *b, uint64_t n)
{
  return UINT64_C(0x100000000) + (uint64_t)a[n & 3u] +
	 (uint64_t)b[(n + UINT64_C(1)) & 3u] +
	 (n & UINT64_C(1023));
}

int *lj_m7_ccall_jit_ptr_ptr_ptr_u64(int *dst, int *src, uint64_t n)
{
  uint64_t i = n & 3u;
  dst[i] = src[(n + UINT64_C(1)) & 3u] + (int)(n & UINT64_C(15));
  return dst + i;
}

int *lj_m7_ccall_jit_ptr_ptr_ptr_i32(int *dst, int *src, int32_t n)
{
  uint32_t u = (uint32_t)n;
  uint32_t i = u & 3u;
  dst[i] = src[(u + 1u) & 3u] + (int)(u & 15u);
  return dst + i;
}

int *lj_m7_ccall_jit_ptr_ptr_ptr_u32(int *dst, int *src, uint32_t n)
{
  uint32_t i = n & 3u;
  dst[i] = src[(n + 1u) & 3u] + (int)(n & 15u);
  return dst + i;
}

void lj_m7_ccall_jit_void_ptr_ptr_i32(int *dst, int *src, int32_t n)
{
  uint32_t u = (uint32_t)n;
  uint32_t i = u & 3u;
  dst[i] += src[(u + 1u) & 3u] + (int)(u & 15u);
}

void lj_m7_ccall_jit_void_ptr_ptr_u32(int *dst, int *src, uint32_t n)
{
  uint32_t i = n & 3u;
  dst[i] += src[(n + 1u) & 3u] + (int)(n & 15u);
}

void lj_m7_ccall_jit_void_ptr_ptr_u64(int *dst, int *src, uint64_t n)
{
  uint64_t i = n & 3u;
  dst[i] += src[(n + UINT64_C(1)) & 3u] + (int)(n & UINT64_C(15));
}

int8_t lj_m7_ccall_jit_i8_0(void)
{
  return -7;
}

int8_t lj_m7_ccall_jit_i8_i32(int32_t a)
{
  return (int8_t)(a - 8);
}

uint8_t lj_m7_ccall_jit_u8_0(void)
{
  return 250;
}

uint8_t lj_m7_ccall_jit_u8_ptr(int *p)
{
  return (uint8_t)(*p + 200);
}

int16_t lj_m7_ccall_jit_i16_0(void)
{
  return -1234;
}

int16_t lj_m7_ccall_jit_i16_i32_ptr(int32_t offset, int *p)
{
  return (int16_t)(p[offset] - 2000);
}

uint16_t lj_m7_ccall_jit_u16_0(void)
{
  return 60000;
}

uint16_t lj_m7_ccall_jit_u16_i32(int32_t a)
{
  return (uint16_t)(60000 + a);
}

int lj_m7_ccall_jit_i8_arg_i32(int8_t a)
{
  return (int)a + 5;
}

int32_t lj_m7_ccall_jit_i32_u32(uint32_t a)
{
  return (int32_t)(a + 5u);
}

int32_t lj_m7_ccall_jit_i32_u32_ptr(uint32_t offset, int *p)
{
  return p[offset & 3u] + (int32_t)(offset & 1023u);
}

int32_t lj_m7_ccall_jit_i32_i64arg(int64_t a)
{
  return (int32_t)(a & INT64_C(1023)) - 7;
}

int32_t lj_m7_ccall_jit_i32_u64arg(uint64_t a)
{
  return (int32_t)(a & UINT64_C(1023)) + 9;
}

uint8_t lj_m7_ccall_jit_u8_u32(uint32_t a)
{
  return (uint8_t)(a + 3u);
}

uint8_t lj_m7_ccall_jit_u8_i64arg(int64_t a)
{
  return (uint8_t)(a + 5);
}

uint8_t lj_m7_ccall_jit_u8_ptr_u64(int *p, uint64_t n)
{
  return (uint8_t)(p[n & 3u] + (int)(n & UINT64_C(255)));
}

uint8_t lj_m7_ccall_jit_u8_ptr_i64(int *p, int64_t n)
{
  uint64_t u = (uint64_t)n;
  return (uint8_t)(p[u & 3u] + (int)(u & UINT64_C(255)));
}

uint8_t lj_m7_ccall_jit_u8_i32_i64(int32_t a, int64_t b)
{
  return (uint8_t)(a + (int32_t)((uint64_t)b & UINT64_C(255)));
}

uint8_t lj_m7_ccall_jit_u8_i32_u64(int32_t a, uint64_t b)
{
  return (uint8_t)(a + (int32_t)(b & UINT64_C(255)));
}

uint8_t lj_m7_ccall_jit_u8_u32_i64(uint32_t a, int64_t b)
{
  return (uint8_t)((a & 255u) + (uint32_t)((uint64_t)b & UINT64_C(255)));
}

uint8_t lj_m7_ccall_jit_u8_u32_u64(uint32_t a, uint64_t b)
{
  return (uint8_t)((a & 255u) + (uint32_t)(b & UINT64_C(255)));
}

void lj_m7_ccall_jit_void_u32(uint32_t a)
{
  lj_m7_ccall_jit_void_count += (int)(a & 15u);
}

void lj_m7_ccall_jit_void_i64arg(int64_t a)
{
  lj_m7_ccall_jit_void_count += (int)(a & INT64_C(31));
}

void lj_m7_ccall_jit_void_u64arg(uint64_t a)
{
  lj_m7_ccall_jit_void_count += (int)(a & UINT64_C(31));
}

void lj_m7_ccall_jit_void_ptr_u64(int *p, uint64_t n)
{
  lj_m7_ccall_jit_void_count += p[n & 3u] + (int)(n & UINT64_C(15));
}

void lj_m7_ccall_jit_void_ptr_i64(int *p, int64_t n)
{
  uint64_t u = (uint64_t)n;
  lj_m7_ccall_jit_void_count += p[u & 3u] + (int)(u & UINT64_C(15));
}

void lj_m7_ccall_jit_void_i32_i64(int32_t a, int64_t b)
{
  lj_m7_ccall_jit_void_count += a + (int32_t)((uint64_t)b & UINT64_C(15));
}

void lj_m7_ccall_jit_void_i32_u64(int32_t a, uint64_t b)
{
  lj_m7_ccall_jit_void_count += a + (int32_t)(b & UINT64_C(15));
}

void lj_m7_ccall_jit_void_u32_i64(uint32_t a, int64_t b)
{
  lj_m7_ccall_jit_void_count += (int)(a & 15u) +
				(int)((uint64_t)b & UINT64_C(15));
}

void lj_m7_ccall_jit_void_u32_u64(uint32_t a, uint64_t b)
{
  lj_m7_ccall_jit_void_count += (int)(a & 15u) + (int)(b & UINT64_C(15));
}

uint64_t lj_m7_ccall_jit_u64_u32arg(uint32_t a)
{
  return UINT64_C(0x100000000) + (uint64_t)a;
}

uint32_t lj_m7_ccall_jit_u32_u64arg(uint64_t a)
{
  return (uint32_t)(a + UINT64_C(0xf0000000));
}

int32_t lj_m7_ccall_jit_i32_ptr_u64(int *p, uint64_t n)
{
  return p[n & 3u] + (int32_t)(n & UINT64_C(1023));
}

int32_t lj_m7_ccall_jit_i32_ptr_i64(int *p, int64_t n)
{
  uint64_t u = (uint64_t)n;
  return p[u & 3u] + (int32_t)(u & UINT64_C(1023));
}

int32_t lj_m7_ccall_jit_i32_i32_i64(int32_t a, int64_t b)
{
  return a + (int32_t)((uint64_t)b & UINT64_C(1023));
}

int32_t lj_m7_ccall_jit_i32_i32_u64(int32_t a, uint64_t b)
{
  return a + (int32_t)(b & UINT64_C(1023));
}

int32_t lj_m7_ccall_jit_i32_u32_i64(uint32_t a, int64_t b)
{
  return (int32_t)((a & 1023u) +
		   (uint32_t)((uint64_t)b & UINT64_C(1023)));
}

int32_t lj_m7_ccall_jit_i32_u32_u64(uint32_t a, uint64_t b)
{
  return (int32_t)((a & 1023u) + (uint32_t)(b & UINT64_C(1023)));
}

uint32_t lj_m7_ccall_jit_u32_ptr_u64(int *p, uint64_t n)
{
  return UINT32_C(0xf0000000) + (uint32_t)p[n & 3u] +
	 (uint32_t)(n & UINT64_C(1023));
}

uint32_t lj_m7_ccall_jit_u32_ptr_i64(int *p, int64_t n)
{
  uint64_t u = (uint64_t)n;
  return UINT32_C(0xf0000000) + (uint32_t)p[u & 3u] +
	 (uint32_t)(u & UINT64_C(1023));
}

uint32_t lj_m7_ccall_jit_u32_i32_i64(int32_t a, int64_t b)
{
  return UINT32_C(0xf0000000) + (uint32_t)a +
	 (uint32_t)((uint64_t)b & UINT64_C(1023));
}

uint32_t lj_m7_ccall_jit_u32_i32_u64(int32_t a, uint64_t b)
{
  return UINT32_C(0xf0000000) + (uint32_t)a +
	 (uint32_t)(b & UINT64_C(1023));
}

uint32_t lj_m7_ccall_jit_u32_u32_i64(uint32_t a, int64_t b)
{
  return UINT32_C(0xf0000000) + (a & 1023u) +
	 (uint32_t)((uint64_t)b & UINT64_C(1023));
}

uint32_t lj_m7_ccall_jit_u32_u32_u64(uint32_t a, uint64_t b)
{
  return UINT32_C(0xf0000000) + (a & 1023u) +
	 (uint32_t)(b & UINT64_C(1023));
}

uint8_t lj_m7_ccall_jit_u8_i64_i32(int64_t a, int32_t b)
{
  return (uint8_t)(((uint64_t)a & UINT64_C(255)) + (uint32_t)b);
}

uint8_t lj_m7_ccall_jit_u8_i64_u32(int64_t a, uint32_t b)
{
  return (uint8_t)(((uint64_t)a & UINT64_C(255)) + (b & 255u));
}

void lj_m7_ccall_jit_void_i64_i32(int64_t a, int32_t b)
{
  lj_m7_ccall_jit_void_count += (int)((uint64_t)a & UINT64_C(15)) +
				(int)(b & 15);
}

void lj_m7_ccall_jit_void_i64_u32(int64_t a, uint32_t b)
{
  lj_m7_ccall_jit_void_count += (int)((uint64_t)a & UINT64_C(15)) +
				(int)(b & 15u);
}

int32_t lj_m7_ccall_jit_i32_i64_i32(int64_t a, int32_t b)
{
  return (int32_t)(((uint64_t)a & UINT64_C(1023)) + (uint32_t)b);
}

int32_t lj_m7_ccall_jit_i32_i64_u32(int64_t a, uint32_t b)
{
  return (int32_t)(((uint64_t)a & UINT64_C(1023)) + (b & 1023u));
}

uint32_t lj_m7_ccall_jit_u32_i64_i32(int64_t a, int32_t b)
{
  return UINT32_C(0xf0000000) +
	 (uint32_t)((uint64_t)a & UINT64_C(1023)) + (uint32_t)b;
}

uint32_t lj_m7_ccall_jit_u32_i64_u32(int64_t a, uint32_t b)
{
  return UINT32_C(0xf0000000) +
	 (uint32_t)((uint64_t)a & UINT64_C(1023)) + (b & 1023u);
}

int *lj_m7_ccall_jit_ptr_i64_i32(int64_t a, int32_t b)
{
  return lj_m7_ccall_jit_values +
	 (((uint32_t)((uint64_t)a & UINT64_C(3)) + (uint32_t)b) % 4u);
}

int *lj_m7_ccall_jit_ptr_i64_u32(int64_t a, uint32_t b)
{
  return lj_m7_ccall_jit_values +
	 (((uint32_t)((uint64_t)a & UINT64_C(3)) + (b & 3u)) % 4u);
}

int64_t lj_m7_ccall_jit_i64_i64_i32(int64_t a, int32_t b)
{
  return INT64_C(0x100000000) +
	 (int64_t)((uint64_t)a & UINT64_C(1023)) + (int64_t)b;
}

int64_t lj_m7_ccall_jit_i64_i64_u32(int64_t a, uint32_t b)
{
  return INT64_C(0x100000000) +
	 (int64_t)((uint64_t)a & UINT64_C(1023)) + (int64_t)(b & 1023u);
}

uint64_t lj_m7_ccall_jit_u64_i64_i32(int64_t a, int32_t b)
{
  return UINT64_C(0x100000000) +
	 ((uint64_t)a & UINT64_C(1023)) + (uint64_t)(uint32_t)b;
}

uint64_t lj_m7_ccall_jit_u64_i64_u32(int64_t a, uint32_t b)
{
  return UINT64_C(0x100000000) +
	 ((uint64_t)a & UINT64_C(1023)) + (uint64_t)(b & 1023u);
}

uint8_t lj_m7_ccall_jit_u8_u64_i32(uint64_t a, int32_t b)
{
  return (uint8_t)((a & UINT64_C(255)) + (uint32_t)b);
}

uint8_t lj_m7_ccall_jit_u8_u64_u32(uint64_t a, uint32_t b)
{
  return (uint8_t)((a & UINT64_C(255)) + (b & 255u));
}

void lj_m7_ccall_jit_void_u64_i32(uint64_t a, int32_t b)
{
  lj_m7_ccall_jit_void_count += (int)(a & UINT64_C(15)) + (int)(b & 15);
}

void lj_m7_ccall_jit_void_u64_u32(uint64_t a, uint32_t b)
{
  lj_m7_ccall_jit_void_count += (int)(a & UINT64_C(15)) + (int)(b & 15u);
}

int32_t lj_m7_ccall_jit_i32_u64_i32(uint64_t a, int32_t b)
{
  return (int32_t)((a & UINT64_C(1023)) + (uint32_t)b);
}

int32_t lj_m7_ccall_jit_i32_u64_u32(uint64_t a, uint32_t b)
{
  return (int32_t)((a & UINT64_C(1023)) + (b & 1023u));
}

uint32_t lj_m7_ccall_jit_u32_u64_i32(uint64_t a, int32_t b)
{
  return UINT32_C(0xf0000000) + (uint32_t)(a & UINT64_C(1023)) +
	 (uint32_t)b;
}

uint32_t lj_m7_ccall_jit_u32_u64_u32(uint64_t a, uint32_t b)
{
  return UINT32_C(0xf0000000) + (uint32_t)(a & UINT64_C(1023)) +
	 (b & 1023u);
}

int *lj_m7_ccall_jit_ptr_u64_i32(uint64_t a, int32_t b)
{
  return lj_m7_ccall_jit_values +
	 (((uint32_t)(a & UINT64_C(3)) + (uint32_t)b) % 4u);
}

int *lj_m7_ccall_jit_ptr_u64_u32(uint64_t a, uint32_t b)
{
  return lj_m7_ccall_jit_values +
	 (((uint32_t)(a & UINT64_C(3)) + (b & 3u)) % 4u);
}

int64_t lj_m7_ccall_jit_i64_u64_i32(uint64_t a, int32_t b)
{
  return INT64_C(0x100000000) + (int64_t)(a & UINT64_C(1023)) +
	 (int64_t)b;
}

int64_t lj_m7_ccall_jit_i64_u64_u32(uint64_t a, uint32_t b)
{
  return INT64_C(0x100000000) + (int64_t)(a & UINT64_C(1023)) +
	 (int64_t)(b & 1023u);
}

uint64_t lj_m7_ccall_jit_u64_u64_i32(uint64_t a, int32_t b)
{
  return UINT64_C(0x100000000) + (a & UINT64_C(1023)) +
	 (uint64_t)(uint32_t)b;
}

uint64_t lj_m7_ccall_jit_u64_u64_u32(uint64_t a, uint32_t b)
{
  return UINT64_C(0x100000000) + (a & UINT64_C(1023)) +
	 (uint64_t)(b & 1023u);
}

uint8_t lj_m7_ccall_jit_u8_i64_ptr(int64_t a, int *p)
{
  return (uint8_t)(((uint64_t)a & UINT64_C(255)) + (uint32_t)*p);
}

uint8_t lj_m7_ccall_jit_u8_u64_ptr(uint64_t a, int *p)
{
  return (uint8_t)((a & UINT64_C(255)) + (uint32_t)*p);
}

void lj_m7_ccall_jit_void_i64_ptr(int64_t a, int *p)
{
  lj_m7_ccall_jit_void_count += (int)((uint64_t)a & UINT64_C(15)) + *p;
}

void lj_m7_ccall_jit_void_u64_ptr(uint64_t a, int *p)
{
  lj_m7_ccall_jit_void_count += (int)(a & UINT64_C(15)) + *p;
}

int32_t lj_m7_ccall_jit_i32_i64_ptr(int64_t a, int *p)
{
  return (int32_t)(((uint64_t)a & UINT64_C(1023)) + (uint32_t)*p);
}

int32_t lj_m7_ccall_jit_i32_u64_ptr(uint64_t a, int *p)
{
  return (int32_t)((a & UINT64_C(1023)) + (uint32_t)*p);
}

uint32_t lj_m7_ccall_jit_u32_i64_ptr(int64_t a, int *p)
{
  return UINT32_C(0xf0000000) +
	 (uint32_t)((uint64_t)a & UINT64_C(1023)) + (uint32_t)*p;
}

uint32_t lj_m7_ccall_jit_u32_u64_ptr(uint64_t a, int *p)
{
  return UINT32_C(0xf0000000) + (uint32_t)(a & UINT64_C(1023)) +
	 (uint32_t)*p;
}

int *lj_m7_ccall_jit_ptr_i64_ptr(int64_t a, int *p)
{
  return lj_m7_ccall_jit_values +
	 (((uint32_t)((uint64_t)a & UINT64_C(3)) + (uint32_t)*p) % 4u);
}

int *lj_m7_ccall_jit_ptr_u64_ptr(uint64_t a, int *p)
{
  return lj_m7_ccall_jit_values +
	 (((uint32_t)(a & UINT64_C(3)) + (uint32_t)*p) % 4u);
}

int64_t lj_m7_ccall_jit_i64_i64_ptr(int64_t a, int *p)
{
  return INT64_C(0x100000000) +
	 (int64_t)((uint64_t)a & UINT64_C(1023)) + (int64_t)*p;
}

int64_t lj_m7_ccall_jit_i64_u64_ptr(uint64_t a, int *p)
{
  return INT64_C(0x100000000) + (int64_t)(a & UINT64_C(1023)) +
	 (int64_t)*p;
}

uint64_t lj_m7_ccall_jit_u64_i64_ptr(int64_t a, int *p)
{
  return UINT64_C(0x100000000) +
	 ((uint64_t)a & UINT64_C(1023)) + (uint64_t)(uint32_t)*p;
}

uint64_t lj_m7_ccall_jit_u64_u64_ptr(uint64_t a, int *p)
{
  return UINT64_C(0x100000000) + (a & UINT64_C(1023)) +
	 (uint64_t)(uint32_t)*p;
}

uint8_t lj_m7_ccall_jit_u8_i64_i64(int64_t a, int64_t b)
{
  return (uint8_t)(((uint64_t)a & UINT64_C(255)) +
		   ((uint64_t)b & UINT64_C(255)));
}

uint8_t lj_m7_ccall_jit_u8_i64_u64(int64_t a, uint64_t b)
{
  return (uint8_t)(((uint64_t)a & UINT64_C(255)) + (b & UINT64_C(255)));
}

uint8_t lj_m7_ccall_jit_u8_u64_i64(uint64_t a, int64_t b)
{
  return (uint8_t)((a & UINT64_C(255)) +
		   ((uint64_t)b & UINT64_C(255)));
}

uint8_t lj_m7_ccall_jit_u8_u64_u64(uint64_t a, uint64_t b)
{
  return (uint8_t)((a & UINT64_C(255)) + (b & UINT64_C(255)));
}

void lj_m7_ccall_jit_void_i64_i64(int64_t a, int64_t b)
{
  lj_m7_ccall_jit_void_count += (int)((uint64_t)a & UINT64_C(15)) +
				(int)((uint64_t)b & UINT64_C(15));
}

void lj_m7_ccall_jit_void_i64_u64(int64_t a, uint64_t b)
{
  lj_m7_ccall_jit_void_count += (int)((uint64_t)a & UINT64_C(15)) +
				(int)(b & UINT64_C(15));
}

void lj_m7_ccall_jit_void_u64_i64(uint64_t a, int64_t b)
{
  lj_m7_ccall_jit_void_count += (int)(a & UINT64_C(15)) +
				(int)((uint64_t)b & UINT64_C(15));
}

void lj_m7_ccall_jit_void_u64_u64(uint64_t a, uint64_t b)
{
  lj_m7_ccall_jit_void_count += (int)(a & UINT64_C(15)) +
				(int)(b & UINT64_C(15));
}

int32_t lj_m7_ccall_jit_i32_i64_i64(int64_t a, int64_t b)
{
  return (int32_t)(((uint64_t)a & UINT64_C(1023)) +
		   ((uint64_t)b & UINT64_C(1023)));
}

int32_t lj_m7_ccall_jit_i32_i64_u64(int64_t a, uint64_t b)
{
  return (int32_t)(((uint64_t)a & UINT64_C(1023)) + (b & UINT64_C(1023)));
}

int32_t lj_m7_ccall_jit_i32_u64_i64(uint64_t a, int64_t b)
{
  return (int32_t)((a & UINT64_C(1023)) +
		   ((uint64_t)b & UINT64_C(1023)));
}

int32_t lj_m7_ccall_jit_i32_u64_u64(uint64_t a, uint64_t b)
{
  return (int32_t)((a & UINT64_C(1023)) + (b & UINT64_C(1023)));
}

uint32_t lj_m7_ccall_jit_u32_i64_i64(int64_t a, int64_t b)
{
  return UINT32_C(0xf0000000) +
	 (uint32_t)((uint64_t)a & UINT64_C(1023)) +
	 (uint32_t)((uint64_t)b & UINT64_C(1023));
}

uint32_t lj_m7_ccall_jit_u32_i64_u64(int64_t a, uint64_t b)
{
  return UINT32_C(0xf0000000) +
	 (uint32_t)((uint64_t)a & UINT64_C(1023)) +
	 (uint32_t)(b & UINT64_C(1023));
}

uint32_t lj_m7_ccall_jit_u32_u64_i64(uint64_t a, int64_t b)
{
  return UINT32_C(0xf0000000) + (uint32_t)(a & UINT64_C(1023)) +
	 (uint32_t)((uint64_t)b & UINT64_C(1023));
}

uint32_t lj_m7_ccall_jit_u32_u64_u64(uint64_t a, uint64_t b)
{
  return UINT32_C(0xf0000000) + (uint32_t)(a & UINT64_C(1023)) +
	 (uint32_t)(b & UINT64_C(1023));
}

int *lj_m7_ccall_jit_ptr_i64_i64(int64_t a, int64_t b)
{
  return lj_m7_ccall_jit_values +
	 (((uint32_t)((uint64_t)a & UINT64_C(3)) +
	   (uint32_t)((uint64_t)b & UINT64_C(3))) % 4u);
}

int *lj_m7_ccall_jit_ptr_i64_u64(int64_t a, uint64_t b)
{
  return lj_m7_ccall_jit_values +
	 (((uint32_t)((uint64_t)a & UINT64_C(3)) +
	   (uint32_t)(b & UINT64_C(3))) % 4u);
}

int *lj_m7_ccall_jit_ptr_u64_i64(uint64_t a, int64_t b)
{
  return lj_m7_ccall_jit_values +
	 (((uint32_t)(a & UINT64_C(3)) +
	   (uint32_t)((uint64_t)b & UINT64_C(3))) % 4u);
}

int *lj_m7_ccall_jit_ptr_u64_u64(uint64_t a, uint64_t b)
{
  return lj_m7_ccall_jit_values +
	 (((uint32_t)(a & UINT64_C(3)) +
	   (uint32_t)(b & UINT64_C(3))) % 4u);
}

int *lj_m7_ccall_jit_ptr_u32(uint32_t a)
{
  return lj_m7_ccall_jit_values + (a & 3u);
}

int *lj_m7_ccall_jit_ptr_u64arg(uint64_t a)
{
  return lj_m7_ccall_jit_values + (a & 3u);
}

int *lj_m7_ccall_jit_ptr_ptr_u64(int *p, uint64_t n)
{
  return p + (n & 3u);
}

int *lj_m7_ccall_jit_ptr_ptr_i64(int *p, int64_t n)
{
  return p + ((uint64_t)n & 3u);
}

int *lj_m7_ccall_jit_ptr_i32_i64(int32_t a, int64_t b)
{
  return lj_m7_ccall_jit_values + ((uint32_t)a + ((uint64_t)b & 3u)) % 4u;
}

int *lj_m7_ccall_jit_ptr_i32_u64(int32_t a, uint64_t b)
{
  return lj_m7_ccall_jit_values + ((uint32_t)a + (b & 3u)) % 4u;
}

int *lj_m7_ccall_jit_ptr_u32_i64(uint32_t a, int64_t b)
{
  return lj_m7_ccall_jit_values + ((a & 3u) + ((uint64_t)b & 3u)) % 4u;
}

int *lj_m7_ccall_jit_ptr_u32_u64(uint32_t a, uint64_t b)
{
  return lj_m7_ccall_jit_values + ((a & 3u) + (b & 3u)) % 4u;
}

double lj_m7_ccall_jit_num0(void)
{
  return 1.5;
}

double lj_m7_ccall_jit_num_i32(int32_t a)
{
  return (double)a + 0.75;
}

double lj_m7_ccall_jit_num_num_i32(double a, int32_t b)
{
  return a + (double)b + 0.375;
}

double lj_m7_ccall_jit_num_i32_num(int32_t a, double b)
{
  return (double)a + b + 0.625;
}

double lj_m7_ccall_jit_num_num_u32(double a, uint32_t b)
{
  return a + (double)(b & 255u) + 0.875;
}

double lj_m7_ccall_jit_num_u32_num(uint32_t a, double b)
{
  return (double)(a & 255u) + b + 0.625;
}

double lj_m7_ccall_jit_num_flt_i32(float a, int32_t b)
{
  return (double)a + (double)b + 0.375;
}

double lj_m7_ccall_jit_num_i32_flt(int32_t a, float b)
{
  return (double)a + (double)b + 0.625;
}

double lj_m7_ccall_jit_num_flt_u32(float a, uint32_t b)
{
  return (double)a + (double)(b & 255u) + 0.875;
}

double lj_m7_ccall_jit_num_u32_flt(uint32_t a, float b)
{
  return (double)(a & 255u) + (double)b + 0.125;
}

double lj_m7_ccall_jit_num_num_i64(double a, int64_t b)
{
  return a + (double)((uint64_t)b & 255u) + 0.375;
}

double lj_m7_ccall_jit_num_i64_num(int64_t a, double b)
{
  return (double)((uint64_t)a & 255u) + b + 0.625;
}

double lj_m7_ccall_jit_num_num_u64(double a, uint64_t b)
{
  return a + (double)(b & 255u) + 0.875;
}

double lj_m7_ccall_jit_num_u64_num(uint64_t a, double b)
{
  return (double)(a & 255u) + b + 0.125;
}

double lj_m7_ccall_jit_num_flt_i64(float a, int64_t b)
{
  return (double)a + (double)((uint64_t)b & 255u) + 0.375;
}

double lj_m7_ccall_jit_num_i64_flt(int64_t a, float b)
{
  return (double)((uint64_t)a & 255u) + (double)b + 0.625;
}

double lj_m7_ccall_jit_num_flt_u64(float a, uint64_t b)
{
  return (double)a + (double)(b & 255u) + 0.875;
}

double lj_m7_ccall_jit_num_u64_flt(uint64_t a, float b)
{
  return (double)(a & 255u) + (double)b + 0.125;
}

double lj_m7_ccall_jit_num_i32_i32(int32_t a, int32_t b)
{
  return (double)a * 2.0 + (double)b + 0.5;
}

double lj_m7_ccall_jit_num_u32_u32(uint32_t a, uint32_t b)
{
  return (double)(a & 255u) + (double)(b & 255u) + 0.875;
}

double lj_m7_ccall_jit_num_i64_u32(int64_t a, uint32_t b)
{
  return (double)((uint64_t)a & 255u) + (double)(b & 255u) + 1.125;
}

double lj_m7_ccall_jit_num_ptr(int *p)
{
  return (double)*p + 0.25;
}

double lj_m7_ccall_jit_num_ptr_ptr(const char *p, char **endp)
{
  if (endp)
    *endp = (char *)p + 2;
  return (double)(unsigned char)p[0] + (double)(unsigned char)p[1] + 2.0;
}

double lj_m7_ccall_jit_num_flt(float a)
{
  return (double)a + 0.125;
}

double lj_m7_ccall_jit_num1(double a)
{
  return a + 0.5;
}

double lj_m7_ccall_jit_num2(double a, double b)
{
  return a + b + 0.25;
}

float lj_m7_ccall_jit_flt0(void)
{
  return 1.5f;
}

float lj_m7_ccall_jit_flt1(float a)
{
  return a + 0.5f;
}

float lj_m7_ccall_jit_flt2(float a, float b)
{
  return a + b + 0.25f;
}

float lj_m7_ccall_jit_flt_i32(int32_t a)
{
  return (float)a + 0.25f;
}

float lj_m7_ccall_jit_flt_ptr(int *p)
{
  return (float)*p + 0.75f;
}

float lj_m7_ccall_jit_flt_i64_u32(int64_t a, uint32_t b)
{
  return (float)(((uint64_t)a & 255u) + (b & 255u)) + 0.5f;
}

float lj_m7_ccall_jit_flt_flt_i32(float a, int32_t b)
{
  return a + (float)b + 0.375f;
}

float lj_m7_ccall_jit_flt_i32_flt(int32_t a, float b)
{
  return (float)a + b + 0.625f;
}

float lj_m7_ccall_jit_flt_flt_u32(float a, uint32_t b)
{
  return a + (float)(b & 255u) + 0.875f;
}

float lj_m7_ccall_jit_flt_u32_flt(uint32_t a, float b)
{
  return (float)(a & 255u) + b + 0.625f;
}

float lj_m7_ccall_jit_flt_num_i32(double a, int32_t b)
{
  return (float)(a + (double)b + 0.375);
}

float lj_m7_ccall_jit_flt_i32_num(int32_t a, double b)
{
  return (float)((double)a + b + 0.625);
}

float lj_m7_ccall_jit_flt_num_u32(double a, uint32_t b)
{
  return (float)(a + (double)(b & 255u) + 0.875);
}

float lj_m7_ccall_jit_flt_u32_num(uint32_t a, double b)
{
  return (float)((double)(a & 255u) + b + 0.125);
}

float lj_m7_ccall_jit_flt_flt_i64(float a, int64_t b)
{
  return a + (float)((uint64_t)b & 255u) + 0.375f;
}

float lj_m7_ccall_jit_flt_i64_flt(int64_t a, float b)
{
  return (float)((uint64_t)a & 255u) + b + 0.625f;
}

float lj_m7_ccall_jit_flt_flt_u64(float a, uint64_t b)
{
  return a + (float)(b & 255u) + 0.875f;
}

float lj_m7_ccall_jit_flt_u64_flt(uint64_t a, float b)
{
  return (float)(a & 255u) + b + 0.125f;
}

float lj_m7_ccall_jit_flt_num_i64(double a, int64_t b)
{
  return (float)(a + (double)((uint64_t)b & 255u) + 0.375);
}

float lj_m7_ccall_jit_flt_i64_num(int64_t a, double b)
{
  return (float)((double)((uint64_t)a & 255u) + b + 0.625);
}

float lj_m7_ccall_jit_flt_num_u64(double a, uint64_t b)
{
  return (float)(a + (double)(b & 255u) + 0.875);
}

float lj_m7_ccall_jit_flt_u64_num(uint64_t a, double b)
{
  return (float)((double)(a & 255u) + b + 0.125);
}

int lj_m7_ccall_jit_void_count_i32(void)
{
  return lj_m7_ccall_jit_void_count;
}

int32_t lj_m7_ccall_jit_i32_num(double a)
{
  return (int32_t)a + 3;
}

int32_t lj_m7_ccall_jit_i32_flt(float a)
{
  return (int32_t)a + 4;
}

void lj_m7_ccall_jit_void0(void)
{
  lj_m7_ccall_jit_void_count++;
}

void lj_m7_ccall_jit_void_num(double a)
{
  lj_m7_ccall_jit_void_count += (int)a;
}

void lj_m7_ccall_jit_void_flt(float a)
{
  lj_m7_ccall_jit_void_count += (int)a;
}

float lj_m7_ccall_jit_flt_num(double a)
{
  return (float)(a + 0.5);
}

void lj_m7_ccall_jit_store_i32(int *p, int v)
{
  *p = v + 9;
}

unsigned int lj_m7_ccall_jit_u32(unsigned int a)
{
  return a + 1u;
}

uint32_t lj_m7_ccall_jit_u32_i32(int32_t a)
{
  return 0xf0000000u + (uint32_t)a;
}

uint32_t lj_m7_ccall_jit_u32_ptr(int *p)
{
  return 0xf0000000u + (uint32_t)*p;
}

uint32_t lj_m7_ccall_jit_u32_i32_ptr(int32_t offset, int *p)
{
  return 0xf0000000u + (uint32_t)p[offset];
}

uint32_t lj_m7_ccall_jit_u32_0(void)
{
  return 0xf0000001u;
}

uint64_t lj_m7_ccall_jit_u64(uint64_t a)
{
  return a + 1;
}

uint64_t lj_m7_ccall_jit_u64_u64_u64(uint64_t a, uint64_t b)
{
  return a + b + 5;
}

uint64_t lj_m7_ccall_jit_u64_i64(int64_t a)
{
  return (uint64_t)(a - 5);
}

uint64_t lj_m7_ccall_jit_u64_i64_u64(int64_t a, uint64_t b)
{
  return (uint64_t)a + b + UINT64_C(11);
}

uint64_t lj_m7_ccall_jit_u64_u64_i64(uint64_t a, int64_t b)
{
  return a + (uint64_t)b + UINT64_C(13);
}

uint64_t lj_m7_ccall_jit_u64_i32(int32_t a)
{
  return UINT64_C(0x100000000) + (uint64_t)(uint32_t)a;
}

uint64_t lj_m7_ccall_jit_u64_ptr(int *p)
{
  return UINT64_C(0x100000000) + (uint64_t)(uint32_t)*p;
}

uint64_t lj_m7_ccall_jit_u64_i32_ptr(int32_t offset, int *p)
{
  return UINT64_C(0x100000000) + (uint64_t)(uint32_t)p[offset];
}

int64_t lj_m7_ccall_jit_i64_ptr_u64(int *p, uint64_t n)
{
  return INT64_C(0x100000000) + (int64_t)p[n & 3u] +
	 (int64_t)(n & UINT64_C(1023));
}

int64_t lj_m7_ccall_jit_i64_ptr_i64(int *p, int64_t n)
{
  uint64_t u = (uint64_t)n;
  return INT64_C(0x100000000) + (int64_t)p[u & 3u] +
	 (int64_t)(u & UINT64_C(1023));
}

int64_t lj_m7_ccall_jit_i64_i32_i64(int32_t a, int64_t b)
{
  return INT64_C(0x100000000) + (int64_t)a +
	 (int64_t)((uint64_t)b & UINT64_C(1023));
}

int64_t lj_m7_ccall_jit_i64_i32_u64(int32_t a, uint64_t b)
{
  return INT64_C(0x100000000) + (int64_t)a +
	 (int64_t)(b & UINT64_C(1023));
}

int64_t lj_m7_ccall_jit_i64_u32_i64(uint32_t a, int64_t b)
{
  return INT64_C(0x100000000) + (int64_t)(a & 1023u) +
	 (int64_t)((uint64_t)b & UINT64_C(1023));
}

int64_t lj_m7_ccall_jit_i64_u32_u64(uint32_t a, uint64_t b)
{
  return INT64_C(0x100000000) + (int64_t)(a & 1023u) +
	 (int64_t)(b & UINT64_C(1023));
}

uint64_t lj_m7_ccall_jit_u64_ptr_u64(int *p, uint64_t n)
{
  return UINT64_C(0x100000000) + (uint64_t)(uint32_t)p[n & 3u] +
	 (n & UINT64_C(1023));
}

uint64_t lj_m7_ccall_jit_u64_ptr_i64(int *p, int64_t n)
{
  uint64_t u = (uint64_t)n;
  return UINT64_C(0x100000000) + (uint64_t)(uint32_t)p[u & 3u] +
	 (u & UINT64_C(1023));
}

uint64_t lj_m7_ccall_jit_u64_i32_i64(int32_t a, int64_t b)
{
  return UINT64_C(0x100000000) + (uint64_t)(uint32_t)a +
	 ((uint64_t)b & UINT64_C(1023));
}

uint64_t lj_m7_ccall_jit_u64_i32_u64(int32_t a, uint64_t b)
{
  return UINT64_C(0x100000000) + (uint64_t)(uint32_t)a +
	 (b & UINT64_C(1023));
}

uint64_t lj_m7_ccall_jit_u64_u32_i64(uint32_t a, int64_t b)
{
  return UINT64_C(0x100000000) + (uint64_t)(a & 1023u) +
	 ((uint64_t)b & UINT64_C(1023));
}

uint64_t lj_m7_ccall_jit_u64_u32_u64(uint32_t a, uint64_t b)
{
  return UINT64_C(0x100000000) + (uint64_t)(a & 1023u) +
	 (b & UINT64_C(1023));
}

uint64_t lj_m7_ccall_jit_u64_0(void)
{
  return ~(uint64_t)0;
}

int *lj_m7_ccall_jit_ptr0(void)
{
  return lj_m7_ccall_jit_values;
}

int lj_m7_ccall_jit_ptr_read_i32(int *p)
{
  return *p;
}

int lj_m7_ccall_jit_ptr_sum_i32(int *a, int *b)
{
  return *a + *b;
}

int lj_m7_ccall_jit_i32_ptr_read_i32(int offset, int *p)
{
  return p[offset];
}

int *lj_m7_ccall_jit_ptr_add_i32(int *p, int offset)
{
  return p + offset;
}

int *lj_m7_ccall_jit_ptr_num(double a)
{
  return lj_m7_ccall_jit_values + ((int)a & 3);
}

int lj_m7_ccall_jit_i32_ptr_ulong_i32(int *p, unsigned long n, int offset)
{
  return p[offset] + (int)(n & 1023UL);
}
