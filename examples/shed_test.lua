-- Host-side behavioral test for examples/shed.lua.
-- Stubs the firmware-provided globals, runs the policy under scenarios,
-- and checks the relay calls it makes.

local POLICY = arg[1] or "examples/shed.lua"

local banks, relays, flags, calls

local function reset(env)
    banks = env.banks
    relays = env.relays or {}
    flags = env.flags or {}
    calls = {}
end

function battery_bank_state(name)
    local b = banks[name]
    if b == nil or b.soc == nil then
        return false, nil, nil, nil
    end
    return true, b.v or 50.0, b.a or 1.0, b.soc
end

function relay_state(n)
    return relays[n] == true, false, relays[n] and 200 or 0
end

function relay_on(n, seconds)
    calls[#calls + 1] = string.format("on(%d,%s)", n, tostring(seconds))
end

function relay_off(n)
    calls[#calls + 1] = string.format("off(%d)", n)
end

function config_is_set(name)
    return flags[name] == true
end

function syslog(...) end

local failures = 0
local function scenario(label, env, expected)
    reset(env)
    dofile(POLICY)
    local got = table.concat(calls, " ")
    if got ~= expected then
        failures = failures + 1
        print(string.format("FAIL %-45s expected [%s] got [%s]", label, expected, got))
    else
        print(string.format("ok   %-45s [%s]", label, got))
    end
end

local full = { ["48v"] = { soc = 80 }, ["24v-a"] = { soc = 90 }, ["24v-b"] = { soc = 90 } }

scenario("all banks charged, everything idle",
    { banks = full },
    "")

scenario("24v low, dcdc starts",
    { banks = { ["48v"] = { soc = 80 }, ["24v-a"] = { soc = 44 }, ["24v-b"] = { soc = 46 } } },
    "on(2,300)")

scenario("24v averages below 50 across unequal banks",
    { banks = { ["48v"] = { soc = 80 }, ["24v-a"] = { soc = 30 }, ["24v-b"] = { soc = 65 } } },
    "on(2,300)")

scenario("24v in dead band, dcdc off stays off",
    { banks = { ["48v"] = { soc = 80 }, ["24v-a"] = { soc = 60 }, ["24v-b"] = { soc = 60 } } },
    "")

scenario("24v in dead band, dcdc on keeps running",
    { banks = { ["48v"] = { soc = 80 }, ["24v-a"] = { soc = 60 }, ["24v-b"] = { soc = 60 } },
      relays = { [2] = true } },
    "on(2,300)")

scenario("24v above 70, dcdc on stops",
    { banks = { ["48v"] = { soc = 80 }, ["24v-a"] = { soc = 75 }, ["24v-b"] = { soc = 75 } },
      relays = { [2] = true } },
    "off(2)")

scenario("24v low but 48v below source minimum",
    { banks = { ["48v"] = { soc = 15 }, ["24v-a"] = { soc = 40 }, ["24v-b"] = { soc = 40 } } },
    "on(3,300)")

scenario("48v low, generator starts",
    { banks = { ["48v"] = { soc = 25 }, ["24v-a"] = { soc = 90 }, ["24v-b"] = { soc = 90 } } },
    "on(3,300)")

scenario("48v in dead band, generator on keeps running",
    { banks = { ["48v"] = { soc = 45 }, ["24v-a"] = { soc = 90 }, ["24v-b"] = { soc = 90 } },
      relays = { [3] = true } },
    "on(3,300)")

scenario("48v above 60, generator on stops",
    { banks = { ["48v"] = { soc = 65 }, ["24v-a"] = { soc = 90 }, ["24v-b"] = { soc = 90 } },
      relays = { [3] = true } },
    "off(3)")

scenario("48v low but generator forbidden",
    { banks = { ["48v"] = { soc = 25 }, ["24v-a"] = { soc = 90 }, ["24v-b"] = { soc = 90 } },
      flags = { forbid_48v_generator = true } },
    "")

scenario("forbidden generator running gets stopped",
    { banks = { ["48v"] = { soc = 45 }, ["24v-a"] = { soc = 90 }, ["24v-b"] = { soc = 90 } },
      relays = { [3] = true },
      flags = { forbid_48v_generator = true } },
    "off(3)")

scenario("force overrides forbid",
    { banks = full,
      flags = { forbid_48v_generator = true, force_48v_generator = true } },
    "on(3,300)")

scenario("force_pi holds pi for an hour",
    { banks = full, flags = { force_pi = true } },
    "on(1,3600)")

scenario("force_48v_to_24v runs dcdc regardless of soc",
    { banks = full, flags = { force_48v_to_24v = true } },
    "on(2,300)")

scenario("one 24v bank missing: no dcdc decision, no off",
    { banks = { ["48v"] = { soc = 80 }, ["24v-a"] = { soc = 40 } },
      relays = { [2] = true } },
    "")

scenario("48v missing: running relays left to deadman",
    { banks = { ["24v-a"] = { soc = 40 }, ["24v-b"] = { soc = 40 } },
      relays = { [2] = true, [3] = true } },
    "")

scenario("48v missing but forces still work",
    { banks = {},
      flags = { force_pi = true, force_48v_to_24v = true, force_48v_generator = true } },
    "on(1,3600) on(2,300) on(3,300)")

if failures > 0 then
    print(string.format("%d scenario(s) failed", failures))
    os.exit(1)
end
print("all scenarios passed")
