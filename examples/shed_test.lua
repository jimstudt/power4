-- Host-side behavioral test for examples/shed.lua.
-- Stubs the firmware-provided globals, runs the policy under scenarios,
-- and checks the relay calls it makes.

local POLICY = arg[1] or "examples/shed.lua"

local banks, relays, flags, numbers, calls, memory

local function reset(env)
    banks = env.banks
    relays = env.relays or {}
    flags = env.flags or {}
    numbers = env.numbers or {}
    memory = env.memory or {}
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
    relays[n] = true
end

function relay_off(n)
    calls[#calls + 1] = string.format("off(%d)", n)
    relays[n] = false
end

function policy_state_bool(name, default)
    assert(#name >= 1 and #name <= 15 and name:match("^[%w_%-]+$"))
    if memory[name] == nil then
        return default or false
    end
    return memory[name]
end

function policy_state_set(name, value)
    assert(#name >= 1 and #name <= 15 and name:match("^[%w_%-]+$"))
    assert(type(value) == "boolean")
    memory[name] = value
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
    if config_bool("occupied", false) and not config_bool("deepSleep", false) then
        if expected == "" then
            expected = "on(1,3600)"
        else
            expected = "on(1,3600) " .. expected
        end
    end
    expected = "on(4,3600)" .. (expected == "" and "" or " " .. expected)
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
      relays = { [2] = true }, memory = { dcdc_charge = true } },
    "on(2,300)")

scenario("10A 24v charge does not defeat hysteresis",
    { banks = { ["48v"] = { soc = 80 }, ["24v-a"] = { soc = 60, a = 10 },
                ["24v-b"] = { soc = 60, a = 10 } },
      relays = { [2] = true }, memory = { dcdc_charge = true } },
    "on(2,300)")

scenario("external 24v charge defeats dcdc hysteresis",
    { banks = { ["48v"] = { soc = 80 }, ["24v-a"] = { soc = 60, a = 10.1 },
                ["24v-b"] = { soc = 60, a = 8 } },
      relays = { [2] = true }, memory = { dcdc_charge = true } },
    "off(2)")

scenario("external 24v charge suppresses low-soc dcdc start",
    { banks = { ["48v"] = { soc = 80 }, ["24v-a"] = { soc = 40, a = 12 },
                ["24v-b"] = { soc = 40, a = 8 } } },
    "")

scenario("24v above 70, dcdc on stops",
    { banks = { ["48v"] = { soc = 80 }, ["24v-a"] = { soc = 75 }, ["24v-b"] = { soc = 75 } },
      relays = { [2] = true }, memory = { dcdc_charge = true } },
    "off(2)")

scenario("24v low but 48v below source minimum",
    { banks = { ["48v"] = { soc = 15 }, ["24v-a"] = { soc = 40 }, ["24v-b"] = { soc = 40 } } },
    "on(3,300)")

scenario("48v low stops running dcdc despite 24v demand",
    { banks = { ["48v"] = { soc = 19 }, ["24v-a"] = { soc = 40 }, ["24v-b"] = { soc = 40 } },
      relays = { [2] = true }, memory = { dcdc_charge = true } },
    "off(2) on(3,300)")

scenario("48v at source minimum blocks dcdc start",
    { banks = { ["48v"] = { soc = 20 }, ["24v-a"] = { soc = 40 }, ["24v-b"] = { soc = 40 } } },
    "on(3,300)")

scenario("48v at source minimum defeats dcdc hysteresis",
    { banks = { ["48v"] = { soc = 20 }, ["24v-a"] = { soc = 60 }, ["24v-b"] = { soc = 60 } },
      relays = { [2] = true }, memory = { dcdc_charge = true } },
    "off(2) on(3,300)")

scenario("48v just above source minimum permits transfer",
    { banks = { ["48v"] = { soc = 20.1 }, ["24v-a"] = { soc = 40 }, ["24v-b"] = { soc = 40 } } },
    "on(2,300) on(3,300)")

scenario("48v low stops dcdc with one 24v bank missing",
    { banks = { ["48v"] = { soc = 19 }, ["24v-a"] = { soc = 40 } },
      relays = { [2] = true }, memory = { dcdc_charge = true } },
    "off(2) on(3,300)")

scenario("48v at minimum stops dcdc with both 24v banks missing",
    { banks = { ["48v"] = { soc = 20 } }, relays = { [2] = true }, memory = { dcdc_charge = true } },
    "off(2) on(3,300)")

scenario("48v low stops dcdc even when generator disallowed",
    { banks = { ["48v"] = { soc = 19 }, ["24v-a"] = { soc = 60 }, ["24v-b"] = { soc = 60 } },
      relays = { [2] = true }, memory = { dcdc_charge = true }, flags = { ["allow-generator"] = false } },
    "off(2)")

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
      relays = { [2] = true }, memory = { dcdc_charge = true } },
    "on(2,300)")

