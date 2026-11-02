/* Native LAN enrollment transport. Only this helper handles the temporary PSK.
   Storage validation stays in pairing_store.py; TLS and device proofs use OpenSSL.
   Codes enter the client on stdin, never argv/environment. This translation unit
   is built separately, rather than linked into the game or benchmark processes. */
#ifdef FUSE_PAIRING_TOOL
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
#define PAIR_MAX (2u * 1024u * 1024u)
#define PAIR_TTL_NS (300ull * 1000000000ull)
static const unsigned char psk_identity[] = "FUSE-PAIR-v1";
static volatile sig_atomic_t b_stop;
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned char arr_code[32];
static uint64_t u_generation, u_expiry, u_renew_generation;
static bool b_code_active;
static int arr_fds[4] = {-1, -1, -1, -1};
static SSL_CTX *p_server_ctx;
static _Thread_local uint64_t u_storage_deadline;

typedef struct {
    int int_fd, int_slot;
    uint32_t u_ip;
    uint64_t u_generation;
    unsigned char arr_psk[32];
} Connection;
typedef struct { uint32_t ip; uint64_t since; unsigned failures; } Rate;
static Rate arr_rates[64];

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void stop_signal(int signal_number) { (void)signal_number; b_stop = 1; }

static bool wait_fd_internal(int fd, short events, uint64_t deadline, bool finish_commit)
{
    while ((!b_stop || finish_commit) && now_ns() < deadline) {
        struct pollfd item = {.fd = fd, .events = events};
        uint64_t left = (deadline - now_ns()) / 1000000ull;
        int rc = poll(&item, 1, (int)(left > 100 ? 100 : left + 1));
        if (rc > 0) return !(item.revents & POLLNVAL);
        if (rc == 0 && finish_commit) return true;
        if (rc < 0 && errno != EINTR) break;
    }
    return false;
}

static bool wait_fd(int fd, short events, uint64_t deadline)
{ return wait_fd_internal(fd, events, deadline, false); }

/* Spawn helpers without a shell, with bounded binary stdout and a hard deadline.
   Closing the parent or cancelling a client terminates its storage child too. */
static int storage(const char *command, const char *arg1, const char *arg2,
                   unsigned char **out, size_t *length)
{
    int pipes[2];
    if (pipe(pipes)) return -1;
    fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
    fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipes[0]);
    posix_spawn_file_actions_addclose(&actions, pipes[1]);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    char *args[] = {"python3", "security/scripts/pairing_store.py", (char *)command,
                    (char *)arg1, (char *)arg2, NULL};
    if (!arg1) args[3] = NULL;
    pid_t child;
    int rc = posix_spawnp(&child, "python3", &actions, NULL, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipes[1]);
    if (rc) { close(pipes[0]); return -1; }
    unsigned char *data = malloc(PAIR_MAX + 1);
    size_t size = 0;
    bool done = false;
    bool install = strcmp(command, "install") == 0, forwarded_stop = false;
    uint64_t deadline = now_ns() + 30000000000ull;
    if (u_storage_deadline && u_storage_deadline < deadline) deadline = u_storage_deadline;
    fcntl(pipes[0], F_SETFL, O_NONBLOCK);
    while (data && size <= PAIR_MAX && wait_fd_internal(pipes[0], POLLIN, deadline, install)) {
        if (install && b_stop && !forwarded_stop) { kill(child, SIGTERM); forwarded_stop = true; }
        ssize_t count = read(pipes[0], data + size, PAIR_MAX + 1 - size);
        if (count == 0) { done = true; break; }
        if (count > 0) size += (size_t)count;
        else if (errno != EAGAIN && errno != EINTR) break;
    }
    close(pipes[0]);
    if (!done || size > PAIR_MAX) kill(child, SIGTERM);
    int status;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (!done || !data || size > PAIR_MAX) { free(data); return -1; }
    data[size] = 0;
    *out = data; *length = size;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static SSL_SESSION *psk_session(SSL *ssl, const unsigned char *psk)
{
    const unsigned char cipher_id[] = {0x13, 0x01};
    SSL_SESSION *session = SSL_SESSION_new();
    const SSL_CIPHER *cipher = SSL_CIPHER_find(ssl, cipher_id);
    if (!session || !cipher || !SSL_SESSION_set1_master_key(session, psk, 32) ||
        !SSL_SESSION_set_cipher(session, cipher) ||
        !SSL_SESSION_set_protocol_version(session, TLS1_3_VERSION)) {
        SSL_SESSION_free(session); return NULL;
    }
    return session;
}

