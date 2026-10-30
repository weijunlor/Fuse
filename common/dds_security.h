/* DDS participant identity and startup policy (separate from game login). */
#ifndef COMMON_DDS_SECURITY_H
#define COMMON_DDS_SECURITY_H

#include "dds/dds.h"

typedef enum
{
    NET_ROLE_SERVER,
    NET_ROLE_CLIENT
} NetRole;

// Call from main before starting threads: comparison mode clears DDS XML.
void dds_security_prepare(void);

/* Secure by default. Errors terminate the process, including when called
from a worker, so the main/render thread cannot continue after DDS failed. */
dds_entity_t dds_security_create_participant(NetRole a_role);

#endif /* COMMON_DDS_SECURITY_H */
