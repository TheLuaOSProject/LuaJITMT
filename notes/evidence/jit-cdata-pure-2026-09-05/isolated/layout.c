#include <stddef.h>
#include <stdio.h>
#include "lj_obj.h"
#include "lj_jit.h"
#include "lj_dispatch.h"
int main(void) {
 printf("jit=%zu flags=%zu rootflag=%zu fold=%zu gg=%zu cdatafid=%u\n",sizeof(jit_State),offsetof(jit_State,flags),offsetof(jit_State,root_startins_pending),offsetof(jit_State,fold),sizeof(GG_State),(unsigned)(GG_OFS(g.gcroot[GCROOT_BASEMT+~LJ_TCDATA])>>2));
 return 0;
}
