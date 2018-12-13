qq.com/*****************************************************************************
 *
 * Copyright (C) 2001 Uppsala University and Ericsson AB.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Erik Nordström, <erik.nordstrom@it.uu.se>
 *
 *****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <linux/sockios.h>
#include <linux/if.h>
#include <linux/wireless.h>
#include <getopt.h>
#include <ctype.h>

#include "defs.h"
#include "debug.h"
#include "timer_queue.h"
#include "params.h"
#include "aodv_socket.h"
#include "aodv_timeout.h"
#include "routing_table.h"
#include "aodv_hello.h"
#include "nl.h"

#ifdef LLFEEDBACK
#include "llf.h"
#endif

/* Global variables: */
int log_to_file = 0;
int rt_log_interval = 0;	/* msecs between routing table logging 0=off */
int unidir_hack = 0;
int rreq_gratuitous = 0;
int expanding_ring_search = 1;
int internet_gw_mode = 0;
int local_repair = 0;
int receive_n_hellos = 0;
int hello_jittering = 1;
int optimized_hellos = 0;
int ratelimit = 1;		/* Option for rate limiting RREQs and RERRs. */
char *progname;
int wait_on_reboot = 1;
int qual_threshold = 0;
int llfeedback = 0;
int gw_prefix = 1;
struct timer worb_timer;	/* Wait on reboot timer */

/* Dynamic configuration values */
int active_route_timeout = ACTIVE_ROUTE_TIMEOUT_HELLO;
int ttl_start = TTL_START_HELLO;
int delete_period = DELETE_PERIOD_HELLO;

static void cleanup();

struct option longopts[] = {
    {"interface", required_argument, NULL, 'i'},
    {"hello-jitter", no_argument, NULL, 'j'},
    {"log", no_argument, NULL, 'l'},
    {"n-hellos", required_argument, NULL, 'n'},
    {"daemon", no_argument, NULL, 'd'},
    {"force-gratuitous", no_argument, NULL, 'g'},
    {"opt-hellos", no_argument, NULL, 'o'},
    {"quality-threshold", required_argument, NULL, 'q'},
    {"log-rt-table", required_argument, NULL, 'r'},
    {"unidir_hack", no_argument, NULL, 'u'},
    {"gateway-mode", no_argument, NULL, 'w'},
    {"help", no_argument, NULL, 'h'},
    {"no-expanding-ring", no_argument, NULL, 'x'},
    {"no-worb", no_argument, NULL, 'D'},
    {"local-repair", no_argument, NULL, 'L'},
    {"rate-limit", no_argument, NULL, 'R'},
    {"version", no_argument, NULL, 'V'},
    {"llfeedback", no_argument, NULL, 'f'},
    {0}
};

void usage(int status)
{
    if (status != 0) {
	fprintf(stderr, "Try `%s --help' for more information.\n", progname);
	exit(status);
    }

    printf
	("\nUsage: %s [-dghjlouwxLDRV] [-i if0,if1,..] [-r N] [-n N] [-q THR]\n\n"
	 "-d, --daemon            Daemon mode, i.e. detach from the console.\n"
	 "-g, --force-gratuitous  Force the gratuitous flag to be set on all RREQ's.\n"
	 "-h, --help              This information.\n"
	 "-i, --interface         Network interfaces to attach to. Defaults to first\n"
	 "                        wireless interface.\n"
	 "-j, --hello-jitter      Toggle hello jittering (default ON).\n"
	 "-l, --log               Log debug output to %s.\n"
	 "-o, --opt-hellos        Send HELLOs only when forwarding data (experimental).\n"
	 "-r, --log-rt-table      Log routing table to %s every N secs.\n"
	 "-n, --n-hellos          Receive N hellos from host before treating as neighbor.\n"
	 "-u, --unidir-hack       Detect and avoid unidirectional links (experimental).\n"
	 "-w, --gateway-mode      Enable experimental Internet gateway support.\n"
	 "-x, --no-expanding-ring Disable expanding ring search for RREQs.\n"
	 "-D, --no-worb           Disable 15 seconds wait on reboot delay.\n"
	 "-L, --local-repair      Enable local repair.\n"
	 "-f, --llfeedback        Enable link layer feedback.\n"
	 "-R, --rate-limit        Toggle rate limiting of RREQs and RERRs (default ON).\n"
	 "-q, --quality-threshold Set a minimum signal quality threshold for control packets.\n"
	 "-V, --version           Show version.\n\n"
	 "Erik Nordström, <erik.nordstrom@it.uu.se>\n\n",
	 progname, AODV_LOG_PATH, AODV_RT_LOG_PATH);

    exit(status);
}