static int client_psk(SSL *ssl, const EVP_MD *md, const unsigned char **identity,
                      size_t *identity_len, SSL_SESSION **session)
{
    if (md && !EVP_MD_is_a(md, "SHA256")) return 0;
    Connection *connection = SSL_get_app_data(ssl);
    *identity = psk_identity; *identity_len = sizeof psk_identity - 1;
    *session = psk_session(ssl, connection->arr_psk);
    return *session != NULL;
}

static int server_psk(SSL *ssl, const unsigned char *identity, size_t identity_len, SSL_SESSION **session)
{
    *session = NULL;
    if (identity_len != sizeof psk_identity - 1 || memcmp(identity, psk_identity, identity_len)) return 1;
    Connection *connection = SSL_get_app_data(ssl);
    pthread_mutex_lock(&state_mutex);
    bool valid = b_code_active && now_ns() < u_expiry;
    if (valid) {
        memcpy(connection->arr_psk, arr_code, 32);
        connection->u_generation = u_generation;
    }
    pthread_mutex_unlock(&state_mutex);
    if (valid) *session = psk_session(ssl, connection->arr_psk);
    return !valid || *session != NULL;
}

static SSL_CTX *tls_context(bool server)
{
    SSL_CTX *ctx = SSL_CTX_new(server ? TLS_server_method() : TLS_client_method());
    if (!ctx) return NULL;
    if (!SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) ||
        !SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION) ||
        !SSL_CTX_set_ciphersuites(ctx, "TLS_AES_128_GCM_SHA256") ||
        !SSL_CTX_set1_groups_list(ctx, "X25519:P-256")) {
        SSL_CTX_free(ctx); return NULL;
    }
    SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);
    SSL_CTX_clear_options(ctx, SSL_OP_ALLOW_NO_DHE_KEX);
    SSL_CTX_set_num_tickets(ctx, 0);
    SSL_CTX_set_max_early_data(ctx, 0);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    if (server) SSL_CTX_set_psk_find_session_callback(ctx, server_psk);
    else SSL_CTX_set_psk_use_session_callback(ctx, client_psk);
    return ctx;
}

static bool ssl_retry(SSL *ssl, int result, uint64_t deadline)
{
    int error = SSL_get_error(ssl, result);
    if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) return false;
    return wait_fd(SSL_get_fd(ssl), error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadline);
}

static bool handshake(SSL *ssl, bool server)
{
    uint64_t deadline = now_ns() + 10000000000ull;
    while (!b_stop && now_ns() < deadline) {
        ERR_clear_error();
        int rc = server ? SSL_accept(ssl) : SSL_connect(ssl);
        if (rc == 1) {
            // Reject certificate fallback, PSK-only exchange, and protocol downgrade.
            return SSL_version(ssl) == TLS1_3_VERSION && SSL_session_reused(ssl) &&
                   SSL_get_negotiated_group(ssl) != 0;
        }
        if (!ssl_retry(ssl, rc, deadline)) break;
    }
    return false;
}

