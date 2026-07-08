-- Shed power policy.
--
-- Banks:
--   "48v"            primary bank, charged by the generator
--   "24v-a", "24v-b" paralleled 24 volt banks, treated as one bank whose
--                    state of charge is the average of the two
--
-- Relays:
--   1  service raspberry pi (manual control only)
--   2  48v -> 24v DC/DC converter, moves energy into the 24v banks
--   3  generator run control, charges the 48v bank
--
-- Policy parameters (define policy <name>=<value> [<seconds>s]).
-- Parameter names are NVS keys, so they are limited to 15 characters.
-- Boolean flags:
--   force_pi        hold the raspberry pi on
--   force_48v_24v   hold the DC/DC converter on
--   force_48v_gen   hold the generator on (overrides allow-generator)
--   allow-generator defaults true; set false to suppress automatic
--                   generator runs
-- Numbers (defaults shown; state of charge percentages):
--   dcdc_start      50  start moving energy into the 24v banks below this
--   dcdc_stop       70  stop moving energy above this
--   dcdc_source_min 20  never drain the 48v bank below this
--   gen_start       30  start the generator below this
--   gen_stop        60  stop the generator above this
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

local HOLD_SECONDS = 300      -- 5 minute deadman for automatic relays
local PI_HOLD_SECONDS = 3600  -- 60 minute deadman for the raspberry pi

-- 24v bank charging hysteresis (average of 24v-a and 24v-b)
local DCDC_START_SOC = config_number("dcdc_start", 50)
local DCDC_STOP_SOC = config_number("dcdc_stop", 70)
local DCDC_SOURCE_MIN_SOC = config_number("dcdc_source_min", 20)

-- 48v bank generator hysteresis
local GENERATOR_START_SOC = config_number("gen_start", 30)
local GENERATOR_STOP_SOC = config_number("gen_stop", 60)

local ready48, _, _, soc48 = battery_bank_state("48v")
local ready24a, _, _, soc24a = battery_bank_state("24v-a")
local ready24b, _, _, soc24b = battery_bank_state("24v-b")

local soc24 = nil
if ready24a and ready24b then
    soc24 = (soc24a + soc24b) / 2
end

-- Service raspberry pi: manual control only. When force_pi is cleared or
-- expires we stop refreshing and let the long hold run out, so there is no
-- relay_off for the pi.
if config_is_set("force_pi") then
    relay_on(PI_RELAY, PI_HOLD_SECONDS)
end

-- 48v -> 24v DC/DC converter. want is true, false, or nil for no decision.
local dcdc_on = relay_state(DCDC_RELAY)
local want_dcdc = nil
if soc24 ~= nil and ready48 then
    want_dcdc = false
    if soc24 < DCDC_START_SOC then
        want_dcdc = true
    elseif dcdc_on and soc24 < DCDC_STOP_SOC then
        want_dcdc = true
    end
    if want_dcdc and soc48 < DCDC_SOURCE_MIN_SOC then
        syslog("dcdc: 48v bank at", soc48, "% is below source minimum, not moving energy")
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
    want_generator = false
    if soc48 < GENERATOR_START_SOC then
        want_generator = true
    elseif generator_on and soc48 < GENERATOR_STOP_SOC then
        want_generator = true
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
