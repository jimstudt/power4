#!/usr/bin/env python3

"""Validate bounded ESP-NOW integration and scanner/report ownership."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANAGER = (ROOT / "main" / "espnow_manager.cpp").read_text(encoding="utf-8")
PROTOCOL = (ROOT / "main" / "espnow_protocol.hpp").read_text(encoding="utf-8")
SCANNER = (ROOT / "main" / "battery_scanner.cpp").read_text(encoding="utf-8")
CONSOLE = (ROOT / "main" / "console.cpp").read_text(encoding="utf-8")
SDK_DEFAULTS = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")

assert "kEspNowProtocolMaxPayloadBytes = 1470" in PROTOCOL
assert "kEspNowProtocolFrameMaxBytes = 900" in PROTOCOL
assert "kEspNowProtocolReportChunkBytes = 480" in PROTOCOL
assert "kReceiveSlotCount = 4" in MANAGER
assert "kSendCallbackTicks = pdMS_TO_TICKS(250)" in MANAGER
assert "ReceivedFrame g_receive_slots[kReceiveSlotCount]" in MANAGER
assert "xQueueCreateStatic" in MANAGER
assert "xQueueReceive(g_free_queue, &slot, 0)" in MANAGER
assert "xQueueSend(g_ready_queue, &slot, 0)" in MANAGER
assert "O_NONBLOCK" in MANAGER
assert "esp_wifi_set_ps(WIFI_PS_NONE)" in MANAGER
assert "WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR" in MANAGER
assert "esp_now_set_peer_rate_config" in MANAGER
assert "WIFI_PHY_RATE_LORA_500K" in MANAGER
assert "WIFI_PHY_RATE_LORA_250K" in MANAGER
assert 'return "auto"' in MANAGER
assert 'return "lr-500"' in MANAGER
assert 'return "lr-250"' in MANAGER
for setting in (
    "config.static_rx_buf_num = 4",
    "config.dynamic_rx_buf_num = 4",
    "config.dynamic_tx_buf_num = 4",
    "config.rx_mgmt_buf_num = 2",
    "config.mgmt_sbuf_num = 6",
    "config.ampdu_rx_enable = 0",
    "config.ampdu_tx_enable = 0",
    "config.nvs_enable = 0",
    "config.espnow_max_encrypt_num = 0",
):
    assert setting in MANAGER

for setting in (
    "# CONFIG_ESP_WIFI_IRAM_OPT is not set",
    "# CONFIG_ESP_WIFI_RX_IRAM_OPT is not set",
):
    assert setting in SDK_DEFAULTS

probe_position = SCANNER.index("probe_battery(g_candidates[i])")
publish_position = SCANNER.index("espnow_manager_publish_reports()")
assert publish_position > probe_position

for report in ("Batteries", "Banks", "Relays", "Inputs"):
    assert f"print_state_report(StateReportKind::{report})" in CONSOLE

print("bounded ESP-NOW integration: ok")
