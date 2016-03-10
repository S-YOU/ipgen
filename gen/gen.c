/*
 * Copyright (c) 2016 Internet Initiative Japan, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <pthread.h>
#include <stdio.h>
#include <getopt.h>
#include <poll.h>
#include <err.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <net/if.h>
#include <net/netmap.h>
#define NETMAP_WITH_LIBS
#include "netmap_user_localdebug.h"
#include <net/netmap_user.h>
#include <machine/atomic.h>
#include <net/ethernet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/udp.h>
#include <netinet/tcp.h>
#include <net/if_arp.h>
#include <arpa/inet.h>

#include "arpresolv.h"
#include "libpkt/libpkt.h"
#include "libaddrlist/libaddrlist.h"
#include "util.h"
#include "webserv.h"
#include "gen.h"
#include "pbuf.h"
#include "sequencecheck.h"
#include "item.h"
#include "genscript.h"
#include "flowparse.h"

#include "pktgen_item.h"

#define	LINKSPEED_1GBPS		1000000000ULL

#define	DEFAULT_IFG		12	/* Inter Packet Gap */
#define	DEFAULT_PREAMBLE	(7 + 1)	/* preamble + SDF */
#define	FCS			4
#define	ETHHDRSIZE		(6 + 6 + 2)
#define	PKTSIZE2FRAMESIZE(x)	((x) + DEFAULT_IFG + DEFAULT_PREAMBLE + FCS)
#define	CALC_MBPS(pktsize, pps)	\
	((PKTSIZE2FRAMESIZE((pktsize) + ETHHDRSIZE) * (pps)) * 8.0 / 1000 / 1000)

#define	PORT_DEFAULT		9	/* discard port */
#define MAXFLOWNUM		65536


static void rfc2544_showresult(void);
static void quit(void);

double ipgen_version = 1.21;

#define DISPLAY_UPDATE_HZ	20
#define DEFAULT_PPS_HZ		1000
int pps_hz = DEFAULT_PPS_HZ;
int opt_npkt_sync = 0x7fffffff;
int opt_nflow = 0;

int use_ipv6 = 0;
int verbose = 0;
char *opt_debug = NULL;
int debug_tcpdump_fd;

int opt_fulldup = 0;
int opt_txonly = 0;
int opt_rxonly = 0;
int opt_gentest = 0;
int opt_addrrange = 0;
int opt_saddr = 0;
int opt_daddr = 0;
int opt_allnet = 0;
int opt_fragment = 0;
int opt_tcp = 0;
int opt_udp = 1;	/* default */
int opt_ipg = 0;
int opt_rfc2544 = 0;
int opt_rfc2544_trial_duration = 60;	/* default 60sec */


#ifdef IPG_HACK
int support_ipg = 0;
#endif


uint16_t opt_srcport_begin = PORT_DEFAULT;
uint16_t opt_srcport_end = PORT_DEFAULT;
uint16_t opt_dstport_begin = PORT_DEFAULT;
uint16_t opt_dstport_end = PORT_DEFAULT;

int opt_srcaddr_af;
int opt_dstaddr_af;
struct in_addr opt_srcaddr_begin;
struct in_addr opt_srcaddr_end;
struct in_addr opt_dstaddr_begin;
struct in_addr opt_dstaddr_end;
struct in6_addr opt_srcaddr6_begin;
struct in6_addr opt_srcaddr6_end;
struct in6_addr opt_dstaddr6_begin;
struct in6_addr opt_dstaddr6_end;

int opt_flowsort = 0;
int opt_flowdump = 0;
char *opt_flowlist = NULL;

int min_pktsize = 46;	/* udp4:46, tcp4:58, udp6:66, tcp6:78 */

int force_redraw_screen = 0;
int do_quit = 0;

struct genscript *genscript;
FILE *logfp;

struct itemlist *itemlist;
char msgbuf[1024];

pthread_t txrxthread0;
pthread_t txrxthread1;
pthread_t controlthread;

const uint8_t eth_zero[6] = { 0, 0, 0, 0, 0, 0 };

/* sizeof(struct seqdata) = 18 bytes */
struct seqdata {
	uint16_t magic;
	uint32_t seq;
	uint32_t seq_flowid;
	uint32_t seq_perflow;
	uint32_t time32;
} __packed;
static uint16_t seq_magic;

struct interface {
	int opened;
	struct nm_desc *nm_desc;
	char ifname[32];
	char decorated_ifname[64];
	char twiddle[32];
	unsigned int pktsize;

	struct {
		uint64_t tx_last;
		uint64_t rx_last;
		uint64_t tx_delta;
		uint64_t rx_delta;
		uint64_t tx_byte_last;
		uint64_t rx_byte_last;
		uint64_t tx_byte_delta;
		uint64_t rx_byte_delta;
		double tx_Mbps;
		double rx_Mbps;
		uint64_t tx_etc;	/* arpreply, icmp-echoreply, etc */
		uint64_t tx;		/* include tx_etc */
		uint64_t rx;		/* not include rx_* */
		uint64_t rx_flow;
		uint64_t rx_arp;
		uint64_t rx_udp;
		uint64_t rx_tcp;
		uint64_t rx_icmpother;
		uint64_t rx_icmpecho;
		uint64_t rx_icmpunreach;
		uint64_t rx_icmpredirect;
		uint64_t rx_other;
		uint64_t tx_underrun;
		uint64_t rx_seqrewind;

		uint64_t rx_seqdrop;
		uint64_t rx_seqdrop_last;
		uint64_t rx_dup;
		uint64_t rx_dup_last;
		uint64_t rx_reorder;
		uint64_t rx_reorder_last;
		uint64_t rx_seqdrop_flow;
		uint64_t rx_seqdrop_flow_last;
		uint64_t rx_dup_flow;
		uint64_t rx_dup_flow_last;
		uint64_t rx_reorder_flow;
		uint64_t rx_reorder_flow_last;

		uint64_t tx_byte;
		uint64_t rx_byte;

		double latency_min;
		double latency_max;
		double latency_avg;
		double latency_sum;		/* for avg */
		uint64_t latency_npkt;		/* for avg */
	} counter;

	struct addresslist *adrlist;

	struct sequencechecker *seqchecker;	/* receive sequence drop checker */
	struct sequencechecker *seqchecker_flowtotal;
	struct sequencechecker **seqchecker_perflow;

	uint64_t sequence_tx;			/* transmit sequence number */
	uint64_t *sequence_tx_perflow;		/* transmit sequence number per flow*/

	uint32_t transmit_pps;
	uint32_t transmit_pps_max;
	uint32_t transmit_txhz;
	double transmit_Mbps;
	int transmit_enable;
	int need_reset_statistics;

	struct pbufq pbufq;

	struct ether_addr eaddr;	/* my ethernet address */
	struct ether_addr gweaddr;	/* gw ethernet address */
	int af_addr;			/* AF_INET or AF_INET6 */
	struct in_addr ipaddr;		/* my IP address */
	struct in_addr ipaddr_mask;	/* my IP address mask */
	struct in6_addr ip6addr;	/* my IPv6 address */
	struct in6_addr ip6addr_mask;	/* my IPv6 address mask */
	int af_gwaddr;			/* AF_INET or AF_INET6 */
	struct in_addr gwaddr;		/* gw IP address */
	struct in6_addr gw6addr;	/* gw IPv6 address */

} interface[2];

static char pktbuffer_ipv4_udp[2][LIBPKT_PKTBUFSIZE] __attribute__((__aligned__(8)));
static char pktbuffer_ipv4_tcp[2][LIBPKT_PKTBUFSIZE] __attribute__((__aligned__(8)));
static char pktbuffer_ipv6_udp[2][LIBPKT_PKTBUFSIZE] __attribute__((__aligned__(8)));
static char pktbuffer_ipv6_tcp[2][LIBPKT_PKTBUFSIZE] __attribute__((__aligned__(8)));


struct timespec currenttime;
static uint32_t currenttime32;
sigset_t used_sigset;

unsigned int
build_template_packet_ipv4(int ifno, char *pkt)
{
	if (ifno == 0) {
		/* for interface0 -> interface1 */
		ethpkt_src(pkt, (u_char *)&interface[0].eaddr);
		ethpkt_dst(pkt, (u_char *)&interface[0].gweaddr);
		ip4pkt_src(pkt, interface[0].ipaddr.s_addr);
		ip4pkt_dst(pkt, interface[1].ipaddr.s_addr);
	} else {
		/* for interface1 -> interface0 */
		ethpkt_src(pkt, (u_char *)&interface[1].eaddr);
		ethpkt_dst(pkt, (u_char *)&interface[1].gweaddr);
		ip4pkt_src(pkt, interface[1].ipaddr.s_addr);
		ip4pkt_dst(pkt, interface[0].ipaddr.s_addr);
	}

	return interface[ifno].pktsize;
}

unsigned int
build_template_packet_ipv6(int ifno, char *pkt)
{
	if (ifno == 0) {
		/* for interface0 -> interface1 */
		ethpkt_src(pkt, (u_char *)&interface[0].eaddr);
		ethpkt_dst(pkt, (u_char *)&interface[0].gweaddr);
		ip6pkt_src(pkt, &interface[0].ip6addr);
		ip6pkt_dst(pkt, &interface[1].ip6addr);
	} else {
		/* for interface1 -> interface0 */
		ethpkt_src(pkt, (u_char *)&interface[1].eaddr);
		ethpkt_dst(pkt, (u_char *)&interface[1].gweaddr);
		ip6pkt_src(pkt, &interface[1].ip6addr);
		ip6pkt_dst(pkt, &interface[0].ip6addr);
	}

	return interface[ifno].pktsize;
}

inline static uint32_t
timespec_pack32(struct timespec *ts)
{
	return ((ts->tv_sec & 0xff) << 24) + (ts->tv_nsec >> 6);
}

inline static void
timespec_unpack32(uint32_t pt, struct timespec *ts)
{
	ts->tv_sec = (pt >> 24) & 0xff;
	ts->tv_nsec = (pt & 0x00ffffff) << 6;
}

inline static int
in_range(int num, int begin, int end)
{
	if (num < begin)
		return 0;
	if (num > end)
		return 0;
	return 1;
}

static inline int
get_flowid_max(int ifno)
{
	return addresslist_get_tuplenum(interface[ifno].adrlist);
}

static inline int
get_flownum(int ifno)
{
	return addresslist_get_tuplenum(interface[ifno].adrlist);
}

void
touchup_tx_packet(char *buf, int ifno)
{
	static unsigned int id;
	struct seqdata seqdata;
	uint32_t flowid;
	const struct address_tuple *tuple;
	int ipv6;

	if (opt_gentest) {
		/* for benchmark (with -X option) */
		static uint32_t x = 0;

		ip4pkt_src(buf, x);
		ip4pkt_dst(buf, x);
		ip4pkt_srcport(buf, x);
		ip4pkt_dstport(buf, x);
		ip4pkt_length(buf, interface[ifno].pktsize);

	} else {
		flowid = addresslist_get_current_tupleid(interface[ifno].adrlist);
		if (flowid >= opt_nflow) {
			addresslist_set_current_tupleid(interface[ifno].adrlist, 0);
			flowid = 0;
		}
		tuple = addresslist_get_current_tuple(interface[ifno].adrlist);
		addresslist_get_tuple_next(interface[ifno].adrlist);

		if (tuple->saddr.af == AF_INET) {
			if (opt_udp)
				memcpy(buf, pktbuffer_ipv4_udp[ifno], interface[ifno].pktsize + sizeof(struct ether_header));
			else
				memcpy(buf, pktbuffer_ipv4_tcp[ifno], interface[ifno].pktsize + sizeof(struct ether_header));

			ip4pkt_src(buf, tuple->saddr.a.addr4.s_addr);
			ip4pkt_dst(buf, tuple->daddr.a.addr4.s_addr);
			ip4pkt_srcport(buf, tuple->sport);
			ip4pkt_dstport(buf, tuple->dport);

			ip4pkt_length(buf, interface[ifno].pktsize);
			ip4pkt_id(buf, id++);
			if (opt_fragment)
				ip4pkt_off(buf, 1200 | IP_MF);

			ipv6 = 0;
		} else {
			if (opt_udp)
				memcpy(buf, pktbuffer_ipv6_udp[ifno], interface[ifno].pktsize + sizeof(struct ether_header));
			else
				memcpy(buf, pktbuffer_ipv6_tcp[ifno], interface[ifno].pktsize + sizeof(struct ether_header));

			ip6pkt_src(buf, &tuple->saddr.a.addr6);
			ip6pkt_dst(buf, &tuple->daddr.a.addr6);
			ip6pkt_srcport(buf, tuple->sport);
			ip6pkt_dstport(buf, tuple->dport);

			ip6pkt_length(buf, interface[ifno].pktsize);

			ipv6 = 1;
		}


		/* fill in sequence number to payload */
		seqdata.magic = seq_magic;
		seqdata.seq = interface[ifno].sequence_tx++;
		if (!opt_gentest) {
			seqdata.seq_flowid = flowid;
			seqdata.seq_perflow = interface[ifno].sequence_tx_perflow[flowid]++;
		}
		seqdata.time32 = currenttime32;

		if (ipv6) 
			ip6pkt_writedata(buf, 0, (char *)&seqdata, sizeof(seqdata));
		else
			ip4pkt_writedata(buf, 0, (char *)&seqdata, sizeof(seqdata));

	}
}

int
packet_generator(char *buf, int ifno)
{
	touchup_tx_packet(buf, ifno);

	if (opt_debug != NULL)
		tcpdumpfile_output(debug_tcpdump_fd, buf, interface[ifno].pktsize + sizeof(struct ether_header));

	return interface[ifno].pktsize;
}

int
statistics_clear(void)
{
	interface[0].need_reset_statistics = 1;
	interface[1].need_reset_statistics = 1;

	return 0;
}

#ifdef IPG_HACK
/* set Transmit Inter Packet Gap */
static void
set_ipg(const char *ifname, unsigned int ipg)
{
	char buf[256];
	unsigned int unit;

	if (strncmp(ifname, "igb", 3) == 0) {
		unit = strtol(ifname + 3, (char **)NULL, 10);
		snprintf(buf, sizeof(buf), "sysctl -q -w dev.igb.%d.tipg=%d > /dev/null", unit, ipg);

		system(buf);
	}
}
#endif /* IPG_HACK */

