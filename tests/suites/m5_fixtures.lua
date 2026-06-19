return function(add)
  add({
    name = "m5_nbtab_model",
    description = "concurrent table protocol standalone C model",
    run = function(t)
      t:cc(t:tmp("lj_t-nbtab-model"), {
        t:path("tests", "t-nbtab-model.c")
      }, {
        default_cflags = false,
        include_src = false,
        cflags = "-std=gnu11 -O2 -Wall -Wextra -Werror -pthread -mcx16"
      })
      t:run({ t:tmp("lj_t-nbtab-model") })
      print("M5 nbtab model tests passed")
    end
  })

  add({
    name = "m5_itype_nan",
    description = "NaN TValue tag C fixture",
    run = function(t)
      t:run_luajit_c_fixture(t:tmp("lj_t-itype-nan"), "t-itype-nan.c")
      print("M5 NaN tag tests passed")
    end
  })

  add({
    name = "m5_itype_sentinel",
    description = "internal table sentinel TValue C fixture",
    run = function(t)
      t:run_luajit_c_fixture(t:tmp("lj_t-itype-sentinel"),
                              "t-itype-sentinel.c")
      print("M5 internal table sentinel tag tests passed")
    end
  })

  add({
    name = "m5_bcdump_compat",
    description = "bytecode dump compatibility C fixture",
    run = function(t)
      t:run_luajit_c_fixture(t:tmp("lj_t-bcdump-compat"),
                              "t-bcdump-compat.c")
      print("M5 bytecode dump compatibility tests passed")
    end
  })

  add({
    name = "m5_registry_root",
    description = "direct registry root publication C fixture",
    run = function(t)
      t:run_luajit_c_fixture(t:tmp("lj_t-registry-root"),
                              "t-registry-root.c")
      print("M5 registry root tests passed")
    end
  })

  add({
    name = "m5_nomm_cache",
    description = "metatable negative-cache policy C fixture",
    run = function(t)
      t:run_luajit_c_fixture(t:tmp("lj_t-nomm-cache"), "t-nomm-cache.c")
      print("M5 nomm cache tests passed")
    end
  })

  add({
    name = "m5_strtab_prep",
    description = "string table representation prep C fixture",
    run = function(t)
      t:run_luajit_c_fixture(t:tmp("lj_t-strtab-prep"),
                              "t-strtab-prep.c")
      print("M5 string table representation prep tests passed")
    end
  })

  add({
    name = "m5_strtab_cas",
    description = "string table CAS publication fixtures",
    run = function(t)
      local out = t:tmp("lj_t-strtab-cas")
      local out_rehash = t:tmp("lj_t-strtab-rehash")
      t:build({ clean = true, quiet = true })
      t:compile_luajit_c_fixture(out, "t-strtab-cas.c")
      t:run({ out }, { timeout = "20s" })
      t:compile_luajit_c_fixture(out_rehash, "t-strtab-rehash.c")
      t:run({ out_rehash }, { timeout = "20s" })
      print("M5 string table CAS publication tests passed")
    end
  })
end
