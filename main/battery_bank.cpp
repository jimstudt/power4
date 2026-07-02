#include "battery_bank.hpp"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

namespace {

constexpr const char *kTag = "battery_bank";
constexpr const char *kNamespace = "config";
constexpr const char *kBanksKey = "banks";
constexpr uint32_t kBanksMagic = 0x5034424b;  // P4BK
constexpr uint16_t kBanksVersion = 1;

struct StoredBatteryBank {
    char name[kBatteryBankNameMax + 1];
    uint8_t battery_count;
    char batteries[CONFIG_POWER4_MAX_BATTERIES][kBatteryNameMax + 1];
};

struct StoredBatteryBanks {
    uint32_t magic;
    uint16_t version;
    uint16_t bank_count;
    StoredBatteryBank banks[kBatteryBankMaxBanks];
};

bool valid_printable_name(const char *name, size_t max_length)
{
    if (name == nullptr || name[0] == '\0') {
        return false;
    }

    size_t length = 0;
    for (; name[length] != '\0'; ++length) {
        const unsigned char ch = static_cast<unsigned char>(name[length]);
        if (length >= max_length || ch <= ' ' || ch > '~') {
            return false;
        }
    }

    return length > 0;
}

esp_err_t open_config(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    return nvs_open(kNamespace, mode, handle);
}

void init_empty(StoredBatteryBanks *banks)
{
    memset(banks, 0, sizeof(*banks));
    banks->magic = kBanksMagic;
    banks->version = kBanksVersion;
}

esp_err_t load_banks(StoredBatteryBanks *banks)
{
    if (banks == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    init_empty(banks);

    nvs_handle_t handle = 0;
    esp_err_t err = open_config(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t length = sizeof(*banks);
    err = nvs_get_blob(handle, kBanksKey, banks, &length);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        init_empty(banks);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (length != sizeof(*banks) || banks->magic != kBanksMagic || banks->version != kBanksVersion ||
        banks->bank_count > kBatteryBankMaxBanks) {
        ESP_LOGW(kTag, "stored battery bank blob is invalid; ignoring it");
        init_empty(banks);
        return ESP_OK;
    }

    return ESP_OK;
}

esp_err_t save_banks(const StoredBatteryBanks &banks)
{
    nvs_handle_t handle = 0;
    esp_err_t err = open_config(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle, kBanksKey, &banks, sizeof(banks));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

int find_bank(const StoredBatteryBanks &banks, const char *name)
{
    for (size_t i = 0; i < banks.bank_count; ++i) {
        if (strcmp(banks.banks[i].name, name) == 0) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

esp_err_t validate_bank_definition(const char *name,
                                   const char *const *battery_names,
                                   size_t battery_count)
{
    if (!battery_bank_valid_name(name) || battery_names == nullptr || battery_count == 0 ||
        battery_count > CONFIG_POWER4_MAX_BATTERIES) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < battery_count; ++i) {
        if (!battery_store_valid_name(battery_names[i])) {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t j = i + 1; j < battery_count; ++j) {
            if (strcmp(battery_names[i], battery_names[j]) == 0) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    return ESP_OK;
}

StoredBatteryBanks *alloc_banks(void)
{
    StoredBatteryBanks *banks = static_cast<StoredBatteryBanks *>(malloc(sizeof(StoredBatteryBanks)));
    if (banks != nullptr) {
        init_empty(banks);
    }
    return banks;
}

}  // namespace

esp_err_t battery_bank_init(void)
{
    StoredBatteryBanks *banks = alloc_banks();
    if (banks == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t err = load_banks(banks);
    if (err != ESP_OK) {
        free(banks);
        return err;
    }

    ESP_LOGI(kTag, "loaded %u battery banks", static_cast<unsigned>(banks->bank_count));
    free(banks);
    return ESP_OK;
}

bool battery_bank_valid_name(const char *name)
{
    return valid_printable_name(name, kBatteryBankNameMax);
}

esp_err_t battery_bank_create(const char *name, const char *const *battery_names, size_t battery_count)
{
    esp_err_t err = validate_bank_definition(name, battery_names, battery_count);
    if (err != ESP_OK) {
        return err;
    }

    StoredBatteryBanks *banks = alloc_banks();
    if (banks == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    err = load_banks(banks);
    if (err != ESP_OK) {
        free(banks);
        return err;
    }

    int index = find_bank(*banks, name);
    if (index < 0) {
        if (banks->bank_count >= kBatteryBankMaxBanks) {
            free(banks);
            return ESP_ERR_NO_MEM;
        }
        index = banks->bank_count;
        ++banks->bank_count;
    }

    StoredBatteryBank &bank = banks->banks[index];
    memset(&bank, 0, sizeof(bank));
    strlcpy(bank.name, name, sizeof(bank.name));
    bank.battery_count = static_cast<uint8_t>(battery_count);
    for (size_t i = 0; i < battery_count; ++i) {
        strlcpy(bank.batteries[i], battery_names[i], sizeof(bank.batteries[i]));
    }

    err = save_banks(*banks);
    free(banks);
    return err;
}

esp_err_t battery_bank_remove(const char *name)
{
    if (!battery_bank_valid_name(name)) {
        return ESP_ERR_INVALID_ARG;
    }

    StoredBatteryBanks *banks = alloc_banks();
    if (banks == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = load_banks(banks);
    if (err != ESP_OK) {
        free(banks);
        return err;
    }

    const int index = find_bank(*banks, name);
    if (index < 0) {
        free(banks);
        return ESP_ERR_NOT_FOUND;
    }

    for (size_t i = static_cast<size_t>(index) + 1; i < banks->bank_count; ++i) {
        banks->banks[i - 1] = banks->banks[i];
    }
    --banks->bank_count;
    memset(&banks->banks[banks->bank_count], 0, sizeof(banks->banks[banks->bank_count]));

    err = save_banks(*banks);
    free(banks);
    return err;
}

esp_err_t battery_bank_list(BatteryBankList *list)
{
    if (list == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *list = BatteryBankList {};

    StoredBatteryBanks *stored = alloc_banks();
    if (stored == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t err = load_banks(stored);
    if (err != ESP_OK) {
        free(stored);
        return err;
    }

    list->count = stored->bank_count;
    for (size_t i = 0; i < stored->bank_count; ++i) {
        BatteryBankDefinition &bank = list->banks[i];
        strlcpy(bank.name, stored->banks[i].name, sizeof(bank.name));
        bank.battery_count = stored->banks[i].battery_count;
        for (size_t j = 0; j < bank.battery_count; ++j) {
            strlcpy(bank.batteries[j], stored->banks[i].batteries[j], sizeof(bank.batteries[j]));
        }
    }

    free(stored);
    return ESP_OK;
}

esp_err_t battery_bank_get_state(const char *name, BatteryBankState *state)
{
    if (!battery_bank_valid_name(name) || state == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *state = BatteryBankState {};

    StoredBatteryBanks *banks = alloc_banks();
    if (banks == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = load_banks(banks);
    if (err != ESP_OK) {
        free(banks);
        return err;
    }

    const int index = find_bank(*banks, name);
    if (index < 0) {
        free(banks);
        return ESP_ERR_NOT_FOUND;
    }

    const StoredBatteryBank &bank = banks->banks[index];
    BatteryBankState computed = {
        .ready = true,
        .voltage_v = 0.0f,
        .current_a = 0.0f,
        .soc_percent = 100.0f,
    };

    for (size_t i = 0; i < bank.battery_count; ++i) {
        BatteryRecord record = {};
        err = battery_store_get(bank.batteries[i], &record);
        if (err == ESP_ERR_NOT_FOUND) {
            state->ready = false;
            free(banks);
            return ESP_OK;
        }
        if (err != ESP_OK) {
            free(banks);
            return err;
        }

        computed.voltage_v += record.voltage_v;
        if (i == 0 || record.current_a > computed.current_a) {
            computed.current_a = record.current_a;
        }
        if (record.soc_percent < computed.soc_percent) {
            computed.soc_percent = record.soc_percent;
        }
    }

    *state = computed;
    free(banks);
    return ESP_OK;
}