static void
reset_ipg(const char *ifname)
{
	char buf[256];
	unsigned int unit;

#ifdef IPG_HACK
	if (!support_ipg)
		return;

	if (strncmp(ifname, "igb", 3) == 0) {
		unit = strtol(ifname + 3, (char **)NULL, 10);
		snprintf(buf, sizeof(buf), "sysctl -q -w dev.igb.%d.tipg=8 > /dev/null", unit);

		system(buf);
	}
#endif /* IPG_HACK */
}

static void
update_transmit_max_sustained_pps(int ifno, int ipg)
{
	uint32_t maxpps;

	maxpps = LINKSPEED_1GBPS / 8 / PKTSIZE2FRAMESIZE(interface[ifno].pktsize + ETHHDRSIZE - DEFAULT_IFG + ipg);

	if (interface[ifno].transmit_pps <= pps_hz)
		maxpps = interface[ifno].transmit_pps;

	interface[ifno].transmit_pps_max = maxpps;
}

static void
calc_ipg(int ifno)
{
	if (!opt_ipg) {
		update_transmit_max_sustained_pps(ifno, DEFAULT_IFG);
		return;
	}

#ifdef IPG_HACK
	if (!support_ipg) {
		update_transmit_max_sustained_pps(ifno, DEFAULT_IFG);
		return;
	}

	if (strncmp(interface[ifno].ifname, "igb", 3) == 0) {
		int new_tipg;

		if (interface[ifno].transmit_pps == 0) {
			new_tipg = INT_MAX;
		} else {
			new_tipg =
			    ((LINKSPEED_1GBPS / 8) / interface[ifno].transmit_pps) -
			    PKTSIZE2FRAMESIZE(interface[ifno].pktsize + ETHHDRSIZE - DEFAULT_IFG);
		}
		new_tipg -= 4;	/* igp(4) NIC, ipg has offset 4 */

		new_tipg -= 1;	/* loosely to set IPG against TX underrun */

		if (new_tipg < 8)
			new_tipg = 8;

		if (new_tipg >= 1024)
			new_tipg = 1023;

		set_ipg(interface[ifno].ifname, new_tipg);
		update_transmit_max_sustained_pps(ifno, new_tipg + 4);
	}
#endif /* IPG_HACK */
}

static void
ipg_enable(int enable)
{
#ifdef IPG_HACK
	if (!support_ipg)
		enable = 0;
#endif
	if (itemlist != NULL) {
		if (enable) {
			itemlist_setvalue(itemlist, ITEMLIST_ID_BUTTON_STEADY, "*");
			itemlist_setvalue(itemlist, ITEMLIST_ID_BUTTON_BURST, NULL);
		} else {
			itemlist_setvalue(itemlist, ITEMLIST_ID_BUTTON_STEADY, NULL);
			itemlist_setvalue(itemlist, ITEMLIST_ID_BUTTON_BURST, "*");
		}
	}

	if (enable) {
		itemlist_setvalue(itemlist, ITEMLIST_ID_BUTTON_BURST, NULL);
		opt_ipg = 1;
		calc_ipg(0);
		calc_ipg(1);

	} else {
		reset_ipg(interface[0].ifname);
		reset_ipg(interface[1].ifname);
		update_transmit_max_sustained_pps(0, DEFAULT_IFG);
		update_transmit_max_sustained_pps(1, DEFAULT_IFG);

		opt_ipg = 0;
	}
}

static void
update_min_pktsize(void)
{
	if (use_ipv6) {
		if (opt_tcp)
			min_pktsize = MAX(min_pktsize, sizeof(struct ip6_hdr) + sizeof(struct tcphdr) + sizeof(struct seqdata));
		else
			min_pktsize = MAX(min_pktsize, sizeof(struct ip6_hdr) + sizeof(struct udphdr) + sizeof(struct seqdata));
	} else {
		if (opt_tcp)
			min_pktsize = MAX(min_pktsize, sizeof(struct ip) + sizeof(struct tcphdr) + sizeof(struct seqdata));
		else
			min_pktsize = MAX(min_pktsize, sizeof(struct ip) + sizeof(struct udphdr) + sizeof(struct seqdata));
	}
}

static void
update_transmit_Mbps(int ifno)
{
	if (interface[ifno].pktsize < min_pktsize)
		interface[ifno].pktsize  = min_pktsize;
	if (interface[ifno].pktsize > 1500)
		interface[ifno].pktsize  = 1500;

	if (interface[ifno].transmit_enable) {
		interface[ifno].transmit_Mbps =
		    (unsigned long long)interface[ifno].transmit_pps *
		    PKTSIZE2FRAMESIZE(interface[ifno].pktsize + ETHHDRSIZE) *
		    8.0 / 1000 / 1000;
	} else {
		interface[ifno].transmit_Mbps = 0.0;
	}
	calc_ipg(ifno);
}

int
setpktsize(int ifno, unsigned int size)
{
	if (size < min_pktsize || size > 1500)
		return -1;

	interface[ifno].pktsize = size;
	update_transmit_Mbps(ifno);

	return 0;
}

unsigned int
getpktsize(int ifno)
{
	return interface[ifno].pktsize;
}

int
setpps(int ifno, unsigned long pps)
{
	interface[ifno].transmit_pps = pps;
	update_transmit_Mbps(ifno);

	return 0;
}

const char *
getifname(int ifno)
{
	return interface[ifno].ifname;
}

unsigned long
getpps(int ifno)
{
	return interface[ifno].transmit_pps;
}

void
transmit_set(int ifno, int on)
{
	if (itemlist != NULL) {
		switch (ifno) {
		case 0:
			if (on) {
				itemlist_setvalue(itemlist, ITEMLIST_ID_IF0_START, "*");
				itemlist_setvalue(itemlist, ITEMLIST_ID_IF0_STOP, NULL);
			} else {
				itemlist_setvalue(itemlist, ITEMLIST_ID_IF0_START, NULL);
				itemlist_setvalue(itemlist, ITEMLIST_ID_IF0_STOP, "*");
			}
			break;
		case 1:
			if (on) {
				itemlist_setvalue(itemlist, ITEMLIST_ID_IF1_START, "*");
				itemlist_setvalue(itemlist, ITEMLIST_ID_IF1_STOP, NULL);
			} else {
				itemlist_setvalue(itemlist, ITEMLIST_ID_IF1_START, NULL);
				itemlist_setvalue(itemlist, ITEMLIST_ID_IF1_STOP, "*");
			}
			break;
		}
	}

	interface[ifno].transmit_enable = on;
	update_transmit_Mbps(ifno);
}

void
interface_up(const char *ifname)
{
	char buf[256];
	snprintf(buf, sizeof(buf), "ifconfig %s up", ifname);
	system(buf);
}

void
interface_wait_linkup(const char *ifname, int verbose)
{
	int i;

	if (!verbose && interface_is_active(ifname))
		return;

	printf("%s: waiting link up.", ifname);
	fflush(stdout);
	for (i = 100; i >= 0; i--) {
		if (interface_is_active(ifname))
			break;
		usleep(500000);
		printf(".");
		fflush(stdout);
	}
	if (i >= 0) {
		printf("OK\n");
	} else {
		printf("giving up\n");
	}
	fflush(stdout);
}

void
interface_setup(int ifno, const char *ifname)
{
	strcpy(interface[ifno].ifname, ifname);
	sprintf(interface[ifno].decorated_ifname, "Interface: %s", ifname);
	getiflinkaddr(ifname, &interface[ifno].eaddr);

	if ((interface[ifno].ipaddr.s_addr == 0) && ipv6_iszero(&interface[ifno].ip6addr)) {
		getifipaddr(ifname, &interface[ifno].ipaddr, &interface[ifno].ipaddr_mask);
		getifip6addr(ifname, &interface[ifno].ip6addr, &interface[ifno].ip6addr_mask);
	}

	if (memcmp(eth_zero, &interface[ifno].gweaddr, ETHER_ADDR_LEN) == 0) {
		/* need to resolv arp */
		struct ether_addr *mac;

		interface_wait_linkup(interface[ifno].ifname, 0);

		mac = arpresolv(ifname, &interface[ifno].ipaddr, &interface[ifno].gwaddr);
		if (mac == NULL) {
			fprintf(stderr, "cannot resolve arp. mac-address of gateway:%s on %s is unknown\n",
			    inet_ntoa(interface[ifno].gwaddr), ifname);
			exit(1);
		}

		memcpy(&interface[ifno].gweaddr, mac, ETHER_ADDR_LEN);

		fprintf(stderr, "arp resolved. %s on %s = %s\n",
		    inet_ntoa(interface[ifno].gwaddr),
		    interface[ifno].ifname,
		    ether_ntoa(&interface[ifno].gweaddr));
	}
}

void
interface_open(int ifno, const char *ifname)
{
	struct nmreq nmreq;

	memset(&nmreq, 0, sizeof(nmreq));

	char *netmapname = malloc(128);
	memset(netmapname, 0, 128);
	sprintf(netmapname, "netmap:%s-0", ifname);

	interface[ifno].nm_desc = nm_open(netmapname, &nmreq, 0, NULL);
	if (interface[ifno].nm_desc == NULL) {
		fprintf(stderr, "cannot open /dev/netmap\n");
		exit(1);
	}

	interface[ifno].seqchecker = seqcheck_new();
	interface[ifno].opened = 1;
}

void
interface_close(int idx)
{
	nm_close(interface[idx].nm_desc);
	reset_ipg(interface[idx].ifname);

	interface[idx].opened = 0;

	if (interface[idx].gwaddr.s_addr != 0) {
		memset(&interface[idx].gweaddr, 0, ETHER_ADDR_LEN);
	}
}

int
interface_need_transmit(int ifno)
{
	int n;

	n = pbufq_nqueued(&interface[ifno].pbufq);

	if (interface[ifno].transmit_enable)
		n += atomic_fetchadd_32(&interface[ifno].transmit_txhz, 0);

	return n;
}

int
interface_load_transmit_packet(int ifno, char *buf, uint16_t *lenp)
{
	if (pbufq_poll(&interface[ifno].pbufq) != NULL) {
		struct pbuf *p;
		p = pbufq_dequeue(&interface[ifno].pbufq);
		memcpy(buf, p->data, p->len);
		*lenp = p->len;
		pbuf_free(p);

		interface[ifno].counter.tx_etc++;

		return 2;	/* control packet */

	} else if (interface[ifno].transmit_enable) {

		for (;;) {
			uint32_t x = atomic_fetchadd_32(&interface[ifno].transmit_txhz, 0);
			if (x) {
				if (atomic_cmpset_32(&interface[ifno].transmit_txhz, x, x - 1))
					break;
			} else {
				return -1;
			}
		}

		int len;
		len = packet_generator(buf, ifno);
		*lenp = len + ETHHDRSIZE;

		return 1;	/* pktgen packet */

	}
	return -1;
}

void
icmpecho_handler(int ifno, char *pkt, int len)
{
	struct pbuf *p;
	int pktlen;

	p = pbuf_alloc(len);
	if (p == NULL) {
		fprintf(stderr, "cannot allocate buffer for icmp request\n");
	} else {
		pktlen = ip4pkt_icmp_echoreply(p->data, pkt, len);
		if (pktlen > 0) {
			ethpkt_src(p->data, (u_char *)&interface[ifno].eaddr);
			ethpkt_dst(p->data, (u_char *)&interface[ifno].gweaddr);
			p->len = pktlen;
			pbufq_enqueue(&interface[ifno].pbufq, p);
		} else {
			pbuf_free(p);
		}
	}
}

void
arp_handler(int ifno, char *pkt)
{
	int pktlen;
	struct ether_addr eaddr;
	struct in_addr ipaddr;
	int op;

	ip4pkt_arpparse(pkt, &op, &eaddr, &ipaddr.s_addr);
	if (op == ARPOP_REPLY) {
		/* ignore arp reply for PacketGenerating */
	}

	/* must to reply arp-query */
	if (op == ARPOP_REQUEST) {
		struct pbuf *p;

		p = pbuf_alloc(ETHER_MAX_LEN);
		if (p == NULL) {
			fprintf(stderr, "cannot allocate buffer for arp request\n");
		} else {
			pktlen = ip4pkt_arpreply(p->data, pkt,
			    interface[ifno].eaddr.octet,
			    interface[ifno].ipaddr.s_addr,
			    interface[ifno].ipaddr_mask.s_addr);

			if (pktlen > 0) {
				p->len = pktlen;
				pbufq_enqueue(&interface[ifno].pbufq, p);
			} else {
				pbuf_free(p);
			}
		}
	}
}

