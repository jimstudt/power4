-- Host-side behavioral tests for examples/house.lua.

local POLICY = arg[1] or "examples/house.lua"

local flags
local inputs
local relays
local calls

function config_is_set(name)
    assert(#name >= 1 and #name <= 15 and name:match("^[%w_%-]+$"),
           "invalid policy parameter name: " .. name)
    return flags[name] == true
end

function input_on(input)
    assert(input == 1 or input == 2, "unexpected input number")
    return inputs[input] == true
end

function relay_state(relay)
    return relays[relay] == true, nil, relays[relay] and 200 or 0
end

function relay_on(relay, seconds)
    calls[#calls + 1] = string.format("on(%d,%d)", relay, seconds)
end

function relay_off(relay)
    calls[#calls + 1] = string.format("off(%d)", relay)
end

function syslog(...) end

local failures = 0

local function scenario(label, environment, expected)
    flags = environment.flags or {}
    inputs = environment.inputs or {}
    relays = environment.relays or {}
    calls = {}

    dofile(POLICY)

    local got = table.concat(calls, " ")
    if got == expected then
        print(string.format("ok   %-42s [%s]", label, got))
    else
        failures = failures + 1
        print(string.format("FAIL %-42s expected [%s] got [%s]", label, expected, got))
    end
end

scenario("no flags keeps only admin computer",
    {},
    "on(2,300)")

scenario("ample power runs all named loads",
    { flags = { amplePower = true } },
    "on(1,300) on(2,300) on(3,300) on(4,300) on(5,300)")

scenario("have power runs internet",
    { flags = { havePower = true } },
    "on(2,300) on(3,300)")

scenario("have power and daylight add camera",
    { flags = { havePower = true, daylight = true } },
    "on(2,300) on(3,300) on(5,300)")

scenario("daylight alone does not run camera",
    { flags = { daylight = true } },
    "on(2,300)")

scenario("scheduled interval needs normal power",
    { flags = { dawn = true } },
    "on(2,300)")

scenario("dawn with power runs network loads",
    { flags = { havePower = true, dawn = true } },
    "on(1,300) on(2,300) on(3,300) on(4,300)")

scenario("noon with power runs network loads",
    { flags = { havePower = true, noon = true } },
    "on(1,300) on(2,300) on(3,300) on(4,300)")

scenario("dusk with power runs network loads",
    { flags = { havePower = true, dusk = true } },
    "on(1,300) on(2,300) on(3,300) on(4,300)")

scenario("force internet runs admin and internet",
    { flags = { ["force-internet"] = true } },
    "on(2,300) on(3,300)")

scenario("force wifi runs powered ethernet",
    { flags = { ["force-wifi"] = true } },
    "on(1,300) on(2,300) on(4,300)")

scenario("force flags combine",
    { flags = { ["force-internet"] = true, ["force-wifi"] = true } },
    "on(1,300) on(2,300) on(3,300) on(4,300)")

scenario("deep sleep overrides ample power and forces",
    {
        flags = {
            amplePower = true,
            daylight = true,
            dawn = true,
            ["force-internet"] = true,
            ["force-wifi"] = true,
            deepSleep = true,
        },
        relays = { true, true, true, true, true },
    },
    "off(1) off(2) off(3) off(4) off(5)")

scenario("DI1 occupied powers the five named relays",
    { inputs = { [1] = true } },
    "on(1,300) on(2,300) on(3,300) on(4,300) on(5,300)")

scenario("DI1 occupied overrides deep sleep",
    { flags = { deepSleep = true }, inputs = { [1] = true } },
    "on(1,300) on(2,300) on(3,300) on(4,300) on(5,300)")

scenario("DI2 powers every relay",
    { inputs = { [2] = true } },
    "on(1,300) on(2,300) on(3,300) on(4,300) on(5,300) on(6,300) on(7,300) on(8,300)")

scenario("DI2 all-relays overrides deep sleep",
    {
        flags = { deepSleep = true },
        inputs = { [2] = true },
        relays = { false, false, true, true, true },
    },
    "on(1,300) on(2,300) on(3,300) on(4,300) on(5,300) on(6,300) on(7,300) on(8,300)")

scenario("undesired running relays turn off",
    { relays = { true, false, true, true, true, true, true, true } },
    "off(1) on(2,300) off(3) off(4) off(5) off(6) off(7) off(8)")

if failures > 0 then
    print(string.format("%d scenario(s) failed", failures))
    os.exit(1)
end

print("all house policy scenarios passed")
