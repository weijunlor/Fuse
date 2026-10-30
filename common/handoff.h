/* Select the threading handoff before starting either application worker. */
#ifndef COMMON_HANDOFF_H
#define COMMON_HANDOFF_H

#include <stdbool.h>

// FUSE_HANDOFF=spsc (default) or mutex (comparison). Invalid values fail startup.
bool handoff_init(void);
bool handoff_spsc(void);

#endif /* COMMON_HANDOFF_H */