int set_kernel_options()
{
    int i, fd = -1;
    char on = '1';
    char off = '0';
    char command[64];

    if ((fd = open("/proc/sys/net/ipv4/ip_forward", O_WRONLY)) < 0)
	return -1;
    if (write(fd, &on, sizeof(char)) < 0)
	return -1;
    close(fd);
    /*
      Disabled for kernel 2.6.28.


    if ((fd = open("/proc/sys/net/ipv4/route/max_delay", O_WRONLY)) < 0)
	return -1;
    if (write(fd, &off, sizeof(char)) < 0)
	return -1;
    close(fd);

    if ((fd = open("/proc/sys/net/ipv4/route/min_delay", O_WRONLY)) < 0)
	return -1;
    if (write(fd, &off, sizeof(char)) < 0)
	return -1;
    close(fd);
    */

    /* Disable ICMP redirects on all interfaces: */

    for (i = 0; i < MAX_NR_INTERFACES; i++) {
	if (!DEV_NR(i).enabled)
	    continue;

	memset(command, '\0', 64);
	sprintf(command, "/proc/sys/net/ipv4/conf/%s/send_redirects",
		DEV_NR(i).ifname);
	if ((fd = open(command, O_WRONLY)) < 0)
	    return -1;
	if (write(fd, &off, sizeof(char)) < 0)
	    return -1;
	close(fd);
	memset(command, '\0', 64);
	sprintf(command, "/proc/sys/net/ipv4/conf/%s/accept_redirects",
		DEV_NR(i).ifname);
	if ((fd = open(command, O_WRONLY)) < 0)
	    return -1;
	if (write(fd, &off, sizeof(char)) < 0)
	    return -1;
	close(fd);
    }
    memset(command, '\0', 64);
    sprintf(command, "/proc/sys/net/ipv4/conf/all/send_redirects");
    if ((fd = open(command, O_WRONLY)) < 0)
	return -1;
    if (write(fd, &off, sizeof(char)) < 0)
	return -1;
    close(fd);


    memset(command, '\0', 64);
    sprintf(command, "/proc/sys/net/ipv4/conf/all/accept_redirects");
    if ((fd = open(command, O_WRONLY)) < 0)
	return -1;
    if (write(fd, &off, sizeof(char)) < 0)
	return -1;
    close(fd);

    return 0;
}

int find_default_gw(void)
{
    FILE *route;
    char buf[100], *l;

    route = fopen("/proc/net/route", "r");

    if (route == NULL) {
	perror("open /proc/net/route");
	exit(-1);
    }

    while (fgets(buf, sizeof(buf), route)) {
	l = strtok(buf, " \t");
	l = strtok(NULL, " \t");
	if (l != NULL) {
	    if (strcmp("00000000", l) == 0) {
		l = strtok(NULL, " \t");
		l = strtok(NULL, " \t");
		if (strcmp("0003", l) == 0) {
		    fclose(route);
		    return 1;
		}
	    }
	}
    }
    fclose(route);
    return 0;
}

/*
 * Returns information on a network interface given its name...
 */
struct sockaddr_in *get_if_info(char *ifname, int type)
{
    int skfd;
    struct sockaddr_in *ina;
    static struct ifreq ifr;

    /* Get address of interface... */
    skfd = socket(AF_INET, SOCK_DGRAM, 0);

    strcpy(ifr.ifr_name, ifname);
    if (ioctl(skfd, type, &ifr) < 0) {
	alog(LOG_ERR, errno, __FUNCTION__,
	     "Could not get address of %s ", ifname);
	close(skfd);
	return NULL;
    } else {
	ina = (struct sockaddr_in *) &ifr.ifr_addr;
	close(skfd);
	return ina;
    }
}

/* This will limit the number of handler functions we can have for
   sockets and file descriptors and so on... */
#define CALLBACK_FUNCS 5
static struct callback {
    int fd;
    callback_func_t func;
} callbacks[CALLBACK_FUNCS];

static int nr_callbacks = 0;

int attach_callback_func(int fd, callback_func_t func)
{
    if (nr_callbacks >= CALLBACK_FUNCS) {
	fprintf(stderr, "callback attach limit reached!!\n");
	exit(-1);
    }
    callbacks[nr_callbacks].fd = fd;
    callbacks[nr_callbacks].func = func;
    nr_callbacks++;
    return 0;
}

/* Here we find out how to load the kernel modules... If the modules
   are located in the current directory. use those. Otherwise fall
   back to modprobe. */