void
interface_receive(int ifno)
{
	char *buf;
	unsigned int cur, n, i;
	int is_ipv6;
	uint16_t len;
	struct netmap_if *nifp;
	struct netmap_ring *rxring;
	struct ether_header *eth;
	struct ip *ip;
	struct ip6_hdr *ip6;
	struct udphdr *udp = NULL;
	struct tcphdr *tcp = NULL;
	struct timespec curtime;

	clock_gettime(CLOCK_MONOTONIC, &curtime);

	nifp = interface[ifno].nm_desc->nifp;
	for (i = interface[ifno].nm_desc->first_rx_ring;
	    i <= interface[ifno].nm_desc->last_rx_ring; i++) {

		rxring = NETMAP_RXRING(nifp, i);
		if (nm_ring_empty(rxring))
			continue;

		cur = rxring->cur;
		for (n = nm_ring_space(rxring); n > 0; n--, cur = nm_ring_next(rxring, cur)) {
			/* receive packet */
			buf = NETMAP_BUF(rxring, rxring->slot[cur].buf_idx);
			len = rxring->slot[cur].len;

			interface[ifno].counter.rx++;
			interface[ifno].counter.rx_byte += PKTSIZE2FRAMESIZE(len);

			/* ignore FLOWCONTROL */
			eth = (struct ether_header *)buf;
			switch (ntohs(eth->ether_type)) {
			case ETHERTYPE_FLOWCONTROL:
				interface[ifno].counter.rx_flow++;
				continue;
			case ETHERTYPE_ARP:
				interface[ifno].counter.rx_arp++;
				arp_handler(ifno, buf);
				continue;
			case ETHERTYPE_IP:
				is_ipv6 = 0;
				break;
			case ETHERTYPE_IPV6:
				is_ipv6 = 1;
				break;
			default:
				interface[ifno].counter.rx_other++;
#if 1
			printf("==== len=%d ====\n", len);
			dumpstr(buf, len);
#endif
				continue;
			}


			if (is_ipv6) {
				/* IPv6 packet */
				ip6 = (struct ip6_hdr *)(eth + 1);
				if (ip6->ip6_nxt == IPPROTO_ICMPV6) {
					struct icmp6_hdr *icmp6 = (struct icmp6_hdr *)(ip6 + 1);	/* XXX: no support extension header */

					switch (icmp6->icmp6_type) {
					case ICMP6_DST_UNREACH:
						interface[ifno].counter.rx_icmpunreach++;
						continue;
					case ND_REDIRECT:
						interface[ifno].counter.rx_icmpredirect++;
						continue;
					case ICMP6_ECHO_REQUEST:
						interface[ifno].counter.rx_icmpecho++;
#if NOTYET
						icmp6echo_handler(ifno, buf, len);
#endif
						continue;
					default:
						interface[ifno].counter.rx_icmpother++;
						printf("icmp6 receive: type=%d, code=%d\n",
						    icmp6->icmp6_type, icmp6->icmp6_code);
						continue;
					}
				}

				switch (ip6->ip6_nxt) {
				case IPPROTO_UDP:
					interface[ifno].counter.rx_udp++;
					udp = (struct udphdr *)(ip6 + 1);
					break;
				case IPPROTO_TCP:
					interface[ifno].counter.rx_tcp++;
					tcp = (struct tcphdr *)(ip6 + 1);
					break;
				default:
					interface[ifno].counter.rx_other++;
					continue;
				}

			} else {
				/* IPv4 packet */
				ip = (struct ip *)(eth + 1);
				if (ip->ip_p == IPPROTO_ICMP) {
					struct icmp *icmp = (struct icmp *)((char *)ip + ip->ip_hl * 4);

					switch (icmp->icmp_type) {
					case ICMP_UNREACH:
						interface[ifno].counter.rx_icmpunreach++;
						continue;
					case ICMP_REDIRECT:
						interface[ifno].counter.rx_icmpredirect++;
						continue;
					case ICMP_ECHO:
						interface[ifno].counter.rx_icmpecho++;
						icmpecho_handler(ifno, buf, len);
						continue;
					default:
						interface[ifno].counter.rx_icmpother++;
						printf("icmp receive: type=%d, code=%d\n",
						    icmp->icmp_type, icmp->icmp_code);
						continue;
					}
				}

				switch (ip->ip_p) {
				case IPPROTO_UDP:
					interface[ifno].counter.rx_udp++;
					udp = (struct udphdr *)((char *)ip + ip->ip_hl * 4);
					break;
				case IPPROTO_TCP:
					interface[ifno].counter.rx_tcp++;
					tcp = (struct tcphdr *)((char *)ip + ip->ip_hl * 4);
					break;
				default:
					interface[ifno].counter.rx_other++;
					continue;
				}
			}

			if ((opt_udp && (udp != NULL)) || (opt_tcp && (tcp != NULL))) {
				/* check sequence */
				struct seqdata *seqdata;
				uint64_t seq, seqflow, nskip;
				uint32_t flowid;
				struct timespec ts, ts_delta;
				double latency;

				if (is_ipv6)
					seqdata = (struct seqdata *)ip6pkt_getptr(buf, 0);
				else
					seqdata = (struct seqdata *)ip4pkt_getptr(buf, 0);

				if (seqdata->magic != seq_magic) {
					/* no ipgen packet? */
					interface[ifno].counter.rx_other++;

				} else {
					timespec_unpack32(seqdata->time32, &ts);

					ts_delta = curtime;
					timespecsub(&ts_delta, &ts);
					ts_delta.tv_sec &= 0xff;
					latency = ts_delta.tv_sec / 1000 + ts_delta.tv_nsec / 1000000.0;

					interface[ifno].counter.latency_sum += latency;
					interface[ifno].counter.latency_npkt++;

					interface[ifno].counter.latency_avg =
					    interface[ifno].counter.latency_sum / 
					    interface[ifno].counter.latency_npkt;


					if ((interface[ifno].counter.latency_min == 0) ||
					    (interface[ifno].counter.latency_min > latency))
						interface[ifno].counter.latency_min = latency;
					if (interface[ifno].counter.latency_max < latency)
						interface[ifno].counter.latency_max = latency;

					seq = seqdata->seq;
					seqflow = seqdata->seq_perflow;
					flowid = seqdata->seq_flowid;
					if (get_flowid_max(ifno) > flowid)
						nskip = seqcheck_receive(interface[ifno].seqchecker_perflow[flowid], seqflow);

					nskip = seqcheck_receive(interface[ifno].seqchecker, seq);
#if 0
					/* DEBUG */
					if (nskip > 2) {
						printf("<seq=%llu, nskip=%llu, tx0=%llu, tx1=%llu>",
						    (unsigned long long)seq,
						    (unsigned long long)nskip,
						    (unsigned long long)interface[0].sequence_tx,
						    (unsigned long long)interface[1].sequence_tx);
						dumpstr(buf, len);
					}
#endif
			}
			}
		}

		rxring->head = rxring->cur = cur;
	}

}

int
interface_transmit(int ifno)
{
	char *buf;
	unsigned int cur, nspace, npkt, n;
	struct netmap_if *nifp;
	struct netmap_ring *txring;
	int sentpkttype;
	struct timespec curtime;

	nifp = interface[ifno].nm_desc->nifp;
	txring = NETMAP_TXRING(nifp, 0);

	nspace = nm_ring_space(txring);
	npkt = interface_need_transmit(ifno);
	n = MIN(nspace, npkt);
	n = MIN(n, opt_npkt_sync);

#if 0
	printf("nspace=%d, npkt=%d\n", nspace, npkt);
#endif

	clock_gettime(CLOCK_MONOTONIC, &curtime);
	currenttime32 = timespec_pack32(&curtime);

	for (cur = txring->cur; n > 0; n--, cur = nm_ring_next(txring, cur)) {
		/* transmit packet */
		buf = NETMAP_BUF(txring, txring->slot[cur].buf_idx);

		sentpkttype = interface_load_transmit_packet(ifno, buf, &txring->slot[cur].len);
		if (sentpkttype < 0)
			break;

		interface[ifno].counter.tx_byte += PKTSIZE2FRAMESIZE(txring->slot[cur].len);
		interface[ifno].counter.tx++;

		txring->slot[cur].flags = (n == 1) ? 0 : NS_MOREFRAG;
	}
	txring->head = txring->cur = cur;

	return 0;
}

/*
 * output json record (1line)
 *  {
 *      "time":12345678,
 *      "statistics":
 *      [
 *          {
 *              "TX":12345,"RXdrop":0,"RXdelta":1000,"TXdelta":1000,"RX":12345,"RXflow":0,"interface":"em0","TXunderrun":0,"TXrate":"100","RXrate":"100"
 *          },
 *          {
 *              "TXrate":"100","RXrate":"100","TXunderrun":0,"interface":"em1","RXflow":0,"RX":12345,"RXdelta":1000,"TXdelta":1000,"RXdrop":0,"TX":12345
 *          }
 *      ]
 *  }
 */
static int
interface_statistics_json(int ifno, char *buf, int buflen)
{
	unsigned long long txdelta_pkt, rxdelta_pkt;
	unsigned long long txdelta_byte, rxdelta_byte;
	long long rxdelta_drop, rxdelta_dup, rxdelta_reorder;
	long long rxdelta_flow_drop, rxdelta_flow_dup, rxdelta_flow_reorder;
	char buf_ipaddr[INET_ADDRSTRLEN], buf_eaddr[sizeof("00:00:00:00:00:00")];
	char buf_gwaddr[INET_ADDRSTRLEN], buf_gweaddr[sizeof("00:00:00:00:00:00")];

	inet_ntop(AF_INET, &interface[ifno].ipaddr, buf_ipaddr, sizeof(buf_ipaddr));
	inet_ntop(AF_INET, &interface[ifno].gwaddr, buf_gwaddr, sizeof(buf_gwaddr));
	ether_ntoa_r(&interface[ifno].eaddr, buf_eaddr);
	ether_ntoa_r(&interface[ifno].gweaddr, buf_gweaddr);

	txdelta_pkt = interface[ifno].counter.tx - interface[ifno].counter.tx_last;
	rxdelta_pkt = interface[ifno].counter.rx - interface[ifno].counter.rx_last;
	txdelta_byte = interface[ifno].counter.tx_byte - interface[ifno].counter.tx_byte_last;
	rxdelta_byte = interface[ifno].counter.rx_byte - interface[ifno].counter.rx_byte_last;

	rxdelta_drop = interface[ifno].counter.rx_seqdrop - interface[ifno].counter.rx_seqdrop_last;
	rxdelta_dup = interface[ifno].counter.rx_dup - interface[ifno].counter.rx_dup_last;
	rxdelta_reorder = interface[ifno].counter.rx_reorder - interface[ifno].counter.rx_reorder_last;

	rxdelta_flow_drop = interface[ifno].counter.rx_seqdrop_flow - interface[ifno].counter.rx_seqdrop_flow_last;
	rxdelta_flow_dup = interface[ifno].counter.rx_dup_flow - interface[ifno].counter.rx_dup_flow_last;
	rxdelta_flow_reorder = interface[ifno].counter.rx_reorder_flow - interface[ifno].counter.rx_reorder_flow_last;


	return snprintf(buf, buflen,
	    "{"
	    "\"interface\":\"%s\","
	    "\"packetsize\":%llu,"

	    "\"address\":\"%s\","
	    "\"macaddr\":\"%s\","
	    "\"gateway-address\":\"%s\","
	    "\"gateway-macaddr\":\"%s\","

	    "\"TX\":%llu,"
	    "\"RX\":%llu,"
	    "\"TXppsconfig\":%llu,"
	    "\"TXpps\":%llu,"
	    "\"RXpps\":%llu,"
	    "\"TXbps\":%llu,"
	    "\"RXbps\":%llu,"
	    "\"TXunderrun\":%llu,"
	    "\"RXdrop\":%llu,"
	    "\"RXdropps\":%lld,"
	    "\"RXdup\":%llu,"
	    "\"RXreorder\":%llu,"

	    "\"RXdrop-perflow\":%llu,"
	    "\"RXdup-perflow\":%llu,"
	    "\"RXreorder-perflow\":%llu,"

	    "\"RXflowcontrol\":%llu,"
	    "\"RXarp\":%llu,"
	    "\"RXother\":%llu,"
	    "\"RXicmpunreach\":%llu,"
	    "\"RXicmpredirect\":%llu,"

	    "\"latency-max\":%.8f,"
	    "\"latency-min\":%.8f,"
	    "\"latency-avg\":%.8f"
	    "}",

	    interface[ifno].ifname,
	    (unsigned long long)interface[ifno].pktsize,
	    buf_ipaddr,
	    buf_eaddr,
	    buf_gwaddr,
	    buf_gweaddr,
	    (unsigned long long)interface[ifno].counter.tx,
	    (unsigned long long)interface[ifno].counter.rx,
	    (unsigned long long)interface[ifno].transmit_pps,
	    (unsigned long long)txdelta_pkt,
	    (unsigned long long)rxdelta_pkt,
	    (unsigned long long)txdelta_byte * 8,
	    (unsigned long long)rxdelta_byte * 8,
	    (unsigned long long)interface[ifno].counter.tx_underrun,
	    (unsigned long long)interface[ifno].counter.rx_seqdrop,
	    (long long)rxdelta_drop,
	    (unsigned long long)interface[ifno].counter.rx_dup,
	    (unsigned long long)interface[ifno].counter.rx_reorder,

	    (unsigned long long)interface[ifno].counter.rx_seqdrop_flow,
	    (unsigned long long)interface[ifno].counter.rx_dup_flow,
	    (unsigned long long)interface[ifno].counter.rx_reorder_flow,

	    (unsigned long long)interface[ifno].counter.rx_flow,
	    (unsigned long long)interface[ifno].counter.rx_arp,
	    (unsigned long long)interface[ifno].counter.rx_other,
	    (unsigned long long)interface[ifno].counter.rx_icmpunreach,
	    (unsigned long long)interface[ifno].counter.rx_icmpredirect,

	    interface[ifno].counter.latency_max,
	    interface[ifno].counter.latency_min,
	    interface[ifno].counter.latency_avg
	);
}

