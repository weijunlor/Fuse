/* Exercise the production client worker without opening an OpenGL window. */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <signal.h>
#include "net_client.h"
#include "types.h"

uint8_t u8_keys;

int main(void)
{
    if (!net_client_prepare()) return 2;
    pthread_t thread;
    if (pthread_create(&thread, NULL, net_client_worker, NULL) != 0) return 2;
    int int_id = -1, int_first_x = -1, int_last_x = -1;
    bool b_moved = false;
    bool b_rejoin_test = getenv("FUSE_TEST_REJOIN") != NULL, b_paused = false;
    int int_after_resume = 0;
    for (int i = 0; i < 1000; i++)
    {
        usleep(10000);
        pthread_mutex_lock(&mutex);
        int_id = int_player_id;
        if (int_id >= 1 && int_id <= MAX_PLAYERS && arr_Players[int_id - 1].b_active)
        {
            int_last_x = arr_Players[int_id - 1].x;
            if (int_first_x < 0) int_first_x = int_last_x;
            u8_keys = int_first_x > 400 ? KEY_LEFT : KEY_RIGHT;
            if (int_last_x != int_first_x) b_moved = true;
        }
        pthread_mutex_unlock(&mutex);
        if (b_moved && b_rejoin_test && !b_paused)
        {
            b_paused = true;
            puts("worker_ready_to_pause=1"); fflush(stdout);
            raise(SIGSTOP); // acceptance harness resumes after the server times us out
        }
        if (b_paused) int_after_resume++;
        if (b_moved && (!b_rejoin_test || int_after_resume >= 300)) break;
    }
    net_client_stop(); pthread_join(thread, NULL);
    printf("worker_id=%d moved=%d\n", int_id, b_moved);
    return b_moved ? 0 : 1;
}