void load_modules(char *ifname)
{
    struct stat st;
    char buf[1024], *l = NULL;
    int found = 0;
    FILE *m;

    memset(buf, '\0', 64);

    if (stat("./kaodv.ko", &st) == 0)
	sprintf(buf, "/sbin/insmod kaodv.ko ifname=%s &>/dev/null", ifname);
    else if (stat("./kaodv.o", &st) == 0)
	sprintf(buf, "/sbin/insmod kaodv.o ifname=%s &>/dev/null", ifname);
    else
	sprintf(buf, "/sbin/modprobe kaodv ifname=%s &>/dev/null", ifname);

    if (system(buf) == -1) {
        fprintf(stderr, "Could not load kaodv module\n");
        exit(-1);
    }

    usleep(100000);

    /* Check result */
    m = fopen("/proc/modules", "r");
    while (fgets(buf, sizeof(buf), m)) {
	l = strtok(buf, " \t");
	if (!strcmp(l, "kaodv"))
	    found++;
	if (!strcmp(l, "ipchains")) {
	    fprintf(stderr,
		    "The ipchains kernel module is loaded and prevents AODV-UU from functioning properly.\n");
	    exit(-1);
	}
    }
    fclose(m);

    if (found < 1) {
	fprintf(stderr,
		"A kernel module could not be loaded, check your installation... %d\n",
		found);
	exit(-1);
    }
}

void remove_modules(void)
{
	int ret;

	ret = system("/sbin/rmmod kaodv &>/dev/null");

	if (ret != 0) {
		fprintf(stderr, "Could not remove kernel module kaodv\n");
	}
}

void host_init(char *ifname)
{
    struct sockaddr_in *ina;
    char buf[1024], tmp_ifname[IFNAMSIZ],
	ifnames[(IFNAMSIZ + 1) * MAX_NR_INTERFACES], *iface;
    struct ifconf ifc;
    struct ifreq ifreq, *ifr;
    int i, iw_sock, if_sock = 0;

    memset(&this_host, 0, sizeof(struct host_info));
    memset(dev_indices, 0, sizeof(unsigned int) * MAX_NR_INTERFACES);

    if (!ifname) {
	/* No interface was given... search for first wireless. */
	iw_sock = socket(PF_INET, SOCK_DGRAM, 0);
	ifc.ifc_len = sizeof(buf);
	ifc.ifc_buf = buf;
	if (ioctl(iw_sock, SIOCGIFCONF, &ifc) < 0) {
	    fprintf(stderr, "Could not get wireless info\n");
	    exit(-1);
	}
	ifr = ifc.ifc_req;
	for (i = ifc.ifc_len / sizeof(struct ifreq); i >= 0; i--, ifr++) {
	    struct iwreq req;

	    strcpy(req.ifr_name, ifr->ifr_name);
	    if (ioctl(iw_sock, SIOCGIWNAME, &req) >= 0) {
		strcpy(tmp_ifname, ifr->ifr_name);
		break;
	    }
	}
	/* Did we find a wireless interface? */
	if (!strlen(tmp_ifname)) {
	    fprintf(stderr, "\nCould not find a wireless interface!\n");
	    fprintf(stderr, "Use -i <interface> to override...\n\n");
	    exit(-1);
	}
	strcpy(ifreq.ifr_name, tmp_ifname);
	if (ioctl(iw_sock, SIOCGIFINDEX, &ifreq) < 0) {
	    alog(LOG_ERR, errno, __FUNCTION__,
		 "Could not get index of %s", tmp_ifname);
	    close(if_sock);
	    exit(-1);
	}
	close(iw_sock);

	ifname = tmp_ifname;

	alog(LOG_NOTICE, 0, __FUNCTION__,
	     "Attaching to %s, override with -i <if1,if2,...>.", tmp_ifname);
    }

    strcpy(ifnames, ifname);

    /* Intitialize the local sequence number an rreq_id to zero */
    this_host.seqno = 1;
    this_host.rreq_id = 0;

    /* Zero interfaces enabled so far... */
    this_host.nif = 0;

    gettimeofday(&this_host.bcast_time, NULL);

    /* Find the indices of all interfaces to broadcast on... */
    if_sock = socket(AF_INET, SOCK_DGRAM, 0);

    iface = strtok(ifname, ",");

    /* OK, now lookup interface information, and store it... */
    do {
	strcpy(ifreq.ifr_name, iface);
	if (ioctl(if_sock, SIOCGIFINDEX, &ifreq) < 0) {
	    alog(LOG_ERR, errno, __FUNCTION__, "Could not get index of %s",
		 iface);
	    close(if_sock);
	    exit(-1);
	}
	this_host.devs[this_host.nif].ifindex = ifreq.ifr_ifindex;

	dev_indices[this_host.nif++] = ifreq.ifr_ifindex;

	strcpy(DEV_IFINDEX(ifreq.ifr_ifindex).ifname, iface);

	/* Get IP-address of interface... */
	ina = get_if_info(iface, SIOCGIFADDR);

	if (ina == NULL)
	    exit(-1);

	DEV_IFINDEX(ifreq.ifr_ifindex).ipaddr = ina->sin_addr;

	/* Get netmask of interface... */
	ina = get_if_info(iface, SIOCGIFNETMASK);

	if (ina == NULL)
	    exit(-1);

	DEV_IFINDEX(ifreq.ifr_ifindex).netmask = ina->sin_addr;

	ina = get_if_info(iface, SIOCGIFBRDADDR);

	if (ina == NULL)
	    exit(-1);

	DEV_IFINDEX(ifreq.ifr_ifindex).broadcast = ina->sin_addr;

	DEV_IFINDEX(ifreq.ifr_ifindex).enabled = 1;

	if (this_host.nif >= MAX_NR_INTERFACES)
	    break;

    } while ((iface = strtok(NULL, ",")));

    close(if_sock);

    /* Load kernel modules */
    load_modules(ifnames);

    /* Enable IP forwarding and set other kernel options... */
    if (set_kernel_options() < 0) {
	fprintf(stderr, "Could not set kernel options!\n");
	exit(-1);
    }
}

