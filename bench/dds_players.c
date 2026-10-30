/* bench/dds_players.c
Headless test players (#2, game rules): joins 2 players from one process
(two nonces), player 1 holds <keys1> and player 2 holds <keys2> (KEY_* bitmask
from common/types.h, sent as an Input every 100 ms), then prints every phase,
bomb holder, lives and explosion change seen in the snapshots and, at the end,
statistics of the gaps between snapshots. The gaps use each snapshot's source
timestamp (when the server called dds_write), so they show how evenly the
server publishes, independent of this program's own scheduling.

Usage: dds_players <seconds> [keys1] [keys2]   (default 8 = right, 4 = left)
  e.g. dds_players 90 10 10: both hold right+down, meet in the corner and pass
  the bomb back and forth for a whole game.
Last line:
  snapshots=<n> gap_ms mean=.. sd=.. min=.. max=.. gaps>25ms=<n> tick_skips=<n>
  (tick_skips: snapshots whose tick is not the previous one + 1, i.e. a
  snapshot the server never sent; at --snapshot-hz below 60 every one counts)

Run it on its own DDS domain to keep away from a game already running:
  CYCLONEDDS_URI='<Domain Id="42"/>' build/server &
  CYCLONEDDS_URI='<Domain Id="42"/>' build/dds_players 20 10 10 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "net.h"
#include "fuse.h"
#include "types.h"
#include "config.h"

int main(int argc, char **argv)
{
    double d_seconds = argc > 1 ? atof(argv[1]) : 10.0;
    int arr_keys[2] = { argc > 2 ? atoi(argv[2]) : KEY_RIGHT, argc > 3 ? atoi(argv[3]) : KEY_LEFT };
    NetTopics net;
    net_create_topics(&net);
    dds_entity_t join_writer = net_create_writer(&net, net.join_topic, NET_RELIABLE_ALL);
    dds_entity_t response_reader = net_create_reader(&net, net.response_topic, NET_RELIABLE_ALL);
    dds_entity_t snapshot_reader = net_create_reader(&net, net.snapshot_topic, NET_BEST_EFFORT);
    dds_entity_t input_writer = net_create_writer(&net, net.input_topic, NET_BEST_EFFORT);

    dds_entity_t ws = dds_create_waitset(net.participant);
    dds_entity_t cond = dds_create_readcondition(snapshot_reader, DDS_ANY_STATE);
    dds_waitset_attach(ws, cond, 0);

    void *arr_p_resp[JOIN_TAKE]; dds_sample_info_t infos[JOIN_TAKE];
    for (int i = 0; i < JOIN_TAKE; i++) arr_p_resp[i] = JoinResponse__alloc();
    void *p_snap = GameSnapshot__alloc(); dds_sample_info_t info;

    JoinRequest req[2] = {0};
    int ids[2] = {0, 0};
    req[0].nonce = net_random_nonce(); req[1].nonce = net_random_nonce();
    dds_time_t start = dds_time(), end = start + (dds_duration_t)(d_seconds * 1e9);
    dds_time_t last_req = start, last_input = 0;
    dds_sleepfor(DDS_MSECS(1000));

    int n = 0; double sum = 0, sum2 = 0, gmin = 1e9, gmax = 0; dds_time_t prev_src = 0;
    int phase = -1, holder = -1, lives[2] = {-1, -1}; unsigned long expl = 0;
    int big_gaps = 0; uint32_t prev_tick = 0; int tick_skips = 0;
    while (dds_time() < end)
    {
        dds_time_t now = dds_time();
        if ((ids[0] == 0 || ids[1] == 0) && now - last_req > DDS_MSECS(500))
        {
            for (int k = 0; k < 2; k++) if (!ids[k]) dds_write(join_writer, &req[k]);
            last_req = now;
        }
        int rc = dds_take(response_reader, arr_p_resp, infos, JOIN_TAKE, JOIN_TAKE);
        for (int i = 0; i < rc; i++)
        {
            const JoinResponse *r = arr_p_resp[i];
            if (!infos[i].valid_data) continue;
            for (int k = 0; k < 2; k++)
                if (r->nonce == req[k].nonce && r->status == JOIN_OK && !ids[k])
                {
                    ids[k] = r->player_id;
                    printf("%6.2f s  joined as player %d (%.0f ms after start)\n",
                           (now - start) / 1e9, ids[k], (now - start) / 1e6);
                }
        }
        if (now - last_input > DDS_MSECS(100))
        {
            for (int k = 0; k < 2; k++) if (ids[k])
            {
                Input in = { .player_id = (uint8_t)ids[k], .keys = (uint8_t)arr_keys[k] };
                dds_write(input_writer, &in);
            }
            last_input = now;
        }

        dds_attach_t trig;
        dds_waitset_wait(ws, &trig, 1, DDS_MSECS(20));
        while (dds_take(snapshot_reader, &p_snap, &info, 1, 1) > 0)
        {
            if (!info.valid_data) continue;
            const GameSnapshot *s = p_snap;
            double t = (dds_time() - start) / 1e9;
            if (prev_src)
            {
                double g = (info.source_timestamp - prev_src) / 1e6;
                n++; sum += g; sum2 += g * g;
                if (g < gmin) gmin = g;
                if (g > gmax) gmax = g;
                if (g > 25) big_gaps++;
                if (s->tick != prev_tick + 1) tick_skips++;
            }
            prev_src = info.source_timestamp; prev_tick = s->tick;
            if (s->phase != phase)
                printf("%6.2f s  tick %u phase %d timer %u ms\n", t, s->tick, s->phase, s->timer_ms), phase = s->phase;
            if (s->bomb_holder != holder)
                printf("%6.2f s  bomb holder %d\n", t, s->bomb_holder), holder = s->bomb_holder;
            for (int k = 0; k < 2; k++) if (ids[k])
            {
                const PlayerState *p = &s->players[ids[k] - 1];
                if (p->lives != lives[k])
                    printf("%6.2f s  player %d lives %d flags 0x%x at (%d,%d)\n", t, ids[k], p->lives, p->flags, p->x, p->y), lives[k] = p->lives;
            }
            if (s->explosions[0].tick != expl)
                printf("%6.2f s  explosion at tick %u (%d,%d)\n", t, s->explosions[0].tick, s->explosions[0].x, s->explosions[0].y), expl = s->explosions[0].tick;
        }
    }
    double mean = n ? sum / n : 0;
    printf("snapshots=%d gap_ms mean=%.3f sd=%.3f min=%.3f max=%.3f gaps>25ms=%d tick_skips=%d\n",
           n + 1, mean, n ? sqrt(sum2 / n - mean * mean) : 0, gmin, gmax, big_gaps, tick_skips);
    return 0;
}
