#!/usr/bin/env bash
# Joining under packet loss: Cyclone's default QoS vs per-topic QoS (#14),
# without and with the join nonce.
#
# Usage: bench/join_loss.sh <label>      (needs build/server and build/dds_joiner)
#
# The loopback interface is made to drop a share of all packets with
#   sudo tc qdisc add dev lo root netem loss <L>%
# (removed again after every trial and on exit). For each mode in MODES and
# each loss rate, TRIALS times: start
# the server, start JOINERS headless clients (build/dds_joiner, same join code
# and QoS as the real client) STAGGER seconds apart, let each run for STAY
# seconds, then collect what each one saw.
#
# Settings (environment): LOSSES ["0 10 30"] percent, TRIALS [10], JOINERS [4],
#   STAGGER [1.5] s (0 = all joiners at the same instant), STAY [10] s,
#   MODES ["default qos nonce"], each one setting the environment of server and
#   joiners:
#     default  FUSE_QOS=default FUSE_JOIN_NONCE=off  (Cyclone QoS, baseline join)
#     qos      FUSE_JOIN_NONCE=off                   (#14 QoS, baseline join)
#     nonce    (nothing)                             (#14 QoS + join nonce)
#   (results before the nonce existed used "tuned" for today's "qos")
#
# Results: bench/results/<label>/setup.txt, report.txt, raw/<mode>_loss<L>_<trial>.txt
# (one dds_joiner line per joiner). Per mode and loss rate the report gives:
#   joined      joiners that got a player ID within STAY s
#   join_ms     first JoinRequest to accepted JoinResponse (median, 90th pct, max)
#   requests    JoinRequests sent per joined client (1 = first one answered)
#   ghosts      player slots the server filled that no joiner holds (a response
#               was lost, the client asked again and got a second slot)
#   dup_ids     joiners holding an ID another joiner also holds (without the
#               nonce every client takes the first response it sees)
#   snaps/s     GameSnapshots received per second by a joined client (60 sent)

set -euo pipefail
cd "$(dirname "$0")/.."

if [ $# -ne 1 ]; then
    sed -n '/^# Usage/,/^$/p' "$0" >&2
    exit 1
fi
LABEL=$1
LOSSES=${LOSSES:-0 10 30}
TRIALS=${TRIALS:-10}
JOINERS=${JOINERS:-4}
STAGGER=${STAGGER:-1.5}
STAY=${STAY:-10}
MODES=${MODES:-default qos nonce}
source bench/security_env.sh
dds_check_clients "$JOINERS"

# A password proof requires a nonce, so disabling nonces is meaningful only
# with both security layers explicitly off. Never silently measure the same
# authenticated behavior under the labels "default", "qos" and "nonce".
if [ "${FUSE_AUTH:-on}" != off ] && [[ " $MODES " =~ [[:space:]](default|qos|tuned)[[:space:]] ]]; then
    echo 'Nonce-off comparisons require FUSE_DDS_SECURITY=off FUSE_AUTH=off; use MODES=nonce to test authenticated joins.' >&2
    exit 1
fi

for bin in build/server build/dds_joiner; do
    [ -x "$bin" ] || { echo "Missing $bin (make && make build/dds_joiner)" >&2; exit 1; }
done
if sudo tc qdisc show dev lo | grep -q netem; then
    echo "lo already has a netem qdisc; remove it first: sudo tc qdisc del dev lo root" >&2
    exit 1
fi

OUT=bench/results/$LABEL
RAW=$OUT/raw
mkdir -p "$RAW"

# Never leave the loopback dropping packets, whatever happens
clear_loss() { sudo tc qdisc del dev lo root 2> /dev/null || true; }
int_server_pid=0
arr_joiner_pids=()
cleanup() {
    [ "$int_server_pid" -eq 0 ] || kill "$int_server_pid" 2>/dev/null || true
    for int_pid in "${arr_joiner_pids[@]}"; do kill "$int_pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
    clear_loss
}
trap cleanup EXIT

source bench/setup_info.sh
write_setup "$LABEL" build/server build/dds_joiner > "$OUT/setup.txt"
printf '# %s  MODES="%s" LOSSES="%s" TRIALS=%s JOINERS=%s STAGGER=%s STAY=%s\n' "$(date -Iseconds)" \
    "$MODES" "$LOSSES" "$TRIALS" "$JOINERS" "$STAGGER" "$STAY" > "$RAW/commands.txt"

for str_qos in $MODES; do
    for int_loss in $LOSSES; do
        for int_trial in $(seq "$TRIALS"); do
            echo "QoS $str_qos, loss $int_loss%, trial $int_trial/$TRIALS"
            str_file=$RAW/${str_qos}_loss${int_loss}_$int_trial.txt
            unset FUSE_QOS FUSE_JOIN_NONCE
            case $str_qos in
                default) export FUSE_QOS=default FUSE_JOIN_NONCE=off ;;
                qos | tuned) export FUSE_JOIN_NONCE=off ;;
                nonce) ;;
                *) echo "Unknown mode $str_qos" >&2; exit 1 ;;
            esac
            # Loss applies to the whole trial, discovery included
            [ "$int_loss" = 0 ] || sudo tc qdisc add dev lo root netem loss "$int_loss%"
            dds_run server build/server > /dev/null 2>&1 &
            int_server_pid=$!
            sleep 0.5
            : > "$str_file"
            arr_joiner_pids=()
            for int_client in $(seq "$JOINERS"); do
                dds_run "client-$int_client" build/dds_joiner "$STAY" 2> /dev/null >> "$str_file" &
                arr_joiner_pids+=($!)
                sleep "$STAGGER"
            done
            for int_pid in "${arr_joiner_pids[@]}"; do wait "$int_pid"; done
            arr_joiner_pids=()
            kill "$int_server_pid" || true
            wait "$int_server_pid" 2>/dev/null || true
            int_server_pid=0
            sleep 0.5
            clear_loss
        done
    done
