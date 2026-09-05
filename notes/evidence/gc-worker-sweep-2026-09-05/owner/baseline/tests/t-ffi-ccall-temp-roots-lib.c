/*
** Shared-library target for interpreted C-call temporary-root coverage.
*/

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define LJ_M7_EXPORT __declspec(dllexport)
#else
#define LJ_M7_EXPORT __attribute__((visibility("default")))
#endif

typedef struct lj_m7_ccall_root_blob {
  uint32_t lane[16];
} lj_m7_ccall_root_blob;

typedef int (*lj_m7_ccall_root_cb)(uint32_t cookie);

/* This aggregate is returned through a caller-owned result buffer on x64
** POSIX and is passed through caller-owned temporary storage on Win64. Keep
** the input loads volatile and after the callback: the callback deliberately
** runs complete collections while both ABI objects are live. */
LJ_M7_EXPORT lj_m7_ccall_root_blob
lj_m7_ccall_root_roundtrip(lj_m7_ccall_root_blob input,
			   lj_m7_ccall_root_cb callback, uint32_t cookie)
{
  volatile const uint32_t *src = input.lane;
  lj_m7_ccall_root_blob output;
  uint32_t delta = (uint32_t)callback(cookie);
  size_t i;
  for (i = 0; i < 16; i++)
    output.lane[i] = (src[i] ^ UINT32_C(0x005a5a5a)) + delta + (uint32_t)i;
  return output;
}
