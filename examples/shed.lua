-- Shed power policy.
--
-- Banks:
--   "48v"            primary bank, charged by the generator
--   "24v-a", "24v-b" paralleled 24 volt banks, treated as one bank whose
--                    state of charge is the average of the two
--
-- Relays:
--   1  service raspberry pi (normally on; deepSleep turns it off)
--   2  48v -> 24v DC/DC converter, moves energy into the 24v banks
--   3  generator run control, charges the 48v bank
--   4  PoE switch for the cameras
--
-- Policy parameters (define policy <name>=<value> [<seconds>s]).
-- Parameter names are NVS keys, so they are limited to 15 characters.
-- Boolean flags:
--   deepSleep       turn off the service raspberry pi; defaults false
--   force_48v_24v   hold the DC/DC converter on
--   force_48v_gen   hold the generator on (overrides allow-generator)
--   allow-generator defaults true; set false to suppress automatic
--                   generator runs
--   enableCameras   power the camera PoE switch; defaults false
-- Numbers (defaults shown; state of charge percentages):
--   dcdc_start      50  start moving energy into the 24v banks below this
--   dcdc_stop       70  stop moving energy above this
--   dcdc_source_min 20  never drain the 48v bank below this
--   gen_start       30  start the generator below this
--   gen_stop        60  stop the generator above this
--   cell_low       3.10 treat any fresh cell at or below this as low (volts)
--   cell_recover   3.25 clear a cell-voltage hold at or above this (volts)
--   cell_max_age    900 ignore cell voltages older than this (seconds)
--
-- The policy runs once a minute. Relays are held on a deadman timer and
-- refreshed each cycle; if this policy stops running, everything except an
-- administratively forced relay turns itself off when its hold expires.
--
-- When a bank's state is not known we make no automatic decision at all:
-- a running relay is neither refreshed nor switched off, so it rides out a
-- brief telemetry dropout and the deadman removes it if the outage persists.

local PI_RELAY = 1
local DCDC_RELAY = 2
local GENERATOR_RELAY = 3
local POE_RELAY = 4

local HOLD_SECONDS = 300      -- 5 minute deadman for automatic relays
local PI_HOLD_SECONDS = 3600  -- 60 minute deadman for the raspberry pi

-- 24v bank charging hysteresis (average of 24v-a and 24v-b)
local DCDC_START_SOC = config_number("dcdc_start", 50)
local DCDC_STOP_SOC = config_number("dcdc_stop", 70)
local DCDC_SOURCE_MIN_SOC = config_number("dcdc_source_min", 20)

-- 48v bank generator hysteresis
local GENERATOR_START_SOC = config_number("gen_start", 30)
local GENERATOR_STOP_SOC = config_number("gen_stop", 60)

local CELL_LOW_V = config_number("cell_low", 3.10)
local CELL_RECOVER_V = config_number("cell_recover", 3.25)
local CELL_MAX_AGE_S = config_number("cell_max_age", 900)
if CELL_LOW_V <= 0 or CELL_RECOVER_V <= CELL_LOW_V or CELL_MAX_AGE_S < 0 then
    error("cell thresholds require 0 < cell_low < cell_recover and cell_max_age >= 0")
end

local ready48, _, _, soc48, min48, cell_age48, cell_uv48 = battery_bank_state("48v")
local ready24a, _, _, soc24a, min24a, cell_age24a, cell_uv24a =
    battery_bank_state("24v-a")
local ready24b, _, _, soc24b, min24b, cell_age24b, cell_uv24b =
    battery_bank_state("24v-b")

local function cell_voltage_fresh(voltage, age)
    return voltage ~= nil and age ~= nil and age <= CELL_MAX_AGE_S
end

local function cell_low_reason(voltage, age, undervoltage)
    if undervoltage == true then
        return "JBD protection alarm"
    end
    if cell_voltage_fresh(voltage, age) and voltage <= CELL_LOW_V then
        return "measured voltage"
    end
    return nil
end

local function cell_recovery_reason(voltage, age, undervoltage)
    if undervoltage == true then
        return "JBD protection alarm"
    end
    if cell_voltage_fresh(voltage, age) and voltage < CELL_RECOVER_V then
        return "measured voltage"
    end
    return nil
end

local soc24 = nil
if ready24a and ready24b then
    soc24 = (soc24a + soc24b) / 2
end

