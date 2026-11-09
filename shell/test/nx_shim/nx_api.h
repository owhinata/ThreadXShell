/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host-only stand-in for NetX Duo's nx_api.h, for the shared NetX operations'
 * test (shell/test/test_net_core.c over shell/cmds/net_core.c, issue #220).
 * Only what net_core.c calls is declared; test_net_core.c defines every one
 * of them as a fake that records the call.  The status values are NetX's.
 */
#ifndef NX_API_H
#define NX_API_H

#include "tx_api.h"

typedef struct NX_IP_STRUCT { int opaque; } NX_IP;
typedef struct NX_PACKET_STRUCT { int opaque; } NX_PACKET;

#define NX_NULL        ((void *)0)
#define NX_SUCCESS     0x00
#define NX_NO_RESPONSE 0x29   /* nx_api.h: no echo reply within the wait */

UINT nx_ip_interface_address_get(NX_IP *ip_ptr, UINT interface_index,
                                 ULONG *ip_address, ULONG *network_mask);
UINT nx_ip_interface_address_set(NX_IP *ip_ptr, UINT interface_index,
                                 ULONG ip_address, ULONG network_mask);
UINT nx_ip_gateway_address_get(NX_IP *ip_ptr, ULONG *ip_address);
UINT nx_ip_gateway_address_set(NX_IP *ip_ptr, ULONG ip_address);
UINT nx_ip_gateway_address_clear(NX_IP *ip_ptr);
UINT nx_icmp_ping(NX_IP *ip_ptr, ULONG ip_address, CHAR *data, ULONG data_size,
                  NX_PACKET **response_ptr, ULONG wait_option);
UINT nx_packet_release(NX_PACKET *packet_ptr);

#endif /* NX_API_H */
