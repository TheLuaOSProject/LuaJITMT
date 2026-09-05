[Thread debugging using libthread_db enabled]
Using host libthread_db library "/lib/x86_64-linux-gnu/libthread_db.so.1".
0x000055c694242570 in lj_jit_token_try ()
#0  0x000055c694242570 in lj_jit_token_try ()
#1  0x000055c6941a0fc0 in gc2_sweep_reclaim_jit.isra ()
#2  0x000055c6941a66bf in lj_gc2_sweep_owner_progress ()
#3  0x000055c6941c958e in gc2_worker_drain_inner ()
#4  0x000055c6941cd548 in lj_gc2_collect_active ()
#5  0x000055c694227549 in api_gc_collect_cp ()
#6  0x000055c69418c85b in lj_vm_cpcall_asm ()
#7  0x000055c6941fd040 in lj_vm_cpcall ()
#8  0x000055c694231e26 in lua_gc ()
#9  0x000055c694185ae6 in test_active_black_direct_publishes_typed (L=L@entry=0x7f8d449b00a0, g=g@entry=0x7f8d449b0150, tg=tg@entry=0x7f8d449b2d40) at /tmp/lj-wide-stamp-corrected-20260905-cqq3p87i/fnew-prerequisites.c:2967
#10 0x000055c694181f64 in main () at /tmp/lj-wide-stamp-corrected-20260905-cqq3p87i/fnew-prerequisites.c:3126
#9  0x000055c694185ae6 in test_active_black_direct_publishes_typed (L=L@entry=0x7f8d449b00a0, g=g@entry=0x7f8d449b0150, tg=tg@entry=0x7f8d449b2d40) at /tmp/lj-wide-stamp-corrected-20260905-cqq3p87i/fnew-prerequisites.c:2967
2967	    lua_gc(L, LUA_GCCOLLECT, 0);
$1 = {
  flags = 25,
  owner_tid = 1,
  next = 0x7f8d446d0000,
  grey = 0x0,
  sweep_epoch = 5,
  live_cells = 116,
  gc2_tabstamp = 0x7f8d4472f010,
  retire_epoch = 63,
  reclaim_cell = 703,
  reclaim_deferred = 0,
  remote_active = 9223372036854775808,
  remote_free = 0x0,
  retire_obj = 0x0,
  progress_g = 0x0,
  prep_bump_cell = 0,
  prep_bump_end = 0,
  gcprep_pending = 0,
  terminal_closed = 0,
  huge_tabstamp = {
    {
      proof = {
        lo = 0,
        hi = 0
      },
      {
        state = 0,
        era = 0
      }
    },
    token = {
      control = 0
    }
  },
  gc2_tabledesc = 0x0
}
phase/cycle/activation 3 6 {
  value = {
    lo = 6,
    hi = 612
  }
}
arena 0x7f8d44750000
623 sweep 3 root 0 life 0 recovery 0 ready 1 late 0 mark 0 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 0,
      hi = 0
    },
    {
      state = 0,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
659 sweep 0 root 1 life 2 recovery 0 ready 1 late 0 mark 1 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 4294967298,
      hi = 0
    },
    {
      state = 4294967298,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
664 sweep 0 root 1 life 2 recovery 0 ready 1 late 0 mark 1 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 0,
      hi = 0
    },
    {
      state = 0,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
673 sweep 0 root 1 life 2 recovery 0 ready 1 late 0 mark 1 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 0,
      hi = 0
    },
    {
      state = 0,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
676 sweep 0 root 1 life 2 recovery 0 ready 1 late 0 mark 1 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 4294967296,
      hi = 0
    },
    {
      state = 4294967296,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
764 sweep 0 root 1 life 2 recovery 0 ready 1 late 0 mark 1 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 0,
      hi = 0
    },
    {
      state = 0,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
767 sweep 0 root 1 life 2 recovery 0 ready 1 late 0 mark 1 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 0,
      hi = 0
    },
    {
      state = 0,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
773 sweep 3 root 0 life 0 recovery 0 ready 1 late 0 mark 0 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 0,
      hi = 0
    },
    {
      state = 0,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
832 sweep 3 root 0 life 0 recovery 0 ready 1 late 0 mark 0 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 0,
      hi = 0
    },
    {
      state = 0,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
886 sweep 3 root 0 life 0 recovery 0 ready 1 late 0 mark 0 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 0,
      hi = 0
    },
    {
      state = 0,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
889 sweep 3 root 0 life 0 recovery 0 ready 1 late 0 mark 0 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 0,
      hi = 0
    },
    {
      state = 0,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
909 sweep 3 root 0 life 0 recovery 0 ready 1 late 0 mark 0 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 0,
      hi = 0
    },
    {
      state = 0,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
912 sweep 3 root 0 life 0 recovery 0 ready 1 late 0 mark 0 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 0,
      hi = 0
    },
    {
      state = 0,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
930 sweep 3 root 0 life 0 recovery 0 ready 1 late 0 mark 0 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 0,
      hi = 0
    },
    {
      state = 0,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
933 sweep 3 root 0 life 0 recovery 0 ready 1 late 0 mark 0 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 0,
      hi = 0
    },
    {
      state = 0,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
arena 0x7f8d446d0000
624 sweep 2 root 0 life 1 recovery 0 ready 1 late 0 mark 0 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 21474836482,
      hi = 0
    },
    {
      state = 21474836482,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
629 sweep 2 root 0 life 1 recovery 0 ready 1 late 0 mark 0 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 21474836485,
      hi = 0
    },
    {
      state = 21474836485,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
634 sweep 2 root 0 life 1 recovery 0 ready 1 late 0 mark 0 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 21474836483,
      hi = 0
    },
    {
      state = 21474836483,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
639 sweep 2 root 0 life 1 recovery 0 ready 1 late 0 mark 0 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 0,
      hi = 0
    },
    {
      state = 0,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
651 sweep 2 root 0 life 1 recovery 0 ready 1 late 0 mark 0 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 0,
      hi = 0
    },
    {
      state = 0,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
678 sweep 2 root 0 life 1 recovery 0 ready 1 late 0 mark 0 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 21474836486,
      hi = 0
    },
    {
      state = 21474836486,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
683 sweep 2 root 0 life 1 recovery 0 ready 1 late 0 mark 0 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 21474836497,
      hi = 0
    },
    {
      state = 21474836497,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
688 sweep 2 root 0 life 1 recovery 0 ready 1 late 0 mark 0 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 21474836490,
      hi = 0
    },
    {
      state = 21474836490,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
693 sweep 1 root 0 life 1 recovery 0 ready 1 late 0 mark 1 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 0,
      hi = 0
    },
    {
      state = 0,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
705 sweep 1 root 0 life 1 recovery 0 ready 1 late 0 mark 1 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 0,
      hi = 0
    },
    {
      state = 0,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
716 sweep 2 root 0 life 1 recovery 0 ready 1 late 0 mark 0 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 0,
      hi = 0
    },
    {
      state = 0,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
726 sweep 3 root 0 life 0 recovery 0 ready 1 late 0 mark 0 kind 0 token 0x0 stamp {
  {
    proof = {
      lo = 0,
      hi = 0
    },
    {
      state = 0,
      era = 0
    }
  },
  token = {
    control = 0
  }
}
[Inferior 1 (process 118991) detached]