static void
sighandler_alrm(int signo)
{
	static char jsonbuf[1024 * 32];
	static unsigned int nhz = 0;
	int i, len;
	uint64_t x;

	if ((nhz + 1) >= pps_hz) {
		clock_gettime(CLOCK_MONOTONIC, &currenttime);

#if 1
		/* update dropcounter */
		for (i = 0; i < 2; i++) {
			if (interface[i].opened) {
				interface[i].counter.rx_seqdrop = 
				    seqcheck_dropcount(interface[i].seqchecker);
				interface[i].counter.rx_dup =
				    seqcheck_dupcount(interface[i].seqchecker);
				interface[i].counter.rx_reorder = 
				    seqcheck_reordercount(interface[i].seqchecker);

				interface[i].counter.rx_seqdrop_flow = 
				    seqcheck_dropcount(interface[i].seqchecker_flowtotal);
				interface[i].counter.rx_dup_flow =
				    seqcheck_dupcount(interface[i].seqchecker_flowtotal);
				interface[i].counter.rx_reorder_flow = 
				    seqcheck_reordercount(interface[i].seqchecker_flowtotal);
			}
		}
#endif

		/* generate json statistics string */
		len = 0;
		len += snprintf(jsonbuf + len, sizeof(jsonbuf) - len, "{\"apiversion\":\"1.2\"");
		len += snprintf(jsonbuf + len, sizeof(jsonbuf) - len, ",\"time\":%.2f",
		    currenttime.tv_sec + currenttime.tv_nsec / 1000000000.0);

		len += snprintf(jsonbuf + len, sizeof(jsonbuf) - len, ",\"statistics\":[");
		if (len <= sizeof(jsonbuf)) {
			len += interface_statistics_json(0, jsonbuf + len , sizeof(jsonbuf) - len);
			if (len <= sizeof(jsonbuf)) {
				jsonbuf[len++] = ',';
				len += interface_statistics_json(1, jsonbuf + len, sizeof(jsonbuf) - len);
				len += snprintf(jsonbuf + len, sizeof(jsonbuf) - len, "]}\n");

				if (logfp != NULL) {
					fputs(jsonbuf, logfp);
					fflush(logfp);
				}

				webserv_stream_broadcast(jsonbuf, len);
			}
		}

		for (i = 0; i < 2; i++) {
			if (!interface[i].opened)
				continue;

			interface[i].counter.tx_delta = interface[i].counter.tx - interface[i].counter.tx_last;
			interface[i].counter.rx_delta = interface[i].counter.rx - interface[i].counter.rx_last;
			interface[i].counter.tx_last = interface[i].counter.tx;
			interface[i].counter.rx_last = interface[i].counter.rx;

			interface[i].counter.tx_byte_delta = interface[i].counter.tx_byte - interface[i].counter.tx_byte_last;
			interface[i].counter.rx_byte_delta = interface[i].counter.rx_byte - interface[i].counter.rx_byte_last;
			interface[i].counter.tx_byte_last = interface[i].counter.tx_byte;
			interface[i].counter.rx_byte_last = interface[i].counter.rx_byte;

			interface[i].counter.tx_Mbps = interface[i].counter.tx_byte_delta * 8.0 / 1000 / 1000;
			interface[i].counter.rx_Mbps = interface[i].counter.rx_byte_delta * 8.0 / 1000 / 1000;

			interface[i].counter.rx_seqdrop_last = interface[i].counter.rx_seqdrop;
		}
	}

	/* check and reset tx pps counter atomically */
	for (i = 0; i < 2; i++) {
		x = ((uint64_t)interface[i].transmit_pps * ((uint64_t)nhz + 1) / pps_hz) -
		    ((uint64_t)interface[i].transmit_pps * ((uint64_t)nhz) / pps_hz);
		if (interface[i].transmit_enable &&
		    ((x = atomic_swap_32(&interface[i].transmit_txhz, x)) != 0)) {
			atomic_add_64(&interface[i].counter.tx_underrun, x);
		}
	}

	if (++nhz >= pps_hz)
		nhz = 0;

	return;
}

static void
quit(void)
{
	static int quitting = 0;

	if (quitting)
		return;
	quitting = 1;

	do_quit = 1;
	alarm(0);

	itemlist_fini_term();

	printf("Exiting...\n");
	fflush(stdout);

	interface_close(0);
	interface_close(1);

	if (opt_rfc2544)
		rfc2544_showresult();

	exit(1);
}

static void
sighandler_int(int signo)
{
	quit();
}

static void
sighandler_tstp(int signo)
{
	itemlist_fini_term();

	signal(SIGTSTP, SIG_DFL);
	killpg(0, SIGTSTP);

	itemlist_init_term();
	force_redraw_screen = 1;
}

static void
sighandler_cont(int signo)
{
	signal(SIGTSTP, sighandler_tstp);
}

