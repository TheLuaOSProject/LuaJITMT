local M = {}

function M.contains(s, needle)
  return s:find(needle, 1, true) ~= nil
end

function M.count_plain(s, needle)
  local count, pos = 0, 1
  while true do
    local first, last = s:find(needle, pos, true)
    if not first then return count end
    count = count + 1
    pos = last + 1
  end
end

function M.count_match(s, pattern)
  local count = 0
  for _ in s:gmatch(pattern) do count = count + 1 end
  return count
end

function M.lines(s)
  local out = {}
  for line in (s .. "\n"):gmatch("(.-)\n") do
    if line ~= "" then out[#out + 1] = line end
  end
  return out
end

function M.iter_lines(s)
  return (s .. "\n"):gmatch("(.-)\n")
end

function M.assert_text_contains(label, data, needle, what)
  what = what or "text"
  if not M.contains(data, needle) then
    error(label .. ": missing " .. what .. ": " .. needle, 2)
  end
end

function M.assert_text_match(label, data, pattern, what)
  what = what or "pattern"
  if not data:match(pattern) then
    error(label .. ": missing " .. what .. ": " .. pattern, 2)
  end
end

function M.assert_text_all_contains(label, data, needles, what)
  for i = 1, #needles do
    M.assert_text_contains(label, data, needles[i], what)
  end
end

function M.assert_text_any_contains(label, data, needles, what)
  what = what or "text"
  for i = 1, #needles do
    if M.contains(data, needles[i]) then return needles[i] end
  end
  error(label .. ": missing any " .. what .. ": " .. table.concat(needles, ", "), 2)
end

function M.is_source_file_content_path(path)
  local p = tostring(path)
  local source_ext = p:match("%.lua$") or p:match("%.c$") or
                     p:match("%.h$") or p:match("%.sh$") or
                     p:match("%.dasc$") or p:match("%.inc$")
  if p:match("^src/") or p:match("/src/") then return true end
  if not source_ext then return false end
  return p:match("^tests/") or p:match("/tests/") or
         p:match("^tools/") or p:match("/tools/") or
         p:match("^aux/") or p:match("/aux/") or
         p:match("^bench/") or p:match("/bench/")
end

function M.assert_not_source_file_content(path, level)
  if M.is_source_file_content_path(path) then
    error("source-file content assertions are not behavior tests: " ..
          tostring(path), (level or 1) + 1)
  end
end

function M.assert_text_contains_count(label, data, needle, mincount, what)
  what = what or "text"
  local n = M.count_plain(data, needle)
  if n < mincount then
    error(label .. ": expected at least " .. mincount .. " " .. what ..
          " occurrences of " .. needle .. ", saw " .. n, 2)
  end
  return n
end

function M.assert_text_match_count(label, data, pattern, mincount, what)
  what = what or "pattern"
  local n = M.count_match(data, pattern)
  if n < mincount then
    error(label .. ": expected at least " .. mincount .. " " .. what ..
          " matches for " .. pattern .. ", saw " .. n, 2)
  end
  return n
end

function M.assert_file_contains(t, path, needle, label)
  M.assert_not_source_file_content(path, 2)
  label = label or path
  M.assert_text_contains(label, t:read(path), needle, "file text")
end

function M.assert_file_match(t, path, pattern, label)
  M.assert_not_source_file_content(path, 2)
  label = label or path
  M.assert_text_match(label, t:read(path), pattern, "file pattern")
end

function M.assert_file_all_contains(t, path, needles, label)
  M.assert_not_source_file_content(path, 2)
  label = label or path
  M.assert_text_all_contains(label, t:read(path), needles, "file text")
end

function M.assert_file_any_contains(t, path, needles, label)
  M.assert_not_source_file_content(path, 2)
  label = label or path
  return M.assert_text_any_contains(label, t:read(path), needles, "file text")
end

function M.assert_dump_contains(t, dump, needle, label)
  local data = t:read(dump)
  label = label or dump
  M.assert_text_contains(label, data, needle, "dump text")
end

function M.assert_dump_match(t, dump, pattern, label)
  local data = t:read(dump)
  label = label or dump
  M.assert_text_match(label, data, pattern, "dump pattern")
end

function M.assert_dump_all_contains(t, dump, needles, label)
  local data = t:read(dump)
  label = label or dump
  M.assert_text_all_contains(label, data, needles, "dump text")
end

function M.assert_dump_contains_count(t, dump, needle, mincount, label)
  local data = t:read(dump)
  label = label or dump
  return M.assert_text_contains_count(label, data, needle, mincount, "dump text")
end

function M.assert_dump_match_count(t, dump, pattern, mincount, label)
  local data = t:read(dump)
  label = label or dump
  return M.assert_text_match_count(label, data, pattern, mincount, "dump pattern")
end

return M