static bool tls_bytes(SSL *ssl, void *data, size_t length, bool write_data, uint64_t deadline)
{
    unsigned char *bytes = data;
    while (length && !b_stop && now_ns() < deadline) {
        size_t count = 0;
        ERR_clear_error();
        int rc = write_data ? SSL_write_ex(ssl, bytes, length, &count) : SSL_read_ex(ssl, bytes, length, &count);
        if (rc == 1 && count) { bytes += count; length -= count; }
        else if (!ssl_retry(ssl, rc, deadline)) return false;
    }
    return length == 0 && !b_stop;
}

static bool send_frame(SSL *ssl, unsigned char type, const void *data, size_t length, uint64_t deadline)
{
    if (length > PAIR_MAX) return false;
    unsigned char header[5] = {type};
    uint32_t size = htonl((uint32_t)length);
    memcpy(header + 1, &size, 4);
    return tls_bytes(ssl, header, 5, true, deadline) && tls_bytes(ssl, (void *)data, length, true, deadline);
}

static bool receive_frame(SSL *ssl, unsigned char *type, unsigned char **data, size_t *length, uint64_t deadline)
{
    unsigned char header[5];
    if (!tls_bytes(ssl, header, 5, false, deadline)) return false;
    uint32_t size;
    memcpy(&size, header + 1, 4); size = ntohl(size);
    if (size > PAIR_MAX) return false;
    unsigned char *buffer = malloc((size_t)size + 1);
    if (!buffer) return false;
    if (!tls_bytes(ssl, buffer, size, false, deadline)) { free(buffer); return false; }
    buffer[size] = 0;
    *type = header[0]; *data = buffer; *length = size;
    return true;
}

static bool device_proof(SSL *ssl, EVP_PKEY *key, unsigned char proof[96], bool verify)
{
    unsigned char binding[32];
    const char label[] = "FUSE-PAIR-DEVICE-v1";
    if (!SSL_export_keying_material(ssl, binding, 32, label, sizeof label - 1, proof, 32, 1)) return false;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    bool valid = false;
    if (ctx) {
        if (verify) valid = EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, key) == 1 &&
                            EVP_DigestVerify(ctx, proof + 32, 64, binding, 32) == 1;
        else {
            size_t size = 64;
            valid = EVP_DigestSignInit(ctx, NULL, NULL, NULL, key) == 1 &&
                    EVP_DigestSign(ctx, proof + 32, &size, binding, 32) == 1 && size == 64;
        }
    }
    EVP_MD_CTX_free(ctx);
    OPENSSL_cleanse(binding, sizeof binding);
    return valid;
}

static bool consume_code(uint64_t generation)
{
    pthread_mutex_lock(&state_mutex);
    bool valid = b_code_active && generation == u_generation && now_ns() < u_expiry;
    if (valid) { b_code_active = false; OPENSSL_cleanse(arr_code, 32); }
    pthread_mutex_unlock(&state_mutex);
    return valid;
}

static void rate_failure(uint32_t ip)
{
    pthread_mutex_lock(&state_mutex);
    size_t chosen = 0;
    for (size_t i = 0; i < 64; i++) {
        if (arr_rates[i].ip == ip) { chosen = i; break; }
        if (arr_rates[i].since < arr_rates[chosen].since) chosen = i;
    }
    Rate *rate = &arr_rates[chosen];
    if (rate->ip != ip || now_ns() - rate->since >= 60000000000ull)
        *rate = (Rate){.ip = ip, .since = now_ns()};
    rate->failures++;
    pthread_mutex_unlock(&state_mutex);
}