static void
usage(void)
{
	fprintf(stderr, "usage: ipgen [options]\n");
	fprintf(stderr, "	-R <ifname>,<gateway-addr>[,<address>[/<prefix>]]\n");
	fprintf(stderr, "					set RX interface\n");
	fprintf(stderr, "	-T <ifname>,<gateway-addr>[,<address>[/<prefix>]]\n");
	fprintf(stderr, "					set TX interface\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "	-H <Hz>				specify control Hz (default: 1000)\n");
	fprintf(stderr, "	-n <npkt>			sync transmit per <npkt>\n");
	fprintf(stderr, "	--ipg				adapt IPG (Inter Packet Gap) if possible\n");
	fprintf(stderr, "	--burst				don't set IPG (default)\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "	-S <script>			autotest script\n");
	fprintf(stderr, "	-L <log>			output statistics to logfile\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "	-s <size>			specify pktsize (udp:46-1500, tcp:58-1500)\n");
	fprintf(stderr, "	-p <pps>			specify pps\n");
	fprintf(stderr, "	-f				full-duplex mode\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "	-v				verbose\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "	-X				packet generation benchmark\n");
	fprintf(stderr, "	-XX				packet generation benchmark with memcpy\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "	--tcp				generate TCP packet\n");
	fprintf(stderr, "	--udp				generate UDP packet (default)\n");
	fprintf(stderr, "	--fragment			generate fragment packet\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "	--allnet			use destination address incrementally\n");
	fprintf(stderr, "	--saddr <begin>[-<end>]		use source address range (default: TX interface address)\n");
	fprintf(stderr, "	--daddr <begin>[-<end>]		use destination address range (default: RX interface address)\n");
	fprintf(stderr, "	--sport <begin>[-<end>]		use source port range (default: 9)\n");
	fprintf(stderr, "	--dport <begin>[-<end>]		use destination port range (default: 9)\n");
	fprintf(stderr, "	--flowlist <file>		read flowlist from file\n");
	fprintf(stderr, "	--flowsort			sort flow list\n");
	fprintf(stderr, "	--flowdump			dump flow list\n");
	fprintf(stderr, "	-F <nflow>			limit <nflow>\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "	--rfc2544			rfc2544 test mode\n");
	fprintf(stderr, "	--rfc2544-trial-duration <sec>	rfc2544 trial duration time (default: 60)\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "	-D <file>			debug. dump all generated packets to <file> as tcpdump file format\n");
	exit(1);
}

void *
txrx_thread_main(void *arg)
{
	struct pollfd pollfd[1];
	int rc;
	int ifno;

	(void)pthread_sigmask(SIG_BLOCK, &used_sigset, NULL);

	ifno = *(int *)arg;

	/* setup poll */
	memset(pollfd, 0, sizeof(pollfd));
	pollfd[0].fd = interface[ifno].nm_desc->fd;

	while (do_quit == 0) {
		pollfd[0].events = POLLIN;
		pollfd[0].revents = 0;

		if (interface[ifno].need_reset_statistics) {
			interface[ifno].need_reset_statistics = 0;
			memset(&interface[ifno].counter, 0, sizeof(interface[ifno].counter));
			seqcheck_clear(interface[ifno].seqchecker);
		}

		if (interface_need_transmit(ifno))
			pollfd[0].events |= POLLOUT;

#if 1
		rc = poll(pollfd, 1, 0);
		if (rc < 0) {
			if (errno == EINTR)
				continue;

			printf("poll: %s\n", strerror(errno));
			continue;
		}
#else
		pollfd[0].revents = POLLIN|POLLOUT;
#endif

		if (pollfd[0].revents & POLLIN)
			interface_receive(ifno);
		if (pollfd[0].revents & POLLOUT)
			interface_transmit(ifno);
	}

	return NULL;
}

void
genscript_play(int unsigned n)
{
	static int nth_test = -1;
	static int period_left = 0;
	struct genscript_item *genitem;

	if (do_quit)
		return;

	period_left--;
	if (period_left <= 0) {
		nth_test++;
		genitem = genscript_get_item(genscript, nth_test);
		if (genitem == NULL) {
			quit();
			return;
		}

		period_left = genitem->period;

		switch (genitem->cmd) {
		case GENITEM_CMD_RESET:
			statistics_clear();
			break;
		case GENITEM_CMD_NOP:
			break;

		case GENITEM_CMD_TX0SET:
			setpktsize(0, genitem->pktsize);
			setpps(0, genitem->pps);
			break;
		case GENITEM_CMD_TX1SET:
			setpktsize(1, genitem->pktsize);
			setpps(1, genitem->pps);
			break;
		}
	}
}

static void
control_tty_handler(int fd, struct itemlist *itemlist)
{
#ifdef __FreeBSD__
	sigset_t sigalrmset;
#endif
	int c, grabbed;

#ifdef __FreeBSD__
	/*
	 * for freebsd bug:
	 * with high frequency SIGALRM, getch() cannot tread KEYPAD
	 */
	sigemptyset(&sigalrmset);
	sigaddset(&sigalrmset, SIGALRM);
	sigprocmask(SIG_BLOCK, &sigalrmset, NULL);
#endif
	c = getch();
#ifdef __FreeBSD__
	sigprocmask(SIG_UNBLOCK, &sigalrmset, NULL);
#endif

	if (opt_rfc2544) {
		if ((c == 'q') || (c == 'Q'))
			quit();
		sprintf(msgbuf, "cannot control in rfc2544 mode");
		return;
	}

	grabbed = itemlist_ttyhandler(itemlist, c);

	if (grabbed)
		return;

	switch (c) {
	case 'q':
	case 'Q':
		quit();
		break;

	case 'z':
	case 'Z':
		statistics_clear();
		break;

#if 0
	case '\0':
		seqcheck_dump(interface[1].seqchecker);
		seqcheck_dump(interface[0].seqchecker);
		for (i = 0; i < 100; i++)
			printf("\n");
		break;
#endif
	}
}


/*
 * RFC2544 test sequence
 */
struct rfc2544_work {
	unsigned int pktsize;
	unsigned int minpps;
	unsigned int maxpps;
	unsigned int limitpps;
	unsigned int curpps;
	unsigned int prevpps;
};

struct rfc2544_work rfc2544_work[] = {
	{	.pktsize = 64 - 18,	.minpps = 1,	.maxpps = 1488095	},
	{	.pktsize = 128 - 18,	.minpps = 1,	.maxpps = 844594	},
	{	.pktsize = 256 - 18,	.minpps = 1,	.maxpps = 452898	},
	{	.pktsize = 512 - 18,	.minpps = 1,	.maxpps = 234962	},
	{	.pktsize = 1024 - 18,	.minpps = 1,	.maxpps = 119731	},
	{	.pktsize = 1280 - 18,	.minpps = 1,	.maxpps = 96153		},
	{	.pktsize = 1408 - 18,	.minpps = 1,	.maxpps = 87535		},
	{	.pktsize = 1518 - 18,	.minpps = 1,	.maxpps = 81274		}
};

static int rfc2544_nthtest = 0;
static int rfc2544_ntest = (sizeof(rfc2544_work) / sizeof(rfc2544_work[0]));

typedef enum {
	RFC2544_START,
	RFC2544_WARMUP0,
	RFC2544_WARMUP,
	RFC2544_RESETTING0,
	RFC2544_RESETTING,
	RFC2544_PPSCHANGE,
	RFC2544_MEASURING0,
	RFC2544_MEASURING,
	RFC2544_DONE0,
	RFC2544_DONE
} rfc2544_state_t;

void
rfc2544_showresult(void)
{
	double mbps;
	unsigned int pps;
	int i, j;

	/*
	 * [example]
	 *	framesize|0M  100M 200M 300M 400M 500M 600M 700M 800M 900M 1Gbps
	 *	---------+----+----+----+----+----+----+----+----+----+----+
	 *	      64 |#######################                            ###.##Mbps, #######/########pps
	 *	     128 |#############################                      ###.##Mbps, #######/########pps
	 *	     256 |#############################################      ###.##Mbps, #######/########pps
	 *	     512 |#################################################  ###.##Mbps, #######/########pps
	 *	    1024 |################################################## ###.##Mbps, #######/########pps
	 *	    1280 |################################################## ###.##Mbps, #######/########pps
	 *	    1408 |################################################## ###.##Mbps, #######/########pps
	 *	    1518 |################################################## ###.##Mbps, #######/########pps
	 *	
	 *	framesize|0   |100k|200k|300k|400k|500k|600k|700k|800k|900k|1.0m|1.1m|1.2m|1.3m|1.4m|1.5m pps
	 *	---------+----+----+----+----+----+----+----+----+----+----+----+----+----+----+----+
	 *	      64 |#################################################################          #######/########pps, ###.##%
	 *	     128 |#################################                                          #######/########pps, ###.##%
	 *	     256 |#################                                                          #######/########pps, ###.##%
	 *	     512 |########                                                                   #######/########pps, ###.##%
	 *	    1024 |#####                                                                      #######/########pps, ###.##%
	 *	    1280 |##                                                                         #######/########pps, ###.##%
	 *	    1408 |#                                                                          #######/########pps, ###.##%
	 *	    1518 |#                                                                          #######/########pps, ###.##%
	 */

	printf("\n");
	printf("\n");
	printf("framesize|0M  100M 200M 300M 400M 500M 600M 700M 800M 900M 1Gbps\n");
	printf("---------+----+----+----+----+----+----+----+----+----+----+\n");
	for (i = 0; i < rfc2544_ntest; i++) {
		printf("%8u |", rfc2544_work[i].pktsize + 18);

		mbps = CALC_MBPS(rfc2544_work[i].pktsize, rfc2544_work[i].curpps);
		for (j = 0; j < mbps / 20; j++)
			printf("#");
		for (; j < 51; j++)
			printf(" ");
		printf("%7.2fMbps, %7u/%7upps\n", mbps, rfc2544_work[i].curpps, rfc2544_work[i].limitpps);
	}
	printf("\n");

	printf("framesize|0   |100k|200k|300k|400k|500k|600k|700k|800k|900k|1.0m|1.1m|1.2m|1.3m|1.4m|1.5m pps\n");
	printf("---------+----+----+----+----+----+----+----+----+----+----+----+----+----+----+----+\n");
	for (i = 0; i < rfc2544_ntest; i++) {
		printf("%8u |", rfc2544_work[i].pktsize + 18);

		pps = rfc2544_work[i].curpps;
		for (j = 0; j < pps / 20000; j++)
			printf("#");
		for (; j < 75; j++)
			printf(" ");
		printf("%7u/%7upps, %6.2f%%\n", rfc2544_work[i].curpps, rfc2544_work[i].limitpps,
		    rfc2544_work[i].curpps * 100.0 / rfc2544_work[i].limitpps);
	}
	printf("\n");
}


static void
rfc2544_down_pps(void)
{
	rfc2544_work[rfc2544_nthtest].prevpps = rfc2544_work[rfc2544_nthtest].curpps;
	rfc2544_work[rfc2544_nthtest].maxpps = rfc2544_work[rfc2544_nthtest].curpps;
	rfc2544_work[rfc2544_nthtest].curpps =
	    (rfc2544_work[rfc2544_nthtest].minpps + rfc2544_work[rfc2544_nthtest].maxpps) / 2;
}

static void
rfc2544_up_pps(void)
{
	rfc2544_work[rfc2544_nthtest].prevpps = rfc2544_work[rfc2544_nthtest].curpps;
	rfc2544_work[rfc2544_nthtest].minpps = rfc2544_work[rfc2544_nthtest].curpps;
	rfc2544_work[rfc2544_nthtest].curpps =
	    (rfc2544_work[rfc2544_nthtest].minpps + rfc2544_work[rfc2544_nthtest].maxpps) / 2;
}

void
rfc2544_test(int unsigned n)
{
	static rfc2544_state_t state = RFC2544_START;
	static struct timespec statetime;
	int measure_done;

	switch (state) {
	case RFC2544_START:
		sprintf(msgbuf, "start rfc2544 test mode. trial-duration is %d sec. warming up...",
		    opt_rfc2544_trial_duration);

		/* disable transmit */
		transmit_set(0, 0);
		transmit_set(1, 1);
		setpps(0, 0);
		setpps(1, 10000);
		setpktsize(0, 0);
		setpktsize(1, 0);
		state = RFC2544_WARMUP0;
		break;

	case RFC2544_WARMUP0:
		memcpy(&statetime, &currenttime, sizeof(struct timeval));
		statetime.tv_sec += 3;	/* wait 3sec */
		state = RFC2544_WARMUP;
		break;
	case RFC2544_WARMUP:
		if (timespeccmp(&currenttime, &statetime, <))
			break;
		state = RFC2544_RESETTING0;
		break;

	case RFC2544_RESETTING0:
		transmit_set(1, 0);
		statistics_clear();

		rfc2544_work[rfc2544_nthtest].limitpps = rfc2544_work[rfc2544_nthtest].maxpps;
		rfc2544_work[rfc2544_nthtest].prevpps = 0;

		if (rfc2544_nthtest == 0) {
			rfc2544_work[rfc2544_nthtest].curpps = 
			    (rfc2544_work[rfc2544_nthtest].minpps + rfc2544_work[rfc2544_nthtest].maxpps) / 2;
		} else if (rfc2544_work[rfc2544_nthtest].maxpps < rfc2544_work[rfc2544_nthtest - 1].curpps) {
			rfc2544_work[rfc2544_nthtest].curpps = rfc2544_work[rfc2544_nthtest].maxpps;
		} else {
			rfc2544_work[rfc2544_nthtest].curpps = rfc2544_work[rfc2544_nthtest - 1].curpps;
		}

		memcpy(&statetime, &currenttime, sizeof(struct timeval));
		statetime.tv_sec += 2;	/* wait 2sec */
		state = RFC2544_RESETTING;
		break;

	case RFC2544_RESETTING:
		statistics_clear();
		if (timespeccmp(&currenttime, &statetime, <))
			break;

		/* enable transmit */
		setpps(1, rfc2544_work[rfc2544_nthtest].curpps);
		setpktsize(1, rfc2544_work[rfc2544_nthtest].pktsize);
		statistics_clear();
		transmit_set(1, 1);

		state = RFC2544_MEASURING0;
		break;

	case RFC2544_PPSCHANGE:
		if (timespeccmp(&currenttime, &statetime, <))
			break;

		statistics_clear();
		state = RFC2544_MEASURING0;
		break;

	case RFC2544_MEASURING0:
		if (rfc2544_work[rfc2544_nthtest].prevpps) {
			sprintf(msgbuf, "measuring pktsize %u, pps %u (%.2fMbps) -> %u (%.2fMbps)",
			    rfc2544_work[rfc2544_nthtest].pktsize,
			    rfc2544_work[rfc2544_nthtest].prevpps,
			    CALC_MBPS(rfc2544_work[rfc2544_nthtest].pktsize, rfc2544_work[rfc2544_nthtest].prevpps),
			    rfc2544_work[rfc2544_nthtest].curpps,
			    CALC_MBPS(rfc2544_work[rfc2544_nthtest].pktsize, rfc2544_work[rfc2544_nthtest].curpps));
		} else {
			sprintf(msgbuf, "measuring pktsize %d, pps %d (%.2fMbps)",
			    rfc2544_work[rfc2544_nthtest].pktsize,
			    rfc2544_work[rfc2544_nthtest].curpps,
			    CALC_MBPS(rfc2544_work[rfc2544_nthtest].pktsize, rfc2544_work[rfc2544_nthtest].curpps));
		}

		memcpy(&statetime, &currenttime, sizeof(struct timeval));
		statetime.tv_sec += opt_rfc2544_trial_duration;
		state = RFC2544_MEASURING;
		break;

	case RFC2544_MEASURING:
		measure_done = 0;

		if (interface[0].counter.rx_seqdrop > 0) {
			/* dropped... */
			if (rfc2544_work[rfc2544_nthtest].minpps >= rfc2544_work[rfc2544_nthtest].curpps - 1) {
				rfc2544_work[rfc2544_nthtest].curpps = rfc2544_work[rfc2544_nthtest].minpps;
				measure_done = 1;

			} else {
				rfc2544_down_pps();
				setpps(1, rfc2544_work[rfc2544_nthtest].curpps);
				statistics_clear();
				memcpy(&statetime, &currenttime, sizeof(struct timeval));
				statetime.tv_sec += 1;	/* wait 2sec */
				state = RFC2544_PPSCHANGE;
			}
		} else if (timespeccmp(&currenttime, &statetime, >)) {
			/* no drop. OK! */
			if (rfc2544_work[rfc2544_nthtest].maxpps <= rfc2544_work[rfc2544_nthtest].curpps + 1) {
				measure_done = 1;
			} else {
				rfc2544_up_pps();
				setpps(1, rfc2544_work[rfc2544_nthtest].curpps);
				state = RFC2544_MEASURING0;
			}
		}

		if (measure_done) {
			sprintf(msgbuf, "done. pktsize %d, maximum pps %d (%.2fMbps)",
			    rfc2544_work[rfc2544_nthtest].pktsize,
			    rfc2544_work[rfc2544_nthtest].curpps,
			    CALC_MBPS(rfc2544_work[rfc2544_nthtest].pktsize, rfc2544_work[rfc2544_nthtest].curpps));

			rfc2544_nthtest++;
			if (rfc2544_nthtest >= rfc2544_ntest) {
				sprintf(msgbuf, "complete");
				state = RFC2544_DONE0;
			} else {
				state = RFC2544_RESETTING0;
			}
		}
		break;

	case RFC2544_DONE0:
		transmit_set(1, 0);
		state = RFC2544_DONE;
		break;

	case RFC2544_DONE:
		do_quit = 1;
		break;
	}

}


/*
 * control_interval() will be called DISPLAY_UPDATE_HZ
 */
static void
control_interval(struct itemlist *itemlist)
{
	static unsigned int ninterval = 0;
	static unsigned int ntwiddle = 0;

	const char *twiddle0[12] = {
		">   >>>  ",
		">>   >>> ",
		">>>   >>>",
		" >>>   >>",
		"  >>>   >",
		"   >>>   ",
		">   >>>  ",
		">>   >>> ",
		">>>   >>>",
		" >>>   >>",
		"  >>>   >",
		"   >>>    "
	};
	const char *twiddle1[12] = {
		"  <<<   <",
		" <<<   <<",
		"<<<   <<<",
		"<<   <<< ",
		"<   <<<  ",
		"   <<<   ",
		"  <<<   <",
		" <<<   <<",
		"<<<   <<<",
		"<<   <<< ",
		"<   <<<  ",
		"   <<<   "
	};

	if (ntwiddle >= 12)
		ntwiddle = 0;

	if (interface[0].transmit_pps && interface[0].transmit_enable) {
		strcpy(interface[0].twiddle, twiddle0[ntwiddle]);
	} else {
		interface[0].twiddle[0] = '\0';
	}

	if (interface[1].transmit_pps && interface[1].transmit_enable) {
		strcpy(interface[1].twiddle, twiddle1[ntwiddle]);
	} else {
		interface[1].twiddle[0] = '\0';
	}

	if ((genscript != NULL) && (ninterval == 0)) {
		/* call once every second */
		genscript_play(ninterval);
	}

	if (opt_rfc2544) {
		rfc2544_test(ninterval);
	}

	itemlist_update(itemlist, 0);

#if 1
	if (ninterval & 1)
		ntwiddle++;
#elif 0
	switch (ninterval) {
	case 0:
	case DISPLAY_UPDATE_HZ / 2:
		ntwiddle++;
		break;
	}
#else
	ntwiddle++;
#endif

	if (++ninterval >= DISPLAY_UPDATE_HZ)
		ninterval = 0;
}

static int
itemlist_callback_burst_steady(struct itemlist *itemlist, struct item *item, void *refptr)
{
	switch (item->id) {
	case ITEMLIST_ID_BUTTON_BURST:
		ipg_enable(0);
		break;

	case ITEMLIST_ID_BUTTON_STEADY:
		ipg_enable(1);
		break;
	}

	return 0;
}

static int
itemlist_callback_nflow(struct itemlist *itemlist, struct item *item, void *refptr)
{
	int *nflow_test;

	nflow_test = (int *)refptr;
	if (*nflow_test < 1)
		*nflow_test = 1;

	if (*nflow_test > get_flownum(0))
		*nflow_test = get_flownum(0);

	return 0;
}

static int
itemlist_callback_pktsize(struct itemlist *itemlist, struct item *item, void *refptr)
{
	uint32_t *pktsize;
	int ifno;

	pktsize = (uint32_t *)refptr;
	if (*pktsize < min_pktsize)
		*pktsize = min_pktsize;
	if (*pktsize > 1500)
		*pktsize = 1500;

	switch (item->id) {
	default:
	case ITEMLIST_ID_IF0_PKTSIZE:
		ifno = 0;
		break;
	case ITEMLIST_ID_IF1_PKTSIZE:
		ifno = 1;
		break;
	}

	interface[ifno].pktsize = *pktsize;
	update_transmit_Mbps(ifno);

	return 0;
}

static int
itemlist_callback_pps(struct itemlist *itemlist, struct item *item, void *refptr)
{
	uint32_t *pps;
	int ifno;

	pps = (uint32_t *)refptr;

	switch (item->id) {
	default:
	case ITEMLIST_ID_IF0_PPS:
		ifno = 0;
		break;
	case ITEMLIST_ID_IF1_PPS:
		ifno = 1;
		break;
	}

	interface[ifno].transmit_pps = *pps;
	update_transmit_Mbps(ifno);

	return 0;
}

static int
itemlist_callback_startstop(struct itemlist *itemlist, struct item *item, void *refptr)
{
	switch (item->id) {
	case ITEMLIST_ID_IF0_START:
		transmit_set(0, 1);
		break;
	case ITEMLIST_ID_IF0_STOP:
		transmit_set(0, 0);
		break;

	case ITEMLIST_ID_IF1_START:
		transmit_set(1, 1);
		break;
	case ITEMLIST_ID_IF1_STOP:
		transmit_set(1, 0);
		break;
	}

	return 0;
}

void
control_init_items(struct itemlist *itemlist)
{
	static int netmap_api = NETMAP_API;

	itemlist_setvalue(itemlist, ITEMLIST_ID_IPGEN_VERSION, &ipgen_version);
	itemlist_setvalue(itemlist, ITEMLIST_ID_NETMAP_API, &netmap_api);

	itemlist_register_item(itemlist, ITEMLIST_ID_IFNAME0, NULL, interface[0].decorated_ifname);
	itemlist_register_item(itemlist, ITEMLIST_ID_IFNAME1, NULL, interface[1].decorated_ifname);
	itemlist_register_item(itemlist, ITEMLIST_ID_TWIDDLE0, NULL, interface[0].twiddle);
	itemlist_register_item(itemlist, ITEMLIST_ID_TWIDDLE1, NULL, interface[1].twiddle);

	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_TX, NULL, &interface[0].counter.tx);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_TX, NULL, &interface[1].counter.tx);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_TX_ETC, NULL, &interface[0].counter.tx_etc);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_TX_ETC, NULL, &interface[1].counter.tx_etc);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_TX_UNDERRUN, NULL, &interface[0].counter.tx_underrun);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_RX_UNDERRUN, NULL, &interface[1].counter.tx_underrun);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_RX, NULL, &interface[0].counter.rx);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_RX, NULL, &interface[1].counter.rx);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_RX_DROP, NULL, &interface[0].counter.rx_seqdrop);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_RX_DROP, NULL, &interface[1].counter.rx_seqdrop);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_RX_DUP, NULL, &interface[0].counter.rx_dup);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_RX_DUP, NULL, &interface[1].counter.rx_dup);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_RX_REORDER, NULL, &interface[0].counter.rx_reorder);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_RX_REORDER, NULL, &interface[1].counter.rx_reorder);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_RX_REORDER_FLOW, NULL, &interface[0].counter.rx_reorder_flow);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_RX_REORDER_FLOW, NULL, &interface[1].counter.rx_reorder_flow);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_RX_FLOW, NULL, &interface[0].counter.rx_flow);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_RX_FLOW, NULL, &interface[1].counter.rx_flow);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_RX_ARP, NULL, &interface[0].counter.rx_arp);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_RX_ARP, NULL, &interface[1].counter.rx_arp);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_RX_ICMPECHO, NULL, &interface[0].counter.rx_icmpecho);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_RX_ICMPECHO, NULL, &interface[1].counter.rx_icmpecho);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_RX_ICMPUNREACH, NULL, &interface[0].counter.rx_icmpunreach);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_RX_ICMPUNREACH, NULL, &interface[1].counter.rx_icmpunreach);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_RX_ICMPREDIRECT, NULL, &interface[0].counter.rx_icmpredirect);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_RX_ICMPREDIRECT, NULL, &interface[1].counter.rx_icmpredirect);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_RX_ICMPOTHER, NULL, &interface[0].counter.rx_icmpother);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_RX_ICMPOTHER, NULL, &interface[1].counter.rx_icmpother);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_RX_OTHER, NULL, &interface[0].counter.rx_other);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_RX_OTHER, NULL, &interface[1].counter.rx_other);

	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_TX_DELTA, NULL, &interface[0].counter.tx_delta);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_TX_DELTA, NULL, &interface[1].counter.tx_delta);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_TX_BYTE_DELTA, NULL, &interface[0].counter.tx_byte_delta);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_TX_BYTE_DELTA, NULL, &interface[1].counter.tx_byte_delta);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_TX_MBPS, NULL, &interface[0].counter.tx_Mbps);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_TX_MBPS, NULL, &interface[1].counter.tx_Mbps);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_RX_DELTA, NULL, &interface[0].counter.rx_delta);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_RX_DELTA, NULL, &interface[1].counter.rx_delta);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_RX_BYTE_DELTA, NULL, &interface[0].counter.rx_byte_delta);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_RX_BYTE_DELTA, NULL, &interface[1].counter.rx_byte_delta);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_RX_MBPS, NULL, &interface[0].counter.rx_Mbps);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_RX_MBPS, NULL, &interface[1].counter.rx_Mbps);

	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_LATENCY_MIN, NULL, &interface[0].counter.latency_min);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_LATENCY_MIN, NULL, &interface[1].counter.latency_min);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_LATENCY_MAX, NULL, &interface[0].counter.latency_max);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_LATENCY_MAX, NULL, &interface[1].counter.latency_max);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_LATENCY_AVG, NULL, &interface[0].counter.latency_avg);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_LATENCY_AVG, NULL, &interface[1].counter.latency_avg);

	itemlist_register_item(itemlist, ITEMLIST_ID_PPS_HZ, NULL, &pps_hz);
	itemlist_register_item(itemlist, ITEMLIST_ID_OPT_NFLOW, itemlist_callback_nflow, &opt_nflow);
	itemlist_register_item(itemlist, ITEMLIST_ID_BUTTON_BURST, itemlist_callback_burst_steady, NULL);
	itemlist_register_item(itemlist, ITEMLIST_ID_BUTTON_STEADY, itemlist_callback_burst_steady, NULL);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_PKTSIZE, itemlist_callback_pktsize, &interface[0].pktsize);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_PKTSIZE, itemlist_callback_pktsize, &interface[1].pktsize);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_PPS, itemlist_callback_pps, &interface[0].transmit_pps);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_PPS, itemlist_callback_pps, &interface[1].transmit_pps);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_PPS_MAX, NULL, &interface[0].transmit_pps_max);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_PPS_MAX, NULL, &interface[1].transmit_pps_max);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_IMPLICIT_MBPS, NULL, &interface[0].transmit_Mbps);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_IMPLICIT_MBPS, NULL, &interface[1].transmit_Mbps);

	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_START, itemlist_callback_startstop, NULL);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF0_STOP, itemlist_callback_startstop, NULL);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_START, itemlist_callback_startstop, NULL);
	itemlist_register_item(itemlist, ITEMLIST_ID_IF1_STOP, itemlist_callback_startstop, NULL);

	itemlist_register_item(itemlist, ITEMLIST_ID_MSGBUF, NULL, msgbuf);

	/* default */
	if (opt_ipg)
		itemlist_setvalue(itemlist, ITEMLIST_ID_BUTTON_STEADY, "*");
	else
		itemlist_setvalue(itemlist, ITEMLIST_ID_BUTTON_BURST, "*");

