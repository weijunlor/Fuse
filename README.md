# FUSE

Real-time multiplayer bomb-passing arena game built on Cyclone DDS, OpenGL and GLFW (CSD3116 team assignment).

Project plan, architecture and optimisation list: [docs/PLAN.md](docs/PLAN.md).

## Quick start (Windows / WSL)

With Windows 11 and an initialized Ubuntu WSL user, run this once on each PC:

```powershell
.\run-setup.bat
```

Choose **Host** or **Player**. Setup installs dependencies and secure DDS,
configures networking, provisions the host when needed and builds FUSE.
Then use `.\run-server.bat` on the host and `.\run-client.bat` on player PCs.
Pair each player once using the host IP and displayed code, then click **Join Game**.
Save WSL work before setup; it restarts WSL to apply network settings.
Options and prerequisites are described under **Run** below.

## Build (WSL / Ubuntu)

The complete setup under **Run** performs these steps automatically. This
section is for manual builds and troubleshooting. Generate CAs, certificates
and demo accounts on the host only; player PCs receive credentials by pairing.

Packages (libssl-dev: OpenSSL headers for the login code in `common/crypto.c`
and the certificate check in `common/dds_security.c`):

```bash
sudo apt install -y pkg-config libssl-dev libgl1-mesa-dev libglfw3-dev libstb-dev cyclonedds-dev cyclonedds-tools iproute2
```

On native Ubuntu 24.04 or later, the packaged Cyclone DDS 0.10.5 includes the
security plugins (in `/usr/lib/x86_64-linux-gnu/libddsc0debian/`, found by the
Makefile), so `setup_wsl.sh` below can be skipped. On WSL, `setup_wsl.sh`
installs these packages and builds the pinned Cyclone DDS from source.

```bash
bash security/scripts/setup_wsl.sh
source security/scripts/env.sh
bash security/scripts/make_ca.sh
bash security/scripts/make_certs.sh
python3 security/scripts/accounts.py demo  # once: private application accounts
make            # build/server, build/client
make baseline   # build/baseline_server, build/baseline_client (original programs)
```

The game build tracks the selected Cyclone DDS version and installation path.
After switching DDS installations, a normal `make` regenerates the message code
and rebuilds the game binaries; source `env.sh` to select the pinned WSL SDK.

## Run

On **each Windows PC**, run the complete setup once from this project folder:

```bat
.\run-setup.bat
```

Choose **Host** for the server PC or **Player** for a player PC. Setup configures
Windows/WSL networking, installs the Linux dependencies, builds the pinned
Cyclone DDS 0.10.5 with security, and builds the game, pairing helper and baseline.
Host setup also creates or validates the CAs, certificates and five login
accounts. Player setup leaves host credentials alone and uses in-game pairing.
Existing keys, passwords, account entries and saved pairings are preserved on reruns.

Windows 11 22H2 or later and an initialized Ubuntu WSL distribution with a
regular Linux user are required. Use the same default WSL distribution as the
launchers. Setup needs internet access and may request the Linux user's `sudo`
password to install packages. Accept the Windows Administrator prompt. Save
your work in all WSL terminals first: setup normally runs `wsl --shutdown`.
It adds or updates Windows
and WSL Hyper-V inbound rules for TCP 7440 and UDP 7400-7401,7410-7429, scoped
to `10.243.66.0/24` on all network profiles. It backs up your existing
`%USERPROFILE%\.wslconfig`, preserves unrelated settings, and sets:

```ini
[wsl2]
networkingMode=mirrored
localhostForwarding=true
firewall=false
```

`firewall=false` disables WSL's
Hyper-V firewall filtering; the Windows firewall rules remain subnet-scoped.
For a different subnet use `.\run-setup.bat -LanSubnet 192.168.1.0/24`.
Use `-Role Host` or `-Role Player` to skip the role prompt. `-NoRestart` defers
WSL shutdown but still installs and builds; shut down WSL before playing to
apply the networking settings. `-WhatIf` previews every stage without starting
WSL or installing anything (Host preview by default). `-SkipDependencies`
skips package/DDS installation when those tools are already available.
Rerunning setup updates the same named rules and preserves saved pairings.
Windows rules use `FUSEPairing` / `FUSEDDS`; WSL rules use
`FUSEPairing-WSL` / `FUSEDDS-WSL` to avoid colliding with inherited Windows rules.
Any firewall write failure stops setup before editing `.wslconfig` or shutting
down WSL; correct the reported error and rerun setup.
Any failed stage stops setup and reports the error. Correct it and rerun the
same command; setup does not reset existing accounts or device assignments.

