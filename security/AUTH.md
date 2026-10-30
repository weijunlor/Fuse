# Application authentication

FUSE now requires both a DDS participant identity and an application login by
default. Certificate setup remains in [README.md](README.md). The original
lecturer programs in `baseline/` are unchanged and do not use application login.

## First run in Ubuntu / WSL

After the existing DDS setup, create five local accounts with random passwords:

```bash
source security/scripts/env.sh
python3 security/scripts/accounts.py demo
make -j4
bash security/scripts/run_secure.sh server ./build/server
# In separate terminals:
bash security/scripts/run_secure.sh client-1 ./build/client
bash security/scripts/run_secure.sh client-2 ./build/client
```

Accounts default to `~/.local/share/fuse/auth/`. `users.db` contains only derived
login keys; `client-1.pass` through `client-5.pass` contain client passwords.
Files are owner-only (600), and the directory is owner-only (700), on the Linux
filesystem outside `/mnt`. The generator refuses existing accounts/password
files; rerunning `demo` does not change credentials. Passwords are never printed,
passed on the command line, logged, or sent over DDS.

The launcher's DDS identity supplies the default username for convenience.
Application usernames and DDS identities remain independent: to choose another
account, set the following variables before launching the client:

```bash
export FUSE_USERNAME=sheeyang
export FUSE_PASSWORD_FILE=/absolute/linux/path/sheeyang.pass
```

Create a named account with a hidden password prompt:

```bash
python3 security/scripts/accounts.py add sheeyang
```

Supported names have 1–15 ASCII letters, digits, underscores or hyphens. Passwords
contain 1–256 UTF-8 bytes without NUL or line breaks. At most 32 accounts can be
loaded, and one live session per account is allowed. A second login cannot evict
an active player; after a crash, wait for the normal player timeout before
logging in again. Missing, malformed, duplicate or readable-by-others credential
files cause startup to fail. Reloading accounts requires a server restart.

Configuration:

| Variable | Default / purpose |
|---|---|
| `FUSE_AUTH` | `on`; invalid values fail startup |
| `FUSE_AUTH_DIR` | `~/.local/share/fuse/auth`; shared default directory |
| `FUSE_AUTH_USERS` | `<auth directory>/users.db`; server database override |
| `FUSE_USERNAME` | `FUSE_DDS_IDENTITY`; client account name |
| `FUSE_PASSWORD_FILE` | `<auth directory>/<username>.pass`; client password override |

For multiple machines, give the server only `users.db`, and each client only its
own password file, together with its separate DDS credential bundle. Set the
same account name on its client. Keep the database private: derived login keys
are sufficient to generate valid login proofs. Neither passwords nor database
files belong in source control or the submission zip.

## Protocol and lifecycle

The existing fixed-size IDL layout is unchanged; its reserved proof/session/MAC
fields are now used. All MAC integer fields use big-endian encoding, independent
of host byte order or C struct padding.

1. Client derives a 32-byte login key with PBKDF2-HMAC-SHA256, 600,000 rounds.
   Following IDL v1, its unique account name is the public salt; no salt exchange
   or additional message type is needed. Password bytes are erased after derivation.
2. Client sends a random nonzero 64-bit nonce, account name and
   `HMAC-SHA256(login_key, nonce_BE64)`. Server verifies against its stored key.
3. Server allocates a player slot and random 16-byte session key. The response
   carries that key XORed with the first 16 bytes of
   `HMAC-SHA256(login_key, "session" || nonce_BE64)`. Other clients see the
   response but cannot recover the joining client's session key.
4. Input carries the first 16 bytes of
   `HMAC-SHA256(session_key, 'I' || player_id || seq_BE32 || keys)`.
   Leave uses `'L' || player_id || seq_BE32`, separating the message types.
5. Server accepts only active-session messages with a valid MAC, supported key
   bits, and a strictly increasing nonzero sequence shared by Input and Leave.
   Invalid messages do not advance the sequence or refresh the player timeout.
   Sequence wrap is rejected; reconnect to obtain a fresh session.
6. Accepted inputs use a token bucket of 120/s with a burst of 30 per player,
   supporting both the normal 10/s repeat and the 100/s comparison input mode.
   Leave is not blocked by an exhausted input bucket. Join verification has a
   separate global bucket of 20/s with a burst of 40. Throttled joins return
   `JOIN_RETRY`, allowing the client to retry without treating it as a bad password.
7. MAC validation and state changes share the player-table mutex. Accepted Leave
   immediately closes the session to further inputs; actual slot removal and
   timeout both erase session secrets. Reusing a slot creates a fresh key.

Retries of the same valid join while its session is active return the same slot
and wrapped key without resetting either side's input sequence. After removal, that nonce
cannot start a new session. The server remembers up to 4,096 admitted nonces for
its current process lifetime and fails closed on exhaustion instead of evicting
history. This history is not persistent across server restarts. Login proofs
are password-based and are not a PAKE; DDS encryption/server permissions remain
the transport protection for this layer.

The helpers use OpenSSL [PBKDF2](https://docs.openssl.org/3.0/man3/PKCS5_PBKDF2_HMAC/),
[HMAC](https://docs.openssl.org/3.0/man3/HMAC/),
[random bytes](https://docs.openssl.org/3.0/man3/RAND_bytes/), and
[constant-time comparison](https://docs.openssl.org/3.0/man3/CRYPTO_memcmp/).

## Tests and comparison mode

```bash
source security/scripts/env.sh
make test-auth                 # crypto vectors and session/lobby lifecycle
make test-auth-integration     # real encrypted/plaintext DDS, production client worker
python3 security/scripts/test_security.py --out bench/results/dds-security-app-auth
```

The authentication suite creates and removes its own temporary accounts and
child processes. It refuses to run alongside an existing game server. Evidence
goes to `bench/results/app-auth-tests/`, including build hashes and individual
logs. Tests cover correct/wrong credentials, idempotent join, movement, tampering,
replay, forged Leave, cross-player MACs, key renewal, timeout cleanup, rate limits,
full replay history, parser failures, and explicit comparison behavior.

To run the old unauthenticated path for a measurement, explicitly disable both
layers in every participating process:

```bash
FUSE_DDS_SECURITY=off FUSE_AUTH=off ./build/server
FUSE_DDS_SECURITY=off FUSE_AUTH=off ./build/client
FUSE_DDS_SECURITY=off FUSE_AUTH=off MOVE=1 bench/profile.sh comparison build
```

`FUSE_AUTH=off` is rejected while DDS security is on. Turning DDS encryption off
alone keeps application authentication enabled. Authenticated joins always use
nonces even if `FUSE_JOIN_NONCE=off` was set; that switch only affects explicit
unauthenticated comparisons. Benchmark setup files record both security modes.
