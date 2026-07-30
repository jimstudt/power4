-- House power policy.
--
-- Relays:
--   1  ethernet
--   2  adminComputer
--   3  internet
--   4  poweredEthernet
--   5  porchCamera
--
-- Boolean policy parameters, set and cleared externally:
--   amplePower      run every relay
--   havePower       run internet; also run the porch camera during daylight
--   daylight        allow the porch camera when havePower is set
--   dawn/noon/dusk  run internet and powered Ethernet during the indicated
--                   interval when either normal power flag is set
--   force-internet  run internet and the admin computer
--   force-wifi      run powered Ethernet
--   deepSleep       turn every policy-controlled relay off; this overrides
--                   all of the other policy parameters
--
-- Physical mode inputs:
--   DI1              request the external inverter by powering the regular
--                    Ethernet equipment and admin computer; the external
--                    controller observes DI2 and starts the inverter
--   DI2              power everything
--
-- The physical mode inputs are local recovery overrides and therefore take
-- precedence over deepSleep. Whenever powered Ethernet is requested, regular
-- Ethernet is requested as a dependency.
--
-- A relay force set with `set relay <n> force-on` is an administrative
-- override below the policy layer and therefore cannot be defeated by
-- deepSleep. Clear such a force before relying on deepSleep.
--
-- The policy runs once a minute. Desired relays receive a five-minute
-- dead-man hold on every cycle. Relays that are no longer desired are
-- switched off immediately.

local ETHERNET_RELAY = 1
local ADMIN_COMPUTER_RELAY = 2
local INTERNET_RELAY = 3
local POWERED_ETHERNET_RELAY = 4
local PORCH_CAMERA_RELAY = 5

local HOLD_SECONDS = 300

local ample_power = config_is_set("amplePower")
local have_power = config_is_set("havePower")
local daylight = config_is_set("daylight")
local scheduled_network =
    config_is_set("dawn") or config_is_set("noon") or config_is_set("dusk")
local force_internet = config_is_set("force-internet")
local force_wifi = config_is_set("force-wifi")
local deep_sleep = config_is_set("deepSleep")
local power_everything = input_on(1)
local run_inverter = input_on(2)

local want_ethernet = false
local want_admin_computer = true
local want_internet = false
local want_powered_ethernet = false
local want_porch_camera = false

if ample_power then
    want_ethernet = true
    want_admin_computer = true
    want_internet = true
    want_powered_ethernet = true
    want_porch_camera = true
else
    if have_power then
        want_internet = true
        want_porch_camera = daylight
    end

    -- amplePower or havePower means normal operation is allowed. With
    -- neither flag set, dawn/noon/dusk cannot bring optional loads online.
    if (ample_power or have_power) and scheduled_network then
        want_internet = true
        want_powered_ethernet = true
    end

    if force_internet then
        want_admin_computer = true
        want_internet = true
    end
    if force_wifi then
        want_powered_ethernet = true
    end
end

if deep_sleep then
    want_ethernet = false
    want_admin_computer = false
    want_internet = false
    want_powered_ethernet = false
    want_porch_camera = false
end

if power_everything then
    want_ethernet = true
    want_admin_computer = true
    want_internet = true
    want_powered_ethernet = true
    want_porch_camera = true
elseif run_inverter then
    want_ethernet = true
    want_admin_computer = true
end

if want_powered_ethernet then
    want_ethernet = true
end

local function apply_relay(relay, wanted)
    local on = relay_state(relay)
    if wanted then
        relay_on(relay, HOLD_SECONDS)
    elseif on then
        relay_off(relay)
    end
end

apply_relay(ETHERNET_RELAY, want_ethernet)
apply_relay(ADMIN_COMPUTER_RELAY, want_admin_computer)
apply_relay(INTERNET_RELAY, want_internet)
apply_relay(POWERED_ETHERNET_RELAY, want_powered_ethernet)
apply_relay(PORCH_CAMERA_RELAY, want_porch_camera)
