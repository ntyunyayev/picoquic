/*
* Author: Christian Huitema
* Copyright (c) 2020, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/* Socket loop implements the "wait for messages" loop common to most servers
 * and many clients.
 *
 * Second step: support simple servers and simple client.
 *
 * The "call loop back" function is called: when ready, after receiving, and after sending. The
 * loop will terminate if the callback return code is not zero -- except for special processing
 * of the migration testing code.
 * TODO: in Windows, use WSA asynchronous calls instead of sendmsg, allowing for multiple parallel sends.
 * TODO: in Linux, use multiple send per call API
 * TDOO: trim the #define list.
 * TODO: support the QuicDoq scenario, manage extra socket.
 */

#ifdef _WINDOWS
#define WIN32_LEAN_AND_MEAN

#include <WinSock2.h>
#include <Windows.h>
#include <assert.h>
#include <iphlpapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <ws2tcpip.h>

#ifndef SOCKET_TYPE
#define SOCKET_TYPE SOCKET
#endif
#ifndef SOCKET_CLOSE
#define SOCKET_CLOSE(x) closesocket(x)
#endif
#ifndef WSA_LAST_ERROR
#define WSA_LAST_ERROR(x) WSAGetLastError()
#endif
#ifndef socklen_t
#define socklen_t int
#endif

#else /* Linux */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#ifndef __USE_XOPEN2K
#define __USE_XOPEN2K
#endif
#ifndef __USE_POSIX
#define __USE_POSIX
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>

#ifndef SOCKET_TYPE
#define SOCKET_TYPE int
#endif
#ifndef INVALID_SOCKET
#define INVALID_SOCKET -1
#endif
#ifndef SOCKET_CLOSE
#define SOCKET_CLOSE(x) close(x)
#endif
#ifndef WSA_LAST_ERROR
#define WSA_LAST_ERROR(x) ((long)(x))
#endif
#endif

#include "picosocks.h"
#include "picoquic.h"
#include "picoquic_internal.h"
#include "picoquic_packet_loop.h"
#include "picoquic_unified_log.h"
#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_string_fns.h>
#include <rte_udp.h>
#include <rte_ip.h>
#include <rte_errno.h>

#include <rte_common.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_string_fns.h>
#include <rte_timer.h>
#include <rte_power.h>
#include <rte_eal.h>
#include <rte_spinlock.h>

//DPDK
#define _DPDK
#define MAX_PKT_BURST 32
#define MAX_RX_QUEUE_PER_LCORE 16
#define MAX_TX_QUEUE_PER_PORT 16
#define MAX_PKT_BURST 32
#define MEMPOOL_CACHE_SIZE 256
#define RTE_TEST_RX_DESC_DEFAULT 1024
#define RTE_TEST_TX_DESC_DEFAULT 1024
struct lcore_queue_conf
{
    unsigned n_rx_port;
    unsigned rx_port_list[MAX_RX_QUEUE_PER_LCORE];
} __rte_cache_aligned;
struct lcore_queue_conf lcore_queue_conf[RTE_MAX_LCORE];

#if defined(_WINDOWS)
static int udp_gso_available = 0;
#else
#if defined(UDP_SEGMENT)
static int udp_gso_available = 1;
#else
static int udp_gso_available = 0;
#endif
#endif