#ifdef IPG_HACK
	if (support_ipg == 0) {
#else
	if (1) {
#endif
		itemlist_editable(itemlist, ITEMLIST_ID_BUTTON_BURST, 0);
		itemlist_editable(itemlist, ITEMLIST_ID_BUTTON_STEADY, 0);
	}


	if (interface[0].transmit_enable)
		itemlist_setvalue(itemlist, ITEMLIST_ID_IF0_START, "*");
	else
		itemlist_setvalue(itemlist, ITEMLIST_ID_IF0_STOP, "*");

	if (interface[1].transmit_enable)
		itemlist_setvalue(itemlist, ITEMLIST_ID_IF1_START, "*");
	else
		itemlist_setvalue(itemlist, ITEMLIST_ID_IF1_STOP, "*");

	itemlist_focus(itemlist, ITEMLIST_ID_IF1_STOP);

	itemlist_update(itemlist, 1);
}


static void
evt_accept_callback(evutil_socket_t fd, short event, void *arg)
{
	struct sockaddr_in sin;
	socklen_t sinlen = sizeof(sin);
	int client;

	client = accept(fd, (struct sockaddr *)&sin, (socklen_t *)&sinlen);
	if (client < 0) {
		warn("accept");
		return;
	}

	webserv_new(client);
}

static void
evt_readable_stdin_callback(evutil_socket_t fd, short event, void *arg)
{
	struct itemlist *itemlist;

	itemlist = (struct itemlist *)arg;
	control_tty_handler(fd, itemlist);	/* fd = STDIN */
}

static void
evt_timeout_callback(evutil_socket_t fd, short event, void *arg)
{
	struct itemlist *itemlist;

	if (do_quit) {
		quit();
		return;
	}

	itemlist = (struct itemlist *)arg;
	control_interval(itemlist);	/* call frequency is DISPLAY_UPDATE_HZ */
}


void *
control_thread_main(void *arg)
{
	struct event ev_tty;
	struct event ev_timer;
	struct event ev_sock;
	struct timeval tv = { 0, 1000000 / DISPLAY_UPDATE_HZ};
	int s;

	itemlist_init_term();
	itemlist = itemlist_new(pktgen_template, pktgen_items, ITEMLIST_ID_NITEMS);
	control_init_items(itemlist);

	webserv_init();

	/* for libevent */
	s = listentcp(INADDR_ANY, 8080);
	event_init();
	event_set(&ev_tty, STDIN_FILENO, EV_READ | EV_PERSIST, evt_readable_stdin_callback, itemlist);
	event_add(&ev_tty, NULL);
	event_set(&ev_timer, -1, EV_PERSIST, evt_timeout_callback, itemlist);
	event_add(&ev_timer, &tv);
	event_set(&ev_sock, s, EV_READ | EV_PERSIST, evt_accept_callback, &ev_sock);
	event_add(&ev_sock, NULL);

	event_dispatch();

	return NULL;
}

void
gentest_main(void)
{
	time_t lastsec = 0;
	uint32_t nsec;
	uint64_t npkt, lpkt;
	static char tmppktbuf[LIBPKT_PKTBUFSIZE] __attribute__((__aligned__(8)));

	clock_gettime(CLOCK_MONOTONIC, &currenttime);
	lastsec = currenttime.tv_sec;

	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &currenttime);
		if (lastsec != currenttime.tv_sec) {
			lastsec = currenttime.tv_sec;
			break;
		}
	}
	nsec = 0;
	npkt = lpkt = 0;

	printf("Packet generation benchmark. pktsize=%d",
	    interface[0].pktsize);
	if (opt_gentest >= 2)
		printf(" with MEMCPY");
	if (opt_gentest >= 3)
		printf(" with CKSUM test");
	printf(" start\n");

	ip4pkt_udp_template(pktbuffer_ipv4_udp[0], 1500 + ETHHDRSIZE);
	build_template_packet_ipv4(0, pktbuffer_ipv4_udp[0]);

	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &currenttime);

		touchup_tx_packet(pktbuffer_ipv4_udp[0], 0);

		if (opt_gentest >= 2)
			memcpy(tmppktbuf, pktbuffer_ipv4_udp[0], interface[0].pktsize + ETHHDRSIZE);
		if (opt_gentest >= 3)
			ip4pkt_test_cksum(tmppktbuf, interface[0].pktsize + ETHHDRSIZE);

		npkt++;
		if (lastsec != currenttime.tv_sec) {
			lastsec = currenttime.tv_sec;
			nsec++;

			printf("%llu pkt generated.",
			    (unsigned long long)npkt - lpkt);

			printf(" totally %llu packet generated in %lu second. average: %llu pps, pktsize %d, %lluMbps\n",
			    (unsigned long long)npkt,
			    (unsigned long)nsec,
			    (unsigned long long)npkt / nsec,
			    interface[0].pktsize,
			    PKTSIZE2FRAMESIZE(interface[0].pktsize + ETHHDRSIZE) * 8ULL * ((unsigned long long)npkt / nsec) / 1000 / 1000);
			fflush(stdout);

			lpkt = npkt;
		}
	}
}

static struct option longopts[] = {
	{	"ipg",		no_argument,		0,	0	},
	{	"burst",	no_argument,		0,	0	},
	{	"allnet",	no_argument,		0,	0	},
	{	"fragment",	no_argument,		0,	0	},
	{	"tcp",		no_argument,		0,	0	},
	{	"udp",		no_argument,		0,	0	},
	{	"sport",	required_argument,	0,	0	},
	{	"dport",	required_argument,	0,	0	},
	{	"saddr",	required_argument,	0,	0	},
	{	"daddr",	required_argument,	0,	0	},
	{	"flowlist",	required_argument,	0,	0	},
	{	"flowsort",	no_argument,		0,	0	},
	{	"flowdump",	no_argument,		0,	0	},
	{	"rfc2544",	no_argument,		0,	0	},
	{	"rfc2544-trial-duration",	required_argument,	0,	0	},
	{	NULL,		0,			NULL,	0	}
};

