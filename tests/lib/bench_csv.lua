local M = {}

local function split_csv(line)
  local out = {}
  for col in (line .. ","):gmatch("(.-),") do
    out[#out + 1] = col
  end
  return out
end

local function header_map(header)
  local map = {}
  for i = 1, #header do map[header[i]] = i end
  return map
end

local function lines(data)
  return (data .. "\n"):gmatch("(.-)\n")
end

local function row_value(row, header, column)
  local idx = header.map[column]
  return idx and row.cols[idx] or nil
end

function M.parse_csv(data)
  local parsed = {
    header = nil,
    rows = {},
    by_name = {}
  }
  for line in lines(data) do
    if line ~= "" then
      if not parsed.header then
        parsed.header = { cols = split_csv(line) }
        parsed.header.map = header_map(parsed.header.cols)
      else
        local row = { cols = split_csv(line) }
        row.name = row.cols[1]
        if row.name and row.name ~= "" then
          parsed.rows[#parsed.rows + 1] = row
          parsed.by_name[row.name] = row
        end
      end
    end
  end
  if not parsed.header then error("empty benchmark CSV", 2) end
  return parsed
end

function M.encode_csv(header, rows)
  local out = { table.concat(header, ",") }
  for i = 1, #rows do
    out[#out + 1] = table.concat(rows[i], ",")
  end
  return table.concat(out, "\n") .. "\n"
end

function M.parse_bench_text(data)
  local parsed = { rows = {}, by_name = {} }
  for line in lines(data) do
    local name, total, ns = line:match("^%s*(%S+)%s+(%S+)%s+(%S+)%s*$")
    if name and name ~= "benchmark" then
      local row = {
        name = name,
        total_s = assert(tonumber(total), "non-numeric total_s for " .. name),
        ns_per_op = assert(tonumber(ns), "non-numeric ns/op for " .. name)
      }
      parsed.rows[#parsed.rows + 1] = row
      parsed.by_name[name] = row
    end
  end
  if #parsed.rows == 0 then error("empty benchmark output", 2) end
  return parsed
end

function M.baseline_rows_from_text(jit_text, interp_text)
  local jit = M.parse_bench_text(jit_text)
  local interp = M.parse_bench_text(interp_text)
  local rows = {}
  for i = 1, #jit.rows do
    local jrow = jit.rows[i]
    local irow = interp.by_name[jrow.name]
    if not irow then error("interpreter output missing benchmark " .. jrow.name, 2) end
    rows[#rows + 1] = {
      jrow.name,
      ("%.4f"):format(jrow.total_s),
      ("%.2f"):format(jrow.ns_per_op),
      ("%.4f"):format(irow.total_s),
      ("%.2f"):format(irow.ns_per_op)
    }
  end
  return rows
end

function M.baseline_csv_from_text(jit_text, interp_text)
  return M.encode_csv({
    "benchmark",
    "jit_total_s",
    "jit_ns_per_op",
    "interp_total_s",
    "interp_ns_per_op"
  }, M.baseline_rows_from_text(jit_text, interp_text))
end

function M.compare(base_csv, current_csv, opts)
  opts = opts or {}
  local column = opts.column or "jit_ns_per_op"
  local max = tonumber(opts.max or 1.10)
  local base = M.parse_csv(base_csv)
  local current = M.parse_csv(current_csv)
  local result = {
    ok = false,
    column = column,
    max = max,
    rows = {},
    errors = {},
    geomean = nil
  }

  if not base.header.map[column] then
    result.errors[#result.errors + 1] = "missing column in baseline: " .. column
    return result
  end
  if not current.header.map[column] then
    result.errors[#result.errors + 1] = "missing column in current: " .. column
    return result
  end

  for name in pairs(current.by_name) do
    if not base.by_name[name] then
      result.errors[#result.errors + 1] =
        "current has no pinned baseline for " .. name
    end
  end

  local sum = 0
  local n = 0
  for i = 1, #base.rows do
    local brow = base.rows[i]
    local name = brow.name
    local crow = current.by_name[name]
    if not crow then
      result.errors[#result.errors + 1] = "current is missing benchmark " .. name
    else
      local bval = tonumber(row_value(brow, base.header, column))
      local cval = tonumber(row_value(crow, current.header, column))
      if not bval or not cval or bval <= 0 or cval <= 0 then
        result.errors[#result.errors + 1] =
          "non-positive benchmark value for " .. name
      else
        local ratio = cval / bval
        result.rows[#result.rows + 1] = {
          name = name,
          baseline = bval,
          current = cval,
          ratio = ratio
        }
        sum = sum + math.log(ratio)
        n = n + 1
      end
    end
  end

  if n > 0 then result.geomean = math.exp(sum / n) end
  result.ok = #result.errors == 0 and n > 0 and result.geomean <= max
  if #result.errors == 0 and n > 0 and result.geomean > max then
    result.errors[#result.errors + 1] =
      ("FAIL: geomean %.6f > %.6f"):format(result.geomean, max)
  end
  return result
end

function M.format_compare(result)
  local out = { "benchmark,baseline,current,ratio" }
  for i = 1, #result.rows do
    local row = result.rows[i]
    out[#out + 1] = ("%s,%.6g,%.6g,%.6f"):format(
      row.name, row.baseline, row.current, row.ratio)
  end
  if result.geomean then
    out[#out + 1] = ("geomean,%.6g,%.6g,%.6f"):format(
      1, result.geomean, result.geomean)
    if result.ok then
      out[#out + 1] = ("PASS: geomean %.6f <= %.6f"):format(
        result.geomean, result.max)
    end
  end
  for i = 1, #result.errors do out[#out + 1] = result.errors[i] end
  return table.concat(out, "\n") .. "\n"
end

return M
