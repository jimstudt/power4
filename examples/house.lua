-- House power policy.
-- Relays: 1 ethernet, 2 adminComputer, 3 internet, 4 poweredEthernet,
-- 5 porchCamera. Relays 6-8 are used only by the DI2 override.
--
-- Parameters: amplePower runs all five named loads; havePower runs internet
-- and the daylight camera; force-internet and force-wifi request their named
-- loads; deepSleep turns off all policy-controlled relays.
--
-- DI1 (occupied) powers the five named loads. DI2 powers all eight relays.
-- Inputs override deepSleep, while relay administrative forces override this
-- policy. poweredEthernet implies ethernet.
--
-- The system timezone converts the UTC clock to local civil time. NOAA's
-- fractional-year approximation calculates sunrise and sunset at 45.127778,
-- -87.246944. Dawn, local noon, and dusk cover five minutes on either side.

local ETHERNET_RELAY = 1
local ADMIN_COMPUTER_RELAY = 2
local INTERNET_RELAY = 3
local POWERED_ETHERNET_RELAY = 4
local PORCH_CAMERA_RELAY = 5
local RELAY_COUNT = 8

local HOLD_SECONDS = 300

local LATITUDE_DEGREES = 45.127778
local LONGITUDE_DEGREES = -87.246944
local SUNRISE_ZENITH_DEGREES = 90.833
local EVENT_WINDOW_SECONDS = 5 * 60
local SECONDS_PER_DAY = 24 * 60 * 60
local MINUTES_PER_DAY = 24 * 60
local MONTH_LENGTHS = {
    31, 28, 31, 30, 31, 30,
    31, 31, 30, 31, 30, 31,
}

local function is_leap_year(year)
    return year % 4 == 0 and (year % 100 ~= 0 or year % 400 == 0)
end

local function days_in_month(year, month)
    if month == 2 and is_leap_year(year) then
        return 29
    end
    return MONTH_LENGTHS[month]
end

local function day_of_year(year, month, day)
    local result = day
    for preceding_month = 1, month - 1 do
        result = result + days_in_month(year, preceding_month)
    end
    return result
end

local function solar_times(year, month, day, utc_offset_minutes)
    local days_this_year = is_leap_year(year) and 366 or 365
    local gamma = 2 * math.pi / days_this_year
        * (day_of_year(year, month, day) - 1)

    local equation_of_time = 229.18 * (
        0.000075
        + 0.001868 * math.cos(gamma)
        - 0.032077 * math.sin(gamma)
        - 0.014615 * math.cos(2 * gamma)
        - 0.040849 * math.sin(2 * gamma)
    )
    local solar_declination =
        0.006918
        - 0.399912 * math.cos(gamma)
        + 0.070257 * math.sin(gamma)
        - 0.006758 * math.cos(2 * gamma)
        + 0.000907 * math.sin(2 * gamma)
        - 0.002697 * math.cos(3 * gamma)
        + 0.001480 * math.sin(3 * gamma)

    local latitude = math.rad(LATITUDE_DEGREES)
    local hour_angle_cosine =
        math.cos(math.rad(SUNRISE_ZENITH_DEGREES))
            / (math.cos(latitude) * math.cos(solar_declination))
        - math.tan(latitude) * math.tan(solar_declination)

    if hour_angle_cosine < -1 or hour_angle_cosine > 1 then
        return nil, nil
    end

    local hour_angle_degrees = math.deg(math.acos(hour_angle_cosine))
    local solar_noon_utc =
        720 - 4 * LONGITUDE_DEGREES - equation_of_time
    local sunrise_utc = solar_noon_utc - 4 * hour_angle_degrees
    local sunset_utc = solar_noon_utc + 4 * hour_angle_degrees

    return (sunrise_utc + utc_offset_minutes) % MINUTES_PER_DAY,
           (sunset_utc + utc_offset_minutes) % MINUTES_PER_DAY
end

local function within_event_window(now_seconds, event_minutes)
    if event_minutes == nil then
        return false
    end
    local difference = math.abs(now_seconds - event_minutes * 60)
    difference = math.min(difference, SECONDS_PER_DAY - difference)
    return difference <= EVENT_WINDOW_SECONDS
end

local function between_sunrise_and_sunset(now_seconds, sunrise, sunset)
    if sunrise == nil or sunset == nil then
        return false
    end
    local now_minutes = now_seconds / 60
    if sunrise <= sunset then
        return now_minutes >= sunrise and now_minutes < sunset
    end
    return now_minutes >= sunrise or now_minutes < sunset
end

local ample_power = config_is_set("amplePower")
local have_power = config_is_set("havePower")
local force_internet = config_is_set("force-internet")
local force_wifi = config_is_set("force-wifi")
local deep_sleep = config_is_set("deepSleep")
local occupied = input_on(1)
local power_everything = input_on(2)

local daylight = false
local dawn = false
local noon = false
local dusk = false
local now = local_time()
if now.valid then
    local now_seconds = now.hour * 3600 + now.minute * 60 + now.second
    local sunrise, sunset = solar_times(
        now.year, now.month, now.day, now.utc_offset_minutes)
    daylight = between_sunrise_and_sunset(now_seconds, sunrise, sunset)
    dawn = within_event_window(now_seconds, sunrise)
    noon = within_event_window(now_seconds, 12 * 60)
    dusk = within_event_window(now_seconds, sunset)
else
    syslog("house: system clock is not valid; time-dependent loads disabled")
end
local scheduled_network = dawn or noon or dusk

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
