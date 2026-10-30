/* Deterministic security regressions. No DDS processes or graphics needed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "auth.h"
#include "app_client.h"
#include "game.h"
#include "lobby.h"
#include "timing.h"

static int int_checks;
#define CHECK(condition) do { int_checks++; if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); } } while (0)

static void hex(const uint8_t *a_in, size_t a_size, char *a_out)
{
    for (size_t i = 0; i < a_size; i++) sprintf(a_out + i * 2, "%02x", a_in[i]);
}

static JoinResponse join_request(const JoinRequest *a_request)
{
    JoinResponse response;
    CHECK(lobby_signal_join(a_request));
    lobby_poll();
    CHECK(lobby_take_result(&response));
    return response;
}

int main(void)
{
    unsetenv("FUSE_AUTH");
    uint8_t arr_key[32], arr_result[32], arr_session[16];
    char arr_hex[65];
    // Independently generated with Python hashlib/hmac; RFC 4231 HMAC case 1.
    CHECK(crypto_login_key("client-1", "test-password", arr_key));
    hex(arr_key, 32, arr_hex);
    CHECK(strcmp(arr_hex, "bece53837711ae09e8e15fcb2ee13a7d4a38cdd2bf69eabbd0412dc7ea4b8c41") == 0);
    uint8_t arr_hmac_key[20]; memset(arr_hmac_key, 0x0b, sizeof arr_hmac_key);
    CHECK(crypto_hmac(arr_hmac_key, sizeof arr_hmac_key, "Hi There", 8, arr_result));
    hex(arr_result, 32, arr_hex);
    CHECK(strcmp(arr_hex, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7") == 0);
    for (int i = 0; i < 16; i++) arr_session[i] = (uint8_t)i;
    CHECK(crypto_message_mac(arr_session, false, 1, 0x01020304, KEY_RIGHT, arr_result));
    hex(arr_result, 16, arr_hex);
    CHECK(strcmp(arr_hex, "6698616bb8809c1abce348451943a12e") == 0);
    CHECK(!crypto_valid_name("../bad"));
    CHECK(!crypto_valid_name("sixteen-letters!"));
    CHECK(!crypto_valid_name(""));
    CHECK(!crypto_login_key("client-1", "", arr_result));

    char arr_path[] = "/tmp/fuse-auth-unit-XXXXXX";
    int int_fd = mkstemp(arr_path); CHECK(int_fd >= 0);
    FILE *p_file = fdopen(int_fd, "w"); CHECK(p_file != NULL);
    hex(arr_key, 32, arr_hex);
    fprintf(p_file, "client-1:%s\nclient-2:%064d\n", arr_hex, 0);
    CHECK(fclose(p_file) == 0);
    setenv("FUSE_AUTH_USERS", arr_path, 1);
    CHECK(auth_init());
    CHECK(auth_enabled());
    AppClient client = {.b_enabled = true};
    strcpy(client.str_name, "client-1"); memcpy(client.arr_login_key, arr_key, 32);
    JoinRequest request;
    CHECK(app_client_request(&client, &request));
    JoinRequest bad = request; bad.proof[0] ^= 1;
    JoinResponse response = join_request(&bad);
    CHECK(response.status == JOIN_BAD_LOGIN && !arr_Players[0].b_active);
    bad = request; memset(bad.name, 'a', sizeof bad.name);
    response = join_request(&bad); CHECK(response.status == JOIN_BAD_LOGIN);
    bad = request; strcpy(bad.name, "unknown");
    response = join_request(&bad); CHECK(response.status == JOIN_BAD_LOGIN);
    bad = request; bad.nonce = 0;
    response = join_request(&bad); CHECK(response.status == JOIN_BAD_LOGIN);
    response = join_request(&request);
    CHECK(response.status == JOIN_OK && response.player_id == 1 && arr_Players[0].b_active);
    CHECK(app_client_response(&client, &request, &response));
    CHECK(!crypto_equal(response.session_key, client.arr_session_key, 16));
    JoinResponse retry = join_request(&request);
    CHECK(retry.player_id == 1 && crypto_equal(retry.session_key, response.session_key, 16));
    bad = request; bad.nonce++;
    CHECK(crypto_join_proof(arr_key, bad.nonce, bad.proof));
    CHECK(join_request(&bad).status == JOIN_BAD_LOGIN); // cannot replace an active account

    Input input = {.player_id = 1, .keys = KEY_RIGHT};
    CHECK(app_client_input(&client, &input));
    uint64_t u64_before = game_last_input_ns(0);
    Input forged = input; forged.seq = UINT32_MAX;
    CHECK(!lobby_receive_input(&forged));
    CHECK(game_last_input_ns(0) == u64_before);
    forged = input; forged.player_id = 2;
    CHECK(!lobby_receive_input(&forged));
    forged = input; forged.player_id = 0; CHECK(!lobby_receive_input(&forged));
    forged.player_id = 255; CHECK(!lobby_receive_input(&forged));
    CHECK(lobby_receive_input(&input));
    CHECK(!lobby_receive_input(&input)); // exact replay
    CHECK(app_client_input(&client, &input));
    CHECK(lobby_receive_input(&input)); // invalid high seq did not poison watermark
    forged = input; forged.keys = 0x80;
    CHECK(app_client_input(&client, &forged));
    CHECK(!lobby_receive_input(&forged)); // valid MAC, unsupported keys
    retry = join_request(&request); CHECK(retry.player_id == 1);
    uint32_t u32_before_retry = client.u32_seq;
    CHECK(app_client_response(&client, &request, &retry));
    CHECK(client.u32_seq == u32_before_retry); // duplicate response cannot reset client seq either
    CHECK(!lobby_receive_input(&input)); // retry did not reset replay watermark

    // Input and Leave share the sequence, but have distinct MAC domains.
    Leave leave = {.player_id = 1, .seq = input.seq};
    memcpy(leave.mac, input.mac, 16); CHECK(!lobby_receive_leave(&leave));
    CHECK(app_client_leave(&client, &leave));
    leave.mac[0] ^= 1; CHECK(!lobby_receive_leave(&leave)); leave.mac[0] ^= 1;
    CHECK(lobby_receive_leave(&leave));
    CHECK(app_client_input(&client, &input)); CHECK(!lobby_receive_input(&input));
    CHECK(!lobby_receive_leave(&leave));
    lobby_poll(); CHECK(!arr_Players[0].b_active);
    CHECK(join_request(&request).status == JOIN_BAD_LOGIN); // logged-out nonce cannot revive
    uint8_t arr_old_session[16]; memcpy(arr_old_session, client.arr_session_key, 16);
    CHECK(app_client_request(&client, &request)); response = join_request(&request);
    CHECK(response.status == JOIN_OK && response.player_id == 1);
    CHECK(app_client_response(&client, &request, &response));
    CHECK(!crypto_equal(arr_old_session, client.arr_session_key, 16));
    CHECK(!lobby_receive_leave(&leave)); CHECK(!lobby_receive_input(&input));

    // A real second account cannot MAC inputs for the first player's slot.
    AppClient second = {.b_enabled = true}; strcpy(second.str_name, "client-2");
    JoinRequest second_request; CHECK(app_client_request(&second, &second_request));
    response = join_request(&second_request); CHECK(response.status == JOIN_OK && response.player_id == 2);
    CHECK(app_client_response(&second, &second_request, &response));
    forged = (Input){.player_id = 1, .keys = KEY_LEFT};
    CHECK(app_client_input(&second, &forged)); CHECK(!lobby_receive_input(&forged));

    // Deterministic token bucket checks with explicit times (no flaky sleeps).
    const uint64_t u64_now = timing_now_ns();
    int int_accepted = 0;
    for (int i = 0; i < AUTH_INPUT_BURST + 10; i++)
    {
        input.player_id = 1; CHECK(app_client_input(&client, &input));
        if (auth_input(&input, u64_now)) int_accepted++;
    }
    CHECK(int_accepted == AUTH_INPUT_BURST);
    CHECK(app_client_input(&client, &input)); CHECK(auth_input(&input, u64_now + 1000000000ull));
    CHECK(app_client_leave(&client, &leave)); CHECK(auth_leave(&leave));
    auth_forget(1); // simulate the slot being removed; the lobby timeout tests below exercise it
    // Timeout forgets real session state, as well as the slot and held keys.
    int_player_timeout_s = 1;
    lobby_check_timeouts(timing_now_ns() + 2000000000ull);
    CHECK(!arr_Players[0].b_active && !arr_Players[1].b_active);
    CHECK(!lobby_receive_input(&forged));
    CHECK(join_request(&second_request).status == JOIN_BAD_LOGIN);

    // Full games do not consume a nonce; the same authenticated request can
    // succeed when a slot opens. Out-of-order seq and sequence zero are rejected.
    CHECK(auth_init());
    CHECK(app_client_request(&client, &request));
    uint64_t u64_test_now = 1000000000ull;
    auth_join(&request, 0, u64_test_now, &response); CHECK(response.status == JOIN_FULL);
    auth_join(&request, 1, u64_test_now, &response); CHECK(response.status == JOIN_OK);
    CHECK(app_client_response(&client, &request, &response));
    input = (Input){.player_id = 1, .seq = 0};
    CHECK(crypto_message_mac(client.arr_session_key, false, 1, 0, 0, input.mac));
    CHECK(!auth_input(&input, u64_test_now));
    CHECK(app_client_input(&client, &input)); CHECK(auth_input(&input, u64_test_now));
    Input older = input;
    CHECK(app_client_input(&client, &input)); CHECK(auth_input(&input, u64_test_now));
    CHECK(!auth_input(&older, u64_test_now));
    auth_forget(1);
    // More than the join burst cannot force unbounded MAC/admission work.
    bad = request; bad.proof[0] ^= 1;
    for (int i = 0; i < 40; i++) auth_join(&bad, 1, u64_test_now, &response);
    CHECK(app_client_request(&client, &request));
    auth_join(&request, 1, u64_test_now, &response); CHECK(response.status == JOIN_RETRY);
    auth_join(&request, 1, u64_test_now + 1000000000ull, &response); CHECK(response.status == JOIN_OK);
    auth_forget(1);
    CHECK(auth_init());
    // Bound memory without evicting replay history: exhaustion fails closed.
    for (int i = 0; i < AUTH_MAX_JOINS; i++)
    {
        request.nonce = (uint64_t)i + 1;
        CHECK(crypto_join_proof(client.arr_login_key, request.nonce, request.proof));
        auth_join(&request, 1, (uint64_t)(i + 1) * 1000000000ull, &response);
        CHECK(response.status == JOIN_OK);
        auth_forget(1);
    }
    request.nonce = AUTH_MAX_JOINS + 1;
    CHECK(crypto_join_proof(client.arr_login_key, request.nonce, request.proof));
    auth_join(&request, 1, (AUTH_MAX_JOINS + 1ull) * 1000000000ull, &response);
    CHECK(response.status == JOIN_BAD_LOGIN);
    request.nonce = 1; CHECK(crypto_join_proof(client.arr_login_key, 1, request.proof));
    auth_join(&request, 1, (AUTH_MAX_JOINS + 2ull) * 1000000000ull, &response);
    CHECK(response.status == JOIN_BAD_LOGIN); // earliest nonce was not evicted

    char arr_password_path[] = "/tmp/fuse-auth-password-XXXXXX";
    int_fd = mkstemp(arr_password_path); CHECK(int_fd >= 0);
    CHECK(write(int_fd, "password\r\n", 10) == 10); close(int_fd);
    char arr_password[CRYPTO_PASSWORD_MAX + 1];
    CHECK(crypto_read_password(arr_password_path, arr_password)); CHECK(strcmp(arr_password, "password") == 0);
    CHECK(chmod(arr_password_path, 0644) == 0); CHECK(!crypto_read_password(arr_password_path, arr_password));
    CHECK(chmod(arr_password_path, 0600) == 0);
    p_file = fopen(arr_password_path, "w"); CHECK(p_file != NULL); fputs("two\nlines\n", p_file); fclose(p_file);
    CHECK(!crypto_read_password(arr_password_path, arr_password));
    p_file = fopen(arr_password_path, "w"); CHECK(p_file != NULL); fclose(p_file);
    CHECK(!crypto_read_password(arr_password_path, arr_password)); CHECK(unlink(arr_password_path) == 0);

    // Credential parser rejects insecure modes, duplicates, malformed hex and empty files.
    CHECK(chmod(arr_path, 0644) == 0); CHECK(!auth_init()); CHECK(chmod(arr_path, 0600) == 0);
    p_file = fopen(arr_path, "w"); CHECK(p_file != NULL); fputs("bad:xx\n", p_file); fclose(p_file);
    CHECK(!auth_init());
    p_file = fopen(arr_path, "w"); CHECK(p_file != NULL);
    fprintf(p_file, "client-1:%s\nclient-1:%s\n", arr_hex, arr_hex); fclose(p_file); CHECK(!auth_init());
    p_file = fopen(arr_path, "w"); CHECK(p_file != NULL); fclose(p_file); CHECK(!auth_init());
    CHECK(unlink(arr_path) == 0); CHECK(!auth_init());
    client.u32_seq = UINT32_MAX; CHECK(!app_client_input(&client, &input));
    CHECK(!app_client_leave(&client, &leave));
    printf("PASS authentication unit/lifecycle checks=%d\n", int_checks);
    return 0;
}