On Windows, run these from separate terminals in this folder:

```bat
.\run-server.bat
.\run-client.bat
```

The batch files call the Linux scripts through your default WSL distribution.

On Linux or inside Ubuntu/WSL, use the same scripts directly in separate terminals:

```bash
bash run-server.sh
bash run-client.sh
```

For complete application setup directly in Linux, first run
`bash run-setup.sh --role host` or `bash run-setup.sh --role player` as your
regular Linux user. Windows users should use `run-setup.bat` so networking is
configured too. The Linux helper also accepts `--skip-dependencies`.

After successful setup, the launchers are ready to use.
Each script builds its target and the pairing helper, sources the DDS environment,
and starts its process with security enabled. The server displays a temporary
pairing code. On a new PC, the client opens a form for the host IP and that code;
after pairing, **Join Game** uses the saved connection. The host runs only the
server, leaving all five player identities available to remote PCs. Both
launchers accept `--no-build` to skip compilation; the batch files forward
these options to the Linux scripts.
Output stays in each terminal. Close a game window to stop that client; Ctrl+C
in the server terminal stops the server independently. Movement is WASD/arrows;
dash is Space/Shift. The game requires WSLg (or another graphical display).

To run each process manually in its own Ubuntu/WSL terminal:

```bash
source security/scripts/env.sh
bash security/scripts/run_secure.sh server ./build/server
bash security/scripts/run_secure.sh client-1 ./build/client   # use client-2..5 for others
```

DDS security is on by default. First-time certificate setup, the pinned WSL
dependency build, security tests and explicit plaintext comparisons are documented
in [security/README.md](security/README.md).
Application login setup, custom accounts and authentication tests are in
[security/AUTH.md](security/AUTH.md).

The Ninja Adventure sprites, atlas layout and regeneration instructions are in
[assets/README.md](assets/README.md).

Concurrency uses SPSC command queues and double-buffered snapshots by default.
Set `FUSE_HANDOFF=mutex` on both server and clients for the comparison path.
Both modes fix the player-ID race. See [concurrency checks and profiling](docs/CONCURRENCY.md).

