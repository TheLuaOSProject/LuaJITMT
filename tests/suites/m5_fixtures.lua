local utils = require("suite_utils")

local contains = utils.contains
local assert_no_lines = utils.assert_no_lines

local function count_matches(data, pattern)
  local count = 0
  for _ in data:gmatch(pattern) do count = count + 1 end
  return count
end

local function source_and_test_files(t)
  local files = t:files(t:path("src"), {
    extensions = { ".c", ".h", ".dasc" }
  })
  local tests = t:files(t:path("tests"), {
    extensions = { ".c", ".h", ".lua" }
  })
  for i = 1, #tests do files[#files + 1] = tests[i] end
  table.sort(files)
  return files
end

local function source_files(t)
  return t:files(t:path("src"), {
    extensions = { ".c", ".h", ".dasc" }
  })
end

local function raw_str_tab(line)
  local pos = 1
  local token = "str" .. ".tab"
  while true do
    local first, last = line:find(token, pos, true)
    if not first then return false end
    local nextc = line:sub(last + 1, last + 1)
    if nextc ~= "h" and (nextc == "" or not nextc:match("[%w_]")) then
      return true
    end
    pos = last + 1
  end
end

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
    description = "internal table sentinel TValue C fixture and markers",
    run = function(t)
      t:run_luajit_c_fixture(t:tmp("lj_t-itype-sentinel"),
                              "t-itype-sentinel.c")
      t:assert_all_contains(t:path("src", "lj_obj.h"), {
        "LJ_LIGHTUD_INTERNAL_SEG",
        "LJ_TFORWARD_BITS",
        "LJ_TKEYLOCK_BITS",
        "tvisforward",
        "tviskeylock",
        "tvistabinternal",
        "setforwardV",
        "setkeylockV"
      })
      print("M5 internal table sentinel tag tests passed")
    end
  })

  add({
    name = "m5_bcdump_compat",
    description = "bytecode dump compatibility C fixture and source guards",
    run = function(t)
      t:run_luajit_c_fixture(t:tmp("lj_t-bcdump-compat"),
                              "t-bcdump-compat.c")
      local files = {
        t:path("src", "lj_bcdump.h"),
        t:path("src", "lj_obj.h"),
        t:path("src", "lj_bcread.c"),
        t:path("src", "lj_bcwrite.c")
      }
      t:assert_all_any_contains(files, {
        "BCDUMP_VERSION_LEGACY",
        "BCDUMP_VERSION_TRANS",
        "BCDUMP_VERSION_LOCKLESS",
        "PROTO2_LEGACYUV",
        "PROTO2_CELLUV",
        "proto_setlegacyuv",
        "proto_setcelluv",
        "bcread_verify_bytecode",
        "bcread_uv_haslocal",
        "BCREAD_CELL_CNEW",
        "bcread_version(ls) != BCDUMP_VERSION_LOCKLESS && op >= BC_CNEW",
        "cellops |= BCREAD_CELL_CNEW",
        "bcwrite_has_legacyuv"
      })
      for i = 1, #files do
        t:assert_not_match(files[i], "#if%s+LJ_MT", "#if LJ_MT")
        t:assert_not_match(files[i], "#ifdef%s+LJ_MT", "#ifdef LJ_MT")
        t:assert_not_contains(files[i], "LUAJIT_THREADSAFE")
      end
      print("M5 bytecode dump compatibility tests passed")
    end
  })

  add({
    name = "m5_registry_root",
    description = "direct registry root publication C fixture and guard",
    run = function(t)
      t:run_luajit_c_fixture(t:tmp("lj_t-registry-root"),
                              "t-registry-root.c")
      local block = t:c_block(t:path("src", "lj_api.c"),
                              "static void copy_slot")
      t:assert_text_ordered("LUA_REGISTRYINDEX write", block, {
        "if (idx == LUA_REGISTRYINDEX)",
        "lj_gc_barrierroot(L, f)",
        "copyTVrel(L, o, f)",
        "} else if (idx < LUA_GLOBALSINDEX)"
      })
      print("M5 registry root tests passed")
    end
  })

  add({
    name = "m5_nomm_cache",
    description = "metatable negative-cache policy C fixture and guards",
    run = function(t)
      t:run_luajit_c_fixture(t:tmp("lj_t-nomm-cache"), "t-nomm-cache.c")
      t:assert_not_match(t:path("src", "lj_meta.c"),
                         "%-%>nomm%s*%|=",
                         "->nomm |=")

      local clears = count_matches(t:read(t:path("src", "lj_api.c")),
                                   "mt%-%>nomm = 0;.*stale metamethod miss") +
                     count_matches(t:read(t:path("src", "lib_base.c")),
                                   "mt%-%>nomm = 0;.*stale metamethod miss")
      if clears ~= 2 then
        error("C API and base setmetatable must clear installed mt nomm")
      end

      local ffunc = t:text_between(t:path("src", "vm_x64.dasc"),
                                   ".ffunc_2 setmetatable",
                                   "mov TAB:RB->metatable, TAB:RA")
      t:assert_text_contains("x64 setmetatable", ffunc,
                             "mov byte TAB:RA->nomm, 0")

      local rec = t:c_block(t:path("src", "lj_ffrecord.c"),
                            "static void LJ_FASTCALL recff_setmetatable")
      t:assert_text_contains("recff_setmetatable", rec, "IRFL_TAB_NOMM")
      t:assert_text_contains("recff_setmetatable", rec, "IRFL_TAB_META")
      print("M5 nomm cache tests passed")
    end
  })

  add({
    name = "m5_strtab_prep",
    description = "string table representation prep C fixture and guards",
    run = function(t)
      t:run_luajit_c_fixture(t:tmp("lj_t-strtab-prep"),
                              "t-strtab-prep.c")

      assert_no_lines(t, "string table users must route through g->str.tabh",
                      source_and_test_files(t), raw_str_tab)

      t:assert_all_any_contains({
        t:path("src", "lj_obj.h"),
        t:path("src", "lj_str.h")
      }, {
        "typedef struct StrTabHdr",
        "StrTabHdr *tabh",
        "#define LJ_STRHASH_DEAD",
        "#define LJ_STRHASH_SECONDARY",
        "#define LJ_STRHASH_LINKMASK"
      })

      assert_no_lines(t, "string hash marker code must not use raw bit0 masks",
                      {
                        t:path("src", "lj_str.c"),
                        t:path("src", "lj_gc.c"),
                        t:path("src", "lj_gc2.c")
                      }, function(line)
        return contains(line, "& ~(uintptr_t)1") or
               contains(line, "& 1)") or
               contains(line, "| (u & 1)") or
               contains(line, "(uintptr_t)1)")
      end)
      print("M5 string table representation prep tests passed")
    end
  })

  add({
    name = "m5_strtab_cas",
    description = "string table CAS publication fixtures and guards",
    run = function(t)
      local out = t:tmp("lj_t-strtab-cas")
      local out_rehash = t:tmp("lj_t-strtab-rehash")
      t:build({ clean = true, quiet = true })
      t:compile_luajit_c_fixture(out, "t-strtab-cas.c")
      t:run({ out }, { timeout = "20s" })
      t:compile_luajit_c_fixture(out_rehash, "t-strtab-rehash.c")
      t:run({ out_rehash }, { timeout = "20s" })

      t:assert_all_any_contains(source_files(t), {
        "LJ_STRTAB_RESIZE",
        "strtab_enter",
        "strtab_leave",
        "strtab_claim",
        "strtab_release",
        "strtab_retire",
        "retired_next",
        "retire_epoch",
        "g->str.retired",
        "lj_str_reclaim_retired",
        "lj_gc2_reclaim_retired",
        "la_xchgptr_acqrel((void **)&g->str.retired",
        "la_load64_acq(&hdr->retire_epoch) < completed_epoch",
        "lj_str_reclaim_retired(g, epoch)",
        "lj_gc2_reclaim_retired(g, epoch)",
        "smr_reclaimed",
        "gc_mark_strtab_mem",
        "gc2_mark_strtab_mem",
        "LJ_STRTAB_ACTIVE_MASK",
        "state | LJ_STRTAB_RESIZE",
        "while (la_load32_acq(&hdr->resize) & LJ_STRTAB_ACTIVE_MASK)",
        "strtab_claim(hdr)",
        "strtab_release(hdr)",
        "lj_str_ref_load_acq",
        "lj_str_ref_store_rel",
        "lj_str_hashhead_acq",
        "lj_str_next_acq",
        "lj_str_next_store_rel",
        "lj_str_bucket_store_rel",
        "strref_cas_rel",
        "la_storeptr_rel((void **)&g->str.tabh",
        "la_add32_rlx(&g->str.num",
        "la_sub32_acqrel(&g->str.num",
        "la_load32_acq(&g->str.num)",
        "la_add32_rlx(&g->str.id"
      })

      local lj_str = t:path("src", "lj_str.c")
      t:assert_not_contains(lj_str, "lj_mem_free(g, oldhdr")
      assert_no_lines(t, "string table chains must use acquire/release helpers",
                      { lj_str }, function(line)
        return line:find("setgcrefp%(") ~= nil or
               line:find("setgcrefr%(") ~= nil or
               contains(line, "setgcref(newtab") or
               contains(line, "lj_str_hashhead(oldtab") or
               contains(line, "lj_str_hashhead(strtab") or
               contains(line, "gcnext(o)") or
               contains(line, "gcrefu(newtab") or
               contains(line, "gcrefu(strtab") or
               contains(line, "gcrefu(*chain") or
               contains(line, "gcrefu(q)")
      end)

      assert_no_lines(t, "GC string-table scans must acquire bucket heads",
                      {
                        t:path("src", "lj_gc.c"),
                        t:path("src", "lj_gc2.c")
                      }, function(line)
        return contains(line, "lj_str_hashhead(strtab")
      end)

      local free_count = count_matches(t:read(lj_str),
                                       "lj_mem_free%(g, hdr, lj_str_tabbytes%(hdr%)%)")
      if free_count ~= 3 then
        error("retired string table headers should only be freed by reclaim and state close")
      end

      assert_no_lines(t, "string count must use atomic add/sub/load helpers",
                      { lj_str, t:path("src", "lj_gc.c") }, function(line)
        return contains(line, "g->str.num--") or
               contains(line, "++g->str.num") or
               contains(line, "if (g->str.num") or
               contains(line, "g->str.num <=") or
               contains(line, "g->str.num +=")
      end)

      assert_no_lines(t, "string IDs must not mutate global id/reseed state in allocation",
                      { lj_str }, function(line)
        return contains(line, "g->str.id++") or
               contains(line, "++g->str.id") or
               contains(line, "g->str.id =") or
               contains(line, "g->str.idreseed") or
               contains(line, "idreseed--")
      end)
      print("M5 string table CAS publication tests passed")
    end
  })
end
