-- Shed power policy.
--
-- Banks:
--   "48v"            primary bank, charged by the generator and small solar array
--   "24v-a", "24v-b" paralleled 24 volt banks, treated as one bank whose
--                    state of charge is the average of the two
--
-- Relays:
--   1  service raspberry pi (on only while occupied and not deep sleeping)
--   2  48v -> 24v DC/DC converter, moves energy into the 24v banks
--   3  generator run control, charges the 48v bank
--   4  regular Ethernet (always on for state updates)
--   5  PoE switch for the cameras and access point
--
-- Policy parameters (define policy <name>=<value> [<seconds>s]).
-- Parameter names are NVS keys, so they are limited to 15 characters.
-- Boolean flags:
--   deepSleep       suppress service raspberry pi power; defaults false
--   force_48v_24v   hold the DC/DC converter on (overrides source protection)
--   force_48v_gen   hold the generator on (overrides allow-generator)
--   allow-generator defaults true; set false to suppress automatic
--                   generator runs
--   enableCameras   power the camera PoE switch; defaults false
--   occupied        power the service pi, PoE switch, and access point;
--                   defaults false
-- Numbers (defaults shown; state of charge percentages):
--   dcdc_start      50  start moving energy into the 24v banks below this
--   dcdc_stop       70  stop moving energy above this
--   dcdc_source_min 20  stop automatic transfer at or below this 48v SOC
--   dcdc_ext_amps   10  external 24v charging above this disables DC/DC
--   solar_start     95  start surplus transfer above this 48v SOC
--   solar_stop      90  stop surplus transfer at or below this 48v SOC
--   solar_24v_max   90  start below / stop above this average 24v SOC
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
-- Unknown bank state does not create an automatic charging demand: a running
-- relay is neither refreshed nor switched off, so it rides out a brief
-- telemetry dropout and the deadman removes it if the outage persists.
-- A known low 48v source still stops transfer when 24v telemetry is missing.
-- The two transfer demands remember their own hysteresis in volatile policy
-- state. A reboot, changed policy, failed cycle, or expired relay hold clears
-- their history; one transfer mode cannot latch the other mode on.

local PI_RELAY = 1
local DCDC_RELAY = 2
local GENERATOR_RELAY = 3
local ETHERNET_RELAY = 4
local POE_RELAY = 5

local HOLD_SECONDS = 300      -- 5 minute deadman for automatic relays
local PI_HOLD_SECONDS = 3600  -- 60 minute deadman for the raspberry pi
local ETHERNET_HOLD_SECONDS = 3600 -- 60 minute deadman for state-update access

-- Ethernet must stay on regardless of occupancy, cameras, deep sleep, or bank
-- telemetry. Refresh it first so a later policy/configuration error does not
-- prevent state updates or remote repair.
relay_on(ETHERNET_RELAY, ETHERNET_HOLD_SECONDS)

assert(type(policy_state_bool) == "function" and type(policy_state_set) == "function",
       "shed policy requires firmware with policy_state_bool and policy_state_set")

-- 24v bank charging hysteresis (average of 24v-a and 24v-b)
local DCDC_START_SOC = config_number("dcdc_start", 50)
local DCDC_STOP_SOC = config_number("dcdc_stop", 70)
local DCDC_SOURCE_MIN_SOC = config_number("dcdc_source_min", 20)
local DCDC_EXTERNAL_CHARGE_A = config_number("dcdc_ext_amps", 10)
if DCDC_EXTERNAL_CHARGE_A < 0 then
    error("dcdc_ext_amps must be at least zero")
end

local SOLAR_START_SOC = config_number("solar_start", 95)
local SOLAR_STOP_SOC = config_number("solar_stop", 90)
local SOLAR_24V_MAX_SOC = config_number("solar_24v_max", 90)
if SOLAR_STOP_SOC < 0 or SOLAR_STOP_SOC >= SOLAR_START_SOC or SOLAR_START_SOC > 100
    or SOLAR_24V_MAX_SOC <= 0 or SOLAR_24V_MAX_SOC > 100 then
    error("solar thresholds require 0 <= solar_stop < solar_start <= 100 and 0 < solar_24v_max <= 100")
end

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
local ready24a, _, amps24a, soc24a, min24a, cell_age24a, cell_uv24a =
    battery_bank_state("24v-a")
local ready24b, _, amps24b, soc24b, min24b, cell_age24b, cell_uv24b =
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

-- Service raspberry pi: power it only while the site is occupied and not in
-- deep sleep. Losing either demand opens the relay immediately.
local pi_on = relay_state(PI_RELAY)
local want_pi = config_bool("occupied", false)
    and not config_bool("deepSleep", false)