int picoquic_packet_loop_open_sockets(int local_port, int local_af, SOCKET_TYPE *s_socket, int *sock_af,
                                      uint16_t *sock_ports, int socket_buffer_size, int nb_sockets_max)
{
    int nb_sockets = (local_af == AF_UNSPEC) ? 2 : 1;

    /* Compute how many sockets are necessary */
    if (nb_sockets > nb_sockets_max)
    {
        DBG_PRINTF("Cannot open %d sockets, max set to %d\n", nb_sockets, nb_sockets_max);
        nb_sockets = 0;
    }
    else if (local_af == AF_UNSPEC)
    {
        sock_af[0] = AF_INET;
        sock_af[1] = AF_INET6;
    }
    else if (local_af == AF_INET || local_af == AF_INET6)
    {
        sock_af[0] = local_af;
    }
    else
    {
        DBG_PRINTF("Cannot open socket(AF=%d), unsupported AF\n", local_af);
        nb_sockets = 0;
    }

    for (int i = 0; i < nb_sockets; i++)
    {
        struct sockaddr_storage local_address;
        int recv_set = 0;
        int send_set = 0;

        if ((s_socket[i] = socket(sock_af[i], SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET ||
            picoquic_socket_set_ecn_options(s_socket[i], sock_af[i], &recv_set, &send_set) != 0 ||
            picoquic_socket_set_pkt_info(s_socket[i], sock_af[i]) != 0 ||
            picoquic_bind_to_port(s_socket[i], sock_af[i], local_port) != 0 ||
            picoquic_get_local_address(s_socket[i], &local_address) != 0)
        {
            DBG_PRINTF("Cannot set socket (af=%d, port = %d)\n", sock_af[i], local_port);
            for (int j = 0; j < i; j++)
            {
                if (s_socket[i] != INVALID_SOCKET)
                {
                    SOCKET_CLOSE(s_socket[i]);
                    s_socket[i] = INVALID_SOCKET;
                }
            }
            nb_sockets = 0;
            break;
        }
        else
        {
            if (local_address.ss_family == AF_INET6)
            {
                sock_ports[i] = ntohs(((struct sockaddr_in6 *)&local_address)->sin6_port);
            }
            else if (local_address.ss_family == AF_INET)
            {
                sock_ports[i] = ntohs(((struct sockaddr_in *)&local_address)->sin_port);
            }

            if (socket_buffer_size > 0)
            {
                socklen_t opt_len;
                int opt_ret;
                int so_sndbuf;
                int so_rcvbuf;

                opt_len = sizeof(int);
                so_sndbuf = socket_buffer_size;
                opt_ret = setsockopt(s_socket[i], SOL_SOCKET, SO_SNDBUF, (const char *)&so_sndbuf, opt_len);
                if (opt_ret != 0)
                {
#ifdef _WINDOWS
                    int sock_error = WSAGetLastError();
#else
                    int sock_error = errno;
#endif
                    opt_ret = getsockopt(s_socket[i], SOL_SOCKET, SO_SNDBUF, (char *)&so_sndbuf, &opt_len);
                    DBG_PRINTF("Cannot set SO_SNDBUF to %d, err=%d, so_sndbuf=%d (%d)",
                               socket_buffer_size, sock_error, so_sndbuf, opt_ret);
                }
                opt_len = sizeof(int);
                so_rcvbuf = socket_buffer_size;
                opt_ret = setsockopt(s_socket[i], SOL_SOCKET, SO_RCVBUF, (const char *)&so_rcvbuf, opt_len);
                if (opt_ret != 0)
                {
#ifdef _WINDOWS
                    int sock_error = WSAGetLastError();
#else
                    int sock_error = errno;
#endif
                    opt_ret = getsockopt(s_socket[i], SOL_SOCKET, SO_RCVBUF, (char *)&so_rcvbuf, &opt_len);
                    DBG_PRINTF("Cannot set SO_RCVBUF to %d, err=%d, so_rcvbuf=%d (%d)",
                               socket_buffer_size, sock_error, so_rcvbuf, opt_ret);
                }
            }
        }
    }

    return nb_sockets;
}

#ifdef _DPDK

int picoquic_packet_loop(picoquic_quic_t *quic,
                         int local_port,
                         int local_af,
                         int dest_if,
                         int socket_buffer_size,
                         int do_not_use_gso,
                         picoquic_packet_loop_cb_fn loop_callback,
                         void *loop_callback_ctx)
{
    //===================DPDK==========================//
    //VARS

    struct rte_mempool *mb_pool;
    static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
    static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;
    static struct rte_ether_addr eth_addr;
    static struct rte_eth_conf port_conf = {
        .rxmode = {
            .split_hdr_size = 0,
        },
        .txmode = {
            .mq_mode = ETH_MQ_TX_NONE,
        },
    };
    struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
    unsigned lcore_id = 1;
    struct lcore_queue_conf *qconf;
    uint16_t portid = 0;
    int ret;
    struct rte_eth_rxconf rxq_conf;
    struct rte_eth_txconf txq_conf;
    struct rte_eth_dev_info dev_info;
    struct rte_eth_dev_tx_buffer *tx_buffer;
    struct rte_mbuf *m;
    struct rte_eth_conf local_port_conf = port_conf;
    struct rte_rte_ether_hdr *eth;
    void *tmp;

    //handling udp packet
    struct rte_ether_hdr *eth_hdr;
    struct vlan_hdr *vh;
    uint16_t *proto;
    struct rte_ipv4_hdr *ip_hdr;

    //setup DPDK
    tx_buffer = rte_zmalloc_socket("tx_buffer",
                                   RTE_ETH_TX_BUFFER_SIZE(MAX_PKT_BURST), 0,
                                   rte_eth_dev_socket_id(0));
    if (tx_buffer == NULL)
    {
        printf("fail to init buffer\n");
        return 0;
    }
    unsigned int nb_mbufs = RTE_MAX(1 * (1 + 1 + MAX_PKT_BURST + 2 * MEMPOOL_CACHE_SIZE), 8192U);
    mb_pool = rte_pktmbuf_pool_create("mbuf_pool", nb_mbufs,
                                      MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                      rte_socket_id());
    if (mb_pool == NULL)
    {
        printf("fail to init mb_pool10\n");
        rte_exit(EXIT_FAILURE, "%s\n", rte_strerror(rte_errno));
        return 0;
    }
    printf("after init\n");
    ret = rte_eth_dev_info_get(0, &dev_info);
    if (ret != 0)
        rte_exit(EXIT_FAILURE,
                 "Error during getting device (port %u) info: %s\n",
                 0, strerror(-ret));

    if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
        local_port_conf.txmode.offloads |=
            DEV_TX_OFFLOAD_MBUF_FAST_FREE;
    ret = rte_eth_dev_configure(0, 1, 1, &local_port_conf);
    if (ret != 0)
    {
        printf("error in dev_configure\n");
        return 0;
    }

    ret = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &nb_rxd,
                                           &nb_txd);
    if (ret < 0)
        rte_exit(EXIT_FAILURE,
                 "Cannot adjust number of descriptors: err=%d, port=%u\n",
                 ret, portid);

    ret = rte_eth_macaddr_get(portid, &eth_addr);

    //init tx queue
    txq_conf = dev_info.default_txconf;
    txq_conf.offloads = local_port_conf.txmode.offloads;
    ret = rte_eth_tx_queue_setup(portid, 0, nb_txd,
                                 rte_eth_dev_socket_id(portid),
                                 &txq_conf);
    if (ret != 0)
    {
        printf("failed to init queue\n");
        return 0;
    }
    ret = rte_eth_tx_buffer_init(tx_buffer, MAX_PKT_BURST);
    if (ret != 0)
    {
        printf("error in buffer_init\n");
        return 0;
    }
    //init rx queue
    rxq_conf = dev_info.default_rxconf;
    rxq_conf.offloads = local_port_conf.rxmode.offloads;
    ret = rte_eth_rx_queue_setup(0, 0, nb_rxd, rte_eth_dev_socket_id(0), &rxq_conf, mb_pool);
    if (ret != 0)
    {
        printf("failed to init rx_queue\n");
    }
    printf("before start \n");
    ret = rte_eth_dev_start(0);
    if (ret != 0)
    {
        printf("failed to start device\n");
    }
    printf("after dpdk setup\n");
    //===================DPDK==========================//
    uint64_t current_time = picoquic_get_quic_time(quic);
    int64_t delay_max = 10000000;
    struct sockaddr_storage addr_from;
    struct sockaddr_storage addr_to;
    int if_index_to;
    uint8_t buffer[1536];
    uint8_t *send_buffer = NULL;
    size_t send_length = 0;
    size_t send_msg_size = 0;
    size_t send_buffer_size = 1536;
    size_t *send_msg_ptr = NULL;
    int bytes_recv;
    picoquic_connection_id_t log_cid;
    SOCKET_TYPE s_socket[PICOQUIC_PACKET_LOOP_SOCKETS_MAX];
    int sock_af[PICOQUIC_PACKET_LOOP_SOCKETS_MAX];
    uint16_t sock_ports[PICOQUIC_PACKET_LOOP_SOCKETS_MAX];
    int nb_sockets = 0;
    int testing_migration = 0; /* Hook for the migration test */
    uint16_t next_port = 0;    /* Data for the migration test */
    picoquic_cnx_t *last_cnx = NULL;
    int loop_immediate = 0;
    int pkts_recv;
#ifdef _WINDOWS
    WSADATA wsaData = {0};
    (void)WSA_START(MAKEWORD(2, 2), &wsaData);
#endif
    send_buffer_size = 0xFFFF;
    send_msg_ptr = &send_msg_size;
    send_buffer = malloc(send_buffer_size);
    if (send_buffer == NULL)
    {
        ret = -1;
    }
    while (ret == 0)
    {
        int socket_rank = -1;
        int64_t delta_t = 0;
        unsigned char received_ecn;

        if_index_to = 0;
        /* TODO: rewrite the code and avoid using the "loop_immediate" state variable */
        printf("before receive\n");
        pkts_recv = rte_eth_rx_burst(0, 0, pkts_burst, MAX_PKT_BURST);
        printf("afte receive\n");
        for (int j = 0; j < ret; j++)
        {
            m = pkts_burst[j];
        }

        if (pkts_recv < 0)
        {
            ret = -1;
        }
        else
        {
            uint64_t loop_time = current_time;

            if (pkts_recv > 0)
            {

                uint16_t len;
                for (int i = 0; i < pkts_recv; i++)
                {
                    /* access IP header of rcv'd pkt */
                    ip_hdr = (struct rte_ipv4_hdr *)(rte_pktmbuf_mtod(pkts_burst[i], char *) + sizeof(struct rte_ether_hdr));

                    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)((unsigned char *)ip_hdr +
                                                                     sizeof(struct rte_ipv4_hdr));
                    unsigned char *payload = (unsigned char *)(udp + 1);
                    int length = udp->dgram_len;
                    printf("before incoming packet\n");
                    (void)picoquic_incoming_packet_ex(quic, payload,
                                                      (size_t)length, (struct sockaddr *)&addr_from,
                                                      (struct sockaddr *)&addr_to, if_index_to, received_ecn,
                                                      &last_cnx, current_time);
                    printf("after incoming packet\n");
                }

                if (loop_callback != NULL)
                {
                    size_t b_recvd = (size_t)bytes_recv;
                    ret = loop_callback(quic, picoquic_packet_loop_after_receive, loop_callback_ctx, &b_recvd);
                }
                if (ret == 0)
                {
                    /* Try to receive more packets if possible */
                    loop_immediate = 1;
                    continue;
                }
            }
            if (ret != PICOQUIC_NO_ERROR_SIMULATE_NAT && ret != PICOQUIC_NO_ERROR_SIMULATE_MIGRATION)
            {
                size_t bytes_sent = 0;
                while (ret == 0)
                {
                    struct sockaddr_storage peer_addr;
                    struct sockaddr_storage local_addr;
                    int if_index = dest_if;
                    int sock_ret = 0;
                    int sock_err = 0;
                    printf("before prep next packet\n");
                    ret = picoquic_prepare_next_packet_ex(quic, loop_time,
                                                          send_buffer, send_buffer_size, &send_length,
                                                          &peer_addr, &local_addr, &if_index, &log_cid, &last_cnx,
                                                          send_msg_ptr);
                    printf("after prep next packet\n");

                    if (ret == 0 && send_length > 0)
                    {
                        SOCKET_TYPE send_socket = INVALID_SOCKET;
                        bytes_sent += send_length;

                        for (int i = 0; i < nb_sockets; i++)
                        {
                            if (sock_af[i] == peer_addr.ss_family)
                            {
                                send_socket = s_socket[i];
                                break;
                            }
                        }
                        int offset = 0;
                        struct rte_ipv4_hdr ip_hdr;
                        struct rte_udp_hdr rte_udp_hdr;
                        struct rte_ether_hdr eth_hdr;
                        printf("before alloc\n");
                        struct rte_mbuf *m = rte_pktmbuf_alloc(mb_pool);
                        printf("after alloc\n");
                        if (m == NULL)
                        {   
                            printf("fail to init pktmbuf\n");
                            rte_exit(EXIT_FAILURE, "%s\n", rte_strerror(rte_errno));
                            return 0;
                        }
                        printf("before setup pkt_udp_up_headers\n");
                        setup_pkt_udp_ip_headers(&ip_hdr, &rte_udp_hdr, send_length);
                        printf("after setup pkt_udp_up_headers\n");
                        printf("before copy buf to pkt\n");
                        copy_buf_to_pkt(&ip_hdr, sizeof(struct rte_ether_hdr), m, offset);
                        printf("after copy buf to pkt\n");
                        offset += sizeof(struct rte_ether_hdr);
                        copy_buf_to_pkt(&ip_hdr, sizeof(struct rte_ipv4_hdr), m, offset);
                        offset += sizeof(struct rte_ipv4_hdr);
                        copy_buf_to_pkt(&rte_udp_hdr, sizeof(struct rte_udp_hdr), m, offset);
                        offset += sizeof(struct rte_udp_hdr);
                        copy_buf_to_pkt(send_buffer, send_buffer_size, m, offset);
                        //inchallah ca marche
                        ret = rte_eth_tx_burst(0, 0, &m, 1);
                        printf("after transmit\n");

                        //     if (sock_ret <= 0)
                        //     {
                        //         if (last_cnx == NULL)
                        //         {
                        //             picoquic_log_context_free_app_message(quic, &log_cid, "Could not send message to AF_to=%d, AF_from=%d, if=%d, ret=%d, err=%d",
                        //                                                   peer_addr.ss_family, local_addr.ss_family, if_index, sock_ret, sock_err);
                        //         }
                        //         else
                        //         {
                        //             picoquic_log_app_message(last_cnx, "Could not send message to AF_to=%d, AF_from=%d, if=%d, ret=%d, err=%d",
                        //                                      peer_addr.ss_family, local_addr.ss_family, if_index, sock_ret, sock_err);

                        //             if (picoquic_socket_error_implies_unreachable(sock_err))
                        //             {
                        //                 picoquic_notify_destination_unreachable(last_cnx, current_time,
                        //                                                         (struct sockaddr *)&peer_addr, (struct sockaddr *)&local_addr, if_index,
                        //                                                         sock_err);
                        //             }
                        //             else if (sock_err == EIO)
                        //             {
                        //                 size_t packet_index = 0;
                        //                 size_t packet_size = send_msg_size;

                        //                 while (packet_index < send_length)
                        //                 {
                        //                     if (packet_index + packet_size > send_length)
                        //                     {
                        //                         packet_size = send_length - packet_index;
                        //                     }
                        //                     sock_ret = picoquic_sendmsg(send_socket,
                        //                                                 (struct sockaddr *)&peer_addr, (struct sockaddr *)&local_addr, if_index,
                        //                                                 (const char *)(send_buffer + packet_index), (int)packet_size, 0, &sock_err);
                        //                     if (sock_ret > 0)
                        //                     {
                        //                         packet_index += packet_size;
                        //                     }
                        //                     else
                        //                     {
                        //                         picoquic_log_app_message(last_cnx, "Retry with packet size=%zu fails at index %zu, ret=%d, err=%d.",
                        //                                                  packet_size, packet_index, sock_ret, sock_err);
                        //                         break;
                        //                     }
                        //                 }
                        //                 if (sock_ret > 0)
                        //                 {
                        //                     picoquic_log_app_message(last_cnx, "Retry of %zu bytes by chunks of %zu bytes succeeds.",
                        //                                              send_length, send_msg_size);
                        //                 }
                        //             }
                        //         }
                        //     }
                        // }
                        // else
                        // {
                        //     break;
                        // }
                    }

                    if (ret == 0 && loop_callback != NULL)
                    {
                        ret = loop_callback(quic, picoquic_packet_loop_after_send, loop_callback_ctx, &bytes_sent);
                    }
                }
            }

            // if (ret == PICOQUIC_NO_ERROR_SIMULATE_NAT || ret == PICOQUIC_NO_ERROR_SIMULATE_MIGRATION)
            // {
            //     /* Two pseudo error codes used for testing migration!
            //      * What follows is really test code, which we write here because it has to handle
            //      * the sockets, which interferes a lot with the handling of the packet loop.
            //      */
            //     SOCKET_TYPE s_mig = INVALID_SOCKET;
            //     int s_mig_af;
            //     int sock_ret;
            //     int testing_nat = (ret == PICOQUIC_NO_ERROR_SIMULATE_NAT);

            //     sock_ret = picoquic_packet_loop_open_sockets(0, sock_af[0], &s_mig, &s_mig_af,
            //                                                  &next_port, socket_buffer_size, 1);
            //     if (sock_ret != 1 || s_mig == INVALID_SOCKET)
            //     {
            //         if (last_cnx != NULL)
            //         {
            //             picoquic_log_app_message(last_cnx, "Could not create socket for migration test, port=%d, af=%d, err=%d",
            //                                      next_port, sock_af[0], sock_ret);
            //         }
            //     }
            //     else if (testing_nat)
            //     {
            //         if (s_socket[0] != INVALID_SOCKET)
            //         {
            //             SOCKET_CLOSE(s_socket[0]);
            //         }
            //         s_socket[0] = s_mig;
            //         sock_ports[0] = next_port;
            //         ret = 0;

            //         if (loop_callback != NULL)
            //         {
            //             struct sockaddr_storage l_addr;
            //             if (picoquic_store_loopback_addr(&l_addr, sock_af[0], sock_ports[0]) == 0)
            //             {
            //                 ret = loop_callback(quic, picoquic_packet_loop_port_update, loop_callback_ctx, &l_addr);
            //             }
            //         }
            //     }
            //     else
            //     {
            //         /* Testing organized migration */
            //         if (nb_sockets < PICOQUIC_PACKET_LOOP_SOCKETS_MAX && last_cnx != NULL)
            //         {
            //             struct sockaddr_storage local_address;
            //             picoquic_store_addr(&local_address, (struct sockaddr *)&last_cnx->path[0]->local_addr);
            //             if (local_address.ss_family == AF_INET6)
            //             {
            //                 ((struct sockaddr_in6 *)&local_address)->sin6_port = next_port;
            //             }
            //             else if (local_address.ss_family == AF_INET)
            //             {
            //                 ((struct sockaddr_in *)&local_address)->sin_port = next_port;
            //             }
            //             s_socket[nb_sockets] = s_mig;
            //             sock_ports[nb_sockets] = next_port;
            //             nb_sockets++;
            //             testing_migration = 1;
            //             ret = picoquic_probe_new_path(last_cnx, (struct sockaddr *)&last_cnx->path[0]->peer_addr,
            //                                           (struct sockaddr *)&local_address, current_time);
            //         }
            //         else
            //         {
            //             SOCKET_CLOSE(s_mig);
            //         }
            //     }
            // }
        }

        if (ret == PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP)
        {
            /* Normal termination requested by the application, returns no error */
            ret = 0;
        }

        /* Close the sockets */
        for (int i = 0; i < nb_sockets; i++)
        {
            if (s_socket[i] != INVALID_SOCKET)
            {
                SOCKET_CLOSE(s_socket[i]);
                s_socket[i] = INVALID_SOCKET;
            }
        }

        if (send_buffer != NULL)
        {
            free(send_buffer);
        }

        return ret;
    }
}

