#ifndef __APP_VELASIGHT_INCLUDE_VS_NETWORK_H
#define __APP_VELASIGHT_INCLUDE_VS_NETWORK_H

#include "vs_types.h"

struct vs_network_s;

int vs_network_open(struct vs_network_s **network);
int vs_network_request_mode(struct vs_network_s *network,
                            enum vs_net_mode_e mode);
int vs_network_get_status(struct vs_network_s *network,
                          struct vs_net_status_s *status);
void vs_network_close(struct vs_network_s *network);

/* Implemented later by the SoftAP server/configuration program. */

int vs_softap_service_start(void);
int vs_softap_service_stop(void);

#endif