/* This signal handler ensures clean exits */
void signal_handler(int type)
{

    switch (type) {
    case SIGSEGV:
	alog(LOG_ERR, 0, __FUNCTION__, "SEGMENTATION FAULT!!!! Exiting!!! "
	     "To get a core dump, compile with DEBUG option.");
    case SIGINT:
    case SIGHUP:
    case SIGTERM:
    default:
	exit(0);
    }
}

int main(int argc, char **argv)
{
    static char *ifname = NULL;	/* Name of interface to attach to */
    fd_set rfds, readers;
    int n, nfds = 0, i;
    int daemonize = 0;
    struct timeval *timeout;
    struct timespec timeout_spec;
    struct sigaction sigact;
    sigset_t mask, origmask;
    /* 初始化相关信息*/

    /* Remember the name of the executable... */
    progname = strrchr(argv[0], '/');

    if (progname)
	progname++;
    else
	progname = argv[0];

    /* Use debug output as default */
    debug = 1;

    memset (&sigact, 0, sizeof(struct sigaction));
    sigact.sa_handler = signal_handler;
        
    /* This server should shut down on these signals. */
    sigaction(SIGTERM, &sigact, 0);
    sigaction(SIGHUP, &sigact, 0);
    sigaction(SIGINT, &sigact, 0);
    
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGINT);
    /* 初始化信号*/
    /* Only capture segmentation faults when we are not debugging... */
#ifndef DEBUG
    sigaddset(&mask, SIGSEGV);
