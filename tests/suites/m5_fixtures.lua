local runtime = require("suite_runtime")

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

  runtime.add_luajit_c_fixture_cases(add, {
    {
      name = "m5_itype_nan",
      description = "NaN TValue tag C fixture",
      output = "lj_t-itype-nan",
      cfile = "t-itype-nan.c",
      message = "M5 NaN tag tests passed"
    },
    {
      name = "m5_itype_sentinel",
      description = "internal table sentinel TValue C fixture",
      output = "lj_t-itype-sentinel",
      cfile = "t-itype-sentinel.c",
      message = "M5 internal table sentinel tag tests passed"
    },
    {
      name = "m5_bcdump_compat",
      description = "bytecode dump compatibility C fixture",
      output = "lj_t-bcdump-compat",
      cfile = "t-bcdump-compat.c",
      message = "M5 bytecode dump compatibility tests passed"
    },
    {
      name = "m5_registry_root",
      description = "direct registry root publication C fixture",
      output = "lj_t-registry-root",
      cfile = "t-registry-root.c",
      message = "M5 registry root tests passed"
    },
    {
      name = "m5_nomm_cache",
      description = "metatable negative-cache policy C fixture",
      output = "lj_t-nomm-cache",
      cfile = "t-nomm-cache.c",
      message = "M5 nomm cache tests passed"
    },
    {
      name = "m5_strtab_prep",
      description = "string table representation prep C fixture",
      output = "lj_t-strtab-prep",
      cfile = "t-strtab-prep.c",
      message = "M5 string table representation prep tests passed"
    }
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
