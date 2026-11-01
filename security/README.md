# DDS security

FUSE uses Cyclone DDS participant certificates, signed topic permissions and
AES-GCM encryption. Security is **on by default**. Game password login and
per-player Input/Leave MACs are also enabled; see [AUTH.md](AUTH.md) for that layer.

## Set up Ubuntu / WSL

From the repository root in Ubuntu:

```bash
bash security/scripts/setup_wsl.sh
source security/scripts/env.sh
bash security/scripts/make_ca.sh
bash security/scripts/make_certs.sh
python3 security/scripts/accounts.py demo
make -j4 all baseline build/dds_joiner build/dds_security_probe build/dds_security_probe_baseline
```

The setup script installs dependencies with sudo and builds Cyclone DDS **0.10.5**
with security, OpenSSL and UDP transport (shared memory disabled). Its source and
build are under `~/.cache/fuse/`; its installation is
`~/.local/opt/cyclonedds-0.10.5`. Use `--no-packages` when packages are already
installed. Source `env.sh` in every new terminal: it selects Linux tools ahead of
Windows tools and sets the DDS compiler, pkg-config and plugin library paths.
`FUSE_CYCLONEDDS_PREFIX` can select another installation; packaged Ubuntu plugins
in `libddsc0debian/` are also detected.

Check the selected installation:

```bash
command -v pkg-config idlc
pkg-config --modversion CycloneDDS
ldd build/server | grep ddsc
ls "$FUSE_DDS_PLUGIN_DIR"/libdds_security_{auth,ac,crypto}.so
```

## Launch a secure game

Run each command in its own Ubuntu terminal, after sourcing `env.sh`:

```bash
bash security/scripts/run_secure.sh server ./build/server
bash security/scripts/run_secure.sh client-1 ./build/client
bash security/scripts/run_secure.sh client-2 ./build/client
```

The launcher accepts `<identity> <executable> [arguments...]`, starts in the repo
root so the atlas is found, and passes an absolute-path XML configuration through
`CYCLONEDDS_URI`. It can secure the unmodified lecturer programs too:

```bash
bash security/scripts/run_secure.sh server ./build/baseline_server
bash security/scripts/run_secure.sh client-1 ./build/baseline_client
```

Stop the FUSE scenario before testing the baseline. Their IDL types differ even
where topic names match. Start baseline clients one at a time: its original join
race is deliberately unchanged.

After environment activation, direct FUSE runs are secure too:

```bash
./build/server                         # defaults to the server certificate
FUSE_DDS_IDENTITY=client-1 ./build/client
```

A client must explicitly select `client-1` through `client-5`. These are DDS
identities, **not** player IDs assigned by the server. Failed security setup exits
the entire FUSE process; it never retries without security. FUSE sets all required
security properties on participant QoS, which takes precedence over security
settings in an external XML file. Baseline binaries use XML only.

## Policies and credentials

The identity CA authenticates participants. A separate permissions CA signs the
governance and permissions documents in S/MIME format with SHA-256. CAs use
RSA-2048 and ten-year certificates; participant certificates and permissions
expire after one year. The permissions CA includes digital-signature key usage
because Cyclone verifies its policy signatures through OpenSSL's S/MIME verifier.

The policy covers domain **0**, the default empty partition, and denies everything
outside this table:

| Identity | Publish | Subscribe |
|---|---|---|
| server | JoinResponse, GameSnapshot, Roster, Position | JoinRequest, Input, Leave |
| clients | JoinRequest, Input, Leave | JoinResponse, GameSnapshot, Roster, Position |

`Position` supports the baseline and `Roster` is reserved for the planned roster.
Governance forbids unauthenticated participants, checks read/write permissions,
and encrypts RTPS, protected discovery and liveliness. Topic-level encryption is
not repeated inside the encrypted RTPS envelope. Initial discovery/authentication
and public certificate information can still be visible in a packet capture.

Generated credentials default to `~/.local/share/fuse/security`. Override with
`FUSE_SECURITY_DIR`, an absolute path on the Linux filesystem outside `/mnt`.
Directories are owner-only and private keys have mode 600. The generator refuses
to overwrite keys, validates existing certificate/key pairs, and preserves valid
certificates and policies on repeated runs. An incomplete or expired credential
set reports an error rather than silently replacing identities.

After changing the tracked policy templates, re-sign with:

