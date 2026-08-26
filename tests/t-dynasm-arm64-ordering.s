.text
.p2align 2
.globl _arm64_ordering_reference
_arm64_ordering_reference:
  ldar w0, [x1]
  ldar x2, [x3]
  ldarb w4, [x5]
  ldarh w6, [x7]
  stlr w8, [x9]
  stlr x10, [x11]
  stlrb w12, [x13]
  stlrh w14, [sp]
  ldar wzr, [x0]
  stlr xzr, [x0]
  dmb oshld
  dmb oshst
  dmb osh
  dmb nshld
  dmb nshst
  dmb nsh
  dmb ishld
  dmb ishst
  dmb ish
  dmb ld
  dmb st
  dmb sy
  dmb #0
  dmb #15
  isb
  isb sy
  isb #0
  isb #15
