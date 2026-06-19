local function contains(s, needle)
  return s:find(needle, 1, true) ~= nil
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

return function(add)
  add({
    name = "m2_gc_header_accessors",
    description = "C-side GC header users go through lj_obj accessors",
    run = function(t)
      local skip = {
        [t:path("src", "lj_obj.h")] = true,
        [t:path("src", "lj_asm_x86.h")] = true
      }
      assert_no_lines(t, "direct C-side GC header access outside whitelist",
                      t:files(t:path("src"), {
                        extensions = { ".c", ".h" }
                      }), function(line, path)
        if skip[path] or contains(path, "/src/host/") then return false end
        return contains(line, "gch.marked") or
               contains(line, "gch.nextgc") or
               contains(line, "->marked") or
               contains(line, "->nextgc")
      end)
      print("guardrail: C-side GC header accessors clean")
    end
  })
end
