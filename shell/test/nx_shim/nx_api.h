/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 *
 * Host-only stand-in for NetX Duo's nx_api.h, for the shared NetX operations'
 * test (shell/test/test_net_core.c over shell/cmds/net_core.c, issue #220)
 * and the telnet console's (shell/test/test_net_tty.c over
 * shell/cmds/net_tty.c, issue #221).  Only what those files call is declared,
 * and each test defines the calls its file makes as fakes that record them.
 * The status and state values are NetX's; the structures carry only the
 * members the shared files read.
 */
#ifndef NX_API_H
#define NX_API_H

#include "tx_api.h"

/* Only the member the link notification writes (NetX's own interface flag). */
typedef struct NX_INTERFACE_STRUCT { UCHAR nx_interface_link_up; } NX_INTERFACE;
typedef struct NX_IP_STRUCT { NX_INTERFACE nx_ip_interface[2]; } NX_IP;
typedef struct NX_PACKET_STRUCT { ULONG nx_packet_length; void *fake; } NX_PACKET;
typedef struct NX_PACKET_POOL_STRUCT { ULONG nx_packet_pool_available; } NX_PACKET_POOL;
typedef struct NX_TCP_SOCKET_STRUCT { UINT nx_tcp_socket_state; } NX_TCP_SOCKET;

/* The boards' nx_user.h settings shell/cmds/net_tty.c requires (it refuses to
 * build without them); this stand-in has both callbacks. */
#define NX_ENABLE_TCP_QUEUE_DEPTH_UPDATE_NOTIFY
#define NX_ENABLE_EXTENDED_NOTIFY_SUPPORT

#define NX_NULL        ((void *)0)
#define NX_NO_WAIT     0
#define NX_WAIT_FOREVER ((ULONG)0xFFFFFFFF)
#define NX_SUCCESS     0x00
#define NX_NO_PACKET   0x01
#define NX_NO_RESPONSE 0x29   /* nx_api.h: no echo reply within the wait */
#define NX_NOT_CLOSED       0x35
#define NX_NOT_LISTEN_STATE 0x36
#define NX_NOT_CONNECTED    0x38
#define NX_WINDOW_OVERFLOW  0x39
#define NX_ALREADY_BOUND    0x22
#define NX_CONNECTION_PENDING 0x48
#define NX_TX_QUEUE_DEPTH   0x49

#define NX_IP_NORMAL        ((ULONG)0x00000000)
#define NX_FRAGMENT_OKAY    ((ULONG)0x00000000)
#define NX_IP_TIME_TO_LIVE  ((ULONG)0x00000080)
#define NX_TCP_PACKET       ((ULONG)56)

#define NX_TCP_CLOSED        1
#define NX_TCP_LISTEN_STATE  2
#define NX_TCP_SYN_RECEIVED  4
#define NX_TCP_ESTABLISHED   5
#define NX_TCP_CLOSE_WAIT    6

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
UINT nx_ip_link_status_change_notify_set(NX_IP *ip_ptr,
        VOID (*link_status_change_notify)(NX_IP *ip_ptr, UINT interface_index,
                                          UINT link_up));

/* The TCP server socket and packet calls of the telnet console. */
UINT nx_tcp_socket_create(NX_IP *ip_ptr, NX_TCP_SOCKET *socket_ptr, CHAR *name,
                          ULONG type_of_service, ULONG fragment,
                          UINT time_to_live, ULONG window_size,
                          VOID (*urgent_data_callback)(NX_TCP_SOCKET *socket_ptr),
                          VOID (*disconnect_callback)(NX_TCP_SOCKET *socket_ptr));
UINT nx_tcp_socket_receive_notify(NX_TCP_SOCKET *socket_ptr,
        VOID (*tcp_receive_notify)(NX_TCP_SOCKET *socket_ptr));
UINT nx_tcp_socket_establish_notify(NX_TCP_SOCKET *socket_ptr,
        VOID (*tcp_establish_notify)(NX_TCP_SOCKET *socket_ptr));
UINT nx_tcp_socket_queue_depth_notify_set(NX_TCP_SOCKET *socket_ptr,
        VOID (*tcp_socket_queue_depth_notify)(NX_TCP_SOCKET *socket_ptr));
UINT nx_tcp_socket_transmit_configure(NX_TCP_SOCKET *socket_ptr,
                                      ULONG max_queue_depth, ULONG timeout,
                                      ULONG max_retries, ULONG timeout_shift);
UINT nx_tcp_server_socket_listen(NX_IP *ip_ptr, UINT port,
                                 NX_TCP_SOCKET *socket_ptr, UINT listen_queue_size,
                                 VOID (*tcp_listen_callback)(NX_TCP_SOCKET *socket_ptr,
                                                             UINT port));
UINT nx_tcp_server_socket_accept(NX_TCP_SOCKET *socket_ptr, ULONG wait_option);
UINT nx_tcp_server_socket_unaccept(NX_TCP_SOCKET *socket_ptr);
UINT nx_tcp_server_socket_relisten(NX_IP *ip_ptr, UINT port,
                                   NX_TCP_SOCKET *socket_ptr);
UINT nx_tcp_socket_disconnect(NX_TCP_SOCKET *socket_ptr, ULONG wait_option);
UINT nx_tcp_socket_receive(NX_TCP_SOCKET *socket_ptr, NX_PACKET **packet_ptr,
                           ULONG wait_option);
UINT nx_tcp_socket_send(NX_TCP_SOCKET *socket_ptr, NX_PACKET *packet_ptr,
                        ULONG wait_option);
UINT nx_packet_allocate(NX_PACKET_POOL *pool_ptr, NX_PACKET **packet_ptr,
                        ULONG packet_type, ULONG wait_option);
UINT nx_packet_data_append(NX_PACKET *packet_ptr, VOID *data_start, ULONG data_size,
                           NX_PACKET_POOL *pool_ptr, ULONG wait_option);
UINT nx_packet_data_extract_offset(NX_PACKET *packet_ptr, ULONG offset,
                                   VOID *buffer_start, ULONG buffer_length,
                                   ULONG *bytes_copied);

#endif /* NX_API_H */