#else

int picoquic_packet_loop(picoquic_quic_t *quic,
                         int local_port,
                         int local_af,
                         int dest_if,
                         int socket_buffer_size,
                         int do_not_use_gso,
                         picoquic_packet_loop_cb_fn loop_callback,
                         void *loop_callback_ctx)
{
    int ret = 0;
    uint64_t current_time = picoquic_get_quic_time(quic);
    int64_t delay_max = 10000000;
    struct sockaddr_storage addr_from;
    struct sockaddr_storage addr_to;
    int if_index_to;
    uint8_t buffer[1536];
    uint8_t *send_buffer = NULL;
    size_t send_length = 0;
    size_t send_msg_size = 0;
    size_t send_buffer_size = 1536;
    size_t *send_msg_ptr = NULL;
    int bytes_recv;
    picoquic_connection_id_t log_cid;
    SOCKET_TYPE s_socket[PICOQUIC_PACKET_LOOP_SOCKETS_MAX];
    int sock_af[PICOQUIC_PACKET_LOOP_SOCKETS_MAX];
    uint16_t sock_ports[PICOQUIC_PACKET_LOOP_SOCKETS_MAX];
    int nb_sockets = 0;
    int testing_migration = 0; /* Hook for the migration test */
    uint16_t next_port = 0;    /* Data for the migration test */
    picoquic_cnx_t *last_cnx = NULL;
    int loop_immediate = 0;
#ifdef _WINDOWS
    WSADATA wsaData = {0};
    (void)WSA_START(MAKEWORD(2, 2), &wsaData);
#endif
    memset(sock_af, 0, sizeof(sock_af));
    memset(sock_ports, 0, sizeof(sock_ports));

    if ((nb_sockets = picoquic_packet_loop_open_sockets(local_port, local_af, s_socket, sock_af,
                                                        sock_ports, socket_buffer_size, PICOQUIC_PACKET_LOOP_SOCKETS_MAX)) == 0)
    {
        ret = PICOQUIC_ERROR_UNEXPECTED_ERROR;
    }
    else if (loop_callback != NULL)
    {
        struct sockaddr_storage l_addr;
        ret = loop_callback(quic, picoquic_packet_loop_ready, loop_callback_ctx, NULL);
        if (picoquic_store_loopback_addr(&l_addr, sock_af[0], sock_ports[0]) == 0)
        {
            ret = loop_callback(quic, picoquic_packet_loop_port_update, loop_callback_ctx, &l_addr);
        }
    }

    if (ret == 0)
    {
        if (udp_gso_available && !do_not_use_gso)
        {
            send_buffer_size = 0xFFFF;
            send_msg_ptr = &send_msg_size;
        }
        send_buffer = malloc(send_buffer_size);
        if (send_buffer == NULL)
        {
            ret = -1;
        }
    }

    /* Wait for packets */
    /* TODO: add stopping condition, was && (!just_once || !connection_done) */
    while (ret == 0)
    {
        int socket_rank = -1;
        int64_t delta_t = 0;
        unsigned char received_ecn;

        if_index_to = 0;
        /* TODO: rewrite the code and avoid using the "loop_immediate" state variable */
        if (!loop_immediate)
        {
            delta_t = picoquic_get_next_wake_delay(quic, current_time, delay_max);
        }
        loop_immediate = 0;

        bytes_recv = picoquic_select_ex(s_socket, nb_sockets,
                                        &addr_from,
                                        &addr_to, &if_index_to, &received_ecn,
                                        buffer, sizeof(buffer),
                                        delta_t, &socket_rank, &current_time);
        if (bytes_recv < 0)
        {
            ret = -1;
        }
        else
        {
            uint64_t loop_time = current_time;

            if (bytes_recv > 0)
            {
                uint16_t current_recv_port = 0;

                if (testing_migration && socket_rank == 0)
                {
                    current_recv_port = next_port;
                }
                else
                {
                    current_recv_port = sock_ports[socket_rank];
                }
                /* Document incoming port */
                if (addr_to.ss_family == AF_INET6)
                {
                    ((struct sockaddr_in6 *)&addr_to)->sin6_port = current_recv_port;
                }
                else if (addr_to.ss_family == AF_INET)
                {
                    ((struct sockaddr_in *)&addr_to)->sin_port = current_recv_port;
                }
                /* Submit the packet to the server */
                (void)picoquic_incoming_packet_ex(quic, buffer,
                                                  (size_t)bytes_recv, (struct sockaddr *)&addr_from,
                                                  (struct sockaddr *)&addr_to, if_index_to, received_ecn,
                                                  &last_cnx, current_time);

                if (loop_callback != NULL)
                {
                    size_t b_recvd = (size_t)bytes_recv;
                    ret = loop_callback(quic, picoquic_packet_loop_after_receive, loop_callback_ctx, &b_recvd);
                }
                if (ret == 0)
                {
                    /* Try to receive more packets if possible */
                    loop_immediate = 1;
                    continue;
                }
            }
            if (ret != PICOQUIC_NO_ERROR_SIMULATE_NAT && ret != PICOQUIC_NO_ERROR_SIMULATE_MIGRATION)
            {
                size_t bytes_sent = 0;
                while (ret == 0)
                {
                    struct sockaddr_storage peer_addr;
                    struct sockaddr_storage local_addr;
                    int if_index = dest_if;
                    int sock_ret = 0;
                    int sock_err = 0;

                    ret = picoquic_prepare_next_packet_ex(quic, loop_time,
                                                          send_buffer, send_buffer_size, &send_length,
                                                          &peer_addr, &local_addr, &if_index, &log_cid, &last_cnx,
                                                          send_msg_ptr);

                    if (ret == 0 && send_length > 0)
                    {
                        SOCKET_TYPE send_socket = INVALID_SOCKET;
                        bytes_sent += send_length;

                        for (int i = 0; i < nb_sockets; i++)
                        {
                            if (sock_af[i] == peer_addr.ss_family)
                            {
                                send_socket = s_socket[i];
                                break;
                            }
                        }

                        if (send_socket == INVALID_SOCKET)
                        {
                            sock_ret = -1;
                            sock_err = -1;
                        }
                        else
                        {
                            if (testing_migration)
                            {
                                /* This code path is only used in the migration tests */
                                uint16_t send_port = (local_addr.ss_family == AF_INET) ? ((struct sockaddr_in *)&local_addr)->sin_port : ((struct sockaddr_in6 *)&local_addr)->sin6_port;

                                if (send_port == next_port)
                                {
                                    send_socket = s_socket[nb_sockets - 1];
                                }
                            }

                            sock_ret = picoquic_sendmsg(send_socket,
                                                        (struct sockaddr *)&peer_addr, (struct sockaddr *)&local_addr, if_index,
                                                        (const char *)send_buffer, (int)send_length, (int)send_msg_size, &sock_err);
                        }

                        if (sock_ret <= 0)
                        {
                            if (last_cnx == NULL)
                            {
                                picoquic_log_context_free_app_message(quic, &log_cid, "Could not send message to AF_to=%d, AF_from=%d, if=%d, ret=%d, err=%d",
                                                                      peer_addr.ss_family, local_addr.ss_family, if_index, sock_ret, sock_err);
                            }
                            else
                            {
                                picoquic_log_app_message(last_cnx, "Could not send message to AF_to=%d, AF_from=%d, if=%d, ret=%d, err=%d",
                                                         peer_addr.ss_family, local_addr.ss_family, if_index, sock_ret, sock_err);

                                if (picoquic_socket_error_implies_unreachable(sock_err))
                                {
                                    picoquic_notify_destination_unreachable(last_cnx, current_time,
                                                                            (struct sockaddr *)&peer_addr, (struct sockaddr *)&local_addr, if_index,
                                                                            sock_err);
                                }
                                else if (sock_err == EIO)
                                {
                                    size_t packet_index = 0;
                                    size_t packet_size = send_msg_size;

                                    while (packet_index < send_length)
                                    {
                                        if (packet_index + packet_size > send_length)
                                        {
                                            packet_size = send_length - packet_index;
                                        }
                                        sock_ret = picoquic_sendmsg(send_socket,
                                                                    (struct sockaddr *)&peer_addr, (struct sockaddr *)&local_addr, if_index,
                                                                    (const char *)(send_buffer + packet_index), (int)packet_size, 0, &sock_err);
                                        if (sock_ret > 0)
                                        {
                                            packet_index += packet_size;
                                        }
                                        else
                                        {
                                            picoquic_log_app_message(last_cnx, "Retry with packet size=%zu fails at index %zu, ret=%d, err=%d.",
                                                                     packet_size, packet_index, sock_ret, sock_err);
                                            break;
                                        }
                                    }
                                    if (sock_ret > 0)
                                    {
                                        picoquic_log_app_message(last_cnx, "Retry of %zu bytes by chunks of %zu bytes succeeds.",
                                                                 send_length, send_msg_size);
                                    }
                                }
                            }
                        }
                    }
                    else
                    {
                        break;
                    }
                }

                if (ret == 0 && loop_callback != NULL)
                {
                    ret = loop_callback(quic, picoquic_packet_loop_after_send, loop_callback_ctx, &bytes_sent);
                }
            }
        }

        if (ret == PICOQUIC_NO_ERROR_SIMULATE_NAT || ret == PICOQUIC_NO_ERROR_SIMULATE_MIGRATION)
        {
            /* Two pseudo error codes used for testing migration!
             * What follows is really test code, which we write here because it has to handle
             * the sockets, which interferes a lot with the handling of the packet loop.
             */
            SOCKET_TYPE s_mig = INVALID_SOCKET;
            int s_mig_af;
            int sock_ret;
            int testing_nat = (ret == PICOQUIC_NO_ERROR_SIMULATE_NAT);

            sock_ret = picoquic_packet_loop_open_sockets(0, sock_af[0], &s_mig, &s_mig_af,
                                                         &next_port, socket_buffer_size, 1);
            if (sock_ret != 1 || s_mig == INVALID_SOCKET)
            {
                if (last_cnx != NULL)
                {
                    picoquic_log_app_message(last_cnx, "Could not create socket for migration test, port=%d, af=%d, err=%d",
                                             next_port, sock_af[0], sock_ret);
                }
            }
            else if (testing_nat)
            {
                if (s_socket[0] != INVALID_SOCKET)
                {
                    SOCKET_CLOSE(s_socket[0]);
                }
                s_socket[0] = s_mig;
                sock_ports[0] = next_port;
                ret = 0;

                if (loop_callback != NULL)
                {
                    struct sockaddr_storage l_addr;
                    if (picoquic_store_loopback_addr(&l_addr, sock_af[0], sock_ports[0]) == 0)
                    {
                        ret = loop_callback(quic, picoquic_packet_loop_port_update, loop_callback_ctx, &l_addr);
                    }
                }
            }
            else
            {
                /* Testing organized migration */
                if (nb_sockets < PICOQUIC_PACKET_LOOP_SOCKETS_MAX && last_cnx != NULL)
                {
                    struct sockaddr_storage local_address;
                    picoquic_store_addr(&local_address, (struct sockaddr *)&last_cnx->path[0]->local_addr);
                    if (local_address.ss_family == AF_INET6)
                    {
                        ((struct sockaddr_in6 *)&local_address)->sin6_port = next_port;
                    }
                    else if (local_address.ss_family == AF_INET)
                    {
                        ((struct sockaddr_in *)&local_address)->sin_port = next_port;
                    }
                    s_socket[nb_sockets] = s_mig;
                    sock_ports[nb_sockets] = next_port;
                    nb_sockets++;
                    testing_migration = 1;
                    ret = picoquic_probe_new_path(last_cnx, (struct sockaddr *)&last_cnx->path[0]->peer_addr,
                                                  (struct sockaddr *)&local_address, current_time);
                }
                else
                {
                    SOCKET_CLOSE(s_mig);
                }
            }
        }
    }

    if (ret == PICOQUIC_NO_ERROR_TERMINATE_PACKET_LOOP)
    {
        /* Normal termination requested by the application, returns no error */
        ret = 0;
    }

    /* Close the sockets */
    for (int i = 0; i < nb_sockets; i++)
    {
        if (s_socket[i] != INVALID_SOCKET)
        {
            SOCKET_CLOSE(s_socket[i]);
            s_socket[i] = INVALID_SOCKET;
        }
    }

    if (send_buffer != NULL)
    {
        free(send_buffer);
    }

    return ret;
}
#endif
