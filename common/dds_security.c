/* Certificate authentication, signed access policies and encrypted DDS.
All crypto is done by Cyclone's plugins. Participant QoS supplies every
security property, taking precedence over security in CYCLONEDDS_URI. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include "dds/features.h"

#include "dds_security.h"
#include "log.h"

#ifndef FUSE_DDS_PLUGIN_DIR_DEFAULT
#define FUSE_DDS_PLUGIN_DIR_DEFAULT ""
#endif

static void security_error(const char *a_format, ...)
{
    va_list args;
    va_start(args, a_format);
    fputs("DDS security: ", stderr);
    vfprintf(stderr, a_format, args);
    fputs("\n", stderr);
    va_end(args);
    exit(EXIT_FAILURE);
}

/* Reject truncated paths before they can accidentally select another file. */
static void format_path(char *a_output, const char *a_format, ...)
{
    va_list args;
    va_start(args, a_format);
    int int_length = vsnprintf(a_output, PATH_MAX, a_format, args);
    va_end(args);
    if (int_length < 0 || int_length >= PATH_MAX)
        security_error("Credential/plugin path is too long");
}

static void file_property(dds_qos_t *a_qos, const char *a_property,
                          const char *a_path, bool a_b_private)
{
    struct stat info;
    if (stat(a_path, &info) != 0 || !S_ISREG(info.st_mode) || access(a_path, R_OK) != 0)
        security_error("Cannot read %s; run bash security/scripts/make_certs.sh", a_path);
    if (a_b_private && (info.st_uid != getuid() || (info.st_mode & 0077) != 0))
        security_error("Private key must belong to you and have mode 600: %s", a_path);
    char arr_uri[PATH_MAX + 5];
    snprintf(arr_uri, sizeof arr_uri, "file:%s", a_path);
    dds_qset_prop(a_qos, a_property, arr_uri);
}

static void plugin_properties(dds_qos_t *a_qos, const char *a_name,
                              const char *a_library, const char *a_functions,
                              const char *a_directory)
{
    char arr_property[128], arr_path[PATH_MAX], arr_function[128];
    if (*a_directory == '\0')
        security_error("Security plugin directory is unknown; source security/scripts/env.sh");
    format_path(arr_path, "%s/libdds_security_%s.so", a_directory, a_library);
    if (access(arr_path, R_OK) != 0)
        security_error("Missing plugin %s; source security/scripts/env.sh", arr_path);
    snprintf(arr_property, sizeof arr_property, "dds.sec.%s.library.path", a_name);
    dds_qset_prop(a_qos, arr_property, arr_path);
    snprintf(arr_property, sizeof arr_property, "dds.sec.%s.library.init", a_name);
    snprintf(arr_function, sizeof arr_function, "init_%s", a_functions);
    dds_qset_prop(a_qos, arr_property, arr_function);
    snprintf(arr_property, sizeof arr_property, "dds.sec.%s.library.finalize", a_name);
    snprintf(arr_function, sizeof arr_function, "finalize_%s", a_functions);
    dds_qset_prop(a_qos, arr_property, arr_function);
}

/* Cyclone validates the CA chain, but a mismatched key can survive local
participant creation until a remote handshake. Catch it at startup instead. */
static void check_identity(const char *a_cert_path, const char *a_key_path)
{
    FILE *p_cert_file = fopen(a_cert_path, "r");
    FILE *p_key_file = fopen(a_key_path, "r");
    if (p_cert_file == NULL || p_key_file == NULL)
        security_error("Cannot open identity certificate/private key");
    X509 *p_cert = PEM_read_X509(p_cert_file, NULL, NULL, NULL);
    // An encrypted external key must fail rather than prompt on worker stdin.
    EVP_PKEY *p_key = PEM_read_PrivateKey(p_key_file, NULL, NULL, "");
    fclose(p_cert_file);
    fclose(p_key_file);
    bool b_valid = p_cert != NULL && p_key != NULL && X509_check_private_key(p_cert, p_key) == 1;
    if (b_valid)
        b_valid = X509_cmp_current_time(X509_get0_notBefore(p_cert)) < 0 &&
                  X509_cmp_current_time(X509_get0_notAfter(p_cert)) > 0;
    X509_free(p_cert);
    EVP_PKEY_free(p_key);
    if (!b_valid)
        security_error("Invalid, expired or mismatched identity certificate/private key: %s", a_cert_path);
}

void dds_security_prepare(void)
{
    const char *str_mode = getenv("FUSE_DDS_SECURITY");
    if (str_mode != NULL && strcmp(str_mode, "on") != 0 && strcmp(str_mode, "off") != 0)
        security_error("FUSE_DDS_SECURITY must be on or off (default on)");
    if (str_mode != NULL && strcmp(str_mode, "off") == 0 && getenv("CYCLONEDDS_URI") != NULL)
        unsetenv("CYCLONEDDS_URI");
}

