/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nx_glue.h
 * @brief   NetX Duo IPv4 bring-up + diagnostics facade
 * (owhinata/stm32f746g-disco#49 P2).
 *
 * Owns the single NetX IP instance over the STM32 ETH MAC: the shared
 * non-cacheable packet pool, the IP thread, ARP/ICMP/UDP/TCP, and the DHCP
 * client (boot default).  Exposes a thin facade so the board's commands never
 * include nx_api.h directly.  The address, DHCP and ping operations are the
 * shared ones (shell/cmds/net_core.h, issue #220) over the objects created here.
 */
#ifndef NX_GLUE_H
#define NX_GLUE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXG_OK         0
#define NXG_ERR_STATE -1   /* NetX not up                                       */
#define NXG_ERR       -2   /* a NetX call failed                                */

/**
 * One-time bring-up from tx_application_define() (after eth_init()): NetX system
 * init, the shared pool (.sdram.eth), the IP instance + IP thread, ARP/ICMP/UDP/
 * TCP, the DHCP client (created here; started later from the eth-link poll
 * thread once the link comes up -- issue #13) and the shared operations' state
 * (net_core_init()).  Only creates ThreadX objects (no
 * blocking), so it is safe before the scheduler.  Returns NXG_OK or a negative.
 */
int  nx_net_init(void);

/** Nonzero once nx_net_init() created the IP instance. */
bool nx_net_is_up(void);

/**
 * Counters for `net info` (issue #202): the Ethernet driver's, NetX's TCP-wide
 * ones (nx_tcp_info_get) and the shared packet pool's.  None of them is reset
 * after boot, and the three groups are read one after another, not as one
 * snapshot.  The driver fields mirror struct nx_eth_stats (nx_eth_driver.h).
 */
struct nx_net_diag {
	/* Ethernet driver */
	uint32_t rx_ok, rx_drop, rx_no_buf;
	uint32_t tx_ok, tx_drop;                 /* tx_drop = the next three        */
	uint32_t tx_drop_mac_down;               /* MAC not running                 */
	uint32_t tx_drop_coalesce;               /* chain too long to coalesce      */
	uint32_t tx_drop_hal;                    /* HAL_ETH_Transmit_IT() refused   */
	uint32_t hal_err_last;                   /* heth ErrorCode at that refusal  */
	uint32_t hal_dma_err_last;               /* heth DMAErrorCode at it         */
	uint32_t hal_state_last;                 /* heth gState at it               */
	uint32_t dma_err, dma_err_last;          /* ErrorCallback count / DMA error */
	/* NetX TCP (all sockets) */
	uint32_t tcp_pkts_sent, tcp_bytes_sent;
	uint32_t tcp_pkts_rcvd, tcp_bytes_rcvd;
	uint32_t tcp_invalid, tcp_rx_dropped, tcp_checksum_err;
	uint32_t tcp_conns, tcp_disconns, tcp_conns_dropped;
	uint32_t tcp_retransmits;
	bool     tcp_valid;                      /* nx_tcp_info_get() succeeded     */
	/* the shared packet pool */
	uint32_t pool_total, pool_free, pool_empty_req, pool_empty_susp;
	uint32_t pool_invalid_rel;
	bool     pool_valid;                     /* nx_packet_pool_info_get() ok    */
};
int  nx_net_diag_get(struct nx_net_diag *out);

/** The NetX IP instance (NX_IP*), opaque to non-NetX callers -- the TCP echo
 *  server (P3) / future sockets cast it back.  NULL before nx_net_init(). */
void *nx_net_ip(void);

/** The shared non-cacheable packet pool (NX_PACKET_POOL*), for backends that
 *  allocate TX packets (the network shell, P4).  NULL before nx_net_init(). */
void *nx_net_pool(void);

#ifdef __cplusplus
}
#endif

#endif /* NX_GLUE_H */
