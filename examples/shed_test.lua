-- Host-side behavioral test for examples/shed.lua.
-- Stubs the firmware-provided globals, runs the policy under scenarios,
-- and checks the relay calls it makes.

local POLICY = arg[1] or "examples/shed.lua"

local banks, relays, flags, numbers, calls

local function reset(env)
    banks = env.banks
    relays = env.relays or {}
    flags = env.flags or {}
    numbers = env.numbers or {}
    calls = {}
end

function battery_bank_state(name)
    local b = banks[name]
    if b == nil or b.soc == nil then
        return false, nil, nil, nil, nil, nil, nil
    end
    return true, b.v or 50.0, b.a or 1.0, b.soc,
           b.min_cell, b.cell_age, b.cell_uv == true
end

function relay_state(n)
    -- Second value is the administrative force: "on", "off", or nil.
    return relays[n] == true, nil, relays[n] and 200 or 0
end

function relay_on(n, seconds)
    calls[#calls + 1] = string.format("on(%d,%s)", n, tostring(seconds))
end

function relay_off(n)
    calls[#calls + 1] = string.format("off(%d)", n)
end

function config_is_set(name)
    -- Parameter names are NVS keys on the device: 15 characters maximum.
    assert(#name >= 1 and #name <= 15 and name:match("^[%w_%-]+$"),
           "invalid policy parameter name: " .. name)
    return flags[name] == true
end

function config_number(name, default)
    assert(#name >= 1 and #name <= 15 and name:match("^[%w_%-]+$"),
           "invalid policy parameter name: " .. name)
    local value = numbers[name]
    if value == nil then
        return default
    end
    return value
end

function config_bool(name, default)
    assert(#name >= 1 and #name <= 15 and name:match("^[%w_%-]+$"),
           "invalid policy parameter name: " .. name)
    local value = flags[name]
    if value == nil then
        return default
    end
    return value == true
end

function syslog(...) end

do
    reset({ banks = {} })
    assert(select("#", battery_bank_state("missing")) == 7)
    local ready, volts, amps, soc, min_cell, cell_age, cell_uv =
        battery_bank_state("missing")
    assert(ready == false and volts == nil and amps == nil and soc == nil)
    assert(min_cell == nil and cell_age == nil and cell_uv == nil)

    reset({ banks = { test = { soc = 55, min_cell = 3.2, cell_age = 4, cell_uv = true } } })
    assert(select("#", battery_bank_state("test")) == 7)
    ready, volts, amps, soc, min_cell, cell_age, cell_uv = battery_bank_state("test")
    assert(ready == true and volts == 50.0 and amps == 1.0 and soc == 55)
    assert(min_cell == 3.2 and cell_age == 4 and cell_uv == true)
end

local failures = 0
local function scenario(label, env, expected)
    reset(env)
    dofile(POLICY)
    if not config_bool("deepSleep", false) then
        if expected == "" then
            expected = "on(1,3600)"
        else
            expected = "on(1,3600) " .. expected
        end
    end
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

scenario("10A 24v charge does not defeat hysteresis",
    { banks = { ["48v"] = { soc = 80 }, ["24v-a"] = { soc = 60, a = 10 },
                ["24v-b"] = { soc = 60, a = 10 } },
      relays = { [2] = true } },
    "on(2,300)")

scenario("external 24v charge defeats dcdc hysteresis",
    { banks = { ["48v"] = { soc = 80 }, ["24v-a"] = { soc = 60, a = 10.1 },
                ["24v-b"] = { soc = 60, a = 8 } },
      relays = { [2] = true } },
    "off(2)")

scenario("external 24v charge suppresses low-soc dcdc start",
    { banks = { ["48v"] = { soc = 80 }, ["24v-a"] = { soc = 40, a = 12 },
                ["24v-b"] = { soc = 40, a = 8 } } },
    "")

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

scenario("weak 24v cell starts dcdc despite high soc",
    { banks = { ["48v"] = { soc = 80, min_cell = 3.30, cell_age = 0 },
                ["24v-a"] = { soc = 90, min_cell = 3.09, cell_age = 0 },
                ["24v-b"] = { soc = 90, min_cell = 3.30, cell_age = 0 } } },
    "on(2,300)")

scenario("24v undervoltage alarm starts dcdc",
    { banks = { ["48v"] = { soc = 80 }, ["24v-a"] = { soc = 90, cell_uv = true },
                ["24v-b"] = { soc = 90 } } },
    "on(2,300)")

scenario("24v alarm holds dcdc through soc recovery",
    { banks = { ["48v"] = { soc = 80 }, ["24v-a"] = { soc = 90, cell_uv = true },
                ["24v-b"] = { soc = 90 } },
      relays = { [2] = true } },
    "on(2,300)")

scenario("cleared 24v alarm and recovered cell releases dcdc",
    { banks = { ["48v"] = { soc = 80 },
                ["24v-a"] = { soc = 90, min_cell = 3.25, cell_age = 0 },
                ["24v-b"] = { soc = 90, min_cell = 3.30, cell_age = 0 } },
      relays = { [2] = true } },
    "off(2)")

scenario("24v cell recovery holds running dcdc",
    { banks = { ["48v"] = { soc = 80 },
                ["24v-a"] = { soc = 90, min_cell = 3.20, cell_age = 0 },
                ["24v-b"] = { soc = 90, min_cell = 3.30, cell_age = 0 } },
      relays = { [2] = true } },
    "on(2,300)")

scenario("external charge overrides 24v cell recovery hold",
    { banks = { ["48v"] = { soc = 80 },
                ["24v-a"] = { soc = 90, a = 11, min_cell = 3.20, cell_age = 0 },
                ["24v-b"] = { soc = 90, a = 8, min_cell = 3.30, cell_age = 0 } },
      relays = { [2] = true } },
    "off(2)")

scenario("stale weak 24v cell falls back to soc",
    { banks = { ["48v"] = { soc = 80 },
                ["24v-a"] = { soc = 90, min_cell = 3.00, cell_age = 901 },
                ["24v-b"] = { soc = 90 } } },
    "")

scenario("48v undervoltage blocks dcdc and starts generator",
    { banks = { ["48v"] = { soc = 80, cell_uv = true },
                ["24v-a"] = { soc = 40 }, ["24v-b"] = { soc = 40 } } },
    "on(3,300)")

scenario("48v alarm holds generator through soc recovery",
    { banks = { ["48v"] = { soc = 80, cell_uv = true },
                ["24v-a"] = { soc = 90 }, ["24v-b"] = { soc = 90 } },
      relays = { [3] = true } },
    "on(3,300)")

scenario("cleared 48v alarm and recovered cell releases generator",
    { banks = { ["48v"] = { soc = 80, min_cell = 3.25, cell_age = 0 },
                ["24v-a"] = { soc = 90 }, ["24v-b"] = { soc = 90 } },
      relays = { [3] = true } },
    "off(3)")

scenario("weak 48v cell starts generator despite high soc",
    { banks = { ["48v"] = { soc = 80, min_cell = 3.10, cell_age = 0 },
                ["24v-a"] = { soc = 90 }, ["24v-b"] = { soc = 90 } } },
    "on(3,300)")

scenario("48v cell recovery holds running generator",
    { banks = { ["48v"] = { soc = 80, min_cell = 3.20, cell_age = 0 },
                ["24v-a"] = { soc = 90 }, ["24v-b"] = { soc = 90 } },
      relays = { [3] = true } },
    "on(3,300)")

scenario("stale weak 48v cell falls back to soc",
    { banks = { ["48v"] = { soc = 80, min_cell = 3.00, cell_age = 901 },
                ["24v-a"] = { soc = 90 }, ["24v-b"] = { soc = 90 } } },
    "")

scenario("48v low but generator not allowed",
    { banks = { ["48v"] = { soc = 25 }, ["24v-a"] = { soc = 90 }, ["24v-b"] = { soc = 90 } },
      flags = { ["allow-generator"] = false } },
    "")

scenario("disallowed generator running gets stopped",
    { banks = { ["48v"] = { soc = 45 }, ["24v-a"] = { soc = 90 }, ["24v-b"] = { soc = 90 } },
      relays = { [3] = true },
      flags = { ["allow-generator"] = false } },
    "off(3)")

scenario("force overrides allow-generator=false",
    { banks = full,
      flags = { ["allow-generator"] = false, force_48v_gen = true } },
    "on(3,300)")

scenario("explicit allow-generator=true acts like default",
    { banks = { ["48v"] = { soc = 25 }, ["24v-a"] = { soc = 90 }, ["24v-b"] = { soc = 90 } },
      flags = { ["allow-generator"] = true } },
    "on(3,300)")

scenario("gen_start raised by parameter starts in dead band",
    { banks = { ["48v"] = { soc = 45 }, ["24v-a"] = { soc = 90 }, ["24v-b"] = { soc = 90 } },
      numbers = { gen_start = 50 } },
    "on(3,300)")

scenario("gen_stop lowered by parameter stops running generator",
    { banks = { ["48v"] = { soc = 45 }, ["24v-a"] = { soc = 90 }, ["24v-b"] = { soc = 90 } },
      relays = { [3] = true },
      numbers = { gen_stop = 40 } },
    "off(3)")

scenario("dcdc thresholds moved by parameters",
    { banks = { ["48v"] = { soc = 80 }, ["24v-a"] = { soc = 60 }, ["24v-b"] = { soc = 60 } },
      numbers = { dcdc_start = 65 } },
    "on(2,300)")

scenario("dcdc_source_min raised by parameter blocks transfer",
    { banks = { ["48v"] = { soc = 25 }, ["24v-a"] = { soc = 40 }, ["24v-b"] = { soc = 40 } },
      numbers = { dcdc_source_min = 30, gen_start = 20 } },
    "")

scenario("external charge threshold is configurable",
    { banks = { ["48v"] = { soc = 80 }, ["24v-a"] = { soc = 60, a = 11 },
                ["24v-b"] = { soc = 60, a = 8 } },
      relays = { [2] = true },
      numbers = { dcdc_ext_amps = 12 } },
    "on(2,300)")

scenario("manual dcdc force overrides external charge",
    { banks = { ["48v"] = { soc = 80 }, ["24v-a"] = { soc = 90, a = 12 },
                ["24v-b"] = { soc = 90, a = 8 } },
      flags = { force_48v_24v = true } },
    "on(2,300)")

scenario("raspberry pi is held on by default",
    { banks = full },
    "")

scenario("deepSleep turns off a running raspberry pi",
    { banks = full, relays = { [1] = true }, flags = { deepSleep = true } },
    "off(1)")

scenario("deepSleep leaves an idle raspberry pi off",
    { banks = full, flags = { deepSleep = true } },
    "")

scenario("enableCameras powers the PoE switch",
    { banks = full, flags = { enableCameras = true } },
    "on(4,300)")

scenario("occupied powers the PoE access point",
    { banks = full, flags = { occupied = true } },
    "on(4,300)")

scenario("camera and occupancy requests share the PoE relay",
    { banks = full, flags = { enableCameras = true, occupied = true } },
    "on(4,300)")

scenario("no camera or occupancy request turns off running PoE",
    { banks = full, relays = { [4] = true } },
    "off(4)")

scenario("explicit false camera and occupancy leave PoE off",
    { banks = full, flags = { enableCameras = false, occupied = false } },
    "")

scenario("force_48v_24v runs dcdc regardless of soc",
    { banks = full, flags = { force_48v_24v = true } },
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
      flags = { force_48v_24v = true, force_48v_gen = true } },
    "on(2,300) on(3,300)")

if failures > 0 then
    print(string.format("%d scenario(s) failed", failures))
    os.exit(1)
end
print("all scenarios passed")
