/*
 * Author: Christian Huitema
 * Copyright (c) 2017, Private Octopus, Inc.
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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WINDOWS
#define WIN32_LEAN_AND_MEAN
#include "getopt.h"
#include <WinSock2.h>
#include <Windows.h>

#define SERVER_CERT_FILE "certs\\cert.pem"
#define SERVER_KEY_FILE "certs\\key.pem"

#else /* Linux */

#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <signal.h>

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
#include <unistd.h>

#define SERVER_CERT_FILE "certs/cert.pem"
#define SERVER_KEY_FILE "certs/key.pem"

#endif

#define MAX_BIND 128

static const int default_server_port = 4443;
static const char *default_server_name = "::";

#include "picoquic.h"
#include "picoquic_packet_loop.h"
#include "picoquic_internal.h"
#include "picoquic_utils.h"
#include "autoqlog.h"
#include "h3zero.h"
#include "democlient.h"
#include "demoserver.h"
#include "siduck.h"
#include "quicperf.h"
#include "picoquic_unified_log.h"
#include "picoquic_logger.h"
#include "picoquic_binlog.h"
#include "performance_log.h"
#include "picoquic_config.h"
#include "picoquic_lb.h"
#include "dpdk_picoquicdemo.h"


// dpdk
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
#include <rte_ether.h>
#include <rte_flow.h>

#define MEMPOOL_CACHE_SIZE 256
#define RTE_TEST_RX_DESC_DEFAULT 1024
#define RTE_TEST_TX_DESC_DEFAULT 1024
#define MAX_NB_OF_PORTS_AND_LCORES 32

#define PICOQUIC_SAMPLE_ALPN "picoquic_sample"
#define PICOQUIC_SAMPLE_SNI "test.example.com"

#define PICOQUIC_SAMPLE_NO_ERROR 0
#define PICOQUIC_SAMPLE_INTERNAL_ERROR 0x101
#define PICOQUIC_SAMPLE_NAME_TOO_LONG_ERROR 0x102
#define PICOQUIC_SAMPLE_NO_SUCH_FILE_ERROR 0x103
#define PICOQUIC_SAMPLE_FILE_READ_ERROR 0x104
#define PICOQUIC_SAMPLE_FILE_CANCEL_ERROR 0x105

#define PICOQUIC_SAMPLE_CLIENT_TICKET_STORE "sample_ticket_store.bin";
#define PICOQUIC_SAMPLE_CLIENT_TOKEN_STORE "sample_token_store.bin";
#define PICOQUIC_SAMPLE_CLIENT_QLOG_DIR ".";
#define PICOQUIC_SAMPLE_SERVER_QLOG_DIR ".";

struct rte_mempool *mb_pools[MAX_NB_OF_PORTS_AND_LCORES];
struct rte_eth_dev_tx_buffer *tx_buffers[MAX_NB_OF_PORTS_AND_LCORES];
struct rte_eth_rxconf rxq_conf;
struct rte_eth_txconf txq_conf;

// server mac
struct rte_ether_addr eth_addr;
uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;

// 1 if in proxy mode
int is_proxy = 0;

// proxy clients/server mac
struct rte_ether_addr client_addr;

// global variables for threads
char *server_name;
int server_port = default_server_port;
int force_migration = 0;
int nb_packets_before_update = 0;
char *client_scenario = NULL;
picoquic_quic_config_t config;
int just_once = 0;
int nb_of_repetition = 1;
int change_ip_client = 0;

// default values
int MAX_PKT_BURST_RX = 32;

int MAX_PKT_BURST_TX = 32;
int dpdk = 0;
int handshake_test = 0;
int request_test = 0;

void print_address(FILE *F_log, struct sockaddr *address, char *label, picoquic_connection_id_t cnx_id)
{
    char hostname[256];

    const char *x = inet_ntop(address->sa_family,
                              (address->sa_family == AF_INET) ? (void *)&(((struct sockaddr_in *)address)->sin_addr) : (void *)&(((struct sockaddr_in6 *)address)->sin6_addr),
                              hostname, sizeof(hostname));

    fprintf(F_log, "%016llx : ", (unsigned long long)picoquic_val64_connection_id(cnx_id));

    if (x != NULL)
    {
        fprintf(F_log, "%s %s, port %d\n", label, x,
                (address->sa_family == AF_INET) ? ((struct sockaddr_in *)address)->sin_port : ((struct sockaddr_in6 *)address)->sin6_port);
    }
    else
    {
        fprintf(F_log, "%s: inet_ntop failed with error # %ld\n", label, WSA_LAST_ERROR(errno));
    }
}

