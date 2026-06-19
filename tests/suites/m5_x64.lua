local function contains(s, needle)
  return s:find(needle, 1, true) ~= nil
end

local function count_plain(s, needle)
  local count, pos = 0, 1
  while true do
    local first, last = s:find(needle, pos, true)
    if not first then return count end
    count = count + 1
    pos = last + 1
  end
end

local function assert_text_not_contains(label, data, needle)
  if contains(data, needle) then
    error(label .. ": forbidden text present: " .. needle, 2)
  end
end

local function assert_no_lines(t, label, paths, pred)
  local hits = {}
  for i = 1, #paths do
    local path = paths[i]
    local n = 0
    for line in (t:read(path) .. "\n"):gmatch("(.-)\n") do
      n = n + 1
      if pred(line, path, n) then
        hits[#hits + 1] = path .. ":" .. n .. ": " .. line
      end
    end
  end
  if #hits > 0 then
    error(label .. ":\n" .. table.concat(hits, "\n"), 2)
  end
end

local function build_and_run_x64_c(t, out, cfile)
  t:cc(out, { t:path("tests", cfile) }, {
    link_luajit = true,
    libs = { "-lm", "-ldl", "-pthread" }
  })
  t:run({ out })
end

local function tget_array_header_smoke()
  return [[
local t = {}
for i = 1, 96 do t[i] = i * 3 end
assert(t[64] == 192)
local k = 70
assert(t[k] == 210)
local function getv(a, key) return a[key] end
assert(getv(t, 80) == 240)
assert(t[120] == nil)
]]
end

local function tgets_node_order_smoke()
  return [[
local t = { foo = 17 }
local sum = 0
for i = 1, 200 do
  sum = sum + t.foo
  t.bar = i
  assert(t.bar == i)
end
assert(sum == 200 * 17)
]]
end

local function getmetatable_node_order_smoke()
  return [[
local token = {}
local t = setmetatable({}, { __metatable = token })
assert(getmetatable(t) == token)
local u = setmetatable({}, {})
assert(type(getmetatable(u)) == "table")
]]
end

local function ipairs_snapshot_smoke()
  return [[
local t = { 10, 20, nil, 40 }
local n, sum = 0, 0
for i, v in ipairs(t) do
  n = n + 1
  sum = sum + i + v
end
assert(n == 2 and sum == 33)
]]
end

local function itern_snapshot_smoke()
  return [[
local t = { [0] = "z", "a", nil, "c", alpha = 11, beta = 12 }
local seen, n = {}, 0
for k, v in pairs(t) do
  seen[k] = v
  n = n + 1
end
assert(n == 5)
assert(seen[0] == "z" and seen[1] == "a" and seen[3] == "c")
assert(seen.alpha == 11 and seen.beta == 12)
]]
end

local function table_next_snapshot_smoke()
  return [[
local t = { [0] = "z", "a", nil, "c", x = 41, y = 42 }
local seen = {}
for k, v in next, t, nil do seen[k] = v end
assert(seen[0] == "z" and seen[1] == "a" and seen[3] == "c")
assert(seen.x == 41 and seen.y == 42)
local n = 0
for k, v in pairs(t) do
  assert(seen[k] == v)
  n = n + 1
end
assert(n == 5)
]]
end

return function(add)
  add({
    name = "m5_x64_getmetatable_node_order",
    description = "x64 getmetatable node-header hmask ordering guard",
    run = function(t)
      local vm = t:path("src", "vm_x64.dasc")
      t:build({ clean = true, quiet = true })
      t:luajit({ "-joff", "-e", getmetatable_node_order_smoke() })
      t:assert_all_contains(vm, {
        "|.ffunc_1 getmetatable",
        "|  mov r8, TAB:RB->node",
        "|  mov r9d, dword [r8+TABNODE_FLAGS_OFS]",
        "|  test r9d, TABNODE_FLAG_RETIRING",
        "|  jnz ->fff_fallback",
        "|  mov RAd, dword [r8+TABNODE_HMASK_OFS]",
        "|  add NODE:RA, r8"
      })

      local block = t:text_between(vm, "|.ffunc_1 getmetatable",
                                   "|.ffunc_2 setmetatable")
      t:assert_text_ordered("getmetatable", block, {
        "mov r8, TAB:RB->node",
        "mov r9d, dword [r8+TABNODE_FLAGS_OFS]",
        "test r9d, TABNODE_FLAG_RETIRING",
        "jnz ->fff_fallback",
        "mov RAd, dword [r8+TABNODE_HMASK_OFS]"
      })
      assert_text_not_contains("getmetatable", block, "TAB:RB->hmask")
      print("M5 x64 getmetatable node-header hmask guard passed")
    end
  })

  add({
    name = "m5_x64_tget_array_header",
    description = "x64 TGET array-header bounds/FORWARD guard",
    run = function(t)
      local vm = t:path("src", "vm_x64.dasc")
      t:build({ clean = true, quiet = true })
      t:luajit({ "-joff", "-e", tget_array_header_smoke() })
      build_and_run_x64_c(t, t:tmp("lj_t-x64-tget-forward"),
                          "t-x64-tget-forward.c")

      t:assert_all_any_contains({
        vm,
        t:path("tests", "t-x64-tget-forward.c")
      }, {
        "TAB_COLO_SLOTS",
        "TABARRAY_ASIZE_OFS",
        "mov ITYPEd, TAB:RB->asize",
        "mov TMPR, TAB:RB->array",
        "lea r8, [RB+TAB_COLO_SLOTS]",
        "mov ITYPEd, dword [TMPR+TABARRAY_ASIZE_OFS]",
        "cmp RCd, ITYPEd",
        "add RC, TMPR",
        "mov64 r9, LJ_TFORWARD_BITS",
        "je ->vmeta_tgetv",
        "je ->vmeta_tgetb",
        "mov r9d, RCd",
        "mov64 r8, LJ_TFORWARD_BITS",
        "jmp ->vmeta_tgetr",
        "t-x64-tget-forward OK"
      })

      local block = t:text_between(vm, "case BC_TGETV:", "case BC_TSETV:")
      for _, spec in ipairs({
        { "mov ITYPEd, TAB:RB->asize", 3 },
        { "mov TMPR, TAB:RB->array", 3 },
        { "lea r8, [RB+TAB_COLO_SLOTS]", 3 },
        { "mov ITYPEd, dword [TMPR+TABARRAY_ASIZE_OFS]", 3 },
        { "cmp RCd, ITYPEd", 3 },
        { "add RC, TMPR", 3 }
      }) do
        if count_plain(block, spec[1]) ~= spec[2] then
          error("x64 TGET array fast paths missing marker: " .. spec[1])
        end
      end
      if count_plain(block, "LJ_TFORWARD_BITS") < 3 then
        error("x64 TGET array fast paths must reject FORWARD values")
      end
      assert_text_not_contains("x64 TGET", block, "cmp RCd, TAB:RB->asize")
      assert_text_not_contains("x64 TGET", block, "add RC, TAB:RB->array")
      print("M5 x64 TGET array-header guard passed")
    end
  })

  add({
    name = "m5_x64_tgets_node_order",
    description = "x64 TGETS node-header hmask and TSETS slow-path guard",
    run = function(t)
      local vm = t:path("src", "vm_x64.dasc")
      t:build({ clean = true, quiet = true })
      t:luajit({ "-joff", "-e", tgets_node_order_smoke() })
      build_and_run_x64_c(t, t:tmp("lj_t-x64-tgets-forward"),
                          "t-x64-tgets-forward.c")

      t:assert_all_any_contains({
        vm,
        t:path("tests", "t-x64-tgets-forward.c")
      }, {
        "|->BC_TGETS_Z:",
        "|  mov r8, TAB:RB->node",
        "|  mov r9d, dword [r8+TABNODE_FLAGS_OFS]",
        "|  test r9d, TABNODE_FLAG_RETIRING",
        "|  jnz ->vmeta_tgets",
        "|  mov TMPRd, dword [r8+TABNODE_HMASK_OFS]",
        "|  add NODE:TMPR, r8",
        "|  mov64 r9, LJ_TFORWARD_BITS",
        "|  cmp ITYPE, r9",
        "|  je ->vmeta_tgets",
        "|->BC_TSETS_Z:",
        "|  jmp ->vmeta_tsets\t\t// M5: no legacy x64 hash-slot store.",
        "t-x64-tgets-forward OK"
      })

      local get = t:text_between(vm, "|->BC_TGETS_Z:", "case BC_TGETB:")
      t:assert_text_ordered("BC_TGETS_Z", get, {
        "mov r8, TAB:RB->node",
        "mov r9d, dword [r8+TABNODE_FLAGS_OFS]",
        "test r9d, TABNODE_FLAG_RETIRING",
        "jnz ->vmeta_tgets",
        "mov TMPRd, dword [r8+TABNODE_HMASK_OFS]",
        "mov64 r9, LJ_TFORWARD_BITS",
        "cmp ITYPE, r9",
        "je ->vmeta_tgets"
      })
      assert_text_not_contains("BC_TGETS_Z", get, "TAB:RB->hmask")
      local set = t:text_between(vm, "|->BC_TSETS_Z:", "case BC_TSETB:")
      t:assert_text_contains("BC_TSETS_Z", set, "jmp ->vmeta_tsets")
      assert_text_not_contains("BC_TSETS_Z", set, "mov [TMPR], ITYPE")
      print("M5 x64 TGETS node-header and TSETS slow-path guard passed")
    end
  })

  add({
    name = "m5_x64_ipairs_snapshot",
    description = "x64 ipairs_aux array/hash snapshot and FORWARD guard",
    run = function(t)
      local vm = t:path("src", "vm_x64.dasc")
      t:build({ clean = true, quiet = true })
      t:luajit({ "-joff", "-e", ipairs_snapshot_smoke() })
      build_and_run_x64_c(t, t:tmp("lj_t-x64-ipairs-forward"),
                          "t-x64-ipairs-forward.c")

      t:assert_all_any_contains({
        vm,
        t:path("src", "lj_tab.c"),
        t:path("tests", "t-x64-ipairs-forward.c")
      }, {
        "TABARRAY_ASIZE_OFS",
        "mov TMPRd, TAB:RB->asize",
        "mov RD, TAB:RB->array",
        "lea r8, [RB+TAB_COLO_SLOTS]",
        "mov TMPRd, dword [RD+TABARRAY_ASIZE_OFS]",
        "mov r8, [RD]",
        "cmp r8, LJ_TNIL;  je ->fff_res0",
        "mov64 r9, LJ_TFORWARD_BITS",
        "cmp r8, r9; jne >4",
        "call extern lj_tab_getint_hop",
        "t-x64-ipairs-forward OK",
        "mov [BASE-8], r8",
        "mov r8, TAB:RB->node",
        "mov r9d, dword [r8+TABNODE_FLAGS_OFS]",
        "test r9d, TABNODE_FLAG_RETIRING",
        "jnz >5",
        "cmp dword [r8+TABNODE_HMASK_OFS], 0; je ->fff_res0"
      })

      local block = t:text_between(vm, "|.ffunc_2 ipairs_aux",
                                   "|.ffunc_1 ipairs")
      t:assert_text_ordered("ipairs_aux", block, {
        "mov TMPRd, TAB:RB->asize",
        "mov RD, TAB:RB->array"
      })
      for _, needle in ipairs({
        "mov r9d, dword [r8+TABNODE_FLAGS_OFS]",
        "test r9d, TABNODE_FLAG_RETIRING",
        "jnz >5",
        "mov64 r9, LJ_TFORWARD_BITS",
        "cmp r8, r9",
        "call extern lj_tab_getint_hop"
      }) do
        t:assert_text_contains("ipairs_aux", block, needle)
      end
      for _, reject in ipairs({
        "cmp aword [RD], LJ_TNIL",
        "mov RB, [RD]",
        "cmp dword TAB:RB->hmask, 0",
        "cmp RAd, TAB:RB->asize"
      }) do
        assert_text_not_contains("ipairs_aux", block, reject)
      end
      print("M5 x64 ipairs_aux node-header snapshot guard passed")
    end
  })

  add({
    name = "m5_x64_itern_snapshot",
    description = "x64 BC_ITERN array/hash snapshot and FORWARD guard",
    run = function(t)
      local vm = t:path("src", "vm_x64.dasc")
      t:build({ clean = true, quiet = true })
      t:luajit({ "-joff", "-e", itern_snapshot_smoke() })
      build_and_run_x64_c(t, t:tmp("lj_t-x64-itern-forward"),
                          "t-x64-itern-forward.c")

      t:assert_all_any_contains({
        vm,
        t:path("tests", "t-x64-itern-forward.c")
      }, {
        "lea r8, [RB+TAB_COLO_SLOTS]",
        "mov TMPRd, dword [ITYPE+TABARRAY_ASIZE_OFS]",
        "mov r8, [ITYPE+RC*8]",
        "mov64 r9, LJ_TFORWARD_BITS",
        "call extern lj_tab_itern_forward",
        "cmp r8, LJ_TNIL; je >4",
        "mov [BASE+RA*8+8], r8",
        "mov r8, TAB:RB->node",
        "mov r9d, dword [r8+TABNODE_FLAGS_OFS]",
        "test r9d, TABNODE_FLAG_RETIRING",
        "jnz >9",
        "mov r9d, dword [r8+TABNODE_HMASK_OFS]",
        "mov r8, NODE:ITYPE->val",
        "cmp r8, LJ_TNIL; je >7",
        "mov r9, NODE:ITYPE->key",
        "t-x64-itern-forward OK"
      })

      local block = t:text_between(vm, "case BC_ITERN:", "case BC_ISNEXT:")
      for _, needle in ipairs({
        "mov r9d, dword [r8+TABNODE_FLAGS_OFS]",
        "test r9d, TABNODE_FLAG_RETIRING",
        "jnz >9"
      }) do
        t:assert_text_contains("BC_ITERN", block, needle)
      end
      if count_plain(block, "mov64 r9, LJ_TFORWARD_BITS") < 2 or
         count_plain(block, "call extern lj_tab_itern_forward") < 2 then
        error("x64 BC_ITERN must resolve forwarded array/hash slots in C")
      end
      for _, reject in ipairs({
        "cmp aword [ITYPE+RC*8], LJ_TNIL",
        "cmp aword NODE:ITYPE->val, LJ_TNIL",
        "mov RB, [ITYPE+RC*8]",
        "mov RC, NODE:ITYPE->val",
        "cmp RCd, TAB:RB->hmask",
        "add NODE:ITYPE, TAB:RB->node"
      }) do
        assert_text_not_contains("BC_ITERN", t:read(vm), reject)
      end
      print("M5 x64 BC_ITERN node-header snapshot guard passed")
    end
  })

  add({
    name = "m5_x64_table_next_snapshot",
    description = "x64 lj_vm_next array/hash snapshot and FORWARD guard",
    run = function(t)
      local vm = t:path("src", "vm_x64.dasc")
      t:build({ clean = true, quiet = true })
      t:luajit({ "-e", table_next_snapshot_smoke() })
      build_and_run_x64_c(t, t:tmp("lj_t-x64-vm-next-forward"),
                          "t-x64-vm-next-forward.c")

      t:assert_all_any_contains({
        vm,
        t:path("src", "lj_tab.c"),
        t:path("tests", "t-x64-vm-next-forward.c")
      }, {
        "mov r10, NEXT_TAB->array",
        "lea NEXT_TMP, [NEXT_TAB+TAB_COLO_SLOTS]",
        "mov NEXT_ASIZE, dword [r10+TABARRAY_ASIZE_OFS]",
        "mov NEXT_TMP, qword [r10+NEXT_IDX*8]",
        "mov64 r11, LJ_TFORWARD_BITS",
        "call extern lj_tab_vmnext_forward",
        "mov r8, NEXT_TAB->node",
        "mov r9d, dword [r8+TABNODE_FLAGS_OFS]",
        "test r9d, TABNODE_FLAG_RETIRING",
        "jnz >3",
        "mov r9d, dword [r8+TABNODE_HMASK_OFS]",
        "mov NEXT_TMP, NODE:NEXT_PTR->val",
        "cmp NEXT_TMP, LJ_TNIL; je >7",
        "t-x64-vm-next-forward OK"
      })

      local block = t:text_between(vm, "->vm_next:",
                                   "|//-----------------------------------------------------------------------")
      for _, needle in ipairs({
        "mov r9d, dword [r8+TABNODE_FLAGS_OFS]",
        "test r9d, TABNODE_FLAG_RETIRING",
        "jnz >3"
      }) do
        t:assert_text_contains("vm_next", block, needle)
      end
      if count_plain(block, "mov64 r11, LJ_TFORWARD_BITS") < 2 or
         count_plain(block, "call extern lj_tab_vmnext_forward") < 2 then
        error("x64 lj_vm_next must resolve forwarded array/hash slots in C")
      end
      for _, reject in ipairs({
        "cmp qword NODE:NEXT_PTR->val, LJ_TNIL",
        "cmp NEXT_IDX, NEXT_TAB->hmask",
        "add NODE:NEXT_PTR, NEXT_TAB->node",
        "mov NEXT_TMP, NEXT_TAB->array"
      }) do
        assert_text_not_contains("vm_next", t:read(vm), reject)
      end
      print("M5 x64 table next node-header snapshot guard passed")
    end
  })

end
