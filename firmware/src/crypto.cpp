#include "crypto.h"
#include <esp_random.h>
#include <mbedtls/gcm.h>

static const size_t NONCE = 12, TAG = 16;

bool sealMsg(const uint8_t key[16], const String &topic, const String &plain, std::vector<uint8_t> &out) {
  out.resize(NONCE + plain.length() + TAG);
  esp_fill_random(out.data(), NONCE);
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  bool ok = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128) == 0 &&
            mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, plain.length(), out.data(), NONCE,
                                      (const uint8_t *)topic.c_str(), topic.length(),
                                      (const uint8_t *)plain.c_str(), out.data() + NONCE, TAG,
                                      out.data() + NONCE + plain.length()) == 0;
  mbedtls_gcm_free(&ctx);
  return ok;
}

bool openMsg(const uint8_t key[16], const String &topic, const uint8_t *in, size_t len, String &plain) {
  if (len < NONCE + TAG) return false;
  size_t n = len - NONCE - TAG;
  std::vector<uint8_t> buf(n + 1, 0);
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  bool ok = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128) == 0 &&
            mbedtls_gcm_auth_decrypt(&ctx, n, in, NONCE, (const uint8_t *)topic.c_str(), topic.length(),
                                     in + NONCE + n, TAG, in + NONCE, buf.data()) == 0;
  mbedtls_gcm_free(&ctx);
  if (ok) plain = String((const char *)buf.data());
  return ok;
}