static void *serve_connection(void *arg)
{
    Connection *connection = arg;
    SSL *ssl = SSL_new(p_server_ctx);
    unsigned char *request = NULL, *bundle = NULL;
    size_t request_size = 0, bundle_size = 0;
    EVP_PKEY *key = NULL;
    bool authenticated = false;
    if (!ssl) goto finish;
    SSL_set_fd(ssl, connection->int_fd); SSL_set_app_data(ssl, connection);
    if (!handshake(ssl, true)) { rate_failure(connection->u_ip); goto finish; }
    uint64_t deadline = now_ns() + 30000000000ull;
    u_storage_deadline = deadline;
    unsigned char type;
    if (!receive_frame(ssl, &type, &request, &request_size, deadline) || type != 1 || request_size != 96) goto finish;
    key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, request, 32);
    if (!key || !device_proof(ssl, key, request, true)) { rate_failure(connection->u_ip); goto finish; }
    if (!consume_code(connection->u_generation)) goto finish;
    authenticated = true;
    char public_hex[65];
    for (int i = 0; i < 32; i++) snprintf(public_hex + i * 2, 3, "%02x", request[i]);
    int rc = storage("issue", public_hex, NULL, &bundle, &bundle_size);
    pthread_mutex_lock(&state_mutex);
    bool current = connection->u_generation == u_generation;
    pthread_mutex_unlock(&state_mutex);
    if (!current || b_stop) goto finish;
    if (rc == 0) {
        if (send_frame(ssl, 0, bundle, bundle_size, deadline)) puts("Pairing credentials sent to assigned device.");
    } else {
        const char *error = rc == 3 ? "All five player identities are assigned." : "Host provisioning needs attention. Ask the host to check its credentials.";
        send_frame(ssl, rc == 3 ? 3 : 2, error, strlen(error), deadline);
    }
finish:
    if (bundle) { OPENSSL_cleanse(bundle, bundle_size); free(bundle); }
    free(request); EVP_PKEY_free(key); SSL_free(ssl);
    OPENSSL_cleanse(connection->arr_psk, 32);
    pthread_mutex_lock(&state_mutex);
    arr_fds[connection->int_slot] = -1;
    if (authenticated && connection->u_generation == u_generation) u_renew_generation = u_generation;
    close(connection->int_fd);
    pthread_mutex_unlock(&state_mutex);
    free(connection);
    return NULL;
}

static void new_code(void)
{
    unsigned char random[32], encoded[45];
    if (RAND_bytes(random, 32) != 1) { fputs("Cannot generate a pairing code.\n", stderr); return; }
    EVP_EncodeBlock(encoded, random, 32);
    for (int i = 0; i < 43; i++) { if (encoded[i] == '+') encoded[i] = '-'; if (encoded[i] == '/') encoded[i] = '_'; }
    encoded[43] = 0;
    pthread_mutex_lock(&state_mutex);
    u_generation++; u_expiry = now_ns() + PAIR_TTL_NS;
    memcpy(arr_code, random, 32); b_code_active = true; u_renew_generation = 0;
    pthread_mutex_unlock(&state_mutex);
    // Never put bearer codes in redirected game logs. Tests/other local frontends
    // can supply an explicit private pipe descriptor instead of a terminal.
    const char *channel = getenv("FUSE_PAIRING_CODE_FD");
    int code_fd = -1;
    bool close_channel = false;
    if (channel) {
        char *end; long value = strtol(channel, &end, 10);
        if (*channel && !*end && value >= 0 && value <= 1024) code_fd = (int)value;
        if (code_fd >= 0) fcntl(code_fd, F_SETFD, FD_CLOEXEC);
    } else if (isatty(STDOUT_FILENO)) code_fd = STDOUT_FILENO;
    else { code_fd = open("/dev/tty", O_WRONLY | O_CLOEXEC); close_channel = code_fd >= 0; }
    if (code_fd >= 0 && dprintf(code_fd, "Pairing code: FUSE-%s\n", encoded) > 0)
        puts("Code displayed privately; expires in 5 minutes. Commands: pair, cancel, devices.");
    else {
        pthread_mutex_lock(&state_mutex); b_code_active = false; OPENSSL_cleanse(arr_code, 32); pthread_mutex_unlock(&state_mutex);
        puts("Pairing needs an interactive terminal to display its code. Gameplay remains available.");
    }
    if (close_channel) close(code_fd);
    fflush(stdout);
    OPENSSL_cleanse(random, sizeof random); OPENSSL_cleanse(encoded, sizeof encoded);
}

