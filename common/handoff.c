#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "handoff.h"

// Written once before pthread_create; read-only for the lifetime of the threads.
static bool b_spsc = true;

bool handoff_init(void)
{
    const char *str_mode = getenv("FUSE_HANDOFF");
    if (str_mode == NULL || strcmp(str_mode, "spsc") == 0)
        b_spsc = true;
    else if (strcmp(str_mode, "mutex") == 0)
        b_spsc = false;
    else
    {
        fprintf(stderr, "FUSE_HANDOFF must be spsc or mutex\n");
        return false;
    }
    return true;
}

bool handoff_spsc(void)
{
    return b_spsc;
}
