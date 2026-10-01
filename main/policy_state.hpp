#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr size_t kPolicyStateCapacity = 8;
constexpr size_t kPolicyStateNameMax = 15;
constexpr size_t kPolicyStateDigestBytes = 20;

// Volatile boolean memory owned exclusively by the policy task. Successful
// cycles of the same program share it; a changed program or failed cycle clears
// it. Fixed storage, no allocation and no NVS writes.
class PolicyState {
public:
    static bool valid_name(const char *name);
    void begin_cycle(const uint8_t digest[kPolicyStateDigestBytes]);
    void finish_cycle(bool success);
    void clear();
    bool get(const char *name, bool default_value) const;
    bool set(const char *name, bool value);

private:
    struct Entry {
        char name[kPolicyStateNameMax + 1] = {};
        bool value = false;
    };
    Entry entries_[kPolicyStateCapacity] = {};
    size_t count_ = 0;
    uint8_t digest_[kPolicyStateDigestBytes] = {};
    bool have_digest_ = false;
};