void usage()
{
    fprintf(stderr, "PicoQUIC demo client and server\n");
    fprintf(stderr, "Usage: picoquicdemo <options> [server_name [port [scenario]]] \n");
    fprintf(stderr, "  For the client mode, specify server_name and port.\n");
    fprintf(stderr, "  For the server mode, use -p to specify the port and -d to specify a binding address.\n");
    picoquic_config_usage();
    fprintf(stderr, "Picoquic demo options:\n");
    fprintf(stderr, "  -f migration_mode     Force client to migrate to start migration:\n");
    fprintf(stderr, "                        -f 1  test NAT rebinding,\n");
    fprintf(stderr, "                        -f 2  test CNXID renewal,\n");
    fprintf(stderr, "                        -f 3  test migration to new address.\n");
    fprintf(stderr, "  -d bind               Set the server's address. Can be repeated multiple times to set multiple addresses.\n");
    fprintf(stderr, "  -r                    (dpdk only) Affinitize bound address to queues using rte_flow.\n");
    fprintf(stderr, "  -u nb                 trigger key update after receiving <nb> packets on client\n");
    fprintf(stderr, "  -1                    Once: close the server after processing 1 connection.\n");

    fprintf(stderr, "\nThe scenario argument specifies the set of files that should be retrieved,\n");
    fprintf(stderr, "and their order. The syntax is:\n");
    fprintf(stderr, "  *{[<stream_id>':'[<previous_stream>':'[<format>:]]]path;}\n");
    fprintf(stderr, "where:\n");
    fprintf(stderr, "  <stream_id>:          The numeric ID of the QUIC stream, e.g. 4. By default, the\n");
    fprintf(stderr, "                        next stream in the logical QUIC order, 0, 4, 8, etc.");
    fprintf(stderr, "  <previous_stream>:    The numeric ID of the previous stream. The GET command will\n");
    fprintf(stderr, "                        be issued after that stream's transfer finishes. By default,\n");
    fprintf(stderr, "                        previous stream in this scenario.\n");
    fprintf(stderr, "  <format>:             Whether the received file should be written to disc as\n");
    fprintf(stderr, "                        binary(b) or text(t). Defaults to text.\n");
    fprintf(stderr, "  <path>:               The name of the document that should be retrieved\n");
    fprintf(stderr, "If no scenario is specified, the client executes the default scenario.\n");
    exit(1);
}

int dpdk_init_mbuf_txbuffer(uint16_t portid, int index)
{

    char mbuf_pool_name[20] = "mbuf_pool_X";
    char tx_buffer_name[20] = "tx_buffer_X";
    int index_of_X;
    char char_i = portid;
    index_of_X = strlen(mbuf_pool_name) - 1;
    mbuf_pool_name[index_of_X] = char_i;
    unsigned nb_mbufs = 8192U;
    int ret = 0;
    mb_pools[index] = rte_pktmbuf_pool_create(mbuf_pool_name, nb_mbufs,
                                              MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                              rte_socket_id());
    if (mb_pools[index] == NULL)
    {
        printf("fail to init mb_pool\n");
        rte_exit(EXIT_FAILURE, "%s\n", rte_strerror(rte_errno));
        return 0;
    }
    ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd, rte_eth_dev_socket_id(portid), &rxq_conf, mb_pools[index]);
    if (ret != 0)
    {
        printf("failed to init rx_queue\n");
    }

    index_of_X = strlen(tx_buffer_name) - 1;
    tx_buffer_name[index_of_X] = char_i;
    tx_buffers[index] = rte_zmalloc_socket(tx_buffer_name,
                                           RTE_ETH_TX_BUFFER_SIZE(MAX_PKT_BURST_TX), 0,
                                           rte_eth_dev_socket_id(portid));
    if (tx_buffers[index] == NULL)
    {
        printf("fail to init buffer\n");
        return 0;
    }

}

