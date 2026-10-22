/* Pause inside the actual owner traversal; retain the real parser scan. */
#define lj_lex_gc2_markroots __wrap_lj_lex_gc2_markroots
#include "../../src/lj_gc2.c"
