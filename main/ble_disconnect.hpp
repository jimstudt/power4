#pragma once

extern "C" {
#include "host/ble_gap.h"
}

// Three bounded attempts to release an established peer connection. Returns
// true only once NimBLE's connection table confirms that the peer is gone.
// The caller must prevent new connections to this peer until cleanup finishes.
bool ble_disconnect_peer(const ble_addr_t &addr);
