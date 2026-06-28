local checks = require("suite_assert")
local runtime = require("suite_runtime")
local utils = require("suite_utils")
local probes = require("local_cell_probes")

local M = {}

local assert_text_any_contains = checks.assert_text_any_contains
local assert_text_all_contains = checks.assert_text_all_contains
local contains = checks.contains
local assert_dump_contains = checks.assert_dump_contains
local assert_dump_match = checks.assert_dump_match
local luajit = runtime.luajit
local luajit_code = runtime.luajit_code
local luajit_capture = runtime.capture_luajit
local luajit_dump = runtime.luajit_dump

local function dump_i(t, dump, code)
  luajit_dump(t, dump, "-jdump=i", code)
end

local function dump_im(t, dump, code)
  luajit_dump(t, dump, "-jdump=im", code)
end

local function assert_dump_not_contains(t, dump, needle, label)
  local data = t:read(dump)
  label = label or dump
  if contains(data, needle) then
    error(label .. ": unexpected dump text: " .. needle, 2)
  end
end

function M.run_source_guards(t)
  local asm = utils.read_source_file(t:path("src", "lj_asm_x86.h"))
  checks.assert_text_contains("x64 closed-upvalue USTORE helper guard", asm,
    "if (ir->o == IR_USTORE && IR(ir->op1)->o == IR_UREFC) {",
    "all-TValue USTORE helper condition")
  if contains(asm,
      "IR_USTORE && irt_isgcv(ir->t) && IR(ir->op1)->o == IR_UREFC") then
    error("x64 closed-upvalue USTORE helper must not be gated to GC values", 2)
  end

end

function M.run_bytecode_guards(t, tmpname)
  local out = t:tmp(tmpname)
  luajit_capture(t, { "-bl", "-e", probes.parser_capture() }, out)
  local bc = t:read(out)
  assert_text_any_contains("captured local parser output", bc,
                           { "CGET", "CSET" }, "bytecode marker")

  luajit_capture(t, { "-bl", "-e", probes.self_capture() }, out)
  bc = t:read(out)
  assert_text_all_contains("self-captured local function", bc,
                           { "CNEW", "CSET" }, "bytecode marker")
  t:remove(out)
end

function M.run_publication_behavior_guards(t)
  luajit(t, { "-e", probes.dumped_closure_behavior() })
  luajit(t, { "-e", probes.debug_local_behavior() })
  luajit(t, { "-e", probes.owner_numeric({
    trace_assert = "expected traced CGET/CSET owner loop",
    second_run = true
  }) })
  luajit(t, { "-e", probes.owner_gc({
    trace_assert = "expected traced GC-valued CSET owner loop",
    second_run = true
  }) })
  luajit(t, { "-e", probes.owner_primitive({
    trace_assert = "expected traced primitive CSET owner loop",
    second_run = true
  }) })
  luajit(t, { "-e", probes.loaded_owner_numeric({
    trace_assert = "expected loaded owner CGET/CSET trace",
    second_run = true
  }) })
  luajit(t, { "-e", probes.child_numeric({
    trace_assert = "expected traced child numeric upvalue loop",
    second_run = true
  }) })
  luajit(t, { "-e", probes.child_gc({
    trace_assert = "expected traced child GC upvalue loop",
    second_run = true
  }) })
  luajit(t, { "-e", probes.loaded_child_numeric({
    trace_assert = "expected loaded child upvalue trace",
    second_run = true
  }) })
  luajit(t, { "-e", probes.loaded_cnew_fnew({
    trace_assert = "expected loaded CNEW creation trace"
  }) })
end