done
unset FUSE_QOS FUSE_JOIN_NONCE

# Summary table
python3 - "$RAW" "$LOSSES" "$TRIALS" "$JOINERS" "$STAY" "$MODES" "$STAGGER" > "$OUT/report.txt" <<'EOF'
import glob, re, statistics, sys
raw, losses, trials, joiners, stay = sys.argv[1], sys.argv[2].split(), int(sys.argv[3]), int(sys.argv[4]), float(sys.argv[5])
modes, stagger = sys.argv[6].split(), sys.argv[7]
def pct(v, p):
    v = sorted(v); return v[min(len(v) - 1, int(p * len(v)))] if v else float('nan')
print('Joining under packet loss (bench/join_loss.sh)')
print(f'{trials} trials x {joiners} headless clients per cell, started {stagger} s apart, each staying {stay:.0f} s;')
print('loss = share of all loopback packets dropped by netem (both directions)\n')
print(f'{"QoS":8} {"loss":>4} {"joined":>8} {"join_ms p50":>11} {"p90":>6} {"max":>6} {"requests":>8} {"ghosts":>7} {"dup_ids":>7} {"snaps/s":>7}')
for qos in modes:
    for loss in losses:
        joined = total = ghosts = dups = 0
        ms, reqs, snaps = [], [], []
        for f in sorted(glob.glob(f'{raw}/{qos}_loss{loss}_*.txt')):
            rows = [dict(kv.split('=') for kv in l.split()) for l in open(f) if l.startswith('joined=')]
            total += joiners
            ids = [int(r['id']) for r in rows if r['joined'] == '1']
            joined += len(ids)
            dups += len(ids) - len(set(ids))
            active = max([int(r['active']) for r in rows] + [0])
            ghosts += max(0, active - len(set(ids)))
            for r in rows:
                if r['joined'] == '1':
                    ms.append(float(r['join_ms'])); reqs.append(int(r['requests']))
                    snaps.append(int(r['snapshots']) / stay)
        print(f'{qos:8} {loss + "%":>4} {f"{joined}/{total}":>8} {pct(ms, .5):11.0f} {pct(ms, .9):6.0f} {max(ms or [0]):6.0f}'
              f' {statistics.mean(reqs or [0]):8.2f} {ghosts:7d} {dups:7d} {statistics.mean(snaps or [0]):7.1f}')
    print()
print('ghosts: slots the server filled that no client holds (summed over trials)')
print('dup_ids: clients holding an ID another client also holds (fixed by the join nonce, not by QoS)')
print('modes: default = Cyclone QoS, no nonce; qos (tuned) = #14 QoS, no nonce; nonce = #14 QoS + join nonce')
print('snaps/s: includes the time before joining, so slightly under the received rate')
EOF
cat "$OUT/report.txt"
