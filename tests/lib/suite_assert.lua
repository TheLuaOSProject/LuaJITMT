local M = {}

-- Public text is command output or a test-owned artifact: logs, benchmark CSVs,
-- manifests, release metadata, and similar product-facing text. Internal
-- implementation constraints are documented beside the constrained code and
-- covered through behavior fixtures, runtime counters, benchmarks, or package
-- artifacts.

local function text_contains(s, needle)
  return s:find(needle, 1, true) ~= nil
end

local function text_count_plain(s, needle)
  local count, pos = 0, 1
  while true do
    local first, last = s:find(needle, pos, true)
    if not first then return count end
    count = count + 1
    pos = last + 1
  end
end

function M.public_text_lines(s)
  local out = {}
  for line in (s .. "\n"):gmatch("(.-)\n") do
    if line ~= "" then out[#out + 1] = line end
  end
  return out
end

function M.public_text_iter_lines(s)
  return (s .. "\n"):gmatch("(.-)\n")
end

function M.assert_public_text_contains(label, data, needle, what)
  what = what or "public text"
  if not text_contains(data, needle) then
    error(label .. ": missing " .. what .. ": " .. needle, 2)
  end
end

function M.assert_public_text_all_contains(label, data, needles, what)
  for i = 1, #needles do
    M.assert_public_text_contains(label, data, needles[i], what)
  end
end

function M.assert_public_text_any_contains(label, data, needles, what)
  what = what or "public text"
  for i = 1, #needles do
    if text_contains(data, needles[i]) then return needles[i] end
  end
  error(label .. ": missing any " .. what .. ": " .. table.concat(needles, ", "), 2)
end

function M.assert_public_text_count(label, data, needle, mincount, what)
  what = what or "public text"
  local n = text_count_plain(data, needle)
  if n < mincount then
    error(label .. ": expected at least " .. mincount .. " " .. what ..
          " occurrences of " .. needle .. ", saw " .. n, 2)
  end
  return n
end

return M
