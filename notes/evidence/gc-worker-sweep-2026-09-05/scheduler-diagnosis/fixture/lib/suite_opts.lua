local M = {}

function M.copy(tbl)
  local out = {}
  if tbl then
    for k, v in pairs(tbl) do out[k] = v end
  end
  return out
end

function M.defaults(opts, defaults)
  local out = M.copy(opts)
  if defaults then
    for k, v in pairs(defaults) do
      if out[k] == nil then out[k] = v end
    end
  end
  return out
end

function M.with(opts, overrides)
  local out = M.copy(opts)
  if overrides then
    for k, v in pairs(overrides) do out[k] = v end
  end
  return out
end

return M
