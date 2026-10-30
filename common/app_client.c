#include "app_client.h"
#include "net.h"
#include "types.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool app_client_init(AppClient *a_client)
{
    memset(a_client, 0, sizeof *a_client);
    a_client->b_enabled = crypto_auth_enabled();
    if (!a_client->b_enabled)
        return true;
    const char *p_name = getenv("FUSE_USERNAME");
    if (p_name == NULL)
        p_name = getenv("FUSE_DDS_IDENTITY");
    if (!crypto_valid_name(p_name))
    {
        fputs("Set FUSE_USERNAME (1-15 letters, digits, _ or -)\n", stderr);
        return false;
    }
    strcpy(a_client->str_name, p_name);
    char arr_file[32], arr_path[4096], arr_password[CRYPTO_PASSWORD_MAX + 1] = {0};
    snprintf(arr_file, sizeof arr_file, "%s.pass", p_name);
    const char *p_path = getenv("FUSE_PASSWORD_FILE");
    if (p_path == NULL)
    {
        if (!crypto_auth_path(arr_file, arr_path, sizeof arr_path))
            return false;
        p_path = arr_path;
    }
    bool b_ok = crypto_read_password(p_path, arr_password) &&
        crypto_login_key(p_name, arr_password, a_client->arr_login_key);
    crypto_clear(arr_password, sizeof arr_password);
    if (!b_ok)
        fputs("Cannot load login password: use a nonempty private file (chmod 600), or run accounts.py demo\n", stderr);
    return b_ok;
}

bool app_client_request(AppClient *a_client, JoinRequest *a_request)
{
    memset(a_request, 0, sizeof *a_request);
    if (!a_client->b_enabled)
    {
        if (net_join_nonce())
            a_request->nonce = net_random_nonce();
        return true;
    }
    do
    {
        if (!crypto_random(&a_request->nonce, sizeof a_request->nonce))
            return false;
    } while (a_request->nonce == 0);
    strcpy(a_request->name, a_client->str_name);
    return crypto_join_proof(a_client->arr_login_key, a_request->nonce, a_request->proof);
}

bool app_client_response(AppClient *a_client, const JoinRequest *a_request,
                          const JoinResponse *a_response)
{
    if (a_response->status != JOIN_OK || a_response->player_id < 1 ||
        a_response->player_id > MAX_PLAYERS ||
        ((a_client->b_enabled || net_join_nonce()) && a_response->nonce != a_request->nonce))
        return false;
    if (!a_client->b_enabled)
    {
        a_client->u32_seq = 0;
        return true;
    }
    uint8_t arr_session[CRYPTO_SESSION_SIZE];
    bool b_ok = crypto_wrap_session(a_client->arr_login_key,
        a_request->nonce, a_response->session_key, arr_session);
    if (b_ok && a_client->u64_session_nonce == a_request->nonce)
        b_ok = crypto_equal(arr_session, a_client->arr_session_key, sizeof arr_session);
    else if (b_ok)
    {
        memcpy(a_client->arr_session_key, arr_session, sizeof arr_session);
        a_client->u64_session_nonce = a_request->nonce;
        a_client->u32_seq = 0;
    }
    crypto_clear(arr_session, sizeof arr_session);
    return b_ok;
}

bool app_client_input(AppClient *a_client, Input *a_input)
{
    if (a_client->u32_seq == UINT32_MAX)
        return false; // Never wrap into the replay window: reconnect instead.
    a_input->seq = ++a_client->u32_seq;
    return !a_client->b_enabled || crypto_message_mac(a_client->arr_session_key,
        false, a_input->player_id, a_input->seq, a_input->keys, a_input->mac);
}

bool app_client_leave(AppClient *a_client, Leave *a_leave)
{
    if (a_client->u32_seq == UINT32_MAX)
        return false;
    a_leave->seq = ++a_client->u32_seq;
    return !a_client->b_enabled || crypto_message_mac(a_client->arr_session_key,
        true, a_leave->player_id, a_leave->seq, 0, a_leave->mac);
}
