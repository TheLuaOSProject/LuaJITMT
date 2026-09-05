#include <stdio.h>
#include <stddef.h>
#include "lj_arena.h"
int main(void) {
 printf("{\"inline_entry\":%zu,\"token_offset\":%zu,\"wide_entry\":%zu,\"wide_offset\":%zu,\"sidecar\":%zu,\"header\":%zu,\"first_cell\":%u,\"huge_inline_offset\":%zu}\n", sizeof(LJGC2TabStamp),offsetof(LJGC2TabStamp,token),sizeof(LJGC2TabWideStamp),offsetof(LJGC2TabStampArena,wide),sizeof(LJGC2TabStampArena),sizeof(GCAhdr),LJ_AFIRST_CELL,offsetof(GCAhdr,huge_tabstamp));
 return 0;
}
