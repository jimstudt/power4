#include <assert.h>
#include <stdio.h>

#include "policy_state.hpp"

int main()
{
    const uint8_t source_a[kPolicyStateDigestBytes] = {1};
    const uint8_t source_b[kPolicyStateDigestBytes] = {2};
    PolicyState memory;
    memory.begin_cycle(source_a);
    assert(!memory.get("dcdc_charge", false));
    assert(memory.get("missing", true));
    assert(memory.set("dcdc_charge", true));
    assert(memory.set("dcdc_solar", false));
    memory.finish_cycle(true);
    memory.begin_cycle(source_a);
    assert(memory.get("dcdc_charge", false));
    assert(!memory.get("dcdc_solar", true));
    assert(memory.set("dcdc_charge", false));
    assert(memory.set("dcdc_solar", true));
    memory.finish_cycle(true);
    memory.begin_cycle(source_a);
    assert(!memory.get("dcdc_charge", true));
    assert(memory.get("dcdc_solar", false));

    // Errors discard both preexisting and partial state from the failed cycle.
    assert(memory.set("partial", true));
    memory.finish_cycle(false);
    memory.begin_cycle(source_a);
    assert(!memory.get("partial", false));
    assert(!memory.get("dcdc_solar", false));

    assert(memory.set("dcdc_charge", true));
    memory.finish_cycle(true);
    memory.begin_cycle(source_b);
    assert(!memory.get("dcdc_charge", false));
    assert(memory.set("dcdc_solar", true));
    memory.clear();  // A failed source read, or a new object after reboot.
    memory.begin_cycle(source_b);
    assert(!memory.get("dcdc_solar", false));

    assert(!memory.set("", true));
    assert(!memory.set("invalid.name", true));
    assert(!memory.set("1234567890123456", true));
    assert(!PolicyState::valid_name(nullptr));
    assert(PolicyState::valid_name("A-z_09"));
    assert(memory.set("123456789012345", true));
    for (size_t i = 1; i < kPolicyStateCapacity; ++i) {
        char name[16];
        snprintf(name, sizeof(name), "slot%u", static_cast<unsigned>(i));
        assert(memory.set(name, true));
    }
    assert(!memory.set("overflow", true));
    assert(memory.set("123456789012345", false));
    assert(!memory.get("123456789012345", true));
    memory.clear();
    assert(memory.set("reused", true));
    puts("Policy state lifecycle and bounds: ok");
}
