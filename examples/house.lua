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
--   amplePower      run all five named house loads
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
--   DI1 / occupied   power the five named house relays
--   DI2              power all eight physical relay channels
--
-- The physical mode inputs are local recovery overrides and therefore take
-- precedence over deepSleep. Whenever powered Ethernet is requested, regular
-- Ethernet is requested as a dependency.
--
-- A relay force set with `set relay <n> force-on|force-off` is an
-- administrative override below the policy layer. It takes precedence over
-- every request here, including the DI2 all-relays request.
--
-- The policy runs once a minute. Desired relays receive a five-minute
-- dead-man hold on every cycle. Relays that are no longer desired are
-- switched off immediately.

local ETHERNET_RELAY = 1
local ADMIN_COMPUTER_RELAY = 2
local INTERNET_RELAY = 3
local POWERED_ETHERNET_RELAY = 4
local PORCH_CAMERA_RELAY = 5
local RELAY_COUNT = 8

local HOLD_SECONDS = 300

local ample_power = config_is_set("amplePower")
local have_power = config_is_set("havePower")
local daylight = config_is_set("daylight")
local scheduled_network =
    config_is_set("dawn") or config_is_set("noon") or config_is_set("dusk")
local force_internet = config_is_set("force-internet")
local force_wifi = config_is_set("force-wifi")
local deep_sleep = config_is_set("deepSleep")
local occupied = input_on(1)
local power_everything = input_on(2)

local wanted = {}
for relay = 1, RELAY_COUNT do
    wanted[relay] = false
end

wanted[ADMIN_COMPUTER_RELAY] = true

if ample_power then
    wanted[ETHERNET_RELAY] = true
    wanted[ADMIN_COMPUTER_RELAY] = true
    wanted[INTERNET_RELAY] = true
    wanted[POWERED_ETHERNET_RELAY] = true
    wanted[PORCH_CAMERA_RELAY] = true
else
    if have_power then
        wanted[INTERNET_RELAY] = true
        wanted[PORCH_CAMERA_RELAY] = daylight
    end

    -- amplePower or havePower means normal operation is allowed. With
    -- neither flag set, dawn/noon/dusk cannot bring optional loads online.
    if (ample_power or have_power) and scheduled_network then
        wanted[INTERNET_RELAY] = true
        wanted[POWERED_ETHERNET_RELAY] = true
    end

    if force_internet then
        wanted[ADMIN_COMPUTER_RELAY] = true
        wanted[INTERNET_RELAY] = true
    end
    if force_wifi then
        wanted[POWERED_ETHERNET_RELAY] = true
    end
end

if deep_sleep then
    for relay = 1, RELAY_COUNT do
        wanted[relay] = false
    end
end

if power_everything then
    for relay = 1, RELAY_COUNT do
        wanted[relay] = true
    end
elseif occupied then
    wanted[ETHERNET_RELAY] = true
    wanted[ADMIN_COMPUTER_RELAY] = true
    wanted[INTERNET_RELAY] = true
    wanted[POWERED_ETHERNET_RELAY] = true
    wanted[PORCH_CAMERA_RELAY] = true
end

if wanted[POWERED_ETHERNET_RELAY] then
    wanted[ETHERNET_RELAY] = true
end

local function apply_relay(relay, wanted)
    local on = relay_state(relay)
    if wanted then
        relay_on(relay, HOLD_SECONDS)
    elseif on then
        relay_off(relay)
    end
end

for relay = 1, RELAY_COUNT do
    apply_relay(relay, wanted[relay])
end
