#include <stdio.h>
#include <stddef.h>
#include "lj_arena.h"
int main(void) {
 printf("{\"stamp_bytes\":%zu,\"sidecar_bytes\":%zu,\"arena_header_bytes\":%zu,\"first_cell\":%u,\"token_offset\":%zu}\n",sizeof(LJGC2TabStamp),sizeof(LJGC2TabStampArena),sizeof(GCAhdr),(unsigned)LJ_AFIRST_CELL,offsetof(LJGC2TabStamp,token));
 return 0;
}
