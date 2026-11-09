/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ThreadX Shell Project
 */
/**
 * @file    nx_eth_driver.h
 * @brief   Clean-room NetX Duo network driver over the STM32 ETH MAC
 * (owhinata/stm32f746g-disco#49 P2).
 *
 * The NetX Duo link driver entry passed to nx_ip_create().  It bridges the IPv4
 * stack to the HAL_ETH MAC (started/configured by port/eth/eth_link.c) using the
 * new descriptor-model zero-copy callbacks (HAL_ETH_RxAllocate/RxLink/TxFree are
 * provided here as strong overrides -- USE_HAL_ETH_REGISTER_CALLBACKS is 0).  RX
 * runs in the NetX IP helper thread via NX_LINK_DEFERRED_PROCESSING (the ETH ISR
 * only posts the deferred event).  All packet payloads come from one MPU
 * non-cacheable pool (the HAL does no D-cache maintenance).
 *
 * Clean-room: the NetX device-driver contract (nx_api.h) and the new HAL_ETH API
 * were the reference; ST's nx_stm32_eth_driver.c (Azure RTOS EULA) was NOT used.
 */
#ifndef NX_ETH_DRIVER_H
#define NX_ETH_DRIVER_H

#include "nx_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The NetX Duo link-driver entry (pass to nx_ip_create()). */
VOID nx_eth_driver(NX_IP_DRIVER *driver_req_ptr);

/** Bind the packet pool the RX path allocates from (the shared non-cacheable
 *  eth pool); call BEFORE nx_ip_create().  Measures the NX_PACKET->payload
 *  offset once for zero-copy buffer<->packet recovery. */
VOID nx_eth_driver_set_pool(NX_PACKET_POOL *pool);

/** Driver RX/TX counters for diagnostics (`net info`).
 *
 *  tx_drop is the sum of the three reasons after it (issue #202): the MAC was
 *  not running (link down / not started), a chain too long for the coalesce
 *  buffer, and HAL_ETH_Transmit_IT() refusing the frame.  On a HAL refusal the
 *  handle's sticky ErrorCode, its DMAErrorCode and its gState at that moment are
 *  kept (the last refusal's values).  dma_err_last is HAL_ETH_GetDMAError() as
 *  the last HAL_ETH_ErrorCallback() saw it (the DMASR bits the HAL latched).
 *  Plain counters, written by the transmitting thread (under the NetX IP mutex),
 *  the IP thread and the ETH ISR; a read is not one snapshot. */
struct nx_eth_stats {
	ULONG rx_ok, rx_drop, rx_no_buf, tx_ok, tx_drop, dma_err;
	ULONG tx_drop_mac_down, tx_drop_coalesce, tx_drop_hal;
	ULONG hal_err_last, hal_dma_err_last, hal_state_last;
	ULONG dma_err_last;
};
VOID nx_eth_driver_get_stats(struct nx_eth_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_ETH_DRIVER_H */