int
main(int argc, char *argv[])
{
	int ifnum[2] = { 0, 1 };
	unsigned int i, j;
	int ch, optidx;
	int rc;
	char ifname[2][64];
	char *testscript = NULL;

	printf("ipgen v%.2f\n", ipgen_version);

#ifdef IPG_HACK
	if (system("sysctl -q dev.igb.0.tipg > /dev/null") == 0) {
		support_ipg = 1;
		printf("igb(4) TIPG feature supported\n");
	}
#endif

	/* XXX */
	seq_magic = getpid() & 0xffff;


	memset(ifname, 0, sizeof(ifname));

	/* initialize instances */
	memset(&interface, 0, sizeof(interface));
	for (i = 0; i < 2; i++) {
		pbufq_init(&interface[i].pbufq);
		setpps(i, 1488095);	/* default maximum pps */
		interface[i].pktsize = min_pktsize;
	}

	while ((ch = getopt_long(argc, argv, "D:F:fH:L:n:p:R:S:s:T:tvX", longopts, &optidx)) != -1) {
		switch (ch) {
		case 'D':
			opt_debug = optarg;
			break;

		case 'T':
		case 'R':
			{
				int ifno, masklen;
				char *p, *s, *tofree;

				ifno = (ch == 'T') ? 1 : 0;
				tofree = s = strdup(optarg);


				/*
				 * parse
				 *    "-Tem0,10.0.0.1"
				 *    "-Tem0,aa:bb:cc:dd:ee:ff"
				 * or "-Tem0,10.0.0.1,10.0.0.2"
				 * or "-Tem0,aa:bb:cc:dd:ee:ff,10.0.0.2"
				 * or "-Tem0,10.0.0.1,10.0.0.2/24"
				 * or "-Tem0,aa:bb:cc:dd:ee:ff,10.0.0.2/24"
				 */
				p = strsep(&s, ",");
				if (s == NULL)
					usage();
				strncpy(ifname[ifno], p, sizeof(ifname[0]));

				p = strsep(&s, ",");
				/* parse IPv4 or IPv6 or MAC-ADDRESS */
				if (inet_pton(AF_INET, p, &interface[ifno].gwaddr) == 1) {
					interface[ifno].af_gwaddr = AF_INET;
				} else if (inet_pton(AF_INET6, p, &interface[ifno].gw6addr) == 1) {
					interface[ifno].af_gwaddr = AF_INET6;
				} else if (ether_aton_r(p, &interface[ifno].gweaddr) == NULL) {
					fprintf(stderr, "Cannot resolve: %s\n", p);
					usage();
					exit(1);
				}

				if (s != NULL) {
					p = strsep(&s, "/");
					/* parse IPv4 or IPv6 */
					if (inet_pton(AF_INET, p, &interface[ifno].ipaddr) == 1) {
						interface[ifno].af_addr = AF_INET;
					} else if (inet_pton(AF_INET6, p, &interface[ifno].ip6addr) == 1) {
						interface[ifno].af_addr = AF_INET6;
						use_ipv6 = 1;
						update_min_pktsize();
					} else {
						fprintf(stderr, "Cannot resolve: %s\n", p);
						usage();
						exit(1);
					}

					if (s == NULL) {
						memset(&interface[ifno].ipaddr_mask, 0xff, sizeof(interface[ifno].ipaddr_mask));
						memset(&interface[ifno].ip6addr_mask, 0xff, sizeof(interface[ifno].ip6addr_mask));
					} else {
						if (strchr(s, '.')) {
							if (use_ipv6) {
								fprintf(stderr, "funny address and mask: %s/%s\n", p, s);
								usage();
								exit(1);
							}
							inet_pton(AF_INET, s, &interface[ifno].ipaddr_mask);
						} else if (strchr(s, ':')) {
							if (!use_ipv6) {
								fprintf(stderr, "funny address and mask: %s/%s\n", p, s);
								usage();
								exit(1);
							}
							inet_pton(AF_INET6, s, &interface[ifno].ip6addr_mask);
						} else {
							masklen = strtol(s, NULL, 10);
							switch (interface[ifno].af_addr) {
							case AF_INET:
								if (masklen > 32) {
									fprintf(stderr, "illegal address mask: %s\n", s);
									usage();
									exit(1);
								}
								interface[ifno].ipaddr_mask.s_addr = htonl(0xffffffff << (32 - masklen));
								break;
							case AF_INET6:
								if (masklen > 128) {
									fprintf(stderr, "illegal address mask: %s\n", s);
									usage();
									exit(1);
								}
								prefix2in6addr(masklen, &interface[ifno].ip6addr_mask);
								break;
							}
						}
					}
				}

				free(tofree);
			}
			break;
		case 'X':
			opt_gentest++;
			break;
		case 'f':
			opt_fulldup++;
			break;
		case 'F':
			opt_nflow = strtol(optarg, (char **)NULL, 10);
			break;

		case 'L':
			logfp = fopen(optarg, "w");
			if (logfp == NULL) {
				err(2, "%s", optarg);
			}
			break;

		case 'n':
			opt_npkt_sync = strtol(optarg, (char **)NULL, 10);
			break;

		case 'H':
			pps_hz = strtol(optarg, (char **)NULL, 10);
			if (pps_hz < 1) {
				fprintf(stderr, "HZ must be greater than 1\n");
				exit(1);
			}
			break;

		case 'p':
			{
				int pps;
				pps = strtol(optarg, (char **)NULL, 10);
				setpps(0, pps);
				setpps(1, pps);
			}
			break;
		case 'S':
			testscript = optarg;
			break;
		case 's':
			{
				int sz;
				sz = strtol(optarg, (char **)NULL, 10);
				if (sz < 46 || sz > 1500) {
					usage();
					exit(1);
				}
				interface[0].pktsize = sz;
				interface[1].pktsize = sz;
			}
			break;
		case 'v':
			verbose++;
			break;
		case 0:
			if (strcmp(longopts[optidx].name, "ipg") == 0) {
				opt_ipg = 1;
			} else if (strcmp(longopts[optidx].name, "burst") == 0) {
				opt_ipg = 0;
			} else if (strcmp(longopts[optidx].name, "allnet") == 0) {
				opt_allnet = 1;
			} else if (strcmp(longopts[optidx].name, "fragment") == 0) {
				opt_fragment = 1;
			} else if (strcmp(longopts[optidx].name, "tcp") == 0) {
				opt_tcp = 1;
				opt_udp = 0;
				min_pktsize = MAX(min_pktsize, sizeof(struct ip) + sizeof(struct tcphdr) + sizeof(struct seqdata));
			} else if (strcmp(longopts[optidx].name, "udp") == 0) {
				opt_udp = 1;
				opt_tcp = 0;
				min_pktsize = MAX(min_pktsize, sizeof(struct ip) + sizeof(struct udphdr) + sizeof(struct seqdata));
			} else if (strcmp(longopts[optidx].name, "sport") == 0) {
				parse_portrange(optarg, &opt_srcport_begin, &opt_srcport_end);
			} else if (strcmp(longopts[optidx].name, "dport") == 0) {
				parse_portrange(optarg, &opt_dstport_begin, &opt_dstport_end);
			} else if (strcmp(longopts[optidx].name, "saddr") == 0) {
				opt_addrrange = 1;
				opt_saddr = 1;
				if (parse_addrrange(optarg, &opt_srcaddr_begin, &opt_srcaddr_end) == 0) {
					opt_srcaddr_af = AF_INET;
				} else if (parse_addr6range(optarg, &opt_srcaddr6_begin, &opt_srcaddr6_end) == 0) {
					opt_srcaddr_af = AF_INET6;
				} else {
					fprintf(stderr, "illegal address range: %s\n", optarg);
					exit(1);
				}
			} else if (strcmp(longopts[optidx].name, "daddr") == 0) {
				opt_addrrange = 1;
				opt_daddr = 1;
				if (parse_addrrange(optarg, &opt_dstaddr_begin, &opt_dstaddr_end) == 0) {
					opt_dstaddr_af = AF_INET;
				} else if (parse_addr6range(optarg, &opt_dstaddr6_begin, &opt_dstaddr6_end) == 0) {
					opt_dstaddr_af = AF_INET6;
				} else {
					fprintf(stderr, "illegal address range: %s\n", optarg);
					exit(1);
				}
			} else if (strcmp(longopts[optidx].name, "flowsort") == 0) {
				opt_flowsort = 1;
			} else if (strcmp(longopts[optidx].name, "flowdump") == 0) {
				opt_flowdump = 1;
			} else if (strcmp(longopts[optidx].name, "flowlist") == 0) {
				opt_flowlist = optarg;
			} else if (strcmp(longopts[optidx].name, "rfc2544") == 0) {
				opt_rfc2544 = 1;
			} else if (strcmp(longopts[optidx].name, "rfc2544-trial-duration") == 0) {
				opt_rfc2544_trial_duration = strtol(optarg, (char **)NULL, 10);
				if (opt_rfc2544_trial_duration < 3)
					opt_rfc2544_trial_duration = 3;
			} else {
				usage();
			}
			break;
		default:
			usage();
		}
	}

	if (opt_addrrange && opt_allnet) {
		fprintf(stderr, "cannot use --allnet and --saddr/--daddr at the same time\n");
		exit(1);
	}

	if (opt_srcaddr_af == 0)
		opt_srcaddr_af = opt_dstaddr_af;
	if (opt_dstaddr_af == 0)
		opt_dstaddr_af = opt_srcaddr_af;
	if (opt_addrrange && (opt_srcaddr_af != opt_dstaddr_af)) {
		fprintf(stderr, "--saddr and --daddr are different address family\n");
		exit(1);
	}

	if ((interface[0].pktsize < min_pktsize) && opt_tcp) {
		fprintf(stderr, "minimal pakcet size is %d when using TCP\n", min_pktsize);
		exit(1);
	}

	if (!in_range(opt_srcport_begin, 0, 65535) ||
	    !in_range(opt_srcport_end, 0, 65535) ||
	    !in_range(opt_dstport_begin, 0, 65535) ||
	    !in_range(opt_dstport_end, 0, 65535)) {
		fprintf(stderr, "illegal port %d-%d, %d-%d\n",
		    opt_srcport_begin, opt_srcport_end,
		    opt_dstport_begin, opt_dstport_end);
		usage();
		exit(1);
	}
	if ((opt_srcport_begin > opt_srcport_end) ||
	    (opt_dstport_begin > opt_dstport_end)) {
		fprintf(stderr, "illegal port order\n");
		usage();
		exit(1);
	}

	if (opt_debug != NULL) {
		debug_tcpdump_fd = tcpdumpfile_open(opt_debug);
		if (debug_tcpdump_fd < 0) {
			fprintf(stderr, "%s: %s\n", opt_debug, strerror(debug_tcpdump_fd));
			exit(1);
		}
	}

	if (opt_gentest) {
		gentest_main();
		exit(1);
	}

	if (testscript != NULL) {
		genscript = genscript_new(testscript);
		if (genscript == NULL)
			err(2, "%s", testscript);

		setpps(0, 0);
		setpps(1, 0);
	}


	if (ifname[0][0] == '\0')
		opt_txonly = 1;
	if (ifname[1][0] == '\0')
		opt_rxonly = 1;

	if (opt_txonly && opt_rxonly) {
		fprintf(stderr, "specify interface with -T and -R\n");
		usage();
		exit(1);
	}

	for (i = 0; i < 2; i++) {
		if (opt_txonly && i == 0)
			continue;
		if (opt_rxonly && i == 1)
			continue;

		if ((memcmp(eth_zero, &interface[i].gweaddr, ETHER_ADDR_LEN) == 0) &&
		    (interface[i].gwaddr.s_addr == 0)) {
			fprintf(stderr, "gateway address is unknown. specify gw address with -T and -R\n");
			usage();
			exit(1);
		}
	}


	/* check console size */
	{
		struct winsize winsize;
		if (ioctl(STDIN_FILENO, TIOCGWINSZ, &winsize) != 0) {
			fprintf(stderr, "cannot get terminal size\n");
			exit(3);
		}
		if ((winsize.ws_row < pktgen_template_line) ||
		    (winsize.ws_col < pktgen_template_column)) {
			fprintf(stderr, "not enough screen size. screen size is %dx%d, requires %dx%d\n",
			    winsize.ws_col, winsize.ws_row,
			    pktgen_template_column, pktgen_template_line);
			exit(3);
		}
	}


	for (i = 0; i < 2; i++) {
		interface[i].transmit_txhz = interface[i].transmit_pps / pps_hz;
	}

	if (!opt_txonly)
		interface_up(ifname[0]);	/* RX */
	if (!opt_rxonly)
		interface_up(ifname[1]);	/* TX */

	if (!opt_txonly)
		interface_setup(0, ifname[0]);	/* RX */
	if (!opt_rxonly)
		interface_setup(1, ifname[1]);	/* TX */


	/*
	 * configure adrlist
	 */
	for (i = 0; i < 2; i++) {
		interface[i].adrlist = addresslist_new();
		addresslist_setlimit(interface[i].adrlist, MAXFLOWNUM);
	}

	if (opt_flowlist != NULL) {
		FILE *fh;
		char *line;
		char buf[1024];
		size_t len, lineno;
		int anyerror;

		fh = fopen(opt_flowlist, "r");
		if (fh == NULL) {
			fprintf(stderr, "%s: %s\n", opt_flowlist, strerror(errno));
			exit(2);
		}

		anyerror = 0;
		for (lineno = 1; ((line = fgets(buf, sizeof(buf), fh)) != NULL); lineno++) {
			while ((*line == ' ') || (*line == '\t'))
				line++;
			if (line[0] == '#')
				continue;

			/* chop '\n' */
			len = strlen(line);
			if (len > 0)
				line[len - 1] = '\0';

			/* for TX */
			if (parse_flowstr(interface[1].adrlist, opt_tcp ? IPPROTO_TCP : IPPROTO_UDP, line, 0) != 0) {
				fprintf(stderr, "%s:%lld: cannot parse: \"%s\"\n", opt_flowlist, (unsigned long long)lineno, line);
				anyerror++;
			}
			/* for RX */
			parse_flowstr(interface[0].adrlist, opt_tcp ? IPPROTO_TCP : IPPROTO_UDP, line, 1);
		}
		fclose(fh);
		if (anyerror)
			exit(2);

	} else {
		struct in_addr xaddr;
		struct in6_addr xaddr6, xaddr6_begin;

		if (opt_addrrange) {
			if (opt_srcaddr_af == AF_INET) {
				/* exclude hostzero address and gw address and broadcast address */
				xaddr.s_addr = interface[1].ipaddr.s_addr | ~interface[1].ipaddr_mask.s_addr;	/* broadcast */
				addresslist_exclude_daddr(interface[0].adrlist, xaddr);
				addresslist_exclude_saddr(interface[1].adrlist, xaddr);
				xaddr.s_addr = interface[1].ipaddr.s_addr & interface[1].ipaddr_mask.s_addr;	/* hostzero */
				addresslist_exclude_daddr(interface[0].adrlist, xaddr);
				addresslist_exclude_saddr(interface[1].adrlist, xaddr);
				xaddr.s_addr = interface[1].gwaddr.s_addr;					/* gw address */
				addresslist_exclude_daddr(interface[0].adrlist, xaddr);
				addresslist_exclude_saddr(interface[1].adrlist, xaddr);

				xaddr.s_addr = interface[0].ipaddr.s_addr | ~interface[0].ipaddr_mask.s_addr;	/* broadcast */
				addresslist_exclude_saddr(interface[0].adrlist, xaddr);
				addresslist_exclude_daddr(interface[1].adrlist, xaddr);
				xaddr.s_addr = interface[0].ipaddr.s_addr & interface[0].ipaddr_mask.s_addr;	/* hostzero */
				addresslist_exclude_saddr(interface[0].adrlist, xaddr);
				addresslist_exclude_daddr(interface[1].adrlist, xaddr);
				xaddr.s_addr = interface[0].gwaddr.s_addr;					/* gw address */
				addresslist_exclude_saddr(interface[0].adrlist, xaddr);
				addresslist_exclude_daddr(interface[1].adrlist, xaddr);

				if (opt_saddr == 0)
					opt_srcaddr_begin.s_addr = opt_srcaddr_end.s_addr = interface[1].ipaddr.s_addr;
				if (opt_daddr == 0)
					opt_dstaddr_begin.s_addr = opt_dstaddr_end.s_addr = interface[0].ipaddr.s_addr;

				rc = addresslist_append(interface[1].adrlist, opt_tcp ? IPPROTO_TCP : IPPROTO_UDP,
				    opt_srcaddr_begin, opt_srcaddr_end,
				    opt_dstaddr_begin, opt_dstaddr_end,
				    opt_srcport_begin, opt_srcport_end,
				    opt_dstport_begin, opt_dstport_end);
				if (rc != 0)
					exit(1);

				rc = addresslist_append(interface[0].adrlist, opt_tcp ? IPPROTO_TCP : IPPROTO_UDP,
				    opt_dstaddr_begin, opt_dstaddr_end,
				    opt_srcaddr_begin, opt_srcaddr_end,
				    opt_dstport_begin, opt_dstport_end,
				    opt_srcport_begin, opt_srcport_end);
				if (rc != 0)
					exit(1);
			} else {
				/* exclude gw address */
				xaddr6 = interface[1].gw6addr;						/* gw address */
				addresslist_exclude_daddr6(interface[0].adrlist, &xaddr6);
				addresslist_exclude_saddr6(interface[1].adrlist, &xaddr6);

				xaddr6 = interface[0].gw6addr;						/* gw address */
				addresslist_exclude_saddr6(interface[0].adrlist, &xaddr6);
				addresslist_exclude_daddr6(interface[1].adrlist, &xaddr6);

				if (opt_saddr == 0)
					opt_srcaddr6_begin = opt_srcaddr6_end = interface[1].ip6addr;
				if (opt_daddr == 0)
					opt_dstaddr6_begin = opt_dstaddr6_end = interface[0].ip6addr;

				rc = addresslist_append6(interface[1].adrlist, opt_tcp ? IPPROTO_TCP : IPPROTO_UDP,
				    &opt_srcaddr6_begin, &opt_srcaddr6_end,
				    &opt_dstaddr6_begin, &opt_dstaddr6_end,
				    opt_srcport_begin, opt_srcport_end,
				    opt_dstport_begin, opt_dstport_end);
				if (rc != 0)
					exit(1);

				rc = addresslist_append6(interface[0].adrlist, opt_tcp ? IPPROTO_TCP : IPPROTO_UDP,
				    &opt_dstaddr6_begin, &opt_dstaddr6_end,
				    &opt_srcaddr6_begin, &opt_srcaddr6_end,
				    opt_dstport_begin, opt_dstport_end,
				    opt_srcport_begin, opt_srcport_end);
				if (rc != 0)
					exit(1);
			}

		} else if (opt_allnet) {

			if (!ipv4_iszero(&interface[0].ipaddr) && !ipv4_iszero(&interface[1].ipaddr)) {
				/* exclude hostzero address and gw address and broadcast address */
				xaddr.s_addr = interface[0].ipaddr.s_addr | ~interface[0].ipaddr_mask.s_addr;	/* broadcast */
				addresslist_exclude_daddr(interface[1].adrlist, xaddr);
				xaddr.s_addr = interface[0].ipaddr.s_addr & interface[0].ipaddr_mask.s_addr;	/* hostzero */
				addresslist_exclude_daddr(interface[1].adrlist, xaddr);
				xaddr.s_addr = interface[0].gwaddr.s_addr;					/* gw address */
				addresslist_exclude_daddr(interface[1].adrlist, xaddr);

				xaddr.s_addr = interface[1].ipaddr.s_addr | ~interface[1].ipaddr_mask.s_addr;	/* broadcast */
				addresslist_exclude_daddr(interface[0].adrlist, xaddr);
				xaddr.s_addr = interface[1].ipaddr.s_addr & interface[1].ipaddr_mask.s_addr;	/* hostzero */
				addresslist_exclude_daddr(interface[0].adrlist, xaddr);
				xaddr.s_addr = interface[1].gwaddr.s_addr;					/* gw address */
				addresslist_exclude_daddr(interface[0].adrlist, xaddr);

				xaddr.s_addr = interface[0].ipaddr.s_addr | ~interface[0].ipaddr_mask.s_addr;	/* broadcast */
				rc = addresslist_append(interface[1].adrlist, opt_tcp ? IPPROTO_TCP : IPPROTO_UDP,
				    interface[1].ipaddr, interface[1].ipaddr,
				    interface[0].ipaddr, xaddr,
				    opt_srcport_begin, opt_srcport_end,
				    opt_dstport_begin, opt_dstport_end);
				if (rc != 0)
					exit(1);

				xaddr.s_addr = interface[1].ipaddr.s_addr | ~interface[0].ipaddr_mask.s_addr;	/* broadcast */
				rc = addresslist_append(interface[0].adrlist, opt_tcp ? IPPROTO_TCP : IPPROTO_UDP,
				    interface[0].ipaddr, interface[0].ipaddr,
				    interface[1].ipaddr, xaddr,
				    opt_dstport_begin, opt_dstport_end,
				    opt_srcport_begin, opt_srcport_end);
				if (rc != 0)
					exit(1);

			} else if (!ipv6_iszero(&interface[0].ip6addr) && !ipv6_iszero(&interface[1].ip6addr)) {
				/* exclude gw address */
				xaddr6 = interface[0].gw6addr;					/* gw address */
				addresslist_exclude_daddr6(interface[1].adrlist, &xaddr6);
				xaddr6 = interface[1].gw6addr;					/* gw address */
				addresslist_exclude_daddr6(interface[0].adrlist, &xaddr6);

				/* e.g.) fd00::1/112 => from fd00:0 to fd00::ffff */
				xaddr6_begin = interface[0].ip6addr_mask;
				ipv6_and(&interface[0].ip6addr, &xaddr6_begin, &xaddr6_begin);	/* beginning of network address */
				ipv6_not(&interface[0].ip6addr_mask, &xaddr6);
				ipv6_or(&interface[0].ip6addr, &xaddr6, &xaddr6);	/* end of network address */
				rc = addresslist_append6(interface[1].adrlist, opt_tcp ? IPPROTO_TCP : IPPROTO_UDP,
				    &interface[1].ip6addr, &interface[1].ip6addr,
				    &xaddr6_begin, &xaddr6,
				    opt_srcport_begin, opt_srcport_end,
				    opt_dstport_begin, opt_dstport_end);
				if (rc != 0)
					exit(1);

				xaddr6_begin = interface[1].ip6addr_mask;
				ipv6_and(&interface[1].ip6addr, &xaddr6_begin, &xaddr6_begin);	/* beginning of network address */
				ipv6_not(&interface[1].ip6addr_mask, &xaddr6);
				ipv6_or(&interface[1].ip6addr, &xaddr6, &xaddr6);	/* last of network address */
				rc = addresslist_append6(interface[0].adrlist, opt_tcp ? IPPROTO_TCP : IPPROTO_UDP,
				    &interface[0].ip6addr, &interface[0].ip6addr,
				    &xaddr6_begin, &xaddr6,
				    opt_dstport_begin, opt_dstport_end,
				    opt_srcport_begin, opt_srcport_end);
				if (rc != 0)
					exit(1);

			} else {
				fprintf(stderr, "no address info on %s and %s\n",
				    interface[0].ifname, interface[1].ifname);
				exit(1);
			}

		} else {
			if (!ipv4_iszero(&interface[0].ipaddr) && !ipv4_iszero(&interface[1].ipaddr)) {
				rc = addresslist_append(interface[1].adrlist, opt_tcp ? IPPROTO_TCP : IPPROTO_UDP,
				    interface[1].ipaddr, interface[1].ipaddr,
				    interface[0].ipaddr, interface[0].ipaddr,
				    opt_srcport_begin, opt_srcport_end,
				    opt_dstport_begin, opt_dstport_end);
				if (rc != 0)
					exit(1);

				rc = addresslist_append(interface[0].adrlist, opt_tcp ? IPPROTO_TCP : IPPROTO_UDP,
				    interface[0].ipaddr, interface[0].ipaddr,
				    interface[1].ipaddr, interface[1].ipaddr,
				    opt_srcport_begin, opt_srcport_end,
				    opt_dstport_begin, opt_dstport_end);
				if (rc != 0)
					exit(1);

			} else if (!ipv6_iszero(&interface[0].ip6addr) && !ipv6_iszero(&interface[1].ip6addr)) {
				rc = addresslist_append6(interface[1].adrlist, opt_tcp ? IPPROTO_TCP : IPPROTO_UDP,
				    &interface[1].ip6addr, &interface[1].ip6addr,
				    &interface[0].ip6addr, &interface[0].ip6addr,
				    opt_srcport_begin, opt_srcport_end,
				    opt_dstport_begin, opt_dstport_end);
				if (rc != 0)
					exit(1);

				rc = addresslist_append6(interface[0].adrlist, opt_tcp ? IPPROTO_TCP : IPPROTO_UDP,
				    &interface[0].ip6addr, &interface[0].ip6addr,
				    &interface[1].ip6addr, &interface[1].ip6addr,
				    opt_srcport_begin, opt_srcport_end,
				    opt_dstport_begin, opt_dstport_end);
				if (rc != 0)
					exit(1);

			} else {
				fprintf(stderr, "no address specified on %s and %s\n",
				    interface[0].ifname, interface[1].ifname);
				exit(1);
			}
		}
	}

	if (addresslist_include_af(interface[0].adrlist, AF_INET6) ||
	    addresslist_include_af(interface[1].adrlist, AF_INET6)) {
		use_ipv6 = 1;
	} else {
		use_ipv6 = 0;
	}
	update_min_pktsize();

	if (opt_flowsort) {
		addresslist_rebuild(interface[1].adrlist);
		addresslist_rebuild(interface[0].adrlist);
	}
	if (opt_flowdump) {
		printf("\nflowlist of TX side interface\n");
		addresslist_dump(interface[1].adrlist);
		printf("\nflowlist of RX side interface\n");
		addresslist_dump(interface[0].adrlist);
		exit(1);
	}

	if (addresslist_get_tuplenum(interface[1].adrlist) == 0) {
		fprintf(stderr, "--saddr: no valid addresses. (hostzero, gateway or broadcast address were excluded)\n");
		exit(1);
	}
	if (addresslist_get_tuplenum(interface[0].adrlist) == 0) {
		fprintf(stderr, "--daddr: no valid addresses. (hostzero, gateway or broadcast address were excluded)\n");
		exit(1);
	}


	printf("HZ=%d\n", pps_hz);
	printf("%s %s %s, ",
	    ifname[1],
	    opt_fulldup ? "<->" : "->",
	    ifname[0]);
	printf("IP pktsize %d, %u pps, %u Mbps (%u bps)\n", interface[0].pktsize, interface[0].transmit_pps,
	    PKTSIZE2FRAMESIZE(interface[0].pktsize + ETHHDRSIZE) * 8 * interface[0].transmit_pps / 1000 / 1000,
	    PKTSIZE2FRAMESIZE(interface[0].pktsize + ETHHDRSIZE) * 8 * interface[0].transmit_pps);


	/*
	 * open netmap devices
	 */
	if (!opt_txonly)
		interface_open(0, ifname[0]);	/* RX */
	if (!opt_rxonly)
		interface_open(1, ifname[1]);	/* TX */

	if (!opt_rxonly)
		interface_wait_linkup(ifname[1], 1);	/* TX */
	if (!opt_txonly)
		interface_wait_linkup(ifname[0], 1);	/* RX */

	for (i = 0; i < 2; i++) {
		char inetbuf1[INET6_ADDRSTRLEN];
		char inetbuf2[INET6_ADDRSTRLEN];

		printf("%s(%s)",
		    interface[i].ifname,
		    ether_ntoa(&interface[i].eaddr));

		switch (interface[i].af_addr) {
		case AF_INET:
			printf(" %s/%s",
			    inet_ntop(AF_INET, &interface[i].ipaddr, inetbuf1, sizeof(inetbuf1)),
			    inet_ntop(AF_INET, &interface[i].ipaddr_mask, inetbuf2, sizeof(inetbuf2)));
			break;
		case AF_INET6:
			printf(" %s/%d",
			    inet_ntop(AF_INET6, &interface[i].ip6addr, inetbuf1, sizeof(inetbuf1)),
			    in6addr2prefix(&interface[i].ip6addr_mask));
			break;
		}

		switch (interface[i].af_gwaddr) {
		case AF_INET:
			printf(" -> %s(%s)\n",
			    ether_ntoa(&interface[i].gweaddr),
			    inet_ntop(AF_INET, &interface[i].gwaddr, inetbuf1, sizeof(inetbuf1)));
			break;
		case AF_INET6:
			printf(" -> %s(%s)\n",
			    ether_ntoa(&interface[i].gweaddr),
			    inet_ntop(AF_INET6, &interface[i].gw6addr, inetbuf1, sizeof(inetbuf1)));
			break;
		default:
			printf(" -> %s\n",
			    ether_ntoa(&interface[i].gweaddr));
			break;
		}
	}

	if (opt_nflow == 0)
		opt_nflow = MAX(get_flownum(0), get_flownum(1));

	/*
	 * allocate per frame seqchecker
	 */
	j = get_flowid_max(0);
	interface[0].sequence_tx_perflow = malloc(sizeof(uint64_t) * j);
	memset(interface[0].sequence_tx_perflow, 0, sizeof(uint64_t) * j);
	interface[0].seqchecker_perflow = malloc(sizeof(struct sequencechecker *) * j);
	interface[0].seqchecker_flowtotal = seqcheck_new();
	for (i = 0; i < j; i++) {
		interface[0].seqchecker_perflow[i] = seqcheck_new();
		if (interface[0].seqchecker_perflow[i] == NULL) {
			fprintf(stderr, "cannot allocate %s flow sequence work %d/%d\n", interface[0].ifname, i, j);
			exit(1);
		}
		seqcheck_setparent(interface[0].seqchecker_perflow[i], interface[0].seqchecker_flowtotal);
	}

	j = get_flowid_max(1);
	interface[1].sequence_tx_perflow = malloc(sizeof(uint64_t) * j);
	memset(interface[1].sequence_tx_perflow, 0, sizeof(uint64_t) * j);
	interface[1].seqchecker_perflow = malloc(sizeof(struct sequencechecker *) * j);
	interface[1].seqchecker_flowtotal = seqcheck_new();
	for (i = 0; i < j; i++) {
		interface[1].seqchecker_perflow[i] = seqcheck_new();
		if (interface[1].seqchecker_perflow[i] == NULL) {
			fprintf(stderr, "cannot allocate %s flow sequence work %d/%d\n", interface[1].ifname, i, j);
			exit(1);
		}
		seqcheck_setparent(interface[1].seqchecker_perflow[i], interface[1].seqchecker_flowtotal);
	}


	for (i = 0; i < 2; i++) {
		ip4pkt_udp_template(pktbuffer_ipv4_udp[i], 1500 + sizeof(struct ether_header));
		build_template_packet_ipv4(i, pktbuffer_ipv4_udp[i]);
		ip4pkt_tcp_template(pktbuffer_ipv4_tcp[i], 1500 + sizeof(struct ether_header));
		build_template_packet_ipv4(i, pktbuffer_ipv4_tcp[i]);
		ip6pkt_udp_template(pktbuffer_ipv6_udp[i], 1500 + sizeof(struct ether_header));
		build_template_packet_ipv6(i, pktbuffer_ipv6_udp[i]);
		ip6pkt_tcp_template(pktbuffer_ipv6_tcp[i], 1500 + sizeof(struct ether_header));
		build_template_packet_ipv6(i, pktbuffer_ipv6_tcp[i]);
	}

	if (!opt_txonly)
		pthread_create(&txrxthread0, NULL, txrx_thread_main, &ifnum[0]);
	if (!opt_rxonly)
		pthread_create(&txrxthread1, NULL, txrx_thread_main, &ifnum[1]);

	/* update transmit flags */
	if (!opt_txonly && opt_fulldup)
		transmit_set(0, 1);
	if (!opt_rxonly)
		transmit_set(1, 1);


	clock_gettime(CLOCK_MONOTONIC, &currenttime);

	(void)sigemptyset(&used_sigset);
	(void)sigaddset(&used_sigset, SIGALRM);
	(void)sigaddset(&used_sigset, SIGHUP);
	(void)sigaddset(&used_sigset, SIGINT);
	(void)sigaddset(&used_sigset, SIGQUIT);
	(void)sigaddset(&used_sigset, SIGTSTP);
	(void)sigaddset(&used_sigset, SIGCONT);

	signal(SIGHUP, sighandler_int);
	signal(SIGINT, sighandler_int);
	signal(SIGQUIT, sighandler_int);
	signal(SIGTSTP, sighandler_tstp);
	signal(SIGCONT, sighandler_cont);

	/* setup signals */
	signal(SIGALRM, sighandler_alrm);
	{
		struct itimerval itv;
		memset(&itv, 0, sizeof(itv));
		itv.it_interval.tv_sec = 0;
		itv.it_interval.tv_usec = 1000000 / pps_hz;
		itv.it_value = itv.it_interval;
		setitimer(ITIMER_REAL, &itv, NULL);
	}

	/* CUI/web interface thread */
	control_thread_main(NULL);

	return 1;
}
