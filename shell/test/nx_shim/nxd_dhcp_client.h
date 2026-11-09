/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host-only stand-in for NetX Duo's DHCP client header (issue #220): the four
 * interface-scoped calls shell/cmds/net_core.c makes, with NetX's status
 * values.  test_net_core.c defines them as a fake DHCP record (not started /
 * acquiring / bound) that refuses a start after a stop without a
 * reinitialise, as NetX documents.
 */
#ifndef NXD_DHCP_CLIENT_H
#define NXD_DHCP_CLIENT_H

#include "nx_api.h"

typedef struct NX_DHCP_STRUCT { int opaque; } NX_DHCP;

#define NX_DHCP_ALREADY_STARTED 0x93
#define NX_DHCP_NOT_BOUND       0x94
#define NX_DHCP_NOT_STARTED     0x96

UINT nx_dhcp_interface_start(NX_DHCP *dhcp_ptr, UINT iface_index);
UINT nx_dhcp_interface_stop(NX_DHCP *dhcp_ptr, UINT iface_index);
UINT nx_dhcp_interface_reinitialize(NX_DHCP *dhcp_ptr, UINT iface_index);
UINT nx_dhcp_interface_force_renew(NX_DHCP *dhcp_ptr, UINT iface_index);

#endif /* NXD_DHCP_CLIENT_H */
