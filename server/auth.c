/* server/auth.c
Login check, session tokens, input validation, rate limiting. */

#include "auth.h"

#include "crypto.h"
#include "config.h"
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct
{
    char str_name[CRYPTO_NAME_MAX + 1];
    uint8_t arr_key[CRYPTO_KEY_SIZE];
} AuthUser;

typedef struct
{
    bool b_active, b_closing;
    int int_user;
    uint64_t u64_nonce, u64_refill_ns;
    uint32_t u32_last_seq;
    double d_tokens;
    uint8_t arr_key[CRYPTO_SESSION_SIZE];
} AuthSession;

static bool b_enabled;
static AuthUser arr_users[AUTH_MAX_USERS];
static int int_users;
static AuthSession arr_sessions[MAX_PLAYERS];
// Nonces are never evicted within a server run. A full ledger fails closed;
// silently evicting entries would make recorded joins replayable again.
static uint64_t arr_used_nonces[AUTH_MAX_JOINS];
static int int_used_nonces;
static double d_join_tokens;
static uint64_t u64_join_refill_ns;

static bool take_token(double *a_tokens, uint64_t *a_last, uint64_t a_now,
                        double a_rate, double a_burst)
{
    if (a_now > *a_last)
    {
        *a_tokens += (double)(a_now - *a_last) / 1e9 * a_rate;
        if (*a_tokens > a_burst)
            *a_tokens = a_burst;
        *a_last = a_now;
    }
    if (*a_tokens < 1.0)
        return false;
    *a_tokens -= 1.0;
    return true;
}

static int hex_digit(char a_ch)
{
    if (a_ch >= '0' && a_ch <= '9') return a_ch - '0';
    if (a_ch >= 'a' && a_ch <= 'f') return a_ch - 'a' + 10;
    if (a_ch >= 'A' && a_ch <= 'F') return a_ch - 'A' + 10;
    return -1;
}

bool auth_init(void)
{
    crypto_clear(arr_users, sizeof arr_users);
    crypto_clear(arr_sessions, sizeof arr_sessions);
    memset(arr_used_nonces, 0, sizeof arr_used_nonces);
    int_users = int_used_nonces = 0;
    d_join_tokens = 40;
    u64_join_refill_ns = 0;
    b_enabled = crypto_auth_enabled();
    if (!b_enabled)
    {
        fputs("Application authentication OFF (explicit plaintext comparison)\n", stderr);
        return true;
    }
    char arr_path[4096];
    const char *p_path = getenv("FUSE_AUTH_USERS");
    if (p_path == NULL)
    {
        if (!crypto_auth_path("users.db", arr_path, sizeof arr_path))
            return false;
        p_path = arr_path;
    }
    FILE *p_file = fopen(p_path, "rb");
    if (p_file == NULL)
    {
        fputs("Cannot open login database; run python3 security/scripts/accounts.py demo\n", stderr);
        return false;
    }
    struct stat info;
    bool b_ok = fstat(fileno(p_file), &info) == 0 && S_ISREG(info.st_mode) &&
                info.st_uid == getuid() && !(info.st_mode & 0077);
    char arr_line[128] = {0};
    while (b_ok && fgets(arr_line, sizeof arr_line, p_file) != NULL)
    {
        size_t int_length = strlen(arr_line);
        if (int_length > 0 && arr_line[int_length - 1] == '\n') arr_line[--int_length] = '\0';
        if (int_length > 0 && arr_line[int_length - 1] == '\r') arr_line[--int_length] = '\0';
        if (int_length == 0 || arr_line[0] == '#')
            continue;
        char *p_colon = strchr(arr_line, ':');
        b_ok = int_users < AUTH_MAX_USERS && p_colon != NULL;
        if (!b_ok) break;
        *p_colon++ = '\0';
        b_ok = crypto_valid_name(arr_line) && strlen(p_colon) == CRYPTO_KEY_SIZE * 2;
        for (int i = 0; b_ok && i < int_users; i++)
            b_ok = strcmp(arr_line, arr_users[i].str_name) != 0;
        for (int i = 0; b_ok && i < CRYPTO_KEY_SIZE; i++)
        {
            int int_hi = hex_digit(p_colon[i * 2]), int_lo = hex_digit(p_colon[i * 2 + 1]);
            b_ok = int_hi >= 0 && int_lo >= 0;
            if (b_ok)
                arr_users[int_users].arr_key[i] = (uint8_t)((int_hi << 4) | int_lo);
        }
        if (b_ok)
            strcpy(arr_users[int_users++].str_name, arr_line);
    }
    b_ok = b_ok && !ferror(p_file) && int_users > 0;
    fclose(p_file);
    crypto_clear(arr_line, sizeof arr_line);
    if (!b_ok)
    {
        crypto_clear(arr_users, sizeof arr_users);
        int_users = 0;
        fputs("Invalid/private login database required (chmod 600, unique name:64-hex-key lines)\n", stderr);
    }
    return b_ok;
}

bool auth_enabled(void) { return b_enabled; }

