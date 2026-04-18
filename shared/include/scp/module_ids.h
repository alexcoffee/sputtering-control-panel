#ifndef SCP_MODULE_IDS_H
#define SCP_MODULE_IDS_H

#include <stdint.h>

/*
 * Central registry for all module IDs on the CAN bus.
 * Keep these unique system-wide.
 */
#define SCP_MODULE_ID_ROUGHING_PUMP 1U
#define SCP_MODULE_ID_PIRANI 2U
#define SCP_MODULE_ID_ION_GAUGE 3U
#define SCP_MODULE_ID_TURBO_PUMP 4U
#define SCP_MODULE_ID_MONITOR 11U

typedef uint8_t scp_module_id_t;

#endif