dds_entity_t dds_security_create_participant(NetRole a_role)
{
    if (a_role != NET_ROLE_SERVER && a_role != NET_ROLE_CLIENT)
        security_error("Invalid participant role");
    const char *str_mode = getenv("FUSE_DDS_SECURITY");
    if (str_mode != NULL && strcmp(str_mode, "on") != 0 && strcmp(str_mode, "off") != 0)
        security_error("FUSE_DDS_SECURITY must be on or off (default on)");
    bool b_secure = str_mode == NULL || strcmp(str_mode, "on") == 0;
    dds_qos_t *p_qos = NULL;
    if (!b_secure)
    {
        /* Explicit comparison mode must not inherit a secure XML config.
        Unset before creating any domain; no network entities exist yet. */
        // Game mains prepared this before starting threads. Standalone probes
        // and other single-threaded callers can prepare here as well.
        dds_security_prepare();
        LOG_WARN("DDS security: OFF (explicit plaintext comparison)\n");
    }
    else
    {
#ifndef DDS_HAS_SECURITY
        security_error("This Cyclone DDS library was built without DDS Security support");
#endif
        const char *str_identity = getenv("FUSE_DDS_IDENTITY");
        if (a_role == NET_ROLE_SERVER && str_identity == NULL)
            str_identity = "server";
        if (str_identity == NULL)
            security_error("Select a client identity: FUSE_DDS_IDENTITY=client-1 (through client-5)");
        bool b_server = strcmp(str_identity, "server") == 0;
        bool b_client = strlen(str_identity) == 8 && strncmp(str_identity, "client-", 7) == 0 &&
                        str_identity[7] >= '1' && str_identity[7] <= '5';
        if ((a_role == NET_ROLE_SERVER && !b_server) || (a_role == NET_ROLE_CLIENT && !b_client))
            security_error("Identity %s does not match this executable's role", str_identity);

        char arr_root[PATH_MAX], arr_path[PATH_MAX];
        const char *str_root = getenv("FUSE_SECURITY_DIR");
        if (str_root != NULL)
            format_path(arr_root, "%s", str_root);
        else
        {
            const char *str_home = getenv("HOME");
            if (str_home == NULL || *str_home == '\0')
                security_error("HOME is missing; set FUSE_SECURITY_DIR to an absolute Linux path");
            format_path(arr_root, "%s/.local/share/fuse/security", str_home);
        }
        if (arr_root[0] != '/' || strncmp(arr_root, "/mnt/", 5) == 0)
            security_error("FUSE_SECURITY_DIR must be an absolute Linux filesystem path outside /mnt");
        const char *str_plugins = getenv("FUSE_DDS_PLUGIN_DIR");
        if (str_plugins == NULL)
            str_plugins = FUSE_DDS_PLUGIN_DIR_DEFAULT;
        p_qos = dds_create_qos();
        if (p_qos == NULL)
            security_error("Cannot allocate participant QoS");
        plugin_properties(p_qos, "auth", "auth", "authentication", str_plugins);
        plugin_properties(p_qos, "crypto", "crypto", "crypto", str_plugins);
        plugin_properties(p_qos, "access", "ac", "access_control", str_plugins);

#define SECURITY_FILE(property, private_key, ...) do { \
        format_path(arr_path, __VA_ARGS__); \
        file_property(p_qos, property, arr_path, private_key); \
    } while (0)
        SECURITY_FILE("dds.sec.auth.identity_ca", false, "%s/ca/identity/cert.pem", arr_root);
        SECURITY_FILE("dds.sec.auth.identity_certificate", false, "%s/identities/%s/cert.pem", arr_root, str_identity);
        SECURITY_FILE("dds.sec.auth.private_key", true, "%s/identities/%s/key.pem", arr_root, str_identity);
        SECURITY_FILE("dds.sec.access.permissions_ca", false, "%s/ca/permissions/cert.pem", arr_root);
        SECURITY_FILE("dds.sec.access.governance", false, "%s/governance.p7s", arr_root);
        SECURITY_FILE("dds.sec.access.permissions", false, "%s/identities/%s/permissions.p7s", arr_root, str_identity);
#undef SECURITY_FILE
        char arr_cert_path[PATH_MAX];
        format_path(arr_cert_path, "%s/identities/%s/cert.pem", arr_root, str_identity);
        format_path(arr_path, "%s/identities/%s/key.pem", arr_root, str_identity);
        check_identity(arr_cert_path, arr_path);
        LOG_INFO("DDS security: ON, identity %s, domain 0\n", str_identity);
    }
    dds_entity_t participant = dds_create_participant(0, p_qos, NULL);
    dds_delete_qos(p_qos);
    if (participant < 0)
        security_error("Participant creation failed: %s", dds_strretcode(-participant));
    return participant;
}
