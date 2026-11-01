/* common/crypto.c
HMAC, session token generation and password hashing. */

#include "crypto.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

bool crypto_auth_enabled(void)
{
    const char *p_mode = getenv("FUSE_AUTH");
    if (p_mode == NULL || strcmp(p_mode, "on") == 0)
        return true;
    const char *p_dds = getenv("FUSE_DDS_SECURITY");
    if (strcmp(p_mode, "off") == 0 && p_dds != NULL && strcmp(p_dds, "off") == 0)
        return false;
    fputs("FUSE_AUTH must be on; off requires explicit FUSE_DDS_SECURITY=off comparison mode\n", stderr);
    exit(EXIT_FAILURE);
}

bool crypto_valid_name(const char *a_name)
{
    if (a_name == NULL)
        return false;
    size_t int_length = strnlen(a_name, CRYPTO_NAME_MAX + 1);
    if (int_length == 0 || int_length > CRYPTO_NAME_MAX)
        return false;
    for (size_t i = 0; i < int_length; i++)
    {
        unsigned char ch = (unsigned char)a_name[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
            return false;
    }
    return true;
}

bool crypto_auth_path(const char *a_file, char *a_out, size_t a_capacity)
{
    const char *p_dir = getenv("FUSE_AUTH_DIR");
    const char *p_home = getenv("HOME");
    int int_length;
    if (p_dir != NULL && p_dir[0] != '\0')
        int_length = snprintf(a_out, a_capacity, "%s/%s", p_dir, a_file);
    else if (p_home != NULL && p_home[0] != '\0')
        int_length = snprintf(a_out, a_capacity, "%s/.local/share/fuse/auth/%s", p_home, a_file);
    else
        return false;
    return int_length >= 0 && (size_t)int_length < a_capacity;
}

bool crypto_read_password(const char *a_path, char a_out[CRYPTO_PASSWORD_MAX + 1])
{
    FILE *p_file = fopen(a_path, "rb");
    if (p_file == NULL)
        return false;
    struct stat info;
    unsigned char arr_bytes[CRYPTO_PASSWORD_MAX + 3];
    size_t int_length = fread(arr_bytes, 1, sizeof arr_bytes, p_file);
    bool b_ok = !ferror(p_file) && fstat(fileno(p_file), &info) == 0 &&
                S_ISREG(info.st_mode) && info.st_uid == getuid() && !(info.st_mode & 0077) &&
                int_length < sizeof arr_bytes;
    fclose(p_file);
    if (int_length > 0 && arr_bytes[int_length - 1] == '\n')
        int_length--;
    if (int_length > 0 && arr_bytes[int_length - 1] == '\r')
        int_length--;
    b_ok = b_ok && int_length > 0 && int_length <= CRYPTO_PASSWORD_MAX;
    for (size_t i = 0; i < int_length; i++)
        b_ok = b_ok && arr_bytes[i] != '\0' && arr_bytes[i] != '\r' && arr_bytes[i] != '\n';
    if (b_ok)
    {
        memcpy(a_out, arr_bytes, int_length);
        a_out[int_length] = '\0';
    }
    else
        crypto_clear(a_out, CRYPTO_PASSWORD_MAX + 1);
    crypto_clear(arr_bytes, sizeof arr_bytes);
    return b_ok;
}

bool crypto_login_key(const char *a_name, const char *a_password, uint8_t a_out[CRYPTO_KEY_SIZE])
{
    size_t int_length = a_password ? strnlen(a_password, CRYPTO_PASSWORD_MAX + 1) : 0;
    if (!crypto_valid_name(a_name) || int_length == 0 || int_length > CRYPTO_PASSWORD_MAX)
        return false;
    // v1 IDL has no salt field: the unique account name is its public salt.
    return PKCS5_PBKDF2_HMAC(a_password, (int)int_length,
        (const unsigned char *)a_name, (int)strlen(a_name), CRYPTO_PBKDF2_ROUNDS,
        EVP_sha256(), CRYPTO_KEY_SIZE, a_out) == 1;
}

bool crypto_random(void *a_out, size_t a_size)
{
    return a_size <= INT_MAX && RAND_bytes(a_out, (int)a_size) == 1;
}

bool crypto_hmac(const void *a_key, size_t a_key_size, const void *a_data,
                 size_t a_data_size, uint8_t a_out[CRYPTO_KEY_SIZE])
{
    unsigned int int_length = 0;
    return a_key_size <= INT_MAX &&
        HMAC(EVP_sha256(), a_key, (int)a_key_size, a_data, a_data_size, a_out, &int_length) != NULL &&
        int_length == CRYPTO_KEY_SIZE;
}

bool crypto_equal(const void *a_left, const void *a_right, size_t a_size)
{
    return CRYPTO_memcmp(a_left, a_right, a_size) == 0;
}

void crypto_clear(void *a_data, size_t a_size)
{
    OPENSSL_cleanse(a_data, a_size);
}

static void put_u64(uint8_t *a_out, uint64_t a_value)
{
    for (int i = 7; i >= 0; i--)
    {
        a_out[i] = (uint8_t)a_value;
        a_value >>= 8;
    }
}

bool crypto_join_proof(const uint8_t a_key[CRYPTO_KEY_SIZE], uint64_t a_nonce,
                       uint8_t a_out[CRYPTO_KEY_SIZE])
{
    uint8_t arr_nonce[8];
    put_u64(arr_nonce, a_nonce);
    return a_nonce != 0 && crypto_hmac(a_key, CRYPTO_KEY_SIZE, arr_nonce, sizeof arr_nonce, a_out);
}

bool crypto_wrap_session(const uint8_t a_key[CRYPTO_KEY_SIZE], uint64_t a_nonce,
                         const uint8_t a_in[CRYPTO_SESSION_SIZE], uint8_t a_out[CRYPTO_SESSION_SIZE])
{
    uint8_t arr_message[15] = "session";
    uint8_t arr_mask[CRYPTO_KEY_SIZE];
    put_u64(arr_message + 7, a_nonce);
    bool b_ok = crypto_hmac(a_key, CRYPTO_KEY_SIZE, arr_message, sizeof arr_message, arr_mask);
    if (b_ok)
        for (int i = 0; i < CRYPTO_SESSION_SIZE; i++)
            a_out[i] = a_in[i] ^ arr_mask[i];
    crypto_clear(arr_mask, sizeof arr_mask);
    return b_ok;
}

bool crypto_message_mac(const uint8_t a_key[CRYPTO_SESSION_SIZE], bool a_leave,
                        uint8_t a_player_id, uint32_t a_seq, uint8_t a_keys,
                        uint8_t a_out[CRYPTO_MAC_SIZE])
{
    // L/I + player ID + sequence (network order), then keys for Input only.
    uint8_t arr_message[7] = {a_leave ? 'L' : 'I', a_player_id,
        (uint8_t)(a_seq >> 24), (uint8_t)(a_seq >> 16), (uint8_t)(a_seq >> 8),
        (uint8_t)a_seq, a_keys};
    uint8_t arr_mac[CRYPTO_KEY_SIZE];
    bool b_ok = crypto_hmac(a_key, CRYPTO_SESSION_SIZE, arr_message,
                            a_leave ? 6 : 7, arr_mac);
    if (b_ok)
        memcpy(a_out, arr_mac, CRYPTO_MAC_SIZE);
    crypto_clear(arr_mac, sizeof arr_mac);
    return b_ok;
}