Cache-line padding (#11) is a compile-time choice: `make PADDING=1` (default)
separates queue/buffer fields and particle worker results; `make PADDING=0`
uses compact fields. Switching the setting rebuilds affected targets without
requiring a clean. `make bench-padding` keeps both benchmark binaries side by
side. See [padding checks and measurements](docs/CONCURRENCY.md#cache-line-padding-11).

Move with arrows or WASD; press Space or either Shift key to dash. A dash
lasts 0.2 seconds with a 1-second cooldown and requires a fresh key press.
Touch another player to pass the bomb; it cannot be passed straight back for
0.5 seconds. Shields block one tag, speed pickups last 5 seconds, and fuse
pickups add 3 seconds to the remaining fuse. Each bomb explosion clears every
player's shield, speed boost and dash state, restoring their base stats.
Embers slow players for their 8-second lifetime.

With two or more connected players, the lobby stays open for 5 seconds, then
a 3-second countdown starts. New players wait for the next lobby once the
countdown begins. Each player starts with 3 lives. The last survivor wins;
after a 5-second win screen, connected players automatically enter a new
lobby with lives restored. Eliminated players spectate until the next match.

### Solo match against a bot

Complete **Host** setup above, including certificates and demo accounts.
Run the following commands from the project
folder in **two separate Ubuntu/WSL terminals**. If a server is already running,
stop it with **Ctrl+C** first.

1. In the first terminal, build and start the secure server with one bot:

```bash
source security/scripts/env.sh
make -j4 build/server
bash security/scripts/run_secure.sh server ./build/server --bots 1
```

2. In the second terminal, start your client:

```bash
bash run-client.sh --player 1
```

The client launcher builds the client and selects its DDS identity and login
credentials automatically. `--player 1` selects the `client-1` account; the
server assigns your in-game player slot separately.

3. Press **Enter** or click **Join Game** in the game window. You and the bot
   make two players, so the five-second lobby wait and three-second countdown
   lead into the match. Move with WASD/arrows and dash with Space/Shift.

Use `--bots 1` for a one-human, one-bot match. The server accepts `--bots 0..4`;
bots count toward the five-player limit. With two or more bots, they can start
playing before you join, so you may need to wait for the next admission lobby.

Pass `--bots` to `./build/server` through `run_secure.sh` as shown above.
`run-server.sh` and `run-server.bat` currently accept only `--no-build`, not
the `--bots` option. Close the client window to leave, and press **Ctrl+C** in
the server terminal to stop the server.

Server profiling options keep each slower path runnable:

| Option | Default | Comparison path |
|---|---|---|
| `--wait` | `sleep` | `busy` (#2, both server threads) |
| `--publish` | `unlocked` | `locked` (#1, snapshot publication) |
| `--collision` | `grid` | `brute` (#8, ember queries) |
| `--embers` | `ring` | `array` (#8b, ember storage: FIFO ring of SoA arrays vs fixed 4096-slot array) |
| `--branches` | `branchless` | `branchy` (#9, overlap/clamp/held-key lookup) |
| `--reduction` | `accum` | `serial` (#10, player counting) |

`--snapshot-hz H` selects a divisor of 60; `--player-timeout S` sets the
heartbeat timeout (5 seconds by default, 0 disables it). Bots use the same
game rules and are exempt from network timeouts.

## Play across computers on the same LAN

Run **one server** and pair each player PC once.
Windows/WSL and native Ubuntu computers can play together. Use the same game
revision on all computers. Connections stay on your LAN; no relay or internet
server is involved. Keep DDS encryption and application authentication enabled.

### One-time pairing (recommended)

1. On the host, complete `run-setup.bat -Role Host` (or
   `bash run-setup.sh --role host` on Ubuntu), then
   run `.\run-server.bat` (Windows) or `bash run-server.sh` (Ubuntu).
2. On each player PC, complete `run-setup.bat -Role Player` (or
   `bash run-setup.sh --role player` on Ubuntu). **Do not
   generate CAs or demo accounts on the player PC.** Run `.\run-client.bat` or
   `bash run-client.sh` without a player-number argument.
3. Enter the host's LAN IPv4 address and paste its complete `FUSE-...` code into
   the game's **Pair this player** screen. Click **Pair**. The host assigns an unused
   identity automatically; the game validates and privately saves its credentials.
4. Click **Join Game**. On subsequent launches, just click **Join Game** again.

For multiple players on one PC/WSL user, run the client launcher once per game
window. Each open window uses a separate saved player profile. The first keeps
your existing pairing; the second through fifth windows pair once with the
host's next code, then reuse their own credentials on later launches. Do not
copy the first player's credentials into another profile: simultaneous logins
with the same account are rejected. After updating from the older launcher,
close all client windows once before reopening them.

The code is a random, single-use, five-minute secret. Pairing uses mutually
authenticated TLS 1.3 with ephemeral key exchange on TCP **7440**; it does not
trust an arbitrary self-signed certificate. Share the displayed code only with
the PC you intend to pair. Gameplay continues to use encrypted DDS over UDP.
This is game-specific credential enrollment, not an RFC 7030 EST implementation.
Keep the generated client identities and password files on the provisioning host;
pairing reads these files and never sends CA private keys. Codes are displayed
on the controlling terminal and excluded from redirected game logs.

In the host terminal, `pair` replaces the current code, `cancel` cancels pending
pairing, and `devices` lists assignments. The next code appears after successful
pairing until all five identities are assigned. Pairing failures leave the game
server running. `--no-pairing` starts the server without the enrollment service.

Each player profile retains a private Ed25519 device key, so retrying interrupted
pairing with a fresh host code recovers that profile's existing identity. Host
assignments survive server restarts. Identities are never automatically recycled;
a sixth player receives a full-capacity message. Replacing a PC whose private data has been lost
requires deliberate host reprovisioning. Deleting the registry alone does not
invalidate credentials previously given to that PC.

Client profiles live under `~/.local/share/fuse/pairings/`, grouped by host CA
fingerprint; `FUSE_PAIRING_DIR` can select another private Linux directory. The
host registry is `<FUSE_SECURITY_DIR>/pairing/devices.json`. Private directories
use mode 700 and files mode 600, outside Windows mounts and the source checkout.
Additional local players' profiles live in `players/2` through `players/5` below
the pairing directory. Closing a window releases its local launch slot; its
device key, credentials and host assignment remain saved.
Failed/cancelled enrollment preserves an existing working connection. **Pair with
another host** opens the form again; the last successful profile becomes active.

Use `python3 security/scripts/lan.py check` to test the saved connection while
the game is logged out. LAN reachability is still required: follow the Windows/
Ubuntu networking steps below if WSL NAT, firewall rules or Wi-Fi isolation block
connections. On the host, include `-PairingHost` when firewall changes are needed.

A client running on the host computer can use the same LAN IP as the other PCs.
It selects the adapter that owns that address; other clients select the adapter
on their route to the host. If an older client repeatedly printed `Join request
sent` after pairing, close and relaunch it to regenerate the connection settings,
then click **Join Game**. Its saved credentials do not need to be paired again.

### Manual credential transfer (optional)

The existing `--player 1..5` launch option and bundle tools remain available for
manually managed setups and benchmarks. Use `--no-pairing` for a manually managed
group so its identities are not also allocated by the automatic pairing registry.

**1. Provision on the host only.** Follow the first-time Build/account setup above
on the host, then export one personal bundle for each remote player in Ubuntu/WSL:

```bash
source security/scripts/env.sh
python3 security/scripts/lan.py export --player 2 --output "$HOME/player-2.fuse-lan"
```

Bundles contain that player's password, identity key/certificate, signed permissions,
governance and public CA certificates. They exclude the CA private keys, host account
database and other players' credentials. Transfer each bundle privately to its intended
player (for example using SSH or a USB drive), then delete the transfer copies.
Bundles must be exported outside the source checkout and are ignored by Git.

**2. Import on each client.** Install the Build dependencies and build the game on
that computer, but **do not generate new CAs or demo accounts there**. Import the
host's bundle instead, from an Ubuntu/WSL terminal in the project directory:

```bash
source security/scripts/env.sh
python3 security/scripts/lan.py import /path/to/player-2.fuse-lan
make -j4
make build/dds_security_probe
```

Import validates the certificates and signed policies, installs private files on
the Linux filesystem, and regenerates runtime paths for that computer. Reimporting
an identical bundle is safe. Conflicting credentials are refused before installation.
If the client already has independently generated credentials, preserve those and
select fresh directories with `FUSE_SECURITY_DIR` and `FUSE_AUTH_DIR`; export the
same variables in every terminal used to import, check or launch that client.

**3. Start the host and connect.** The server prints available addresses and client
commands. Use its LAN IPv4 address, such as `192.168.1.50`:

```bat
REM On the host:
.\run-server.bat
REM On the second computer:
.\run-client.bat --player 2 --server 192.168.1.50
```

On native Ubuntu use `bash run-server.sh` and
`bash run-client.sh --player 2 --server 192.168.1.50`. Click **Join Game** in the
existing menu. The host can also run a client with its own unused player number.
Explicit server connections use unicast DDS discovery, so multicast is unnecessary.
Omitting `--server` retains automatic discovery. Both launchers accept
`--interface NAME` (the Linux interface name, such as `eth0` or `wlan0`); otherwise
an explicit-server client selects the interface from its route to that server.
The host automatically selects the usable IPv4 adapter on its lowest-metric
default route, using the same adapter for DDS and pairing. For example, if WSL
routes your LAN through `eth3`, no interface argument is needed. With no usable
default route, a sole LAN adapter is selected; if several remain, pairing asks
for `--interface NAME`. An explicit flag or `FUSE_DDS_INTERFACE` overrides
automatic selection, which is useful when a VPN changes the default route.

**4. Check connectivity before changing the network.** With the host running, run
this on a client using its assigned player number:

```bash
source security/scripts/env.sh
python3 security/scripts/lan.py check --server 192.168.1.50 --player 2
```

Run the check while that account is logged out of the game. It takes about 12 seconds
and checks routing, WSL mode, credentials, secure DDS discovery, authenticated joining,
movement, snapshots and Leave. Login rejection means the server was reached; import
the matching host bundle. A full/in-progress lobby is reported separately. A discovery
timeout can mean a wrong address, stopped server, different CAs, routing, a firewall
or Wi-Fi client isolation; it does not establish which one caused the failure.

**Windows/WSL setup, only if needed:** from PowerShell in the project directory:

```powershell
.\security\scripts\setup_lan.ps1                  # inspect only; no admin needed
.\security\scripts\setup_lan.ps1 -EnableMirrored  # only if WSL currently uses NAT
```

NAT addresses printed inside WSL normally cannot be reached from another computer.
Mirrored networking requires Windows 11 22H2 or later and a current WSL installation.
The helper backs up `.wslconfig` and preserves unrelated settings. After enabling it,
save your work in **all** WSL terminals, run `wsl --shutdown`, reopen Ubuntu, restart
the host, and retry the check. The helper never shuts down WSL automatically.
See [Microsoft's WSL networking documentation](https://learn.microsoft.com/en-us/windows/wsl/networking#mirrored-mode-networking).

If connectivity checks indicate firewall filtering after the address, routing and
credentials have been checked, run this in an **Administrator PowerShell** on the
affected Windows computer, substituting your actual LAN subnet:

```powershell
.\security\scripts\setup_lan.ps1 -ConfigureFirewall -PairingHost -LanSubnet 192.168.1.0/24
```

This adds/updates only the `FUSE-LAN-UDP` Windows rule and `FUSE-LAN-UDP-WSL`
Hyper-V rule, scoped to that subnet and **Private** networks. It allows inbound
UDP 7400–7401 and 7410–7429 (domain 0, participant indices 0–9), and preserves
firewall defaults. Use a trusted Private network; the helper does not change the
network's category. `-PairingHost` additionally permits TCP 7440 in host-only rules
`FUSE-LAN-PAIR-TCP` and `FUSE-LAN-PAIR-TCP-WSL`; omit it on player PCs. `-WhatIf`
previews either setup operation.

On Ubuntu, inspect `sudo ufw status` first. If UFW is active and blocking the game,
allow only your LAN subnet and the same UDP ports:

```bash
sudo ufw allow from 192.168.1.0/24 to any port 7400:7401 proto udp
sudo ufw allow from 192.168.1.0/24 to any port 7410:7429 proto udp
# On the pairing host only:
sudo ufw allow from 192.168.1.0/24 to any port 7440 proto tcp
```

Guest/school Wi-Fi may block direct device-to-device traffic. Use a LAN or hotspot
that permits it. One host per session is supported; the server IP is a DDS discovery
address, not a separate room identifier. Direct launch/configuration also accepts
`FUSE_SERVER_IP` and `FUSE_DDS_INTERFACE`; launcher flags override these variables.

LAN regressions (inside Ubuntu/WSL after building the probe):

```bash
python3 security/scripts/test_lan.py -v
unshare -Urn python3 security/scripts/test_lan.py --integration
make test-pairing
unshare -Urn python3 security/scripts/test_pairing.py --integration
```

The second command uses temporary user/network namespaces and a private virtual
Ethernet link, tests unicast gameplay and rejoining without changing the host's
interfaces/firewall, and requires unprivileged user namespaces to be available.
The pairing integration also joins three windows sharing one local profile root
alongside two remote players, then restarts all five using saved profiles, with
automatic adapter selection and multicast disabled.
It supplements a real two-computer test; run `lan.py check` on the second computer
and verify both game windows show the same players and movement.

## Checks

```bash
make test           # deterministic game/lobby/bot/collision/input regressions
make test-network   # isolated real DDS scenarios; no window required
make bench          # collision, branch, reduction and particle benchmarks
python3 -m unittest discover -s tests -p 'test_setup_*.py' -v  # isolated setup checks
```

Windows setup checks (mocked networking/UAC/WSL; no machine changes):

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/test_run_setup.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/test_full_setup.ps1
```

## Layout

| Folder | Contents |
|---|---|
| `idl/` | DDS message definitions |
| `generated/` | idlc output (do not edit) |
| `common/` | Code shared by server and client |
| `server/`, `client/` | Each program, one module per system |
| `third_party/` | External code (stb_easy_font) |
| `security/` | DDS Security XML, certificate scripts |
| `bench/` | Microbenchmarks and results for the optimisation slides |
| `baseline/` | Original lecturer programs, untouched, for before/after measurements |
| `docs/` | Diagrams, slides, AI use log |

## Team

| Member | Responsibilities |
|---|---|
| Zhikai | Server, game rules, lobby, bots, collision and reduction benchmarks |
| Weijun | Client, rendering, assets and performance measurements |
| Sheeyang | Security and concurrency |

## AI use

See `docs/ai_log.md`.