#endif

    /* Block the signals we are watching here
     *改为用pselect处理它们。* /
    sigprocmask（SIG_BLOCK，＆mask，＆origmask）;

    / *解析命令行：* /
    而（1）{
	int opt;

	opt = getopt_long（argc，argv，“ i：fjln：dghoq：r：s：uwxDLRV ”，longopts，0）;

	if（opt == EOF）
	    打破 ;

	switch（opt）{
	案例 0：
	    打破 ;
	案例 ' d '：
	    debug = 0 ;
	    daemonize = 1 ;
	    打破 ;
	案例 ' f '：
	    llfeedback = 1 ;
	    active_route_timeout = ACTIVE_ROUTE_TIMEOUT_LLF;
	    打破 ;
	案例 ' g '：
	    rreq_gratuitous =！rreq_gratuitous;
	    打破 ;
	案例 '我'：
ifname 	    = optarg ;
	    打破 ;
	案例 ' j '：
	    hello_jittering =！hello_jittering;
	    打破 ;
	案例 ' l '：
	    log_to_file =！log_to_file;
	    打破 ;
	案例 ' n '：
	    if（optarg && isdigit（* optarg））{
		receive_n_hellos = atoi（optarg）;
		if（receive_n_hellos < 2）{
		    fprintf（stderr，“ -  n应该至少为2！\ n ”）;
		    退出（ - 1）;
		}
	    }
	    打破 ;
	案例 ' o '：
	    optimized_hellos =！optimized_hellos;
	    打破 ;
	案例 ' q '：
	    if（optarg && isdigit（* optarg））
		qual_threshold = atoi（optarg）;
	    打破 ;
	案例 ' r '：
	    if（optarg && isdigit（* optarg））
		rt_log_interval = atof（optarg）* 1000 ;
	    打破 ;
	案例 '你'：
	    unidir_hack =！unidir_hack;
	    打破 ;
	案例 ' w '：
	    internet_gw_mode =！internet_gw_mode;
	    打破 ;
	案例 ' x '：
	    expanding_ring_search =！expanding_ring_search;
	    打破 ;
	案例 ' L '：
	    local_repair =！local_repair;
	    打破 ;
	案例 ' D '：
	    wait_on_reboot =！wait_on_reboot;
	    打破 ;
	案例 ' R '：
	    ratelimit =！ratelimit;
	    打破 ;
	案例 ' V '：
	    的printf
		（“ \ n AODV-UU v ％s，％s ©乌普萨拉大学和爱立信AB。\ n作者：ErikNordström，<erik.nordstrom@it.uu.se> \ n \ n ”，
		 AODV_UU_VERSION，DRAFT_VERSION）;
	    退出（0）;
	    打破 ;
	案件 '？'：
	案例 '：'：
	    退出（0）;
	默认值：
	    用法（0）;
	}
    }
    / *检查我们是否以root身份运行* /
    if（geteuid（）！= 0）{
	fprintf（stderr，“必须是root \ n ”）;
	退出（1）;
    }

    / *从终端分离* /
    if（daemonize）{
	if（fork（）！= 0）
	    退出（0）;
	/ *关闭stdin，stdout和stderr ... * /
	/ *   close（0）; * /
	关闭（1）;
	关闭（2）;
	setsid（）;
    }
    / *确保我们在退出时清理... * /
    atexit（（void *）＆cleanup）;

    / *初始化数据结构和服务...... * /
    rt_table_init（）;
    log_init（）;
    / *    packet_queue_init（）; * /
    host_init（ifname）;
    / *    packet_input_init（）; * /
    nl_init（）;
    nl_send_conf_msg（）;
    aodv_socket_init（）;
＃IFDEF LLFEEDBACK
    if（llfeedback）{
	llf_init（）;
    }
＃ENDIF

    / *设置插座供观看...... * /
    FD_ZERO（和读者）;
    for（i = 0 ; i <nr_callbacks; i ++）{
	FD_SET（回调[i] .fd，和读者）;
	if（callbacks [i] .fd > = nfds）
	    nfds = callbacks [i]。fd + 1 ;
    }

    / *设置重启计时器的等待... * /
    if（wait_on_reboot）{
	timer_init（＆worb_timer，wait_on_reboot_timeout，＆wait_on_reboot）;
	timer_set_timeout（＆worb_timer，DELETE_PERIOD）;
	alog（LOG_NOTICE，0，__ FUNCTION__，
	     “等待重启时％d毫秒。禁用\” - D \“。”，
	     DELETE_PERIOD）;
    }

    / *安排第一个Hello * /
    if（！optimized_hellos &&！llfeedback）
	hello_start（）;

    if（rt_log_interval）
	log_rt_table_init（）;

    而（1）{
	memcpy（（char *）＆rfds，（char *）＆readers，sizeof（rfds））;

	timeout = timer_age_queue（）;
	
	timeout_spec。tv_sec = timeout-> tv_sec ;
	timeout_spec。tv_nsec = timeout-> tv_usec * 1000 ;

	if（（n = pselect（nfds，＆rfds，NULL，NULL，＆timeout_spec，＆origmask））< 0）{
	    if（错误！= EINTR）
		alog（LOG_WARNING，errno，__ FUNCTION__，
		     “选择失败（主循环）” ;
	    继续 ;
	}

	if（n> 0）{
	    for（i = 0 ; i <nr_callbacks; i ++）{
		如果（FD_ISSET（回调[i]中。FD，＆RFDS））{
		    / *我们在执行时不需要任何计时器SIGALRM
		       回调函数，因此我们阻止计时器... * /
		    （* callbacks [i] .func）（callbacks [i] .fd）;
		}
	    }
	}
    }				 / *主循环* /
    返回 0 ;
}

静态 空 清理（void）
{
    DEBUG（LOG_DEBUG，0，“清理！”）;
    rt_table_destroy（）;
    aodv_socket_cleanup（）;
＃IFDEF LLFEEDBACK
    if（llfeedback）
	llf_cleanup（）;
＃ENDIF
    log_cleanup（）;
    nl_cleanup（）;
    remove_modules（）;
}