// client is scaling on the number of ports
int dpdk_init_port_client(uint16_t portid)
{
    int ret = 0;
    int queueid = 0;
    struct rte_eth_dev_info dev_info;

    static struct rte_eth_conf local_port_conf = {
        .rxmode = {
            .split_hdr_size = 0,
        },
        .txmode = {
            .mq_mode = ETH_MQ_TX_NONE,
        },
    };
    ret = rte_eth_dev_info_get(portid, &dev_info);
    rxq_conf = dev_info.default_rxconf;
    rxq_conf.offloads = local_port_conf.rxmode.offloads;
    if (ret != 0)
        rte_exit(EXIT_FAILURE,
                 "Error during getting device (port %u) info: %s\n",
                 0, strerror(-ret));

    if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
        local_port_conf.txmode.offloads |=
            DEV_TX_OFFLOAD_MBUF_FAST_FREE;
    ret = rte_eth_dev_configure(portid, 1, 1, &local_port_conf);
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

    // init tx queue
    txq_conf = dev_info.default_txconf;
    txq_conf.offloads = local_port_conf.txmode.offloads;
    ret = rte_eth_tx_queue_setup(portid, queueid, nb_txd,
                                 rte_eth_dev_socket_id(portid),
                                 &txq_conf);
    if (ret != 0)
    {
        printf("failed to init queue\n");
        return 0;
    }
}