```bash
bash security/scripts/make_certs.sh --refresh-policies
```

This refreshes policy validity up to each identity certificate's expiration; it
does not renew an expired certificate. For certificate renewal, provision a fresh
credential directory explicitly, verify it, then switch `FUSE_SECURITY_DIR`.
For multiple machines, give each participant only its own identity key/certificate,
permissions file, the governance file and both public CA certificates, preserving
the same directory layout. Keep the two CA private keys on the provisioning
machine. Run the launcher after moving a bundle to regenerate local absolute
paths. Existing signed policies must be provisioned before running the launcher;
launching does not need CA private keys. Never put generated keys in the source
zip or commit them.

## Plaintext comparisons and benchmarks

Select plaintext explicitly in every terminal/process involved:

```bash
FUSE_DDS_SECURITY=off FUSE_AUTH=off ./build/server
FUSE_DDS_SECURITY=off FUSE_AUTH=off ./build/client
FUSE_DDS_SECURITY=off FUSE_AUTH=off MOVE=1 bench/profile.sh comparison build
```

FUSE comparison mode clears `CYCLONEDDS_URI` before participant creation so a
previous secure launch cannot contaminate the comparison. It logs `OFF` visibly.
Benchmark launchers also clear the variable in plaintext mode, including for
historical binaries. In secure mode they assign server/client identities, apply
the XML to historical binaries, and support up to five distinct clients.
`setup.txt` records the mode. Use the same mode on both sides of an optimisation
comparison; keep existing historical result folders unchanged.
Application authentication remains on if only DDS encryption is disabled.

## Acceptance tests and packet evidence

```bash
source security/scripts/env.sh
python3 security/scripts/test_security.py --gui
```

Omit `--gui` on machines without a display. The suite checks secure movement/Leave, explicit plaintext operation, missing
credentials, role mismatch, invalid mode, certificate/key mismatch, tampered and
expired permissions, forbidden writes/reads, untrusted and plaintext peers,
continued operation of a trusted peer, and the unchanged baseline. It refuses to
start alongside another server and stops only its own child processes. Fixtures
containing private keys live in temporary Linux directories, outside the repo.

Evidence goes to `bench/results/dds-security-wsl/`: `report.txt`, `results.json`,
`setup.json`, raw logs, and packet captures. Captures produced by the automated
suite use Cyclone's built-in UDP packet logging; IP/UDP headers are synthetic,
while the RTPS packet bytes are the bytes sent/received. Open the baseline
plaintext/secure captures in Wireshark to compare readable DATA with encrypted
RTPS SEC_PREFIX/SEC_BODY/SEC_POSTFIX traffic. These captures and public certificate
metadata contain no private keys. With `--gui`, real WSLg client screenshots and
their logs are saved alongside the headless evidence. The suite uses
`FUSE_SCREENSHOT_DELAY=8` so both secure handshakes finish before capture; normal
automatic screenshots retain their two-second default (valid override: 0–60 s).
An incomplete trailing packet record caused by stopping the server is trimmed;
the retained captures must decode successfully in tshark.

For an independent network capture, run in another terminal while the demo runs:

```bash
sudo tcpdump -i lo -s0 -w /tmp/fuse-secure.pcap udp
```

## Troubleshooting

- **Missing Cyclone/plugin:** source `env.sh`; check `pkg-config` and `ldd` agree
  with the plugin installation. Override `FUSE_DDS_PLUGIN_DIR` only for a matching
  installation. Do not mix source-built and distribution DDS libraries.
- **Missing identity/key:** generate certificates on the provisioning machine;
  choose a client identity explicitly. Do not point at Windows-mounted key files.
- **Signature/purpose/validity error:** check clock time, CA certificates and
  signed policy files. Re-sign policy changes with `--refresh-policies`; preserve
  identity keys. A different CA requires a freshly provisioned credential bundle.
- **No peer match:** check both peers use the same CAs, governance and domain 0;
  plaintext peers are deliberately rejected. Cyclone errors go to stderr.
- **WSLg window fails:** verify `DISPLAY`/`WAYLAND_DISPLAY` are present. Headless
  probes exercise DDS independently of graphics.

References: [Cyclone configuration and signing](https://cyclonedds.io/docs/cyclonedds/latest/security/example_configuration.html),
[DDS security behavior](https://cyclonedds.io/docs/cyclonedds/0.10.3/security.html).