static void cancel_code(void)
{
    pthread_mutex_lock(&state_mutex);
    u_generation++; b_code_active = false; u_renew_generation = 0;
    OPENSSL_cleanse(arr_code, 32);
    for (int i = 0; i < 4; i++) if (arr_fds[i] >= 0) shutdown(arr_fds[i], SHUT_RDWR);
    pthread_mutex_unlock(&state_mutex);
}

static int host(const char *address, int port)
{
    unsigned char *output = NULL; size_t length = 0;
    if (!address) {
        if (storage("bind", NULL, NULL, &output, &length) != 0) return 1;
        output[strcspn((char *)output, "\r\n")] = 0; address = (char *)output;
    }
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (inet_pton(AF_INET, address, &local.sin_addr) != 1) { free(output); return 1; }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (fd < 0 || bind(fd, (struct sockaddr *)&local, sizeof local) || listen(fd, 8)) {
        perror("Pairing listener (game server can continue)"); if (fd >= 0) close(fd); free(output); return 1;
    }
    p_server_ctx = tls_context(true);
    if (!p_server_ctx) { close(fd); free(output); return 1; }
    printf("Pairing host: %s TCP %d\n", address, port); free(output);
    new_code();
    bool read_input = true; char command[128]; size_t used = 0;
    while (!b_stop) {
        pthread_mutex_lock(&state_mutex);
        bool expired = b_code_active && now_ns() >= u_expiry;
        bool renew = u_renew_generation && u_renew_generation == u_generation;
        u_renew_generation = 0;
        pthread_mutex_unlock(&state_mutex);
        if (expired) { cancel_code(); puts("Pairing code expired. Enter pair for a new code."); fflush(stdout); }
        if (renew) {
            unsigned char *count = NULL; size_t size = 0;
            int rc = storage("count", NULL, NULL, &count, &size);
            if (rc == 0 && size && count[0] < '5') new_code();
            else { puts(rc == 0 ? "All identities assigned. Enter pair to repair a saved device." :
                        "Pairing state unavailable. Check host provisioning, then enter pair to retry."); fflush(stdout); }
            free(count);
        }
        struct pollfd items[2] = {{.fd = fd, .events = POLLIN}, {.fd = read_input ? 0 : -1, .events = POLLIN}};
        if (poll(items, 2, 100) < 0) continue;
        if (items[1].revents & (POLLIN | POLLHUP)) {
            char ch; ssize_t n = read(0, &ch, 1);
            if (n <= 0) read_input = false;
            else if (ch == '\n') {
                command[used] = 0; used = 0;
                if (!strcmp(command, "pair")) { cancel_code(); new_code(); }
                else if (!strcmp(command, "cancel")) { cancel_code(); puts("Pairing cancelled."); fflush(stdout); }
                else if (!strcmp(command, "devices")) {
                    unsigned char *text = NULL; size_t size = 0;
                    if (!storage("devices", NULL, NULL, &text, &size)) fwrite(text, 1, size, stdout);
                    free(text); fflush(stdout);
                }
            } else if (ch != '\r' && used < sizeof command - 1) command[used++] = ch;
        }
        if (!(items[0].revents & POLLIN)) continue;
        struct sockaddr_in peer; socklen_t peer_size = sizeof peer;
        int accepted = accept(fd, (struct sockaddr *)&peer, &peer_size);
        if (accepted < 0) continue;
        fcntl(accepted, F_SETFL, O_NONBLOCK); fcntl(accepted, F_SETFD, FD_CLOEXEC);
        pthread_mutex_lock(&state_mutex);
        bool blocked = !b_code_active || now_ns() >= u_expiry;
        for (int i = 0; i < 64; i++) if (arr_rates[i].ip == peer.sin_addr.s_addr &&
            now_ns() - arr_rates[i].since < 60000000000ull && arr_rates[i].failures >= 5) blocked = true;
        int slot = -1;
        if (!blocked) for (int i = 0; i < 4; i++) if (arr_fds[i] < 0) { slot = i; arr_fds[i] = accepted; break; }
        pthread_mutex_unlock(&state_mutex);
        if (slot < 0) { close(accepted); continue; }
        Connection *connection = calloc(1, sizeof *connection);
        pthread_t thread;
        if (connection) *connection = (Connection){.int_fd = accepted, .int_slot = slot, .u_ip = peer.sin_addr.s_addr};
        if (!connection || pthread_create(&thread, NULL, serve_connection, connection)) {
            pthread_mutex_lock(&state_mutex); arr_fds[slot] = -1; pthread_mutex_unlock(&state_mutex);
            close(accepted); free(connection);
        } else pthread_detach(thread);
    }
    close(fd); cancel_code();
    for (;;) {
        bool busy = false;
        pthread_mutex_lock(&state_mutex);
        for (int i = 0; i < 4; i++) if (arr_fds[i] >= 0) busy = true;
        pthread_mutex_unlock(&state_mutex);
        if (!busy) break;
        struct timespec pause = {.tv_nsec = 10000000}; nanosleep(&pause, NULL);
    }
    SSL_CTX_free(p_server_ctx);
    return 0;
}