int dpdk_init_flow_rules(uint16_t nb_of_queues, struct sockaddr_storage* bind, int bind_nr) {
    int portid = 0;
    int ret = 0;
    for (int queueid = 0; queueid < nb_of_queues; queueid++) {
        if (true) {
            struct rte_flow_attr attr = {0};
            struct rte_flow_item pattern[3] = {0};
            struct rte_flow_action actions[2] = {0};
            struct rte_flow_item_eth eth = {0};

            bzero(&eth,sizeof(struct rte_flow_item_eth));


            struct rte_flow *flow;
            struct rte_flow_error error;
            struct rte_flow_action_queue queue = {0};

            attr.ingress = 1;
            attr.group = 0;

            /* setting the eth to pass all packets */
            pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
            pattern[0].spec = &eth;

            const int p = 0;
            /* set the dst ipv4 packet to the required value */
            struct sockaddr* addr = &bind[queueid % bind_nr];
            if (addr->sa_family == AF_INET) {
                printf("v4 addr");
                struct rte_flow_item_ipv4 ipv4 = {0};
                struct rte_flow_item_ipv4 ipv4_mask = {0};
                ipv4.hdr.dst_addr = htonl(((struct sockaddr_in*)addr)->sin_addr.s_addr);
                pattern[p].type = RTE_FLOW_ITEM_TYPE_IPV4;
                pattern[p].spec = &ipv4;
                pattern[p].mask = &ipv4_mask;
            } else {
                printf("v6addr ", addr);
                struct rte_flow_item_ipv6 ipv6 = {0};
                bzero(&ipv6,sizeof(struct rte_flow_item_ipv6));
                struct rte_flow_item_ipv6 ipv6_mask = {0};
                bzero(&ipv6_mask,sizeof(struct rte_flow_item_ipv6));
                for (int i = 0; i < 16; i++) {
                    ipv6.hdr.dst_addr[i] = ((struct sockaddr_in6*)addr)->sin6_addr.__in6_u.__u6_addr8[i];
                    printf("%x",ipv6.hdr.dst_addr[i]);
                    ipv6_mask.hdr.dst_addr[i] = 0xff;
                }
                printf("\n");
                pattern[p].type = RTE_FLOW_ITEM_TYPE_IPV6;
                pattern[p].spec = &ipv6;
                pattern[p].mask = &ipv6_mask;
                pattern[p].last = 0;
            }

            /* end the pattern array */
            pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

            /* create the drop action */
            actions[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
            actions[0].conf = &queue;
            queue.index = queueid;
            actions[1].type = RTE_FLOW_ACTION_TYPE_END;

            /* validate and create the flow rule */
            if (!(ret = rte_flow_validate(portid, &attr, pattern, actions, &error))) {
                flow = rte_flow_create(portid, &attr, pattern, actions, &error);
                if (!flow)
                    printf("Could not create flow rule : %d (%s)!\n", ret, error.message);
                else
                    printf("Rule for queue %d installed!\n", queueid);
            } else {
                printf("Could not valide flow rule : %d (%s)!\n", ret, error.message);
            }
        }
    }
}

// client is scaling on the number of cores
int dpdk_init_port_server(uint16_t nb_of_queues)
{
    int ret = 0;
    int portid = 0;
    struct rte_eth_rxconf rxq_conf;
    struct rte_eth_txconf txq_conf;
    struct rte_eth_dev_info dev_info;

    static struct rte_eth_conf local_port_conf = {
        .rxmode = {
            .mq_mode = ETH_MQ_RX_RSS,
        },
        .rx_adv_conf = {
            .rss_conf = {
                .rss_key = NULL,
                .rss_hf = ETH_RSS_IP | ETH_RSS_TCP | ETH_RSS_UDP,
            },
        },
        .txmode = {
            .mq_mode = ETH_MQ_TX_NONE,
        },
    };
    ret = rte_eth_dev_info_get(0, &dev_info);
    if (ret != 0)
        rte_exit(EXIT_FAILURE,
                 "Error during getting device (port %u) info: %s\n",
                 0, strerror(-ret));

    // We do not use reference counting in buffers, so we can enable FAST_FREE
    if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
        local_port_conf.txmode.offloads |=
            DEV_TX_OFFLOAD_MBUF_FAST_FREE;

    ret = rte_eth_dev_configure(portid, nb_of_queues, nb_of_queues, &local_port_conf);
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

    char mbuf_pool_name[20] = "mbuf_pool_X";
    char tx_buffer_name[20] = "tx_buffer_X";
    int index_of_X;
    char char_i;
    unsigned nb_mbufs = 8192U * nb_of_queues;

    mb_pools[0] = rte_pktmbuf_pool_create(mbuf_pool_name, nb_mbufs,
                                          MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                                          rte_socket_id());

    if (mb_pools[0] == NULL)
    {
        printf("fail to init mb_pool\n");
        rte_exit(EXIT_FAILURE, "%s\n", rte_strerror(rte_errno));
        return 0;
    }

    for (int queueid = 0; queueid < nb_of_queues; queueid++)
    {
        // init tx queue
        txq_conf = dev_info.default_txconf;
        txq_conf.offloads = local_port_conf.txmode.offloads;
        ret = rte_eth_tx_queue_setup(portid, queueid, nb_txd,
                                     rte_eth_dev_socket_id(portid),
                                     &txq_conf);
        if (ret != 0)
        {
            printf("failed to init queue\n");
            return 0;
        }

        // init rx queue
        rxq_conf = dev_info.default_rxconf;
        rxq_conf.offloads = local_port_conf.rxmode.offloads;

        ret = rte_eth_rx_queue_setup(portid, queueid, nb_rxd, rte_eth_dev_socket_id(portid), &rxq_conf, mb_pools[portid]);
        if (ret != 0)
        {
            printf("failed to init rx_queue\n");
        }
        char_i = queueid + '0';
        index_of_X = strlen(tx_buffer_name) - 1;
        tx_buffer_name[index_of_X] = char_i;
        tx_buffers[queueid] = rte_zmalloc_socket(tx_buffer_name,
                                                 RTE_ETH_TX_BUFFER_SIZE(MAX_PKT_BURST_RX), 0,
                                                 rte_eth_dev_socket_id(0));
        if (tx_buffers[queueid] == NULL)
        {
            printf("fail to init buffer\n");
            return 0;
        }
    }
}

static int
client_job(void *arg)
{
    unsigned *tab = (unsigned *)arg;
    unsigned portid = *((unsigned *)arg);
    printf("port_id : %u\n",portid);
    unsigned queueid = 0;
    unsigned lcore_id = rte_lcore_id();

    // giving a different IP for each client using the portid
    // two variant here, usefull when testing RSS
    uint32_t ip;
    if(change_ip_client){
        ip = (20U << 24) | (portid << 16) | (0 << 8) | 1;
    }
    else{
        ip = (198U << 24) | (18 << 16) | (portid << 8) | 1;
    }
    struct in_addr ip_addr;
    ip_addr.s_addr = rte_cpu_to_be_32(ip);
    printf("The IP address of client %u is %u\n", portid, rte_cpu_to_be_32(ip));

    struct sockaddr_storage addr_from;

    (*(struct sockaddr_in *)(&addr_from)).sin_family = AF_INET;
    (*(struct sockaddr_in *)(&addr_from)).sin_port = htons(4443);
    (*(struct sockaddr_in *)(&addr_from)).sin_addr.s_addr = rte_cpu_to_be_32(ip);

    // proxy_mode

    if (is_proxy)
    {
        unsigned main_port = 1;
        unsigned proxy_port = 0;
        proxy_ctx_t proxy_ctx;
        proxy_ctx.portid = proxy_port;
        proxy_ctx.queueid = 0;
        proxy_ctx.mb_pool= mb_pools[main_port];
        proxy_ctx.client_addr = &client_addr;

        quic_client(server_name,
                    server_port,
                    &config,
                    force_migration,
                    nb_packets_before_update,
                    client_scenario, handshake_test, request_test,
                    dpdk,
                    MAX_PKT_BURST_RX,
                    MAX_PKT_BURST_TX,  
                    main_port, queueid, &addr_from, &eth_addr, mb_pools[main_port], tx_buffers[main_port],&proxy_ctx);
    }
    else
    {
        // handshake test
        if (handshake_test)
        {
            struct timeval start_time;
            struct timeval current_time;
            for (int i = 0; i < nb_of_repetition; i++)
            {
                gettimeofday(&start_time, NULL);
                gettimeofday(&current_time, NULL);
                int counter = 0;
                while ((current_time.tv_sec - start_time.tv_sec) < 20)
                {
                    quic_client(server_name,
                                server_port,
                                &config,
                                force_migration,
                                nb_packets_before_update,
                                client_scenario, handshake_test, request_test,
                                dpdk,
                                MAX_PKT_BURST_RX,
                                MAX_PKT_BURST_TX,  
                                portid, queueid, &addr_from, &eth_addr, mb_pools[portid], tx_buffers[portid],NULL);
                    counter++;
                    gettimeofday(&current_time, NULL);
                }
                printf("Number of handshakes served : %d\n", counter);
                sleep(2);
            }
        }
        else
        {
            for (int i = 0; i < nb_of_repetition; i++)
            {
                quic_client(server_name,
                            server_port,
                            &config,
                            force_migration,
                            nb_packets_before_update,
                            client_scenario, handshake_test, request_test,
                            dpdk,
                            MAX_PKT_BURST_RX, 
                            MAX_PKT_BURST_TX,
                            portid, queueid, &addr_from, &eth_addr, mb_pools[portid], tx_buffers[portid],NULL);
                sleep(2);
            }
        }
    }
}

static int
server_job(void *arg)
{
    unsigned portid = 0;
    demo_config_t* demo_config = (demo_config_t*)arg;
    struct sockaddr_storage addr_from = demo_config->bind;


    if(is_proxy){
        unsigned main_port = 0;
        unsigned proxy_port = 1;
        proxy_ctx_t proxy_ctx;
        proxy_ctx.portid = proxy_port;
        proxy_ctx.queueid = 0;
        proxy_ctx.mb_pool= mb_pools[main_port];
        proxy_ctx.client_addr = &client_addr;

        quic_server(server_name,
                &config,
                demo_config,
                just_once,
                dpdk,
                MAX_PKT_BURST_RX,
                MAX_PKT_BURST_TX,  portid, &addr_from, NULL, mb_pools[main_port], tx_buffers[demo_config->queueid],&proxy_ctx);
    

    }
    else{
        quic_server(server_name,
                &config,
                demo_config,
                just_once,
                dpdk,
                MAX_PKT_BURST_RX,
                MAX_PKT_BURST_TX,  portid, &addr_from, NULL, mb_pools[portid], tx_buffers[demo_config->queueid],NULL);
    }
}

int check_ports_lcores_numbers()
{
    int nbr_of_ports = 0;
    int nbr_of_lcores = 0;
    unsigned portid;
    unsigned lcore_id;

    RTE_ETH_FOREACH_DEV(portid)
    {
        nbr_of_ports++;
    }

    RTE_LCORE_FOREACH_WORKER(lcore_id)
    {
        nbr_of_lcores++;
    }
    if (nbr_of_lcores != nbr_of_ports)
    {
        printf("nbr_of_lcores : %u\n", nbr_of_lcores);
        printf("nbr_of_ports %u\n", nbr_of_ports);
        return -1;
    }
    return 0;
}

int get_nb_port()
{
    int count = 0;
    unsigned portid;
    RTE_ETH_FOREACH_DEV(portid)
    {
        count++;
    }
    return count;
}

int get_nb_core()
{
    int count = 0;
    unsigned lcore_id;
    RTE_LCORE_FOREACH_WORKER(lcore_id)
    {
        count++;
    }
    return count;
}

int str_to_mac(char *mac_txt, struct rte_ether_addr *mac_addr)
{
    int values[6];
    int i;
    if (6 == sscanf(mac_txt, "%x:%x:%x:%x:%x:%x%*c",
                    &values[0], &values[1], &values[2],
                    &values[3], &values[4], &values[5]))
    {
        /* convert to uint8_t */
        for (i = 0; i < 6; ++i)
        {
            (mac_addr->addr_bytes)[i] = (uint8_t)values[i];
        }
        return 0;
    }

    else
    {
        printf("invalid mac address : %s\n", mac_txt);
        return -1;
    }
}

demo_config_t demo_configs[MAX_NB_OF_PORTS_AND_LCORES];

void sig_handler(int signum) {
    unsigned lcore_id;
    printf("SIG received, terminating server...\n");

    RTE_LCORE_FOREACH_WORKER(lcore_id)
    {
        demo_configs[lcore_id].is_running = false;
    }
}

int main(int argc, char **argv)
{
    char option_string[512];
    int opt;
    char default_server_cert_file[512];
    char default_server_key_file[512];
    struct sockaddr_storage bind[MAX_BIND];
    int bind_n = 0;
    //should use those values for client
    (*(struct sockaddr_in *)(&bind[0])).sin_family = AF_INET;
    (*(struct sockaddr_in *)(&bind[0])).sin_port = htons(4443);
    (*(struct sockaddr_in *)(&bind[0])).sin_addr.s_addr = inet_addr("10.100.0.2");

    int isIpv4 = 1;
    int is_client = 0;
    int ret;
    unsigned portid;
    unsigned lcore_id;
    unsigned args[2];
    int flow = 0;
    server_name = default_server_name;

    if (strcmp(argv[1], "--dpdk") == 0)
    {
        dpdk = 1;
    }
    argc--;
    argv++;
    if (dpdk)
    {
        ret = rte_eal_init(argc, argv);
        if (ret < 0)
            rte_panic("Cannot init EAL\n");
        argc -= ret;
        argv += ret;
        printf("EAL setup finshed\n");
    }
#ifdef _WINDOWS
    WSADATA wsaData = {0};
    (void)WSA_START(MAKEWORD(2, 2), &wsaData);
#endif
    picoquic_config_init(&config);
    char params[] = "u:f:A:N:@:*:2:d:3H1rX";
    int length = strlen(params);
    memcpy(option_string, params, length);
    ret = picoquic_config_option_letters(option_string + length, sizeof(option_string) - length, NULL);

    if (ret == 0)
    {
        /* Get the parameters */
        while ((opt = getopt(argc, argv, option_string)) != -1)
        {
            switch (opt)
            {
            case 'u':
                if ((nb_packets_before_update = atoi(optarg)) <= 0)
                {
                    fprintf(stderr, "Invalid number of packets: %s\n", optarg);
                    usage();
                }
                break;
            case 'f':
                force_migration = atoi(optarg);
                if (force_migration <= 0 || force_migration > 3)
                {
                    fprintf(stderr, "Invalid migration mode: %s\n", optarg);
                    usage();
                }
                break;
            case 'r':
                flow = 1;
                break;
            case 'H':
                handshake_test = 1;
                break;
            case '1':
                just_once = 1;
                break;
            case 'd':
                 ret = inet_pton(AF_INET, optarg, &((*(struct sockaddr_in *)(&bind[bind_n])).sin_addr.s_addr ));
                 if (ret != 1) {
                    ret = inet_pton(AF_INET6, optarg, &((*(struct sockaddr_in6 *)(&bind[bind_n])).sin6_addr ));
                    if (ret == 1) {
                        bind[bind_n].ss_family = AF_INET6;
                    }
                 } else {
                    bind[bind_n].ss_family = AF_INET;
                 }
                if (ret != 1) {
                            fprintf(stderr, "Invalid IPv4 or IPv6 address: %s\n", optarg);
                    usage();
                } else
                    bind_n++;

                break;
            case 'X':
                //Set new IPs for the clients when testing RSS
                change_ip_client = 1;
                break;
                
            case '2':
                if (str_to_mac(optarg, &client_addr) != 0)
                {
                    return -1;
                }
                break;
            case '3':
                request_test = 1;
                break;

            case 'A':
                if (str_to_mac(optarg, &eth_addr) != 0)
                {
                    return -1;
                }
                break;
            case '@':
                MAX_PKT_BURST_RX = atoi(optarg);
                break;
            case '*':
                MAX_PKT_BURST_TX = atoi(optarg);
                break;
            case 'N':
                ;
                int rep = atoi(optarg);
                if (rep > 0)
                {
                    nb_of_repetition = atoi(optarg);
                }
                break;
            default:
                if (picoquic_config_command_line(opt, &optind, argc, (char const **)argv, optarg, &config) != 0)
                {
                    usage();
                }
                break;
            }
        }
    }

    /* Simplified style params */
    if (optind < argc)
    {
        server_name = argv[optind++];
        is_client = 1;
    }

    if (optind < argc)
    {
        if ((server_port = atoi(argv[optind++])) <= 0)
        {
            fprintf(stderr, "Invalid port: %s\n", optarg);
            usage();
        }
    }

    if (optind < argc)
    {
        client_scenario = argv[optind++];
    }

    if (optind < argc)
    {
        usage();
    }
    if (config.alpn != NULL && strcmp(config.alpn, "proxy") == 0)
    {
        is_proxy = 1;
    }

    if (bind_n == 0)
        bind_n = 1;

    if (is_client == 0)
    {
        if(dpdk){
            signal(SIGINT,sig_handler); // Register signal handler
        }
        
        if (config.server_port == 0)
        {
            config.server_port = server_port;
        }

        if (config.server_cert_file == NULL &&
            picoquic_get_input_path(default_server_cert_file, sizeof(default_server_cert_file), config.solution_dir, SERVER_CERT_FILE) == 0)
        {
            /* Using set option call to ensure proper memory management*/
            picoquic_config_set_option(&config, picoquic_option_CERT, default_server_cert_file);
        }

        if (config.server_key_file == NULL &&
            picoquic_get_input_path(default_server_key_file, sizeof(default_server_key_file), config.solution_dir, SERVER_KEY_FILE) == 0)
        {
            /* Using set option call to ensure proper memory management*/
            picoquic_config_set_option(&config, picoquic_option_KEY, default_server_key_file);
        }

        /* Run as server */
        unsigned index_lcore = 0;
        printf("Starting Picoquic server (v%s) on port %d, server name = %s, just_once = %d, do_retry = %d\n",
               PICOQUIC_VERSION, config.server_port, server_name, just_once, config.do_retry);

        unsigned portids[MAX_NB_OF_PORTS_AND_LCORES];
        int index_port = 0;

        //We assign one queue per core
        if(dpdk){
            
            RTE_LCORE_FOREACH_WORKER(lcore_id)
            {
                demo_configs[lcore_id].is_running = 1;
                demo_configs[lcore_id].queueid = index_lcore;
                demo_configs[lcore_id].bind = bind[index_lcore % bind_n];

                index_lcore++;
            }
        }

        if (is_proxy)
        {
            RTE_ETH_FOREACH_DEV(portid)
            {
                portids[index_port] = portid;
                dpdk_init_port_client(portid);
                dpdk_init_mbuf_txbuffer(portid, index_port);
                ret = rte_eth_dev_start(portid);
                if (ret != 0)
                {
                    printf("failed to start device\n");
                }
                index_port++;
            }
            
            unsigned index_lcore = 0;
            RTE_LCORE_FOREACH_WORKER(lcore_id)
            {
                rte_eal_remote_launch(server_job, &demo_configs[lcore_id], lcore_id);
            }
        }
        else
        {
            if (dpdk)
            {
                dpdk_init_port_server(get_nb_core());
                ret = rte_eth_dev_start(0);
                if (ret != 0)
                {
                    printf("failed to start device\n");
                }

                if (flow)
                    dpdk_init_flow_rules(get_nb_core(), bind, bind_n);

                RTE_LCORE_FOREACH_WORKER(lcore_id)
                {
                    printf("Launch %d\n", lcore_id);
                    rte_eal_remote_launch(server_job, &demo_configs[lcore_id], lcore_id);
                }
            }
            else
            {
                ret = quic_server(server_name, &config,
                &demo_configs[0],
                just_once, dpdk, MAX_PKT_BURST_RX,MAX_PKT_BURST_TX, 0,
                NULL, NULL, NULL, NULL, NULL);
            }
        }

        printf("Server exit with code = %d\n", ret);
    }
    else
    {
        unsigned portids[MAX_NB_OF_PORTS_AND_LCORES];
        int index_port = 0;

        /* Run as client */
        if (is_proxy)
        {
            RTE_ETH_FOREACH_DEV(portid)
            {
                portids[index_port] = portid;
                dpdk_init_port_client(portid);
                dpdk_init_mbuf_txbuffer(portid, index_port);
                ret = rte_eth_dev_start(portid);
                if (ret != 0)
                {
                    printf("failed to start device\n");
                }
                index_port++;
            }
            
            unsigned index_lcore = 0;

            printf("Starting Picoquic (v%s) connection to server = %s, port = %d\n", PICOQUIC_VERSION, server_name, server_port);
            RTE_LCORE_FOREACH_WORKER(lcore_id)
            {
                rte_eal_remote_launch(client_job, &portids[index_lcore], lcore_id);
                index_lcore++;
            }
        }
        else
        {
            if (dpdk)
            {
                
                RTE_ETH_FOREACH_DEV(portid)
                {
                    portids[index_port] = portid;
                    dpdk_init_port_client(portid);
                    dpdk_init_mbuf_txbuffer(portid, index_port);
                    ret = rte_eth_dev_start(portid);
                    if (ret != 0)
                    {
                        printf("failed to start device\n");
                    }
                    index_port++;
                }
                if (check_ports_lcores_numbers() != 0)
                {
                    printf("mismatch between the number of lcore and ports\n");
                    return -1;
                }
                unsigned index_lcore = 0;

                printf("Starting Picoquic (v%s) connection to server = %s, port = %d\n", PICOQUIC_VERSION, server_name, server_port);
                RTE_LCORE_FOREACH_WORKER(lcore_id)
                {

                    rte_eal_remote_launch(client_job, &portids[index_lcore], lcore_id);
                    index_lcore++;
                }
            }
            else
            {
                if (handshake_test)
                {
                    struct timeval start_time;
                    struct timeval current_time;
                    
                    for (int i = 0; i < nb_of_repetition; i++){
                        gettimeofday(&start_time, NULL);
                        gettimeofday(&current_time, NULL);
                        int counter = 0;
                        while ((current_time.tv_sec - start_time.tv_sec) < 20)
                        {
                            ret = quic_client(server_name, server_port, &config,
                                            force_migration, nb_packets_before_update, client_scenario, handshake_test, request_test, dpdk, 
                                            MAX_PKT_BURST_RX,
                                            MAX_PKT_BURST_TX,  0, 0, NULL, NULL, NULL, NULL,NULL);
                            counter++;
                            gettimeofday(&current_time, NULL);
                        }
                        printf("Number of request served : %d\n", counter);
                        sleep(2);
                    }
                    
                }
                else
                {
                    printf("Starting Picoquic (v%s) connection to server = %s, port = %d\n", PICOQUIC_VERSION, server_name, server_port);
                    for (int i = 0; i < nb_of_repetition; i++)
                    {
                        ret = quic_client(server_name, server_port, &config,
                                          force_migration, nb_packets_before_update, client_scenario, handshake_test, request_test, dpdk, 
                                          MAX_PKT_BURST_RX,
                                          MAX_PKT_BURST_TX,  0, 0, NULL, NULL, NULL, NULL,NULL);
                        sleep(2);
                    }
                }
            }
        }
        printf("Client exit with code = %d\n", ret);
    }
    rte_eal_mp_wait_lcore();
    /* clean up the EAL */
    rte_eal_cleanup();
    picoquic_config_clear(&config);
}