function M.run_jit_dump_guards(t, dump)
  dump_i(t, dump, probes.owner_numeric({ flush = false, hotexit = true }))
  assert_dump_contains(t, dump, "TRACE 1 stop -> loop", "owner numeric trace")
  assert_dump_contains(t, dump, "UREFC", "owner numeric UREFC")
  assert_dump_contains(t, dump, "ULOAD", "owner numeric ULOAD")
  assert_dump_contains(t, dump, "USTORE", "owner numeric USTORE")

  dump_im(t, dump, probes.owner_numeric({ flush = false, hotexit = true }))
  assert_dump_contains(t, dump, "->lj_func_storeuv_forjit",
		       "owner numeric USTORE helper lowering")

  dump_i(t, dump, probes.owner_primitive({ flush = false, hotexit = true }))
  assert_dump_contains(t, dump, "TRACE 1 stop -> loop", "owner primitive trace")
  assert_dump_contains(t, dump, "UREFC", "owner primitive UREFC")
  assert_dump_contains(t, dump, "USTORE", "owner primitive USTORE")

  dump_im(t, dump, probes.owner_primitive({ flush = false, hotexit = true }))
  assert_dump_contains(t, dump, "->lj_func_storeuv_forjit",
		       "owner primitive USTORE helper lowering")

  dump_i(t, dump, probes.owner_gc({ flush = false, hotexit = true }))
  assert_dump_contains(t, dump, "TRACE 1 stop -> loop", "owner GC-valued trace")
  assert_dump_contains(t, dump, "UREFC", "owner GC-valued UREFC")
  assert_dump_contains(t, dump, "USTORE", "owner GC-valued USTORE")
  assert_dump_contains(t, dump, "OBAR", "owner GC-valued OBAR")

  dump_i(t, dump, probes.loaded_owner_numeric({ hotexit = true }))
  assert_dump_contains(t, dump, "TRACE 1 stop -> loop", "loaded v4 CGET/CSET trace")
  assert_dump_contains(t, dump, "UREFC", "loaded v4 CGET/CSET UREFC")
  assert_dump_contains(t, dump, "ULOAD", "loaded v4 CGET/CSET ULOAD")
  assert_dump_contains(t, dump, "USTORE", "loaded v4 CGET/CSET USTORE")

  dump_i(t, dump, probes.source_cnew_fnew({
    trace_assert = "source CNEW/FNEW creation should trace"
  }))
  assert_dump_match(t, dump, "CALLS.*lj_func_newuvcell_forjit", "source CNEW helper call")
  assert_dump_match(t, dump, "CALLA.*lj_func_newL_gc_forjit", "source FNEW helper call")
  assert_dump_contains(t, dump, "UREFC", "source CNEW/FNEW UREFC")
  assert_dump_contains(t, dump, "USTORE", "source CNEW/FNEW USTORE")
  assert_dump_contains(t, dump, "OBAR", "source CNEW/FNEW OBAR")

  dump_i(t, dump, probes.loaded_cnew_fnew({
    trace_assert = "loaded CNEW/FNEW creation should trace"
  }))
  assert_dump_match(t, dump, "CALLS.*lj_func_newuvcell_forjit", "loaded CNEW helper call")
  assert_dump_match(t, dump, "CALLA.*lj_func_newL_gc_forjit", "loaded FNEW helper call")
  assert_dump_contains(t, dump, "UREFC", "loaded CNEW/FNEW UREFC")
  assert_dump_contains(t, dump, "USTORE", "loaded CNEW/FNEW USTORE")
  assert_dump_contains(t, dump, "OBAR", "loaded CNEW/FNEW OBAR")

  dump_i(t, dump, probes.self_recursive_call({
    hotexit = true,
    trace_assert = "self-recursive immutable call should trace"
  }))
  assert_dump_contains(t, dump, "TRACE 1", "self-recursive immutable trace")
  assert_dump_contains(t, dump, "UREFC", "self-recursive local-cell UGET")
  assert_dump_contains(t, dump, "ULOAD", "self-recursive local-cell UGET")

  dump_i(t, dump, probes.source_mixed_raw_local({
    hotexit = true,
    trace_assert = "source mixed raw-local CNEW/FNEW should trace"
  }))
  assert_dump_contains(t, dump, "TRACE 1 stop -> loop", "source mixed raw-local FNEW trace")
  assert_dump_contains(t, dump, "TMPREF", "source mixed raw-local TMPREF")
  assert_dump_match(t, dump, "CALLS.*lj_func_syncslot_forjit", "source mixed raw-local sync helper")
  assert_dump_match(t, dump, "CALLA.*lj_func_newL_gc_forjit", "source mixed raw-local FNEW helper")
  assert_dump_contains(t, dump, "UREFC", "source mixed raw-local UREFC")
  assert_dump_contains(t, dump, "USTORE", "source mixed raw-local USTORE")

  dump_i(t, dump, probes.loaded_mixed_raw_local({
    hotexit = true,
    trace_assert = "loaded mixed raw-local CNEW/FNEW should trace"
  }))
  assert_dump_contains(t, dump, "TRACE 1 stop -> loop", "loaded mixed raw-local FNEW trace")
  assert_dump_match(t, dump, "CALLS.*lj_func_syncslot_forjit", "loaded mixed raw-local sync helper")
  assert_dump_match(t, dump, "CALLA.*lj_func_newL_gc_forjit", "loaded mixed raw-local FNEW helper")

  dump_i(t, dump, probes.source_first_promotion({
    hotexit = true,
    trace_assert = "source first-promotion FNEW should trace"
  }))
  assert_dump_contains(t, dump, "TRACE 1 stop -> loop", "source first-promotion FNEW trace")
  assert_dump_match(t, dump, "CALLS.*lj_func_promoteuv_forjit", "source first-promotion helper")
  assert_dump_contains(t, dump, "NULL", "source first-promotion stack snapshot argument")
  assert_dump_match(t, dump, "SLOAD.*I", "source first-promotion inherited cell reload")
  assert_dump_contains(t, dump, "UREFC", "source first-promotion UREFC")
  assert_dump_contains(t, dump, "USTORE", "source first-promotion USTORE")

  dump_i(t, dump, probes.loaded_first_promotion({
    hotexit = true,
    trace_assert = "loaded first-promotion FNEW should trace"
  }))
  assert_dump_contains(t, dump, "TRACE 1 stop -> loop", "loaded first-promotion FNEW trace")
  assert_dump_match(t, dump, "CALLS.*lj_func_promoteuv_forjit", "loaded first-promotion helper")
  assert_dump_match(t, dump, "CALLA.*lj_func_newL_gc_forjit", "loaded first-promotion FNEW helper")

  dump_i(t, dump, probes.assigned_before_fnew({
    hotexit = true,
    trace_assert = "assigned-before-FNEW creation should trace"
  }))
  assert_dump_contains(t, dump, "TRACE 1 stop -> loop", "assigned-before-FNEW trace")
  assert_dump_match(t, dump, "CALLS.*lj_func_syncslot_forjit", "assigned-before-FNEW sync helper")
  assert_dump_match(t, dump, "CALLA.*lj_func_newL_gc_forjit", "assigned-before-FNEW FNEW helper")
  assert_dump_not_contains(t, dump, "lj_func_promoteuv_forjit",
			   "assigned-before-FNEW discarded promotion")
end

function M.run_jit_runtime_guards(t)
  luajit_code(t, probes.pre_fnew_update({
    hotexit = true,
    trace_assert = "pre-FNEW promoted local update should trace"
  }))
  luajit_code(t, probes.post_fnew_update({
    hotexit = true,
    trace_assert = "post-FNEW promoted local update should trace"
  }))
end

return M