static int client(const char *address, int port)
{
    int result = 1, fd = -1;
    SSL_CTX *ctx = NULL; SSL *ssl = NULL; EVP_PKEY *key = NULL;
    unsigned char *output = NULL, *bundle = NULL; size_t length = 0, bundle_size = 0;
    Connection connection = {0}; char code[64] = {0};
    char directory[] = "/tmp/fuse-pair-client-XXXXXX", bundle_path[128] = {0};
    if (!fgets(code, sizeof code, stdin)) goto finish;
    code[strcspn(code, "\r\n")] = 0;
    if (strlen(code) != 48 || strncmp(code, "FUSE-", 5)) { puts("Paste the complete FUSE pairing code."); goto finish; }
    unsigned char encoded[45]; memcpy(encoded, code + 5, 43); encoded[43] = '='; encoded[44] = 0;
    for (int i = 0; i < 43; i++) {
        if (encoded[i] == '-') encoded[i] = '+'; else if (encoded[i] == '_') encoded[i] = '/';
        else if (!((encoded[i] >= 'A' && encoded[i] <= 'Z') || (encoded[i] >= 'a' && encoded[i] <= 'z') ||
                   (encoded[i] >= '0' && encoded[i] <= '9'))) { puts("Invalid pairing code."); goto finish; }
    }
    unsigned char decoded[33];
    if (EVP_DecodeBlock(decoded, encoded, 44) != 33) goto finish;
    unsigned char canonical[45];
    EVP_EncodeBlock(canonical, decoded, 32);
    bool canonical_code = CRYPTO_memcmp(canonical, encoded, 44) == 0;
    OPENSSL_cleanse(canonical, sizeof canonical);
    if (!canonical_code) {
        OPENSSL_cleanse(decoded, sizeof decoded); OPENSSL_cleanse(encoded, sizeof encoded);
        puts("Invalid pairing code."); goto finish;
    }
    memcpy(connection.arr_psk, decoded, 32);
    OPENSSL_cleanse(decoded, sizeof decoded); OPENSSL_cleanse(code, sizeof code); OPENSSL_cleanse(encoded, sizeof encoded);
    if (storage("device", NULL, NULL, &output, &length)) { puts("Cannot create the private device key."); goto finish; }
    output[strcspn((char *)output, "\r\n")] = 0;
    FILE *file = fopen((char *)output, "rb");
    if (file) { key = PEM_read_PrivateKey(file, NULL, NULL, ""); fclose(file); }
    free(output); output = NULL;
    if (!key) { puts("Cannot load the private device key."); goto finish; }
    struct sockaddr_in remote = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (inet_pton(AF_INET, address, &remote.sin_addr) != 1) { puts("Enter a valid host IPv4 address."); goto finish; }
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) goto finish;
    int rc = connect(fd, (struct sockaddr *)&remote, sizeof remote);
    if (rc && errno == EINPROGRESS && wait_fd(fd, POLLOUT, now_ns() + 10000000000ull)) {
        int error; socklen_t size = sizeof error;
        rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) || error;
    }
    if (rc) { puts("Cannot reach the pairing host. Check its IP and LAN setup."); goto finish; }
    ctx = tls_context(false); if (!ctx) goto finish;
    ssl = SSL_new(ctx); if (!ssl) goto finish;
    SSL_set_fd(ssl, fd); SSL_set_app_data(ssl, &connection);
    if (!handshake(ssl, false)) { puts("Pairing code rejected or expired. Ask the host for a fresh code."); goto finish; }
    unsigned char proof[96]; size_t public_size = 32;
    if (!EVP_PKEY_get_raw_public_key(key, proof, &public_size) || public_size != 32 || !device_proof(ssl, key, proof, false)) goto finish;
    uint64_t deadline = now_ns() + 30000000000ull;
    u_storage_deadline = deadline;
    unsigned char type;
    if (!send_frame(ssl, 1, proof, sizeof proof, deadline) ||
        !receive_frame(ssl, &type, &bundle, &bundle_size, deadline)) {
        puts("Pairing interrupted. Retry with a fresh host code."); goto finish;
    }
    if (type != 0) {
        puts(type == 3 ? "All five player identities are assigned." : "Host provisioning needs attention."); goto finish;
    }
    if (b_stop || !mkdtemp(directory)) goto finish;
    snprintf(bundle_path, sizeof bundle_path, "%s/client.fuse-lan", directory);
    int bundle_fd = open(bundle_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (bundle_fd < 0) goto finish;
    size_t written = 0;
    while (written < bundle_size) {
        ssize_t n = write(bundle_fd, bundle + written, bundle_size - written);
        if (n <= 0) break;
        written += (size_t)n;
    }
    bool saved = written == bundle_size && fsync(bundle_fd) == 0;
    close(bundle_fd);
    if (!saved || b_stop) goto finish;
    rc = storage("install", bundle_path, address, &output, &length);
    // A late Cancel cannot undo a completed activation. The storage helper
    // masks SIGTERM only across the atomic commit and emits this acknowledgement.
    bool committed = output && strstr((char *)output, "PAIRING-COMMITTED\n") != NULL;
    if (!committed) { puts("Pairing did not complete. Retry with a fresh host code."); goto finish; }
    puts("Paired. Your connection is saved."); result = 0;
finish:
    if (bundle_path[0]) { unlink(bundle_path); rmdir(directory); }
    if (bundle) { OPENSSL_cleanse(bundle, bundle_size); free(bundle); }
    free(output); EVP_PKEY_free(key); SSL_free(ssl); SSL_CTX_free(ctx);
    if (fd >= 0) close(fd);
    OPENSSL_cleanse(connection.arr_psk, 32); OPENSSL_cleanse(code, sizeof code);
    return result;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN); signal(SIGTERM, stop_signal); signal(SIGINT, stop_signal);
    if (argc < 2 || argc > 4) return 2;
    int port = argc == 4 ? atoi(argv[3]) : 7440;
    if (port < 1 || port > 65535) return 2;
    if (!strcmp(argv[1], "host")) return host(argc >= 3 ? argv[2] : NULL, port);
    if (!strcmp(argv[1], "client") && argc >= 3) return client(argv[2], port);
    return 2;
}
#endif /* FUSE_PAIRING_TOOL */
