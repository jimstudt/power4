#include "policy_state.hpp"

#include <string.h>

bool PolicyState::valid_name(const char *name)
{
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i <= kPolicyStateNameMax; ++i) {
        const char c = name[i];
        if (c == '\0') {
            return true;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return false;
}

void PolicyState::clear()
{
    count_ = 0;
    have_digest_ = false;
}

void PolicyState::begin_cycle(const uint8_t digest[kPolicyStateDigestBytes])
{
    if (!have_digest_ || memcmp(digest_, digest, sizeof(digest_)) != 0) {
        clear();
        memcpy(digest_, digest, sizeof(digest_));
        have_digest_ = true;
    }
}

void PolicyState::finish_cycle(bool success)
{
    if (!success) {
        clear();
    }
}

bool PolicyState::get(const char *name, bool default_value) const
{
    if (!valid_name(name)) {
        return default_value;
    }
    for (size_t i = 0; i < count_; ++i) {
        if (strcmp(entries_[i].name, name) == 0) {
            return entries_[i].value;
        }
    }
    return default_value;
}

bool PolicyState::set(const char *name, bool value)
{
    if (!valid_name(name)) {
        return false;
    }
    for (size_t i = 0; i < count_; ++i) {
        if (strcmp(entries_[i].name, name) == 0) {
            entries_[i].value = value;
            return true;
        }
    }
    if (count_ == kPolicyStateCapacity) {
        return false;
    }
    strcpy(entries_[count_].name, name);
    entries_[count_].value = value;
    ++count_;
    return true;
}
