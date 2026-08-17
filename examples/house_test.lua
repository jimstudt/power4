-- Host-side behavioral tests for examples/house.lua.

local POLICY = arg[1] or "examples/house.lua"
local MAX_UPLOAD_BYTES = 16 * 1024

local policy_file = assert(io.open(POLICY, "rb"))
local policy_size = assert(policy_file:seek("end"))
policy_file:close()
assert(policy_size <= MAX_UPLOAD_BYTES,
       string.format("policy is %d bytes; upload limit is %d",
                     policy_size, MAX_UPLOAD_BYTES))

local flags
local inputs
local relays
local calls
local clock

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

function local_time()
    local value = clock or {}
    return {
        year = value.year or 2026,
        month = value.month or 1,
        day = value.day or 1,
        weekday = value.weekday or 4,
        hour = value.hour or 0,
        minute = value.minute or 0,
        second = value.second or 0,
        valid = value.valid ~= false,
        utc_offset_minutes = value.utc_offset_minutes or -360,
        daylight_saving = value.daylight_saving == true,
        zone = value.zone or "CST",
        timezone = "US/Central",
        utc = false,
    }
end

local failures = 0

local function scenario(label, environment, expected)
    flags = environment.flags or {}
    inputs = environment.inputs or {}
    relays = environment.relays or {}
    clock = environment.clock
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

scenario("ample power runs wired loads and cameras",
    { flags = { amplePower = true } },
    "on(1,300) on(2,300) on(3,300) on(4,300) on(5,300)")

scenario("have power runs internet",
    { flags = { havePower = true } },
    "on(2,300) on(3,300)")

scenario("daylight porch camera does not require PoE",
    {
        flags = { havePower = true },
        clock = { hour = 13 },
    },
    "on(2,300) on(3,300) on(5,300)")

scenario("daylight alone does not run camera",
    { clock = { hour = 13 } },
    "on(2,300)")

scenario("scheduled interval needs normal power",
    { clock = { hour = 12 } },
    "on(2,300)")

scenario("computed dawn runs PoE cameras",
    {
        flags = { havePower = true },
        clock = { hour = 7, minute = 23 },
    },
    "on(1,300) on(2,300) on(3,300) on(4,300)")

scenario("outside computed dawn omits PoE cameras",
    {
        flags = { havePower = true },
        clock = { hour = 7, minute = 22 },
    },
    "on(2,300) on(3,300)")

scenario("computed noon runs PoE cameras",
    {
        flags = { havePower = true },
        clock = { hour = 12 },
    },
    "on(1,300) on(2,300) on(3,300) on(4,300) on(5,300)")

scenario("computed dusk runs PoE cameras",
    {
        flags = { havePower = true },
        clock = { hour = 16, minute = 20 },
    },
    "on(1,300) on(2,300) on(3,300) on(4,300)")

scenario("outside computed dusk omits PoE cameras",
    {
        flags = { havePower = true },
        clock = { hour = 16, minute = 22 },
    },
    "on(2,300) on(3,300)")

scenario("summer CDT offset computes the dawn window",
    {
        flags = { havePower = true },
        clock = {
            year = 2026,
            month = 7,
            day = 30,
            hour = 5,
            minute = 26,
            utc_offset_minutes = -300,
            daylight_saving = true,
            zone = "CDT",
        },
    },
    "on(1,300) on(2,300) on(3,300) on(4,300)")

scenario("invalid system clock disables time-dependent loads",
    {
        flags = { havePower = true },
        clock = { valid = false, hour = 13 },
    },
    "on(2,300) on(3,300)")

scenario("force internet runs admin and internet",
    { flags = { ["force-internet"] = true } },
    "on(2,300) on(3,300)")

scenario("force wifi runs WiFi and its Ethernet dependency",
    { flags = { ["force-wifi"] = true } },
    "on(1,300) on(2,300) on(6,300)")

scenario("force flags combine",
    { flags = { ["force-internet"] = true, ["force-wifi"] = true } },
    "on(1,300) on(2,300) on(3,300) on(6,300)")

scenario("forced WiFi also runs with ample power",
    { flags = { amplePower = true, ["force-wifi"] = true } },
    "on(1,300) on(2,300) on(3,300) on(4,300) on(5,300) on(6,300)")

scenario("deep sleep overrides ample power and forces",
    {
        flags = {
            amplePower = true,
            ["force-internet"] = true,
            ["force-wifi"] = true,
            deepSleep = true,
        },
        relays = { true, true, true, true, true, true },
    },
    "off(1) off(2) off(3) off(4) off(5) off(6)")

scenario("DI1 occupied powers the six named relays",
    { inputs = { [1] = true } },
    "on(1,300) on(2,300) on(3,300) on(4,300) on(5,300) on(6,300)")

scenario("DI1 occupied overrides deep sleep",
    { flags = { deepSleep = true }, inputs = { [1] = true } },
    "on(1,300) on(2,300) on(3,300) on(4,300) on(5,300) on(6,300)")

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
