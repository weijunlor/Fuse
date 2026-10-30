/* Client login state, shared with the headless acceptance/benchmark clients.
Owned by the DDS worker; only immutable login material is prepared by main. */
#ifndef COMMON_APP_CLIENT_H
#define COMMON_APP_CLIENT_H
#include "crypto.h"
#include "fuse.h"

typedef struct
{
    bool b_enabled;
    char str_name[CRYPTO_NAME_MAX + 1];
    uint8_t arr_login_key[CRYPTO_KEY_SIZE];
    uint8_t arr_session_key[CRYPTO_SESSION_SIZE];
    uint64_t u64_session_nonce;
    uint32_t u32_seq;
} AppClient;

bool app_client_init(AppClient *a_client);
bool app_client_request(AppClient *a_client, JoinRequest *a_request);
bool app_client_response(AppClient *a_client, const JoinRequest *a_request,
                          const JoinResponse *a_response);
bool app_client_input(AppClient *a_client, Input *a_input);
bool app_client_leave(AppClient *a_client, Leave *a_leave);
#endif