scenario("cleared 24v alarm and recovered cell releases dcdc",
    { banks = { ["48v"] = { soc = 80 },
                ["24v-a"] = { soc = 90, min_cell = 3.25, cell_age = 0 },
                ["24v-b"] = { soc = 90, min_cell = 3.30, cell_age = 0 } },
      relays = { [2] = true }, memory = { dcdc_charge = true } },
    "off(2)")

scenario("24v cell recovery holds running dcdc",
    { banks = { ["48v"] = { soc = 80 },
                ["24v-a"] = { soc = 90, min_cell = 3.20, cell_age = 0 },
                ["24v-b"] = { soc = 90, min_cell = 3.30, cell_age = 0 } },
      relays = { [2] = true }, memory = { dcdc_charge = true } },
    "on(2,300)")

scenario("external charge overrides 24v cell recovery hold",
    { banks = { ["48v"] = { soc = 80 },
                ["24v-a"] = { soc = 90, a = 11, min_cell = 3.20, cell_age = 0 },
                ["24v-b"] = { soc = 90, a = 8, min_cell = 3.30, cell_age = 0 } },
      relays = { [2] = true }, memory = { dcdc_charge = true } },
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

scenario("48v undervoltage stops dcdc despite missing 24v data",
    { banks = { ["48v"] = { soc = 80, cell_uv = true } }, relays = { [2] = true }, memory = { dcdc_charge = true } },
    "off(2) on(3,300)")

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

scenario("configured source minimum stops running dcdc at boundary",
    { banks = { ["48v"] = { soc = 30 }, ["24v-a"] = { soc = 60 }, ["24v-b"] = { soc = 60 } },
      relays = { [2] = true }, memory = { dcdc_charge = true }, numbers = { dcdc_source_min = 30 } },
    "off(2)")

scenario("external charge threshold is configurable",
    { banks = { ["48v"] = { soc = 80 }, ["24v-a"] = { soc = 60, a = 11 },
                ["24v-b"] = { soc = 60, a = 8 } },
      relays = { [2] = true }, memory = { dcdc_charge = true },
      numbers = { dcdc_ext_amps = 12 } },
    "on(2,300)")

scenario("manual dcdc force overrides external charge",
    { banks = { ["48v"] = { soc = 80 }, ["24v-a"] = { soc = 90, a = 12 },
                ["24v-b"] = { soc = 90, a = 8 } },
      flags = { force_48v_24v = true } },
    "on(2,300)")

scenario("unoccupied leaves an idle raspberry pi off",
    { banks = full },
    "")

scenario("unoccupied turns off a running raspberry pi",
    { banks = full, relays = { [1] = true } },
    "off(1)")

scenario("occupied holds the raspberry pi on",
    { banks = full, flags = { occupied = true } },
    "on(5,300)")

scenario("deepSleep overrides occupancy for a running raspberry pi",
    { banks = full, relays = { [1] = true },
      flags = { occupied = true, deepSleep = true } },
    "off(1) on(5,300)")

scenario("deepSleep and occupancy leave an idle raspberry pi off",
    { banks = full, flags = { occupied = true, deepSleep = true } },
    "on(5,300)")

scenario("enableCameras powers the PoE switch",
    { banks = full, flags = { enableCameras = true } },
    "on(5,300)")

scenario("occupied powers the PoE access point",
    { banks = full, flags = { occupied = true } },
    "on(5,300)")

scenario("camera and occupancy requests share the PoE relay",
    { banks = full, flags = { enableCameras = true, occupied = true } },
    "on(5,300)")

scenario("no PoE demand turns off relay 5 while Ethernet stays on",
    { banks = full, relays = { [4] = true, [5] = true } },
    "off(5)")

scenario("explicit false camera and occupancy leave PoE off",
    { banks = full, flags = { enableCameras = false, occupied = false } },
    "")

scenario("Ethernet stays on during deep sleep with missing telemetry",
    { banks = {}, relays = { [1] = true, [4] = true, [5] = true },
      flags = { deepSleep = true } },
    "off(1) off(5)")

scenario("occupied PoE and Ethernet remain on during deep sleep",
    { banks = full, relays = { [1] = true },
      flags = { deepSleep = true, occupied = true } },
    "off(1) on(5,300)")

scenario("force_48v_24v runs dcdc regardless of soc",
    { banks = full, flags = { force_48v_24v = true } },
    "on(2,300)")

scenario("manual force still overrides the automatic source minimum",
    { banks = { ["48v"] = { soc = 20 }, ["24v-a"] = { soc = 40 }, ["24v-b"] = { soc = 40 } },
      flags = { force_48v_24v = true } },
    "on(2,300) on(3,300)")

scenario("one 24v bank missing: no dcdc decision, no off",
    { banks = { ["48v"] = { soc = 80 }, ["24v-a"] = { soc = 40 } },
      relays = { [2] = true }, memory = { dcdc_charge = true } },
    "")

scenario("48v missing: running relays left to deadman",
    { banks = { ["24v-a"] = { soc = 40 }, ["24v-b"] = { soc = 40 } },
      relays = { [2] = true, [3] = true } },
    "")

scenario("48v missing but forces still work",
    { banks = {},
      flags = { force_48v_24v = true, force_48v_gen = true } },
    "on(2,300) on(3,300)")

local function solar_env(source_soc, dest_soc, active)
    return {
        banks = { ["48v"] = { soc = source_soc },
                  ["24v-a"] = { soc = dest_soc }, ["24v-b"] = { soc = dest_soc } },
        relays = { [2] = active == true },
        memory = { dcdc_solar = active == true, dcdc_charge = false },
    }
end

scenario("surplus solar starts above 95 with 24v below 90",
    solar_env(95.1, 89.9, false), "on(2,300)")
scenario("surplus solar does not start at exactly 95",
    solar_env(95, 80, false), "")
scenario("surplus solar does not start with 24v at 90",
    solar_env(96, 90, false), "")
scenario("surplus solar holds through source dead band",
    solar_env(92, 80, true), "on(2,300)")
scenario("surplus solar stops with source at exactly 90",
    solar_env(90, 80, true), "off(2)")
scenario("surplus solar stops with source below 90",
    solar_env(89.9, 80, true), "off(2)")
scenario("active surplus solar holds with 24v at exactly 90",
    solar_env(96, 90, true), "on(2,300)")
scenario("surplus solar stops with 24v above 90",
    solar_env(96, 90.1, true), "off(2)")

do
    local env = solar_env(96, 85, false)
    env.banks["24v-a"].soc = 95
    env.banks["24v-b"].soc = 75
    scenario("surplus solar uses average 24v SOC", env, "on(2,300)")
end

-- Real sequences carry only explicit firmware state and relay outputs across
-- policy executions, just as the fresh Lua environment on the device does.
do
    local env = solar_env(96, 60, false)
    scenario("solar alone starts while normal charging stays idle", env, "on(2,300)")
    assert(memory.dcdc_solar and not memory.dcdc_charge)
    env.banks["48v"].soc = 92
    scenario("solar alone continues without latching normal charging", env, "on(2,300)")
    assert(memory.dcdc_solar and not memory.dcdc_charge)
    env.banks["48v"].soc = 90
    scenario("solar exits without normal hysteresis taking over", env, "off(2)")
    env.banks["48v"].soc = 92
    scenario("stopped solar does not restart in its dead band", env, "")
end

do
    local env = solar_env(94, 40, false)
    scenario("normal charging can start without solar surplus", env, "on(2,300)")
    assert(memory.dcdc_charge and not memory.dcdc_solar)
    env.banks["24v-a"].soc = 75
    env.banks["24v-b"].soc = 75
    scenario("normal charging cannot activate solar in its dead band", env, "off(2)")
end

do
    local env = solar_env(96, 40, false)
    scenario("both transfer rules can request the relay", env, "on(2,300)")
    assert(memory.dcdc_charge and memory.dcdc_solar)
    env.banks["48v"].soc = 90
    env.banks["24v-a"].soc = 60
    env.banks["24v-b"].soc = 60
    scenario("normal charging continues after solar stops", env, "on(2,300)")
    assert(memory.dcdc_charge and not memory.dcdc_solar)
    env.banks["24v-a"].soc = 70
    env.banks["24v-b"].soc = 70
    scenario("transfer stops once both demands end", env, "off(2)")
end

do
    local env = solar_env(96, 40, false)
    scenario("both demands start before normal charging finishes", env, "on(2,300)")
    env.banks["48v"].soc = 92
    env.banks["24v-a"].soc = 75
    env.banks["24v-b"].soc = 75
    scenario("solar continues after normal charging stops", env, "on(2,300)")
    assert(not memory.dcdc_charge and memory.dcdc_solar)
    env.banks["24v-a"].soc = 91
    env.banks["24v-b"].soc = 91
    env.banks["24v-a"].cell_uv = true
    scenario("cell protection can request transfer above solar target", env, "on(2,300)")
    assert(memory.dcdc_charge and not memory.dcdc_solar)
end

do
    local env = solar_env(96, 80, true)
    env.banks["24v-a"].a = 10.1
    scenario("external 24v charging stops surplus solar too", env, "off(2)")
    assert(not memory.dcdc_charge and not memory.dcdc_solar)
    env.banks["24v-a"].a = 8
    env.banks["48v"].soc = 92
    scenario("external charging clears solar hysteresis", env, "")
end

do
    local env = solar_env(96, 80, true)
    env.banks["48v"].cell_uv = true
    scenario("source undervoltage overrides surplus solar", env, "off(2) on(3,300)")
    assert(not memory.dcdc_charge and not memory.dcdc_solar)
    env.banks["48v"].cell_uv = false
    env.banks["48v"].soc = 92
    scenario("source protection clears solar hysteresis", env, "off(3)")
end

scenario("critical 48v SOC overrides both active demands",
    { banks = { ["48v"] = { soc = 20 }, ["24v-a"] = { soc = 40 }, ["24v-b"] = { soc = 40 } },
      relays = { [2] = true }, memory = { dcdc_charge = true, dcdc_solar = true } },
    "off(2) on(3,300)")

do
    local env = solar_env(92, 80, true)
    env.banks["24v-b"] = nil
    scenario("missing telemetry leaves solar transfer to deadman", env, "")
    assert(memory.dcdc_solar)
    env.banks["24v-b"] = { soc = 80 }
    scenario("solar resumes across brief telemetry dropout", env, "on(2,300)")
    env.relays[2] = false  -- Deadman expiry or an administrative force-off.
    scenario("expired relay hold clears solar hysteresis", env, "")
    assert(not memory.dcdc_solar)
end

do
    local env = solar_env(96, 80, false)
    env.numbers = { solar_start = 98, solar_stop = 93, solar_24v_max = 85 }
    scenario("surplus solar thresholds are configurable", env, "")
    env.banks["48v"].soc = 99
    scenario("configured solar entry threshold starts transfer", env, "on(2,300)")
    env.banks["48v"].soc = 93
    scenario("configured solar stop threshold stops transfer", env, "off(2)")
end

do
    local env = solar_env(92, 80, true)
    env.memory = {} -- Firmware restart, policy replacement, or failed cycle.
    scenario("lost mode history does not infer solar from relay state", env, "off(2)")
end

for _, invalid in ipairs({
    { solar_start = 90, solar_stop = 90 },
    { solar_start = 101 }, { solar_stop = -1 },
    { solar_24v_max = 0 }, { solar_24v_max = 101 },
}) do
    reset({ banks = full, numbers = invalid })
    local ok, err = pcall(dofile, POLICY)
    assert(not ok and tostring(err):find("solar thresholds", 1, true))
    assert(table.concat(calls, " ") == "on(4,3600)",
           "invalid thresholds must keep Ethernet on without controlling other relays")
end

if failures > 0 then
    print(string.format("%d scenario(s) failed", failures))
    os.exit(1)
end
print("all scenarios passed")