void auth_join(const JoinRequest *a_request, int a_free_id, uint64_t a_now_ns,
               JoinResponse *a_response)
{
    memset(a_response, 0, sizeof *a_response);
    a_response->nonce = a_request->nonce;
    a_response->status = JOIN_BAD_LOGIN;
    if (!take_token(&d_join_tokens, &u64_join_refill_ns, a_now_ns, 20, 40))
    {
        a_response->status = JOIN_RETRY;
        return;
    }
    if (!crypto_valid_name(a_request->name) || a_request->nonce == 0)
        return;
    int int_user = -1;
    for (int i = 0; i < int_users; i++)
        if (strcmp(a_request->name, arr_users[i].str_name) == 0)
            int_user = i;
    // Unknown users still run an HMAC and constant-time comparison.
    uint8_t arr_dummy[CRYPTO_KEY_SIZE] = {0}, arr_proof[CRYPTO_KEY_SIZE];
    const uint8_t *p_key = int_user < 0 ? arr_dummy : arr_users[int_user].arr_key;
    bool b_valid = crypto_join_proof(p_key, a_request->nonce, arr_proof) &&
        crypto_equal(arr_proof, a_request->proof, sizeof arr_proof);
    crypto_clear(arr_proof, sizeof arr_proof);
    if (!b_valid || int_user < 0)
        return;
    int int_slot = -1;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (arr_sessions[i].b_active && arr_sessions[i].int_user == int_user)
        {
            if (arr_sessions[i].u64_nonce != a_request->nonce || arr_sessions[i].b_closing)
                return; // One live session per account; cannot evict another login.
            int_slot = i;
        }
    AuthSession new_session = {0};
    bool b_new = int_slot < 0;
    if (b_new)
    {
        for (int i = 0; i < int_used_nonces; i++)
            if (arr_used_nonces[i] == a_request->nonce)
                return;
        if (int_used_nonces == AUTH_MAX_JOINS)
            return;
        if (a_free_id < 1 || a_free_id > MAX_PLAYERS)
        {
            a_response->status = JOIN_FULL;
            return;
        }
        int_slot = a_free_id - 1;
        if (arr_sessions[int_slot].b_active)
            return;
        if (!crypto_random(new_session.arr_key, sizeof new_session.arr_key))
            return;
        new_session.b_active = true;
        new_session.int_user = int_user;
        new_session.u64_nonce = a_request->nonce;
        new_session.u64_refill_ns = a_now_ns;
        new_session.d_tokens = AUTH_INPUT_BURST;
    }
    const AuthSession *p_selected = b_new ? &new_session : &arr_sessions[int_slot];
    if (!crypto_wrap_session(p_key, a_request->nonce, p_selected->arr_key,
                              a_response->session_key))
    {
        crypto_clear(&new_session, sizeof new_session);
        return;
    }
    // Publish the session only after every crypto operation succeeded.
    if (b_new)
    {
        arr_sessions[int_slot] = new_session;
        arr_used_nonces[int_used_nonces++] = a_request->nonce;
    }
    crypto_clear(&new_session, sizeof new_session);
    a_response->status = JOIN_OK;
    a_response->player_id = (uint8_t)(int_slot + 1);
}

static AuthSession *valid_message(uint8_t a_id, uint32_t a_seq, uint8_t a_keys,
                                   bool a_leave, const uint8_t *a_mac)
{
    if (a_id < 1 || a_id > MAX_PLAYERS || a_seq == 0)
        return NULL;
    AuthSession *p_session = &arr_sessions[a_id - 1];
    if (!p_session->b_active || p_session->b_closing || a_seq <= p_session->u32_last_seq)
        return NULL;
    uint8_t arr_mac[CRYPTO_MAC_SIZE];
    bool b_ok = crypto_message_mac(p_session->arr_key, a_leave, a_id, a_seq, a_keys, arr_mac) &&
        crypto_equal(arr_mac, a_mac, sizeof arr_mac);
    crypto_clear(arr_mac, sizeof arr_mac);
    return b_ok ? p_session : NULL;
}

bool auth_input(const Input *a_input, uint64_t a_now_ns)
{
    if (a_input->keys & ~(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_DASH))
        return false;
    if (!b_enabled)
        return true;
    AuthSession *p_session = valid_message(a_input->player_id, a_input->seq,
                                            a_input->keys, false, a_input->mac);
    if (p_session == NULL || !take_token(&p_session->d_tokens, &p_session->u64_refill_ns,
                                         a_now_ns, AUTH_INPUT_RATE, AUTH_INPUT_BURST))
        return false;
    // Failed MACs, replays and throttled messages never move the watermark.
    p_session->u32_last_seq = a_input->seq;
    return true;
}

bool auth_leave(const Leave *a_leave)
{
    if (!b_enabled)
        return true;
    AuthSession *p_session = valid_message(a_leave->player_id, a_leave->seq, 0, true, a_leave->mac);
    if (p_session == NULL)
        return false;
    p_session->u32_last_seq = a_leave->seq;
    p_session->b_closing = true; // Reject later input while removal is queued.
    return true;
}

void auth_forget(int a_player_id)
{
    if (a_player_id >= 1 && a_player_id <= MAX_PLAYERS)
        crypto_clear(&arr_sessions[a_player_id - 1], sizeof arr_sessions[0]);
}
