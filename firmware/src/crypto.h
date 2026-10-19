// AES-128-GCM sealing for MQTT payloads: nonce(12) | ciphertext | tag(16).
// The MQTT topic is bound in as associated data so a message cannot be
// replayed onto another owner's topic.
#pragma once
#include <Arduino.h>
#include <vector>

bool sealMsg(const uint8_t key[16], const String &topic, const String &plain, std::vector<uint8_t> &out);
bool openMsg(const uint8_t key[16], const String &topic, const uint8_t *in, size_t len, String &plain);
