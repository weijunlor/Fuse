/* common/crypto.h
HMAC, session token generation and password hashing. */

#ifndef COMMON_CRYPTO_H
#define COMMON_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CRYPTO_NAME_MAX 15
#define CRYPTO_PASSWORD_MAX 256
#define CRYPTO_KEY_SIZE 32
#define CRYPTO_SESSION_SIZE 16
#define CRYPTO_MAC_SIZE 16
#define CRYPTO_PBKDF2_ROUNDS 600000

// Fail closed on invalid configuration. off is only for plaintext comparisons.
bool crypto_auth_enabled(void);
bool crypto_valid_name(const char *a_name);
bool crypto_auth_path(const char *a_file, char *a_out, size_t a_capacity);
bool crypto_read_password(const char *a_path, char a_out[CRYPTO_PASSWORD_MAX + 1]);
bool crypto_login_key(const char *a_name, const char *a_password,
                      uint8_t a_out[CRYPTO_KEY_SIZE]);
bool crypto_random(void *a_out, size_t a_size);
bool crypto_hmac(const void *a_key, size_t a_key_size, const void *a_data,
                 size_t a_data_size, uint8_t a_out[CRYPTO_KEY_SIZE]);
bool crypto_equal(const void *a_left, const void *a_right, size_t a_size);
void crypto_clear(void *a_data, size_t a_size);

// Integers are explicitly encoded big-endian: never MAC a padded C struct.
bool crypto_join_proof(const uint8_t a_key[CRYPTO_KEY_SIZE], uint64_t a_nonce,
                       uint8_t a_out[CRYPTO_KEY_SIZE]);
// Symmetric XOR wrapping, using the first 16 bytes of HMAC(key, "session" || nonce).
bool crypto_wrap_session(const uint8_t a_key[CRYPTO_KEY_SIZE], uint64_t a_nonce,
                         const uint8_t a_in[CRYPTO_SESSION_SIZE],
                         uint8_t a_out[CRYPTO_SESSION_SIZE]);
// Domain separation prevents an Input MAC from being used as a Leave MAC.
bool crypto_message_mac(const uint8_t a_key[CRYPTO_SESSION_SIZE], bool a_leave,
                        uint8_t a_player_id, uint32_t a_seq, uint8_t a_keys,
                        uint8_t a_out[CRYPTO_MAC_SIZE]);

#endif /* COMMON_CRYPTO_H */
