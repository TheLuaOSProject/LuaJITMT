return function(mark, measure)
  assert(type(mark) == "function" and type(measure) == "function")

  local ffi_ok, ffi = pcall(require, "ffi")
  local ffi_eq_ct, ffi_eq_calls, ffi_eq_tokens
  if ffi_ok then
    ffi_ok = pcall(function()
      ffi.cdef[[
        typedef struct { int value; } lj_arm64_meta_publication_eq_t;
      ]]
      ffi_eq_ct = ffi.metatype("lj_arm64_meta_publication_eq_t", {
        __eq = function(lhs, rhs)
          ffi_eq_calls = ffi_eq_calls + 1
          local token = {
            tag = "ffi-eq-token",
            nested = { tag = "ffi-eq-token-nested" },
            lhs = lhs.value,
            rhs = rhs.value
          }
          ffi_eq_tokens[#ffi_eq_tokens + 1] = token
          return token
        end
      })
    end)
  end

  return {
    baseline = function()
      mark()
      local value = 17
      return measure(), value
    end,

    arithmetic = function()
      local mt = {
        __add = function(lhs, rhs)
          return {
            tag = "add-result",
            nested = { tag = "add-result-nested" },
            lhs = lhs,
            rhs = rhs
          }
        end
      }
      local lhs = setmetatable({
        tag = "add-left",
        nested = { tag = "add-left-nested" }
      }, mt)
      local rhs = setmetatable({
        tag = "add-right",
        nested = { tag = "add-right-nested" }
      }, mt)
      mark()
      local result = lhs + rhs
      return measure(), result, lhs, rhs
    end,

    length = function()
      local calls = 0
      local mt = {
        __len = function(value)
          calls = calls + 1
          return {
            tag = "len-result",
            nested = { tag = "len-result-nested" },
            value = value
          }
        end
      }
      local value = setmetatable({
        tag = "len-input",
        nested = { tag = "len-input-nested" }
      }, mt)
      mark()
      local result = #value
      return measure(), result, calls, value
    end,

    ordering = function()
      local lt_calls, le_calls, fallback_calls = 0, 0, 0
      local mismatch_lhs_calls, mismatch_rhs_calls = 0, 0
      local tokens, fallback_tokens = {}, {}
      local mt = {
        __lt = function(lhs, rhs)
          lt_calls = lt_calls + 1
          local token = {
            tag = "lt-token",
            nested = { tag = "lt-token-nested" },
            lhs = lhs,
            rhs = rhs
          }
          tokens[#tokens + 1] = token
          return token
        end,
        __le = function(lhs, rhs)
          le_calls = le_calls + 1
          local token = {
            tag = "le-token",
            nested = { tag = "le-token-nested" },
            lhs = lhs,
            rhs = rhs
          }
          tokens[#tokens + 1] = token
          return token
        end
      }
      local lhs = setmetatable({
        tag = "order-left",
        nested = { tag = "order-left-nested" }
      }, mt)
      local rhs = setmetatable({
        tag = "order-right",
        nested = { tag = "order-right-nested" }
      }, mt)
      local fallback_lt = function(fallback_lhs, fallback_rhs)
        fallback_calls = fallback_calls + 1
        local token = {
          tag = "le-fallback-token",
          nested = { tag = "le-fallback-token-nested" },
          lhs = fallback_lhs,
          rhs = fallback_rhs
        }
        fallback_tokens[#fallback_tokens + 1] = token
        return false
      end
      local fallback_lhs = setmetatable({
        tag = "order-fallback-left",
        nested = { tag = "order-fallback-left-nested" }
      }, { __lt = fallback_lt })
      local fallback_rhs = setmetatable({
        tag = "order-fallback-right",
        nested = { tag = "order-fallback-right-nested" }
      }, { __lt = fallback_lt })
      local mismatch_lhs = setmetatable({}, {
        __lt = function()
          mismatch_lhs_calls = mismatch_lhs_calls + 1
          return true
        end
      })
      local mismatch_rhs = setmetatable({}, {
        __lt = function()
          mismatch_rhs_calls = mismatch_rhs_calls + 1
          return true
        end
      })
      mark()
      local lt = lhs < rhs
      local le = lhs <= rhs
      -- Lua 5.1 implements <= without __le as not (rhs < lhs).
      local fallback_le = fallback_lhs <= fallback_rhs
      local mismatch_ok, mismatch_err, mismatch_failures
      mismatch_failures = 0
      for i = 1, 64 do
        local ok, err = pcall(function()
          return mismatch_lhs < mismatch_rhs
        end)
        if not ok then
          mismatch_failures = mismatch_failures + 1
          mismatch_err = err
        end
        mismatch_ok = ok
        if i % 8 == 0 then collectgarbage("collect") end
      end
      return measure(), lt, le, lt_calls, le_calls, tokens, lhs, rhs,
             fallback_le, fallback_calls, fallback_tokens,
             fallback_lhs, fallback_rhs,
             mismatch_ok, mismatch_err,
             mismatch_lhs_calls, mismatch_rhs_calls, mismatch_failures
    end,

    table_equality = function()
      local same_calls, shared_calls, one_sided_calls = 0, 0, 0
      local different_lhs_calls, different_rhs_calls = 0, 0
      local tokens = {}
      local same_mt = {
        __eq = function(lhs, rhs)
          same_calls = same_calls + 1
          local token = {
            tag = "eq-same-token",
            nested = { tag = "eq-same-token-nested" },
            lhs = lhs,
            rhs = rhs
          }
          tokens[#tokens + 1] = token
          return token
        end
      }
      local shared_eq = function(lhs, rhs)
        shared_calls = shared_calls + 1
        local token = {
          tag = "eq-shared-token",
          nested = { tag = "eq-shared-token-nested" },
          lhs = lhs,
          rhs = rhs
        }
        tokens[#tokens + 1] = token
        return token
      end
      local same_lhs = setmetatable({
        tag = "eq-same-left",
        nested = { tag = "eq-same-left-nested" }
      }, same_mt)
      local same_rhs = setmetatable({
        tag = "eq-same-right",
        nested = { tag = "eq-same-right-nested" }
      }, same_mt)
      local shared_lhs = setmetatable({
        tag = "eq-shared-left",
        nested = { tag = "eq-shared-left-nested" }
      }, { __eq = shared_eq })
      local shared_rhs = setmetatable({
        tag = "eq-shared-right",
        nested = { tag = "eq-shared-right-nested" }
      }, { __eq = shared_eq })
      local different_lhs = setmetatable({
        tag = "eq-different-left",
        nested = { tag = "eq-different-left-nested" }
      }, {
        __eq = function()
          different_lhs_calls = different_lhs_calls + 1
          return true
        end
      })
      local different_rhs = setmetatable({
        tag = "eq-different-right",
        nested = { tag = "eq-different-right-nested" }
      }, {
        __eq = function()
          different_rhs_calls = different_rhs_calls + 1
          return true
        end
      })
      local one_sided_lhs = setmetatable({
        tag = "eq-one-sided-left",
        nested = { tag = "eq-one-sided-left-nested" }
      }, {
        __eq = function()
          one_sided_calls = one_sided_calls + 1
          return true
        end
      })
      local one_sided_rhs = {
        tag = "eq-one-sided-right",
        nested = { tag = "eq-one-sided-right-nested" }
      }
      mark()
      local identity = same_lhs == same_lhs
      local same = same_lhs == same_rhs
      local same_ne = same_lhs ~= same_rhs
      local shared = shared_lhs == shared_rhs
      local different = different_lhs == different_rhs
      local different_ne = different_lhs ~= different_rhs
      local one_sided = one_sided_lhs == one_sided_rhs
      return measure(), identity, same, same_ne, shared,
             different, different_ne, one_sided,
             same_calls, shared_calls,
             different_lhs_calls, different_rhs_calls, one_sided_calls,
             tokens, same_lhs, shared_lhs, different_lhs,
             one_sided_lhs, one_sided_rhs
    end,

    userdata_equality = function()
      local proxy = rawget(_G, "newproxy")
      if type(proxy) ~= "function" then
        mark()
        return measure(), false
      end

      local same_calls, shared_calls, one_sided_calls = 0, 0, 0
      local different_lhs_calls, different_rhs_calls = 0, 0
      local tokens = {}
      local same_lhs = proxy(true)
      local same_rhs = proxy(same_lhs)
      local same_mt = getmetatable(same_lhs)
      same_mt.__eq = function(lhs, rhs)
        same_calls = same_calls + 1
        local token = {
          tag = "ud-eq-same-token",
          nested = { tag = "ud-eq-same-token-nested" },
          lhs = lhs,
          rhs = rhs
        }
        tokens[#tokens + 1] = token
        return token
      end

      local shared_eq = function(lhs, rhs)
        shared_calls = shared_calls + 1
        local token = {
          tag = "ud-eq-shared-token",
          nested = { tag = "ud-eq-shared-token-nested" },
          lhs = lhs,
          rhs = rhs
        }
        tokens[#tokens + 1] = token
        return token
      end
      local shared_lhs, shared_rhs = proxy(true), proxy(true)
      getmetatable(shared_lhs).__eq = shared_eq
      getmetatable(shared_rhs).__eq = shared_eq

      local different_lhs, different_rhs = proxy(true), proxy(true)
      getmetatable(different_lhs).__eq = function()
        different_lhs_calls = different_lhs_calls + 1
        return true
      end
      getmetatable(different_rhs).__eq = function()
        different_rhs_calls = different_rhs_calls + 1
        return true
      end

      local one_sided_lhs, one_sided_rhs = proxy(true), proxy(true)
      getmetatable(one_sided_lhs).__eq = function()
        one_sided_calls = one_sided_calls + 1
        return true
      end

      mark()
      local identity = same_lhs == same_lhs
      local same = same_lhs == same_rhs
      local same_ne = same_lhs ~= same_rhs
      local shared = shared_lhs == shared_rhs
      local different = different_lhs == different_rhs
      local different_ne = different_lhs ~= different_rhs
      local one_sided = one_sided_lhs == one_sided_rhs
      return measure(), true, identity, same, same_ne, shared,
             different, different_ne, one_sided,
             same_calls, shared_calls,
             different_lhs_calls, different_rhs_calls, one_sided_calls,
             tokens, same_lhs, same_rhs, shared_lhs, shared_rhs,
             different_lhs, different_rhs, one_sided_lhs, one_sided_rhs
    end,

    ffi_equality = function()
      if not ffi_ok then
        mark()
        return measure(), false, false, 0, nil, nil, nil
      end
      ffi_eq_calls, ffi_eq_tokens = 0, {}
      local lhs, rhs = ffi_eq_ct(71), ffi_eq_ct(71)
      mark()
      local equal = lhs == rhs
      return measure(), true, equal, ffi_eq_calls, ffi_eq_tokens, lhs, rhs
    end,

    concatenation = function()
      local calls = 0
      local mt = {}
      mt.__concat = function(lhs, rhs)
        calls = calls + 1
        return setmetatable({
          tag = "concat-result-" .. calls,
          nested = { tag = "concat-result-nested-" .. calls },
          lhs = lhs,
          rhs = rhs
        }, mt)
      end
      local lhs = setmetatable({
        tag = "concat-left",
        nested = { tag = "concat-left-nested" }
      }, mt)
      local middle = setmetatable({
        tag = "concat-middle",
        nested = { tag = "concat-middle-nested" }
      }, mt)
      local rhs = setmetatable({
        tag = "concat-right",
        nested = { tag = "concat-right-nested" }
      }, mt)
      mark()
      local result = lhs .. middle .. rhs
      return measure(), result, calls
    end,

    callable = function()
      local mt = {
        __call = function(self, argument)
          return {
            tag = "call-result",
            nested = { tag = "call-result-nested" },
            self = self,
            argument = argument
          }
        end
      }
      local callable = setmetatable({
        tag = "callable-input",
        nested = { tag = "callable-input-nested" }
      }, mt)
      local argument = {
        tag = "call-argument",
        nested = { tag = "call-argument-nested" }
      }
      mark()
      local result = callable(argument)
      return measure(), result, callable, argument
    end,

    metatables = function()
      local target = {
        tag = "metatable-target",
        nested = { tag = "metatable-target-nested" }
      }
      local first = {
        tag = "metatable-first",
        nested = { tag = "metatable-first-nested" }
      }
      local second = {
        tag = "metatable-second",
        nested = { tag = "metatable-second-nested" }
      }
      local protected_token = {
        tag = "metatable-protected-token",
        nested = { tag = "metatable-protected-token-nested" }
      }
      local protected = {
        tag = "metatable-protected",
        nested = { tag = "metatable-protected-nested" },
        __metatable = protected_token
      }

      mark()
      assert(getmetatable(target) == nil)
      assert(debug.getmetatable(target) == nil)
      assert(setmetatable(target, first) == target)
      assert(getmetatable(target) == first)
      assert(debug.getmetatable(target) == first)
      assert(setmetatable(target, second) == target)
      assert(getmetatable(target) == second)
      assert(setmetatable(target, nil) == target)
      assert(getmetatable(target) == nil)
      assert(setmetatable(target, protected) == target)
      assert(getmetatable(target) == protected_token)
      assert(debug.getmetatable(target) == protected)

      local protected_ok, protected_err =
        pcall(setmetatable, target, first)
      local target_ok, target_err = pcall(setmetatable, 17, first)
      local value_ok, value_err = pcall(setmetatable, {}, 17)
      assert(not protected_ok and not target_ok and not value_ok)
      assert(debug.getmetatable(target) == protected)
      return measure(), target, first, second, protected, protected_token,
             protected_ok, protected_err, target_ok, target_err,
             value_ok, value_err
    end,

    getmetatable_results = function()
      mark()
      local nil_result = getmetatable({})
      local raw_result = getmetatable(setmetatable({}, {
        tag = "getmetatable-raw-result",
        nested = { tag = "getmetatable-raw-result-nested" }
      }))
      local protected_result = getmetatable(setmetatable({}, {
        __metatable = {
          tag = "getmetatable-protected-result",
          nested = { tag = "getmetatable-protected-result-nested" }
        }
      }))
      local false_result = getmetatable(setmetatable({}, {
        __metatable = false
      }))
      return measure(), nil_result, raw_result, protected_result, false_result
    end
  }
end
