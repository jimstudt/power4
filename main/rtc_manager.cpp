#include "rtc_manager.hpp"

#include <string.h>

#include "board_i2c.hpp"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

namespace {

constexpr const char *kTag = "rtc_manager";
constexpr int kI2cTimeoutMs = 100;
constexpr uint8_t kControl1Register = 0x00;
constexpr uint8_t kSecondsRegister = 0x04;
constexpr uint8_t kControl1Stop = 0x20;
constexpr uint8_t kControl1TwelveHour = 0x02;
constexpr uint8_t kControl1CapSelect = 0x01;
constexpr uint8_t kSecondsOscillatorStop = 0x80;

RtcStatus g_status = {};
RtcHardwareConfig g_hardware = {};
i2c_master_dev_handle_t g_device = nullptr;

uint8_t to_bcd(uint8_t value)
{
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

bool from_bcd(uint8_t value, uint8_t mask, uint8_t *result)
{
    value &= mask;
    const uint8_t high = static_cast<uint8_t>((value >> 4) & 0x0f);
    const uint8_t low = static_cast<uint8_t>(value & 0x0f);
    if (high > 9 || low > 9) {
        return false;
    }
    *result = static_cast<uint8_t>(high * 10 + low);
    return true;
}

bool leap_year(uint16_t year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

uint8_t days_in_month(uint16_t year, uint8_t month)
{
    static constexpr uint8_t kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 0 || month > 12) {
        return 0;
    }
    return month == 2 && leap_year(year) ? 29 : kDays[month - 1];
}

esp_err_t read_registers(uint8_t start, uint8_t *data, size_t length)
{
    return i2c_master_transmit_receive(g_device,
                                       &start,
                                       1,
                                       data,
                                       length,
                                       kI2cTimeoutMs);
}

esp_err_t write_registers(uint8_t start, const uint8_t *data, size_t length)
{
    uint8_t buffer[9] = {};
    ESP_RETURN_ON_FALSE(length <= sizeof(buffer) - 1,
                        ESP_ERR_INVALID_SIZE,
                        kTag,
                        "RTC write is too long");
    buffer[0] = start;
    memcpy(buffer + 1, data, length);
    return i2c_master_transmit(g_device, buffer, length + 1, kI2cTimeoutMs);
}

}  // namespace

bool rtc_datetime_valid(const RtcDateTime &value)
{
    return value.year >= 2000 &&
           value.year <= 2099 &&
           value.month >= 1 &&
           value.month <= 12 &&
           value.day >= 1 &&
           value.day <= days_in_month(value.year, value.month) &&
           value.hour <= 23 &&
           value.minute <= 59 &&
           value.second <= 59;
}

uint8_t rtc_weekday(uint16_t year, uint8_t month, uint8_t day)
{
    static constexpr uint8_t kMonthOffsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    uint16_t adjusted_year = year;
    if (month < 3) {
        --adjusted_year;
    }
    return static_cast<uint8_t>((adjusted_year +
                                 adjusted_year / 4 -
                                 adjusted_year / 100 +
                                 adjusted_year / 400 +
                                 kMonthOffsets[month - 1] +
                                 day) %
                                7);
}

esp_err_t rtc_manager_start(const BoardConfig &board)
{
    g_status = {};
    g_hardware = board.rtc;
    g_status.present = g_hardware.kind == RtcHardwareKind::Pcf85063a;
    if (!g_status.present) {
        g_status.initialization_result = ESP_ERR_NOT_SUPPORTED;
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t err = board_i2c_add_device(g_hardware.i2c_address,
                                          board.i2c.frequency_hz,
                                          &g_device);
    if (err == ESP_OK) {
        uint8_t control = 0;
        err = read_registers(kControl1Register, &control, 1);
    }

    g_status.initialization_result = err;
    g_status.initialized = err == ESP_OK;
    if (err == ESP_OK) {
        ESP_LOGI(kTag, "PCF85063A available at I2C address 0x%02x", g_hardware.i2c_address);
    } else {
        ESP_LOGE(kTag, "failed to initialize RTC: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t rtc_manager_get_status(RtcStatus *status)
{
    if (status == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    *status = g_status;
    return ESP_OK;
}

esp_err_t rtc_manager_read(RtcDateTime *value)
{
    ESP_RETURN_ON_FALSE(value != nullptr, ESP_ERR_INVALID_ARG, kTag, "missing RTC result");
    ESP_RETURN_ON_FALSE(g_status.initialized,
                        g_status.present ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_SUPPORTED,
                        kTag,
                        "RTC is unavailable");

    uint8_t registers[7] = {};
    ESP_RETURN_ON_ERROR(read_registers(kSecondsRegister, registers, sizeof(registers)),
                        kTag,
                        "failed to read RTC");

    RtcDateTime decoded = {};
    decoded.oscillator_stopped = (registers[0] & kSecondsOscillatorStop) != 0;
    uint8_t year = 0;
    if (!from_bcd(registers[0], 0x7f, &decoded.second) ||
        !from_bcd(registers[1], 0x7f, &decoded.minute) ||
        !from_bcd(registers[2], 0x3f, &decoded.hour) ||
        !from_bcd(registers[3], 0x3f, &decoded.day) ||
        !from_bcd(registers[4], 0x07, &decoded.weekday) ||
        !from_bcd(registers[5], 0x1f, &decoded.month) ||
        !from_bcd(registers[6], 0xff, &year)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    decoded.year = static_cast<uint16_t>(2000 + year);
    if (!rtc_datetime_valid(decoded) || decoded.weekday > 6) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *value = decoded;
    return ESP_OK;
}

esp_err_t rtc_manager_set(const RtcDateTime &value)
{
    ESP_RETURN_ON_FALSE(g_status.initialized,
                        g_status.present ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_SUPPORTED,
                        kTag,
                        "RTC is unavailable");
    ESP_RETURN_ON_FALSE(rtc_datetime_valid(value),
                        ESP_ERR_INVALID_ARG,
                        kTag,
                        "invalid RTC date or time");

    uint8_t control = 0;
    ESP_RETURN_ON_ERROR(read_registers(kControl1Register, &control, 1),
                        kTag,
                        "failed to read RTC control");
    control &= static_cast<uint8_t>(~kControl1TwelveHour);
    if (g_hardware.capacitor_12_5_pf) {
        control |= kControl1CapSelect;
    } else {
        control &= static_cast<uint8_t>(~kControl1CapSelect);
    }

    uint8_t stopped = static_cast<uint8_t>(control | kControl1Stop);
    ESP_RETURN_ON_ERROR(write_registers(kControl1Register, &stopped, 1),
                        kTag,
                        "failed to stop RTC");

    const uint8_t registers[7] = {
        to_bcd(value.second),
        to_bcd(value.minute),
        to_bcd(value.hour),
        to_bcd(value.day),
        to_bcd(rtc_weekday(value.year, value.month, value.day)),
        to_bcd(value.month),
        to_bcd(static_cast<uint8_t>(value.year - 2000)),
    };
    esp_err_t err = write_registers(kSecondsRegister, registers, sizeof(registers));
    const esp_err_t restart_err = write_registers(kControl1Register, &control, 1);
    if (err != ESP_OK) {
        return err;
    }
    ESP_RETURN_ON_ERROR(restart_err, kTag, "failed to restart RTC");

    RtcDateTime verify = {};
    ESP_RETURN_ON_ERROR(rtc_manager_read(&verify), kTag, "failed to verify RTC");
    ESP_RETURN_ON_FALSE(!verify.oscillator_stopped &&
                            verify.year == value.year &&
                            verify.month == value.month &&
                            verify.day == value.day &&
                            verify.hour == value.hour &&
                            verify.minute == value.minute,
                        ESP_ERR_INVALID_RESPONSE,
                        kTag,
                        "RTC verification failed");
    return ESP_OK;
}