if want_pi then
    relay_on(PI_RELAY, PI_HOLD_SECONDS)
elseif pi_on then
    relay_off(PI_RELAY)
end

-- Camera PoE switch and access point. When neither cameras nor occupancy needs
-- them, open the relay immediately rather than waiting for its hold to expire.
local poe_on = relay_state(POE_RELAY)
local want_poe = config_bool("enableCameras", false)
    or config_bool("occupied", false)
if want_poe then
    relay_on(POE_RELAY, HOLD_SECONDS)
elseif poe_on then
    relay_off(POE_RELAY)
end

-- 48v -> 24v DC/DC converter. want is true, false, or nil for no decision.
local dcdc_on = relay_state(DCDC_RELAY)
local charge_active = dcdc_on and policy_state_bool("dcdc_charge", false)
local solar_active = dcdc_on and policy_state_bool("dcdc_solar", false)
local want_charge = charge_active
local want_solar = solar_active
local want_dcdc = nil
if soc24 ~= nil and ready48 then
    -- The DC/DC converter normally contributes about 8A to each 24v bank.
    -- More than this threshold means another source (generator or solar) is
    -- charging the 24v side, so it should take over even while hysteresis or
    -- cell recovery would otherwise keep the converter running.
    local external_24v_charging = amps24a > DCDC_EXTERNAL_CHARGE_A
        or amps24b > DCDC_EXTERNAL_CHARGE_A
    local cell24a_low_reason = cell_low_reason(min24a, cell_age24a, cell_uv24a)
    local cell24b_low_reason = cell_low_reason(min24b, cell_age24b, cell_uv24b)
    local cell24_low = cell24a_low_reason ~= nil or cell24b_low_reason ~= nil
    local cell24a_recovery_reason = cell_recovery_reason(min24a, cell_age24a, cell_uv24a)
    local cell24b_recovery_reason = cell_recovery_reason(min24b, cell_age24b, cell_uv24b)
    local cell24_recovering = cell24a_recovery_reason ~= nil
        or cell24b_recovery_reason ~= nil
    want_charge = false
    if soc24 < DCDC_START_SOC or cell24_low then
        want_charge = true
        if cell24_low then
            syslog("dcdc: 24v cell low; 24v-a source",
                   cell24a_low_reason or "none", "24v-b source",
                   cell24b_low_reason or "none", "min cells", min24a, min24b)
        end
    elseif charge_active and (soc24 < DCDC_STOP_SOC or cell24_recovering) then
        want_charge = true
        if cell24_recovering then
            syslog("dcdc: waiting for 24v cell recovery; 24v-a source",
                   cell24a_recovery_reason or "none", "24v-b source",
                   cell24b_recovery_reason or "none", "min cells", min24a, min24b)
        end
    end

    -- Surplus transfer has its own latch. The destination limit only applies
    -- to this demand; normal charging/cell recovery can still request transfer.
    want_solar = (soc48 > SOLAR_START_SOC and soc24 < SOLAR_24V_MAX_SOC)
        or (solar_active and soc48 > SOLAR_STOP_SOC and soc24 <= SOLAR_24V_MAX_SOC)
    want_dcdc = want_charge or want_solar

    if external_24v_charging then
        if want_dcdc or dcdc_on then
            syslog("dcdc: external 24v charging detected, not moving energy; currents",
                   amps24a, amps24b, "threshold", DCDC_EXTERNAL_CHARGE_A)
        end
        want_charge = false
        want_solar = false
        want_dcdc = false
    end
else
    syslog("dcdc: bank state incomplete, cannot determine charging demand")
end

-- Source protection does not depend on 24v telemetry or charging hysteresis.
-- Stop a running converter on this policy cycle, without waiting for deadman.
if ready48 then
    local source_cell_low_reason = cell_low_reason(min48, cell_age48, cell_uv48)
    if soc48 <= DCDC_SOURCE_MIN_SOC or source_cell_low_reason ~= nil then
        if want_dcdc or dcdc_on then
            syslog("dcdc: 48v source low, not moving energy; soc", soc48,
                   "cell source", source_cell_low_reason or "SOC", "min_cell", min48)
        end
        want_charge = false
        want_solar = false
        want_dcdc = false
    end
end
if want_solar and not solar_active then
    syslog("dcdc: surplus solar transfer requested; 48v SOC", soc48, "24v SOC", soc24)
elseif solar_active and not want_solar then
    syslog("dcdc: surplus solar transfer request ended; 48v SOC", soc48, "24v SOC", soc24)
end
policy_state_set("dcdc_charge", want_charge)
policy_state_set("dcdc_solar", want_solar)
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