-- Service raspberry pi: normally kept on. Deep sleep opens its relay
-- immediately; otherwise the long deadman hold is refreshed each cycle.
local pi_on = relay_state(PI_RELAY)
if config_bool("deepSleep", false) then
    if pi_on then
        relay_off(PI_RELAY)
    end
else
    relay_on(PI_RELAY, PI_HOLD_SECONDS)
end

-- Camera PoE switch. Disabling the cameras opens the relay immediately rather
-- than waiting for its hold to expire.
local poe_on = relay_state(POE_RELAY)
if config_bool("enableCameras", false) then
    relay_on(POE_RELAY, HOLD_SECONDS)
elseif poe_on then
    relay_off(POE_RELAY)
end

-- 48v -> 24v DC/DC converter. want is true, false, or nil for no decision.
local dcdc_on = relay_state(DCDC_RELAY)
local want_dcdc = nil
if soc24 ~= nil and ready48 then
    local cell24a_low_reason = cell_low_reason(min24a, cell_age24a, cell_uv24a)
    local cell24b_low_reason = cell_low_reason(min24b, cell_age24b, cell_uv24b)
    local cell24_low = cell24a_low_reason ~= nil or cell24b_low_reason ~= nil
    local cell24a_recovery_reason = cell_recovery_reason(min24a, cell_age24a, cell_uv24a)
    local cell24b_recovery_reason = cell_recovery_reason(min24b, cell_age24b, cell_uv24b)
    local cell24_recovering = cell24a_recovery_reason ~= nil
        or cell24b_recovery_reason ~= nil
    want_dcdc = false
    if soc24 < DCDC_START_SOC or cell24_low then
        want_dcdc = true
        if cell24_low then
            syslog("dcdc: 24v cell low; 24v-a source",
                   cell24a_low_reason or "none", "24v-b source",
                   cell24b_low_reason or "none", "min cells", min24a, min24b)
        end
    elseif dcdc_on and (soc24 < DCDC_STOP_SOC or cell24_recovering) then
        want_dcdc = true
        if cell24_recovering then
            syslog("dcdc: waiting for 24v cell recovery; 24v-a source",
                   cell24a_recovery_reason or "none", "24v-b source",
                   cell24b_recovery_reason or "none", "min cells", min24a, min24b)
        end
    end
    local source_cell_low_reason = cell_low_reason(min48, cell_age48, cell_uv48)
    if want_dcdc and (soc48 < DCDC_SOURCE_MIN_SOC or source_cell_low_reason ~= nil) then
        syslog("dcdc: 48v source low, not moving energy; soc", soc48,
               "cell source", source_cell_low_reason or "SOC", "min_cell", min48)
        want_dcdc = false
    end
else
    syslog("dcdc: bank state not ready, no automatic control")
end
if config_is_set("force_48v_24v") then
    want_dcdc = true
end

if want_dcdc == true then
    relay_on(DCDC_RELAY, HOLD_SECONDS)
elseif want_dcdc == false and dcdc_on then
    syslog("dcdc: stopping, 24v at", soc24, "%")
    relay_off(DCDC_RELAY)
end

-- Generator on the 48v bank. want is true, false, or nil for no decision.
local generator_on = relay_state(GENERATOR_RELAY)
local want_generator = nil
if ready48 then
    local cell48_low_reason = cell_low_reason(min48, cell_age48, cell_uv48)
    local cell48_recovery_reason = cell_recovery_reason(min48, cell_age48, cell_uv48)
    want_generator = false
    if soc48 < GENERATOR_START_SOC or cell48_low_reason ~= nil then
        want_generator = true
        if cell48_low_reason ~= nil then
            syslog("generator: 48v cell low; min_cell", min48,
                   "source", cell48_low_reason)
        end
    elseif generator_on and (soc48 < GENERATOR_STOP_SOC or cell48_recovery_reason ~= nil) then
        want_generator = true
        if cell48_recovery_reason ~= nil then
            syslog("generator: waiting for 48v cell recovery; min_cell", min48,
                   "source", cell48_recovery_reason)
        end
    end
else
    syslog("generator: 48v bank not ready, no automatic control")
end
if want_generator and not config_bool("allow-generator", true) then
    syslog("generator: wanted but disabled by allow-generator=false")
    want_generator = false
end
if config_is_set("force_48v_gen") then
    want_generator = true
end

if want_generator == true then
    relay_on(GENERATOR_RELAY, HOLD_SECONDS)
elseif want_generator == false and generator_on then
    syslog("generator: stopping, 48v at", soc48, "%")
    relay_off(GENERATOR_RELAY)
end
