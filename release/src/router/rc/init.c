/*

	Copyright 2005, Broadcom Corporation
	All Rights Reserved.

	THIS SOFTWARE IS OFFERED "AS IS", AND BROADCOM GRANTS NO WARRANTIES OF ANY
	KIND, EXPRESS OR IMPLIED, BY STATUTE, COMMUNICATION OR OTHERWISE. BROADCOM
	SPECIFICALLY DISCLAIMS ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS
	FOR A SPECIFIC PURPOSE OR NONINFRINGEMENT CONCERNING THIS SOFTWARE.

*/


#include "rc.h"

#include <ctype.h>
#include <termios.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <time.h>
#include <errno.h>
#include <paths.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <sys/klog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <wlutils.h>
#include <bcmdevs.h>
#if defined(TCONFIG_BLINK) || defined(TCONFIG_BCMARM) /* RT-N+ */
#include <bcmparams.h>
#endif

#ifndef WL_BSS_INFO_VERSION
#error WL_BSS_INFO_VERSION
#endif

#define SHELL "/bin/sh"

/* needed by logmsg() */
#define LOGMSG_DISABLE	DISABLE_SYSLOG_OSM
#define LOGMSG_NVDEBUG	"init_debug"

int restore_defaults_fb = 0;


static int fatalsigs[] = {
	SIGILL,
	SIGABRT,
	SIGFPE,
	SIGPIPE,
	SIGBUS,
	SIGSYS,
	SIGTRAP,
	SIGPWR
};

static int initsigs[] = {
	SIGHUP,
	SIGUSR1,
	SIGUSR2,
	SIGINT,
	SIGQUIT,
	SIGALRM,
	SIGTERM
};

static char *defenv[] = {
	"TERM=vt100",
	"HOME=/",
	"PATH=/usr/bin:/bin:/usr/sbin:/sbin",
	"SHELL=" SHELL,
	"USER=root",
	NULL
};

static void restore_defaults(void)
{
	int restore_defaults = 0;
#if defined(TCONFIG_BLINK) || defined(TCONFIG_BCMARM) /* RT-N+ */
	struct sysinfo info;
#endif

	/* Restore defaults if told to or OS has changed */
	if (!restore_defaults)
		restore_defaults = !nvram_match("restore_defaults", "0");

	if (restore_defaults)
		fprintf(stderr, "\n## Restoring defaults... ##\n");

	restore_defaults_fb = restore_defaults;

	/* Restore defaults if necessary */
	eval("nvram", "defaults", "--initcheck");

	nvram_set("os_name", "linux");
	nvram_set("os_version", tomato_version);
	nvram_set("os_date", tomato_buildtime);

#if defined(TCONFIG_BLINK) || defined(TCONFIG_BCMARM) /* RT-N+ */
	/* Adjust et and wl thresh value after reset (for wifi-driver and et_linux.c) */
	if (restore_defaults) {
		memset(&info, 0, sizeof(struct sysinfo));
		sysinfo(&info);
		if (info.totalram <= (TOMATO_RAM_LOW_END * 1024)) { /* Router with less than 50 MB RAM */
			/* Set to 512 as long as onboard memory <= 50 MB RAM */
			nvram_set("wl_txq_thresh", "512");
			nvram_set("et_txq_thresh", "512");
#ifdef TCONFIG_USBAP
			nvram_set("wl_rpcq_rxthresh", "512");
#endif

		}
		else if (info.totalram <= (TOMATO_RAM_MID_END * 1024)) { /* Router with less than 100 MB RAM */
			nvram_set("wl_txq_thresh", "1024");
			nvram_set("et_txq_thresh", "1536");
#ifdef TCONFIG_USBAP
			nvram_set("wl_rpcq_rxthresh", "1024");
#endif
		}
		else { /* Router with more than 100 MB RAM */
			nvram_set("wl_txq_thresh", "1024");
			nvram_set("et_txq_thresh", "3300");
#ifdef TCONFIG_USBAP
			nvram_set("wl_rpcq_rxthresh", "1024");
#endif
		}
	}
#endif /* TCONFIG_BLINK || TCONFIG_BCMARM */
}

#ifdef TCONFIG_BLINK /* RT-N/RTAC */
/* set pci/#/#/ccode,regrev, wl#_country_code,regrev */
static inline void set_regulation(int card, char *code, char *rev)
{
	char path[32];
	snprintf(path, sizeof(path), "pci/%d/1/regrev", card + 1);
	nvram_set(path, rev);
	snprintf(path, sizeof(path), "pci/%d/1/ccode", card + 1);
	nvram_set(path, code);
	snprintf(path, sizeof(path), "wl%d_country_rev", card);
	nvram_set(path, rev);
	snprintf(path, sizeof(path), "wl%d_country_code", card);
	nvram_set(path, code);
	if (!card) {
		nvram_set("wl_country_rev", rev);
		nvram_set("wl_country_code", code);
	}
}

static void set_extra_param(struct nvram_tuple *t, char *str)
{
	char buf[256];

	while (t->name) {
		snprintf(buf, sizeof(buf), str, t->name);
		nvram_set(buf, t->value);
		t++;
	}
}
#endif /* TCONFIG_BLINK */

/* Set terminal settings to reasonable defaults */
static void set_term(int fd)
{
	struct termios tty;

	tcgetattr(fd, &tty);

	/* set control chars */
	tty.c_cc[VINTR]  = 3;	/* C-c */
	tty.c_cc[VQUIT]  = 28;	/* C-\ */
	tty.c_cc[VERASE] = 127; /* C-? */
	tty.c_cc[VKILL]  = 21;	/* C-u */
	tty.c_cc[VEOF]   = 4;	/* C-d */
	tty.c_cc[VSTART] = 17;	/* C-q */
	tty.c_cc[VSTOP]  = 19;	/* C-s */
	tty.c_cc[VSUSP]  = 26;	/* C-z */

	/* use line dicipline 0 */
	tty.c_line = 0;

	/* Make it be sane */
	tty.c_cflag &= CBAUD|CBAUDEX|CSIZE|CSTOPB|PARENB|PARODD;
	tty.c_cflag |= CREAD|HUPCL|CLOCAL;


	/* input modes */
	tty.c_iflag = ICRNL | IXON | IXOFF;

	/* output modes */
	tty.c_oflag = OPOST | ONLCR;

	/* local modes */
	tty.c_lflag =
		ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE | IEXTEN;

	tcsetattr(fd, TCSANOW, &tty);
}

static int console_init(void)
{
	int fd;

	/* Clean up */
	ioctl(0, TIOCNOTTY, 0);
	close(0);
	close(1);
	close(2);
	setsid();

	/* Reopen console */
	if ((fd = open(_PATH_CONSOLE, O_RDWR)) < 0) {
		/* Avoid debug messages is redirected to socket packet if no exist a UART chip */
		open("/dev/null", O_RDONLY);
		open("/dev/null", O_WRONLY);
		open("/dev/null", O_WRONLY);
		perror(_PATH_CONSOLE);
		return errno;
	}
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);

	ioctl(0, TIOCSCTTY, 1);
	tcsetpgrp(0, getpgrp());
	set_term(0);

	return 0;
}

/*
 * Waits for a file descriptor to change status or unblocked signal
 * @param	fd	file descriptor
 * @param	timeout	seconds to wait before timing out or 0 for no timeout
 * @return	1 if descriptor changed status or 0 if timed out or -1 on error
 */
static int waitfor(int fd, int timeout)
{
	fd_set rfds;
	struct timeval tv = { timeout, 0 };

	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);

	return select(fd + 1, &rfds, NULL, NULL, (timeout > 0) ? &tv : NULL);
}

static pid_t run_shell(int timeout, int nowait)
{
	pid_t pid;
	int sig;

	/* Wait for user input */
	if (waitfor(STDIN_FILENO, timeout) <= 0)
		return 0;

	switch (pid = fork()) {
	case -1:
		perror("fork");
		return 0;
	case 0:
		/* Reset signal handlers set for parent process */
		for (sig = 0; sig < (_NSIG-1); sig++)
			signal(sig, SIG_DFL);

		/* Reopen console */
		console_init();
		printf("\n\nFreshTomato %s\n\n", tomato_version);

		/* Now run it. The new program will take over this PID,
		 * so nothing further in init.c should be run. */
		execve(SHELL, (char *[]) { SHELL, NULL }, defenv);

		/* We're still here? Some error happened. */
		perror(SHELL);
		exit(errno);
	default:
		if (nowait) {
			return pid;
		}
		else {
			waitpid(pid, NULL, 0);
			return 0;
		}
	}
}

int console_main(int argc, char *argv[])
{
	for (;;) run_shell(0, 0);

	return 0;
}

static void shutdn(int rb)
{
	unsigned int i;
	int act;
	sigset_t ss;

	logmsg(LOG_DEBUG, "*** %s: shutdn rb=%d", __FUNCTION__, rb);

	sigemptyset(&ss);
	for (i = 0; i < sizeof(fatalsigs) / sizeof(fatalsigs[0]); i++)
		sigaddset(&ss, fatalsigs[i]);
	for (i = 0; i < sizeof(initsigs) / sizeof(initsigs[0]); i++)
		sigaddset(&ss, initsigs[i]);
	sigprocmask(SIG_BLOCK, &ss, NULL);

	for (i = 30; i > 0; --i) {
		if (((act = check_action()) == ACT_IDLE) || (act == ACT_REBOOT))
			break;

		logmsg(LOG_DEBUG, "*** %s: busy with %d. Waiting before shutdown... %d", __FUNCTION__, act, i);
		sleep(1);
	}
	set_action(ACT_REBOOT);

	/* Disconnect pppd - need this for PPTP/L2TP to finish gracefully */
	killall("xl2tpd", SIGTERM);
	killall("pppd", SIGTERM);

	logmsg(LOG_DEBUG, "*** %s: TERM", __FUNCTION__);
	kill(-1, SIGTERM);
	sleep(3);
	sync();

	logmsg(LOG_DEBUG, "*** %s: KILL", __FUNCTION__);
	kill(-1, SIGKILL);
	sleep(1);
	sync();

	eval("umount", "-f", "/jffs");
	sleep(1);

	if (rb != -1) {
		led(LED_WLAN, LED_OFF);
		if (rb == 0) {
			for (i = 4; i > 0; --i) {
				led(LED_DMZ, LED_ON);
				led(LED_WHITE, LED_ON);
				usleep(250000);
				led(LED_DMZ, LED_OFF);
				led(LED_WHITE, LED_OFF);
				usleep(250000);
			}
		}
	}

	reboot(rb ? RB_AUTOBOOT : RB_HALT_SYSTEM);

	do {
		sleep(1);
	} while (1);
}

static void handle_fatalsigs(int sig)
{
	logmsg(LOG_DEBUG, "*** %s: fatal sig=%d", __FUNCTION__, sig);
	shutdn(-1);
}

/* Fixed the race condition & incorrect code by using sigwait()
 * instead of pause(). But SIGCHLD is a problem, since other
 * code: 1) messes with it and 2) depends on CHLD being caught so
 * that the pid gets immediately reaped instead of left a zombie.
 * Pidof still shows the pid, even though it's in zombie state.
 * So this SIGCHLD handler reaps and then signals the mainline by
 * raising ALRM.
 */
static void handle_reap(int sig)
{
	chld_reap(sig);
	raise(SIGALRM);
}

static int check_nv(const char *name, const char *value)
{
	const char *p;
	if (!nvram_match("manual_boot_nv", "1")) {
		if (((p = nvram_get(name)) == NULL) || (strcmp(p, value) != 0)) {
			logmsg(LOG_DEBUG, "*** %s: error: critical variable %s is invalid. Resetting", __FUNCTION__, name);
			nvram_set(name, value);
			return 1;
		}
	}

	return 0;
}

#ifndef TCONFIG_BCMARM
static int invalid_mac(const char *mac)
{
	if ((!mac) || (!(*mac)) || (strncasecmp(mac, "00:90:4c", 8) == 0))
		return 1;

	int i = 0, s = 0;
	while (*mac) {
		if (isxdigit(*mac)) {
			i++;
		}
		else if (*mac == ':') {
			if ((i == 0) || (i / 2 - 1 != s))
				break;
			++s;
		}
		else {
			s = -1;
		}
		++mac;
	}

	return !(i == 12 && s == 5);
}

static int get_mac_from_mt0(unsigned long address)
{
	FILE *fp;
	char m[6], s[18];

	snprintf(s, sizeof(s), MTD_DEV(%dro), 0);
	if ((fp = fopen(s, "rb"))) {
		fseek(fp, address, SEEK_SET);
		fread(m, sizeof(m), 1, fp);
		fclose(fp);
		snprintf(s, sizeof(s), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);

		if (!invalid_mac(s)) {
			nvram_set("et0macaddr", s);
			return 0;
		}
	}
	else {
		return -(errno);
	}

	return -EINVAL;
}

static int find_dir320_mac_addr(void)
{
	FILE *fp;
	char *buffer, s[18];
	int i, part, size, found = 0;

	if (!mtd_getinfo("board_data", &part, &size))
		goto out;
	snprintf(s, sizeof(s), MTD_DEV(%dro), part);

	if ((fp = fopen(s, "rb"))) {
		buffer = malloc(size);
		memset(buffer, 0, size);
		fread(buffer, size, 1, fp);
		if (!memcmp(buffer, "RGCFG1", 6)) {
			for (i = 6; i < size - 24; i++) {
				if (!memcmp(buffer + i, "lanmac=", 7)) {
					memcpy(s, buffer + i + 7, 17);
					s[17] = 0;
					nvram_set("et0macaddr", s);
					found = 1;
				}
				else if (!memcmp(buffer + i, "wanmac=", 7)) {
					memcpy(s, buffer + i + 7, 17);
					s[17] = 0;
					nvram_set("il0macaddr", s);
					if (!found) {
						inc_mac(s, -1);
						nvram_set("et0macaddr", s);
					}
					found = 1;
				}
			}
		}
		free(buffer);
		fclose(fp);
	}
out:
	if (!found) {
		strlcpy(s, nvram_safe_get("wl0_hwaddr"), sizeof(s));
		inc_mac(s, -2);
		nvram_set("et0macaddr", s);
	}
	return 1;
}
#endif /* !TCONFIG_BCMARM */

static int init_vlan_ports(void)
{
	int dirty = 0;
	int model = get_model();

#if defined(TCONFIG_BLINK) || defined(TCONFIG_BCMARM) /* RT-N+ */
	char vlanports[] = "vlanXXXXports";
	char vlanhw[] = "vlanXXXXhwname";
	char vlanvid[] = "vlanXXXXvid";
	char nvvalue[8] = { 0 };
	int num;
	const char *ports, *hwname, *vid;

	/* FreshTomato: check and prepare nvram VLAN values before we start (vlan mapping) */
	for (num = 0; num < TOMATO_VLANNUM; num ++) {
		/* get vlan infos from nvram */
		snprintf(vlanports, sizeof(vlanports), "vlan%dports", num);
		snprintf(vlanhw, sizeof(vlanhw), "vlan%dhwname", num);
		snprintf(vlanvid, sizeof(vlanvid), "vlan%dvid", num);

		hwname = nvram_get(vlanhw);
		ports = nvram_get(vlanports);

		/* check if we use vlanX */
		if ((hwname && strlen(hwname)) || (ports && strlen(ports))) {
			vid = nvram_get(vlanvid);
			if ((vid == NULL) || (vid && !strlen(vid))) { /* create nvram vlanXvid if missing, we need it! (default ex. Vlan 4 --> Vid 4) */
				snprintf(nvvalue, sizeof(nvvalue), "%d", num);
				nvram_set(vlanvid, nvvalue);
			}
		}
	}
#endif /* TCONFIG_BLINK || TCONFIG_BCMARM */

	switch (model) {
#if !defined(TCONFIG_BLINK) && !defined(TCONFIG_BCMARM) /* RT only */
	case MODEL_RTN12:
		dirty |= check_nv("vlan0ports", "3 2 1 0 5*"); /* L1 L2 L3 L4 CPU */
		dirty |= check_nv("vlan1ports", "4 5"); /* WAN CPU */
		break;
#endif/* !TCONFIG_BLINK && !TCONFIG_BCMARM */
	case MODEL_WRT54G:
		switch (check_hw_type()) {
		case HW_BCM5352E: /* G v4, GS v3, v4 */
			dirty |= check_nv("vlan0ports", "3 2 1 0 5*");
			break;
		}
		break;
	case MODEL_WTR54GS:
		dirty |= check_nv("vlan0ports", "0 5*");
		dirty |= check_nv("vlan1ports", "1 5");
		dirty |= check_nv("vlan_enable", "1");
		break;
	case MODEL_WL500GP:
	case MODEL_WL500GE:
	case MODEL_WL500GPv2:
	case MODEL_WL520GU:
	case MODEL_WL330GE:
		if (nvram_match("vlan1ports", "0 5u")) /* 520GU or 330GE or WL500GE? */
			dirty |= check_nv("vlan1ports", "0 5");
		else if (nvram_match("vlan1ports", "4 5u"))
			dirty |= check_nv("vlan1ports", "4 5");
		break;
	case MODEL_WL500GD:
		dirty |= check_nv("vlan0ports", "1 2 3 4 5*");
		dirty |= check_nv("vlan1ports", "0 5");
		break;
	case MODEL_DIR320:
	case MODEL_H618B:
		dirty |= (nvram_get("vlan2ports") != NULL);
		nvram_unset("vlan2ports");
		dirty |= check_nv("vlan0ports", "1 2 3 4 5*");
		dirty |= check_nv("vlan1ports", "0 5");
		break;
	case MODEL_WRT310Nv1:
		dirty |= check_nv("vlan1ports", "1 2 3 4 8*");
		dirty |= check_nv("vlan2ports", "0 8");
		break;
	case MODEL_WL1600GL:
		dirty |= check_nv("vlan0ports", "0 1 2 3 5*");
		dirty |= check_nv("vlan1ports", "4 5");
		break;
	case MODEL_RTN10:
		dirty |= check_nv("vlan1ports", "4 5");
		break;
	case MODEL_WNR3500L:
	case MODEL_WRT320N:
#ifdef TCONFIG_BLINK /* RTN/RTAC */
	case MODEL_WNR3500LV2:
	case MODEL_RTN66U:
#endif
	case MODEL_RTN16:
		dirty |= check_nv("vlan1ports", "4 3 2 1 8*");
		dirty |= check_nv("vlan2ports", "0 8");
		break;
	case MODEL_WNR2000v2:
#ifdef TCONFIG_BLINK /* RTN/RTAC */
		dirty |= check_nv("vlan1ports", "4 3 2 1 5*");
		dirty |= check_nv("vlan2ports", "0 5");
#else /* to be checked: why? */
		dirty |= check_nv("vlan0ports", "4 3 2 1 5*");
		dirty |= check_nv("vlan1ports", "0 5");
#endif
		break;
	case MODEL_WRT610Nv2:
	case MODEL_F5D8235v3:
		dirty |= check_nv("vlan1ports", "1 2 3 4 8*");
		dirty |= check_nv("vlan2ports", "0 8");
		break;
	case MODEL_F7D3301:
	case MODEL_F7D4301:
#ifdef TCONFIG_BLINK /* RTN/RTAC */
	case MODEL_WNDR3700v3:
	case MODEL_WNDR4000:
#endif /* TCONFIG_BLINK */
		dirty |= check_nv("vlan1ports", "3 2 1 0 8*");
		dirty |= check_nv("vlan2ports", "4 8");
		break;
#ifdef TCONFIG_BLINK /* RTN/RTAC */
	case MODEL_E900:
	case MODEL_E1500:
	case MODEL_E1550:
	case MODEL_E2500:
	case MODEL_DIR620C1:
#endif /* TCONFIG_BLINK */
	case MODEL_F7D3302:
	case MODEL_F7D4302:
		dirty |= check_nv("vlan1ports", "0 1 2 3 5*");
		dirty |= check_nv("vlan2ports", "4 5");
		break;
#ifdef TCONFIG_BLINK /* RTN/RTAC */
	case MODEL_RTN15U:
	case MODEL_E3200:
#endif /* TCONFIG_BLINK */
	case MODEL_E4200:
		dirty |= check_nv("vlan1ports", "0 1 2 3 8*");
		dirty |= check_nv("vlan2ports", "4 8");
		break;
	case MODEL_WRT160Nv3:
		if (nvram_match("vlan1ports", "1 2 3 4 5*")) {
			/* fix lan port numbering on CSE41, CSE51 */
			dirty |= check_nv("vlan1ports", "4 3 2 1 5*");
		}
		else if (nvram_match("vlan1ports", "1 2 3 4 8*")) {
			/* WRT310Nv2 ? */
			dirty |= check_nv("vlan1ports", "4 3 2 1 8*");
		}
		break;

#ifdef TCONFIG_BLINK /* RTN/RTAC */
	case MODEL_EA6500V1:
		dirty |= check_nv("vlan1ports", "0 1 2 3 8*");
		dirty |= check_nv("vlan2ports", "4 8");
		dirty |= check_nv("boot_wait", "on");
		dirty |= check_nv("wait_time", "5");
		break;
	case MODEL_DIR865L:
	case MODEL_W1800R:
		dirty |= check_nv("vlan1ports", "1 2 3 4 8*");
		dirty |= check_nv("vlan2ports", "0 8");
		dirty |= check_nv("boot_wait", "on");
		dirty |= check_nv("wait_time", "5");
		break;
	case MODEL_TDN80:
		dirty |= check_nv("vlan1ports", "0 1 2 3 8*");
		dirty |= check_nv("vlan2ports", "4 8");
		dirty |= check_nv("boot_wait", "on");
		dirty |= check_nv("wait_time", "5");
		break;
	case MODEL_R6300V1:
	case MODEL_WNDR4500:
	case MODEL_WNDR4500V2:
		dirty |= check_nv("vlan1ports", "0 1 2 3 8*");
		dirty |= check_nv("vlan2ports", "4 8");
		/* must flash it through tftp */
		dirty |= check_nv("boot_wait", "on");
		dirty |= check_nv("wait_time", "5");
		break;
	case MODEL_D1800H:
		dirty |= check_nv("vlan1ports", "1 2 3 4 8*");
		dirty |= check_nv("vlan2ports", "0 8");
		dirty |= check_nv("ledbh0", "11");
		dirty |= check_nv("ledbh1", "11");
		dirty |= check_nv("ledbh2", "11");
		dirty |= check_nv("ledbh11", "136");
		/* must flash it through tftp */
		dirty |= check_nv("boot_wait", "on");
		dirty |= check_nv("wait_time", "5");
		break;
	case MODEL_RTN53:
	case MODEL_F9K1102:
		dirty |= check_nv("vlan2ports", "0 1 2 3 5*");
		dirty |= check_nv("vlan1ports", "4 5");
		break;
	case MODEL_RTN53A1:
		dirty |= check_nv("vlan1ports", "4 5");
		dirty |= check_nv("vlan2ports", "3 2 1 0 5*");
		break;
	case MODEL_HG320:
	case MODEL_H218N:
		dirty |= check_nv("vlan1ports", "1 2 3 4 5*");
		dirty |= check_nv("vlan2ports", "0 5");
		break;
	case MODEL_RG200E_CA:
		dirty |= check_nv("vlan1ports", "4 3 2 1 5*");
		dirty |= check_nv("vlan2ports", "0 5");
		break;
	case MODEL_RTN10U:
	case MODEL_CW5358U:
		dirty |= check_nv("vlan0ports", "1 2 3 4 5*");
		dirty |= check_nv("vlan1ports", "0 5");
		break;
	case MODEL_RTN10P:
	case MODEL_RTN12A1:
	case MODEL_RTN12B1:
	case MODEL_RTN12C1:
	case MODEL_RTN12D1:
	case MODEL_RTN12VP:
	case MODEL_RTN12K:
	case MODEL_RTN12HP:
		dirty |= check_nv("vlan0ports", "3 2 1 0 5*"); /* L1 L2 L3 L4 CPU */
		dirty |= check_nv("vlan1ports", "4 5"); /* WAN CPU */
		break;
	case MODEL_WNDR3400:
	case MODEL_WNDR3400v2:
	case MODEL_WNDR3400v3:
		/* Note port order is important (or reversed display, if "0 1 2 3 5*" used for vlan1ports) -> doesn't work, invert in Web GUI */
		dirty |= check_nv("vlan1ports", "0 1 2 3 5*");
		/* And change "4 5u" to "4 5" to make WAN port work */
		dirty |= check_nv("vlan2ports", "4 5");
		break;
	case MODEL_TDN60:
		dirty |= check_nv("vlan1ports", "1 2 3 4 8*");
		dirty |= check_nv("vlan2ports", "0 8");
		dirty |= check_nv("boot_wait", "on");
		dirty |= check_nv("wait_time", "5");
		break;
	case MODEL_TDN6:
		dirty |= check_nv("vlan1ports", "1 2 3 4 5*");
		dirty |= check_nv("vlan2ports", "0 5");
		dirty |= check_nv("boot_wait", "on");
		dirty |= check_nv("wait_time", "5");
		break;
	case MODEL_E1000v2:
	case MODEL_L600N:
		dirty |= check_nv("vlan1ports", "1 2 3 4 5*");
		dirty |= check_nv("vlan2ports", "0 5");
		break;
#endif /* TCONFIG_BLINK */

	} /* switch (model) */

	return dirty;
}

static void check_bootnv(void)
{
	int dirty;
	int model;
#ifndef TCONFIG_BCMARM
	int hardware;
	char mac[18];
#endif

	model = get_model();
	dirty = check_nv("wl0_leddc", "0x640000") | check_nv("wl1_leddc", "0x640000");

	switch (model) {
	case MODEL_WTR54GS:
		dirty |= check_nv("vlan0hwname", "et0");
		dirty |= check_nv("vlan1hwname", "et0");
		break;
	case MODEL_WBRG54:
		dirty |= check_nv("wl0gpio0", "130");
		break;
	case MODEL_WR850GV1:
	case MODEL_WR850GV2:
		/* need to cleanup some variables... */
		if ((nvram_get("t_model") == NULL) && (nvram_get("MyFirmwareVersion") != NULL)) {
			nvram_unset("MyFirmwareVersion");
			nvram_set("restore_defaults", "1");
		}
		break;
	case MODEL_WL330GE:
		dirty |= check_nv("wl0gpio1", "0x02");
		break;
	case MODEL_WL500W:
		/* fix WL500W mac adresses for WAN port */
		if (invalid_mac(nvram_get("et1macaddr"))) {
			strlcpy(mac, nvram_safe_get("et0macaddr"), sizeof(mac));
			inc_mac(mac, 1);
			dirty |= check_nv("et1macaddr", mac);
		}
		dirty |= check_nv("wl0gpio0", "0x88");
		break;
	case MODEL_WL500GP:
		dirty |= check_nv("sdram_init", "0x0009"); /* 32MB; defaults: 0x000b, 0x0009 */
		dirty |= check_nv("wl0gpio0", "136");
		break;
	case MODEL_WL500GPv2:
	case MODEL_WL520GU:
		dirty |= check_nv("wl0gpio1", "136");
		break;
	case MODEL_WL500GD:
		dirty |= check_nv("vlan0hwname", "et0");
		dirty |= check_nv("vlan1hwname", "et0");
		dirty |= check_nv("boardflags", "0x00000100"); /* set BFL_ENETVLAN */
		nvram_unset("wl0gpio0");
		break;
	case MODEL_DIR320:
		if ((strlen(nvram_safe_get("et0macaddr")) == 12) || (strlen(nvram_safe_get("il0macaddr")) == 12)) {
			dirty |= find_dir320_mac_addr();
		}
		if (nvram_get("vlan2hwname") != NULL) {
			nvram_unset("vlan2hwname");
			dirty = 1;
		}
		dirty |= check_nv("wandevs", "vlan1");
		dirty |= check_nv("vlan1hwname", "et0");
		dirty |= check_nv("wl0gpio0", "8");
		dirty |= check_nv("wl0gpio1", "0");
		dirty |= check_nv("wl0gpio2", "0");
		dirty |= check_nv("wl0gpio3", "0");
	case MODEL_WL1600GL:
		if (invalid_mac(nvram_get("et0macaddr")))
			dirty |= (get_mac_from_mt0(0x1ffa0) == 0);
		break;
	case MODEL_WRT160Nv1:
	case MODEL_WRT310Nv1:
	case MODEL_WRT300N:
		dirty |= check_nv("wl0gpio0", "8");
		break;
	case MODEL_WNR3500L:
		dirty |= check_nv("boardflags", "0x00000710"); /* needed to enable USB */
		dirty |= check_nv("vlan2hwname", "et0");
		dirty |= check_nv("ledbh0", "7");
		break;
	case MODEL_WNR2000v2:
		dirty |= check_nv("ledbh5", "8");
		break;
	case MODEL_WRT320N:
		dirty |= check_nv("reset_gpio", "5");
		dirty |= check_nv("ledbh0", "136");
		dirty |= check_nv("ledbh1", "11");
		/* fall through, same as RT-N16 */
	case MODEL_RTN16:
		dirty |= check_nv("vlan2hwname", "et0");
		break;
	case MODEL_WRT610Nv2:
		dirty |= check_nv("vlan2hwname", "et0");
		dirty |= check_nv("pci/1/1/ledbh2", "8");
		dirty |= check_nv("sb/1/ledbh1", "8");
		if (invalid_mac(nvram_get("pci/1/1/macaddr"))) {
			strlcpy(mac, nvram_safe_get("et0macaddr"), sizeof(mac));
			inc_mac(mac, 3);
			dirty |= check_nv("pci/1/1/macaddr", mac);
		}
		break;
	case MODEL_F7D3301:
	case MODEL_F7D3302:
	case MODEL_F7D4301:
	case MODEL_F7D4302:
	case MODEL_F5D8235v3:
		if (nvram_match("sb/1/macaddr", nvram_safe_get("et0macaddr"))) {
			strlcpy(mac, nvram_safe_get("et0macaddr"), sizeof(mac));
			inc_mac(mac, 2);
			dirty |= check_nv("sb/1/macaddr", mac);
			inc_mac(mac, 1);
			dirty |= check_nv("pci/1/1/macaddr", mac);
		}
	case MODEL_E4200:
		dirty |= check_nv("vlan2hwname", "et0");
		if (strncasecmp(nvram_safe_get("pci/1/1/macaddr"), "00:90:4c", 8) == 0 ||
		    strncasecmp(nvram_safe_get("sb/1/macaddr"), "00:90:4c", 8) == 0) {
			strlcpy(mac, nvram_safe_get("et0macaddr"), sizeof(mac));
			inc_mac(mac, +2);
			dirty |= check_nv("sb/1/macaddr", mac);
			inc_mac(mac, +4);
			dirty |= check_nv("pci/1/1/macaddr", mac);
		}
		break;
	case MODEL_WRT160Nv3:
#ifdef TCONFIG_BLINK /* RTN/RTAC */
	case MODEL_E900:
	case MODEL_E1000v2:
	case MODEL_E1500:
	case MODEL_E1550:
	case MODEL_E2500:
	case MODEL_E3200:
	case MODEL_L600N:
	case MODEL_DIR620C1:
#endif
		dirty |= check_nv("vlan2hwname", "et0");
		break;
	case MODEL_WRT54G:
	if (strncmp(nvram_safe_get("pmon_ver"), "CFE", 3) != 0)
		return;

		hardware = check_hw_type();
		if (!nvram_get("boardtype") ||
		    !nvram_get("boardnum") ||
		    !nvram_get("boardflags") ||
		    !nvram_get("clkfreq") ||
		    !nvram_get("os_flash_addr") ||
		    !nvram_get("dl_ram_addr") ||
		    !nvram_get("os_ram_addr") ||
		    !nvram_get("scratch") ||
		    !nvram_get("et0macaddr") ||
		    ((hardware != HW_BCM4704_BCM5325F) && (!nvram_get("vlan0ports") || !nvram_get("vlan0hwname")))) {
			logmsg(LOG_DEBUG, "*** %s: unable to find critical settings, erasing NVRAM", __FUNCTION__);
			mtd_erase("nvram");
			goto REBOOT;
		}

		dirty |= check_nv("aa0", "3");
		dirty |= check_nv("wl0gpio0", "136");
		dirty |= check_nv("wl0gpio2", "0");
		dirty |= check_nv("wl0gpio3", "0");
		dirty |= check_nv("cctl", "0");
		dirty |= check_nv("ccode", "0");

		/* fix WL mac for 2.4 GHz, was always 00:90:4C:5F:00:2A after upgrade/nvram full erase (for every router) */
		if (strncasecmp(nvram_safe_get("il0macaddr"), "00:90:4c", 8) == 0 ||
		    strncasecmp(nvram_safe_get("wl0_hwaddr"), "00:90:4c", 8) == 0) {
			strlcpy(mac, nvram_safe_get("et0macaddr"), sizeof(mac));
			inc_mac(mac, +2);
			dirty |= check_nv("il0macaddr", mac);
			dirty |= check_nv("wl0_hwaddr", mac);
		}

		switch (hardware) {
		case HW_BCM5325E:
			/* Lower the DDR ram drive strength , the value will be stable for all boards
			 * Latency 3 is more stable for all ddr
			 */
			dirty |= check_nv("sdram_init", "0x010b");
			dirty |= check_nv("sdram_config", "0x0062");
			if (!nvram_match("debug_clkfix", "0"))
				dirty |= check_nv("clkfreq", "216");
			if (dirty)
				nvram_set("sdram_ncdl", "0x0");

			dirty |= check_nv("pa0itssit", "62");
			dirty |= check_nv("pa0b0", "0x15eb");
			dirty |= check_nv("pa0b1", "0xfa82");
			dirty |= check_nv("pa0b2", "0xfe66");
			//dirty |= check_nv("pa0maxpwr", "0x4e");
			break;
		case HW_BCM5352E: /* G v4, GS v3, v4 */
			dirty |= check_nv("sdram_init", "0x010b");
			dirty |= check_nv("sdram_config", "0x0062");
			if (dirty)
				nvram_set("sdram_ncdl", "0x0");

			dirty |= check_nv("pa0itssit", "62");
			dirty |= check_nv("pa0b0", "0x168b");
			dirty |= check_nv("pa0b1", "0xfabf");
			dirty |= check_nv("pa0b2", "0xfeaf");
			//dirty |= check_nv("pa0maxpwr", "0x4e");
			break;
		case HW_BCM5354G:
			dirty |= check_nv("pa0itssit", "62");
			dirty |= check_nv("pa0b0", "0x1326");
			dirty |= check_nv("pa0b1", "0xFB51");
			dirty |= check_nv("pa0b2", "0xFE87");
			//dirty |= check_nv("pa0maxpwr", "0x4e");
			break;
		case HW_BCM4704_BCM5325F:
			/* nothing to do */
			break;
		default:
			dirty |= check_nv("pa0itssit", "62");
			dirty |= check_nv("pa0b0", "0x170c");
			dirty |= check_nv("pa0b1", "0xfa24");
			dirty |= check_nv("pa0b2", "0xfe70");
			//dirty |= check_nv("pa0maxpwr", "0x48");
			break;
		}
		break;

#ifdef TCONFIG_BLINK /* RTN/RTAC */
	case MODEL_H618B:
		dirty |= check_nv("wandevs", "vlan1");
		dirty |= check_nv("vlan0hwname", "et0");
		dirty |= check_nv("vlan1hwname", "et0");
		break;
	case MODEL_WNR3500LV2:
		dirty |= check_nv("vlan2hwname", "et0");
		break;
	case MODEL_HG320:
	case MODEL_RG200E_CA:
	case MODEL_H218N:
		dirty |= check_nv("vlan1hwname", "et0");
		dirty |= check_nv("vlan2hwname", "et0");
		dirty |= check_nv("boardflags", "0x710"); /* set BFL_ENETVLAN, enable VLAN */
		dirty |= check_nv("reset_gpio", "30");
		break;
	case MODEL_F9K1102:
		if (nvram_match("sb/1/macaddr", nvram_safe_get("et0macaddr"))) {
			strlcpy(mac, nvram_safe_get("et0macaddr"), sizeof(mac));
			inc_mac(mac, 2);
			dirty |= check_nv("sb/1/macaddr", mac);
			inc_mac(mac, 1);
			dirty |= check_nv("wl0_hwaddr", mac);
#ifdef TCONFIG_USBAP
			inc_mac(mac, 1);
			dirty |= check_nv("wl1_hwaddr", mac);
			dirty |= check_nv("0:macaddr", mac);
#endif
		}
		break;
	case MODEL_WNDR4000:
	case MODEL_WNDR3700v3:
		/* Have to check MAC addresses, specific configuration needed: 
		 * Part of MAC information is in CFE, the rest in board_data (which easily gets broken when playing with firmware ... :-))
		 * Note that after a clean (30/30/30) reset, addresses are "broken" ... but the code below fixes them, tied to et0macaddr!
		 * Also, CFE will update what it sees based on NVRAM ... 
		 * so after 30/30/30 reset it sees different values than after a full Tomato boot (that fixes these, updating NVRAM)
		 * Use this approach for all WNDR routers (here, and below)
		 */
		dirty |= check_nv("vlan2hwname", "et0");
		strlcpy(mac, nvram_safe_get("et0macaddr"), sizeof(mac));
		//inc_mac(mac, 2);
		dirty |= check_nv("sb/1/macaddr", mac);
		inc_mac(mac, -1);
		dirty |= check_nv("pci/1/1/macaddr", mac);
		break;
	case MODEL_WNDR3400:
	case MODEL_WNDR3400v2:
	case MODEL_WNDR3400v3:
		dirty |= check_nv("vlan2hwname", "et0");
		strlcpy(mac, nvram_safe_get("et0macaddr"), sizeof(mac));
		//inc_mac(mac, 2);
		dirty |= check_nv("sb/1/macaddr", mac);
		inc_mac(mac, -1);
		if (model == MODEL_WNDR3400)
			dirty |= check_nv("pci/1/1/macaddr", mac);
		else
			dirty |= check_nv("wl1_hwaddr", mac);
		/* Have to check wl ifname(s) ... if not set before eth config, 5 GHz radio does not come up properly */
		dirty |= check_nv("wl0_ifname", "eth0");
		dirty |= check_nv("wl1_ifname", "eth1");
		break;
	case MODEL_R6300V1:
	case MODEL_WNDR4500:
	case MODEL_WNDR4500V2:
		dirty |= check_nv("vlan1hwname", "et0");
		dirty |= check_nv("vlan2hwname", "et0");
		strlcpy(mac, nvram_safe_get("et0macaddr"), sizeof(mac));
		unsigned int i;
		for (i = 0; i < strlen(mac); i ++) {
			if (mac[i] =='-') mac[i] = ':';
			mac[i] = toupper(mac[i]);
		}
		nvram_set("et0macaddr",mac);
		inc_mac(mac, 3);
		dirty |= check_nv("pci/1/1/macaddr", mac);
		inc_mac(mac, 2);
		dirty |= check_nv("pci/2/1/macaddr", mac);
		nvram_unset("vlan0hwname");
		break;
	case MODEL_EA6500V1:
		dirty |= check_nv("vlan2hwname", "et0");
		if (strncasecmp(nvram_safe_get("pci/2/1/macaddr"), "00:90:4c", 8) == 0) {
			strlcpy(mac, nvram_safe_get("et0macaddr"), sizeof(mac));
			inc_mac(mac, 3);
			dirty |= check_nv("pci/2/1/macaddr", mac);
		}
		break;
#endif /* TCONFIG_BLINK */

	} /* switch (model) */

	dirty |= init_vlan_ports();

	if (dirty) {
		nvram_commit();

REBOOT: /* do a simple reboot */
		sync();
		logmsg(LOG_DEBUG, "*** %s: Reboot after check NV params / set VLANS...", __FUNCTION__);
		reboot(RB_AUTOBOOT);
		exit(0);
	}
}

static int init_nvram(void)
{
	unsigned long bf;
	unsigned long n;
	int model;
	char s[256];
	unsigned long features = 0;
	const char *mfr = "Broadcom";
	const char *name = NULL;
	const char *ver = NULL;

	model = get_model();
	snprintf(s, sizeof(s), "%d", model);
	nvram_set("t_model", s);

	switch (model) {
	case MODEL_WRT54G:
		mfr = "Linksys";
		name = "WRT54G/GS/GL";
		if (!nvram_match("t_fix1", (char *)name)) {
			/* fix MAC addresses */
			strlcpy(s, nvram_safe_get("et0macaddr"), sizeof(s));	/* get et0 MAC address for LAN */
			inc_mac(s, +2);						/* MAC + 1 will be for WAN */
			nvram_set("il0macaddr", s);				/* fix WL mac for 2.4 GHz, was always 00:90:4C:5F:00:2A after upgrade/nvram full erase (for every router) */
		}
		switch (check_hw_type()) {
		case HW_BCM4712:
			nvram_set("gpio2", "adm_eecs");
			nvram_set("gpio3", "adm_eesk");
			nvram_unset("gpio4");
			nvram_set("gpio5", "adm_eedi");
			nvram_set("gpio6", "adm_rc");
			break;
		case HW_BCM4702:
			nvram_unset("gpio2");
			nvram_unset("gpio3");
			nvram_unset("gpio4");
			nvram_unset("gpio5");
			nvram_unset("gpio6");
			break;
		case HW_BCM5352E:
			nvram_set("opo", "0x0008");
			nvram_set("ag0", "0x02");
			/* fall through */
		default:
			nvram_set("gpio2", "ses_led");
			nvram_set("gpio3", "ses_led2");
			nvram_set("gpio4", "ses_button");
			features = SUP_SES | SUP_WHAM_LED;
			break;
		}
		break;
	case MODEL_WTR54GS:
		mfr = "Linksys";
		name = "WTR54GS";
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan0 eth1");
			nvram_set("gpio2", "ses_button");
			nvram_set("reset_gpio", "7");
		}
		nvram_set("pa0itssit", "62");
		nvram_set("pa0b0", "0x1542");
		nvram_set("pa0b1", "0xfacb");
		nvram_set("pa0b2", "0xfec7");
		/* nvram_set("pa0maxpwr", "0x4c"); */
#ifdef TCONFIG_BLINK /* RTN/RTAC */
		gpio_write(1 << 2, 1);
#endif
		features = SUP_SES;
		break;
	case MODEL_WRTSL54GS:
		mfr = "Linksys";
		name = "WRTSL54GS";
		features = SUP_SES | SUP_WHAM_LED;
		break;
	case MODEL_WHRG54S:
		mfr = "Buffalo";
		name = "WHR-G54S";
		features = SUP_SES | SUP_AOSS_LED | SUP_BRAU;
		break;
	case MODEL_WHRHPG54:
	case MODEL_WZRRSG54HP:
	case MODEL_WZRHPG54:
		mfr = "Buffalo";
		features = SUP_SES | SUP_AOSS_LED | SUP_HPAMP;
		switch (model) {
		case MODEL_WZRRSG54HP:
			name = "WZR-RS-G54HP";
			break;
		case MODEL_WZRHPG54:
			name = "WZR-HP-G54";
			break;
		default:
			name = "WHR-HP-G54";
			features = SUP_SES | SUP_AOSS_LED | SUP_BRAU | SUP_HPAMP;
			break;
		}

		bf = strtoul(nvram_safe_get("boardflags"), NULL, 0);
		switch (bf) {
		case 0x0758:
		case 0x1758:
		case 0x2758:
		case 0x3758:
			if (nvram_is_empty("wlx_hpamp") || nvram_match("wlx_hpamp", "")) {
				if (nvram_get_int("wl_txpwr") > 10)
					nvram_set("wl_txpwr", "10");
				nvram_set("wlx_hpamp", "1");
				nvram_set("wlx_hperx", "0");
			}

			n = bf;
			if (nvram_match("wlx_hpamp", "0")) {
				n &= ~0x2000UL;
			}
			else {
				n |= 0x2000UL;
			}
			if (nvram_match("wlx_hperx", "0")) {
				n |= 0x1000UL;
			}
			else {
				n &= ~0x1000UL;
			}
			if (bf != n) {
				snprintf(s, sizeof(s), "0x%lX", n);
				nvram_set("boardflags", s);
			}
			break;
		default:
			logmsg(LOG_WARNING, "unexpected: boardflag=%lX", bf);
			break;
		}
		break;
	case MODEL_WBRG54:
		mfr = "Buffalo";
		name = "WBR-G54";
		break;
	case MODEL_WBR2G54:
		mfr = "Buffalo";
		name = "WBR2-G54";
		features = SUP_SES | SUP_AOSS_LED;
		break;
	case MODEL_WHR2A54G54:
		mfr = "Buffalo";
		name = "WHR2-A54G54";
		features = SUP_SES | SUP_AOSS_LED | SUP_BRAU;
		break;
	case MODEL_WHR3AG54:
		mfr = "Buffalo";
		name = "WHR3-AG54";
		features = SUP_SES | SUP_AOSS_LED;
		break;
	case MODEL_WZRG54:
		mfr = "Buffalo";
		name = "WZR-G54";
		features = SUP_SES | SUP_AOSS_LED;
		break;
	case MODEL_WZRRSG54:
		mfr = "Buffalo";
		name = "WZR-RS-G54";
		features = SUP_SES | SUP_AOSS_LED;
		break;
	case MODEL_WVRG54NF:
		mfr = "Buffalo";
		name = "WVR-G54-NF";
		features = SUP_SES;
		break;
	case MODEL_WZRG108:
		mfr = "Buffalo";
		name = "WZR-G108";
		features = SUP_SES | SUP_AOSS_LED;
		break;
	case MODEL_RT390W:
		mfr = "Fuji";
		name = "RT390W";
		break;
	case MODEL_WR850GV1:
		mfr = "Motorola";
		name = "WR850G v1";
		features = SUP_NONVE;
		break;
	case MODEL_WR850GV2:
		mfr = "Motorola";
		name = "WR850G v2/v3";
		features = SUP_NONVE;
		break;
	case MODEL_WL500GP:
		mfr = "Asus";
		name = "WL-500gP";
		features = SUP_SES;
#ifdef TCONFIG_USB
		nvram_set("usb_ohci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan0 eth1 eth2 eth3"); /* set to "vlan0 eth2" by DD-WRT; default: vlan0 eth1 */
		}
		break;
	case MODEL_WL500W:
		mfr = "Asus";
		name = "WL-500W";
		features = SUP_SES | SUP_80211N;
#ifdef TCONFIG_USB
		nvram_set("usb_ohci", "-1");
#endif
		break;
	case MODEL_WL500GE:
		mfr = "Asus";
		name = "WL-550gE";
		/* features = ? */
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		break;
	case MODEL_WX6615GT:
		mfr = "SparkLAN";
		name = "WX-6615GT";
		/* features = ? */
		break;
	case MODEL_MN700:
		mfr = "Microsoft";
		name = "MN-700";
		break;
	case MODEL_WR100:
		mfr = "Viewsonic";
		name = "WR100";
		break;
	case MODEL_WLA2G54L:
		mfr = "Buffalo";
		name = "WLA2-G54L";
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan0 eth1 eth2");
			nvram_set("wl_ifname", "eth1");
			nvram_set("wan_ifname", "none");
		}
		break;
	case MODEL_TM2300:
		mfr = "Dell";
		name = "TrueMobile 2300";
		break;
	case MODEL_WZRG300N:
		mfr = "Buffalo";
		name = "WZR-G300N";
		features = SUP_SES | SUP_AOSS_LED | SUP_BRAU | SUP_80211N;
		break;
	case MODEL_WRT160Nv1:
	case MODEL_WRT300N:
		mfr = "Linksys";
		name = (model == MODEL_WRT300N) ? "WRT300N v1" : "WRT160N v1";
		features = SUP_SES | SUP_80211N;
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("wan_ifnameX", "eth1");
			nvram_set("lan_ifnames", "eth0 eth2");
		}
		break;
	case MODEL_WRT310Nv1:
		mfr = "Linksys";
		name = "WRT310N v1";
		features = SUP_SES | SUP_80211N | SUP_WHAM_LED | SUP_1000ET;
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifname", "eth1");
		}
		break;

	/*
	 * add here MIPS models if WL_BSS_INFO_VERSION < 108
	 */

#if WL_BSS_INFO_VERSION >= 108

	case MODEL_WHRG125:
		mfr = "Buffalo";
		name = "WHR-G125";
		features = SUP_SES | SUP_AOSS_LED | SUP_BRAU;
		nvram_set("opo", "0x0008");
		nvram_set("ag0", "0x0C");
		break;
	case MODEL_RTN10:
#ifdef TCONFIG_BLINK /* RTN/RTAC */
	case MODEL_RTN10P:
#endif
		mfr = "Asus";
#ifdef TCONFIG_BLINK /* RTN/RTAC */
		name = nvram_match("boardrev", "0x1153") ? "RT-N10P" : "RT-N10";
#else
		name = "RT-N10";
#endif
		features = SUP_SES | SUP_80211N;
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan0 eth1");
			nvram_set("wan_ifnameX", "vlan1");
			nvram_set("wl_ifname", "eth1");
		}
		break;
#ifdef TCONFIG_BLINK /* RTN/RTAC */
	case MODEL_RTN12A1:
#else
	case MODEL_RTN12:
#endif
		mfr = "Asus";
#ifdef TCONFIG_BLINK /* RTN/RTAC */
		name = "RT-N12 A1";
#else
		name = "RT-N12";
#endif
		features = SUP_SES | SUP_BRAU | SUP_80211N;
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan0 eth1");
			nvram_set("wan_ifnameX", "vlan1");
			nvram_set("wl_ifname", "eth1");
		}
		break;
	case MODEL_RTN16:
		mfr = "Asus";
		name = "RT-N16";
		features = SUP_SES | SUP_80211N | SUP_1000ET;
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifname", "eth1");
			nvram_set("vlan_enable", "1");
		}
		break;
	case MODEL_WNR3500L:
		mfr = "Netgear";
		name = "WNR3500L/U/v2";
		features = SUP_SES | SUP_AOSS_LED | SUP_80211N | SUP_1000ET;
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("sromrev", "3");
			nvram_set("lan_ifnames", "vlan1 eth1");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifname", "eth1");
		}
		break;
	case MODEL_WNR2000v2:
		mfr = "Netgear";
		name = "WNR2000 v2";
		features = SUP_SES | SUP_AOSS_LED | SUP_80211N;
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan0 eth1");
			nvram_set("wan_ifnameX", "vlan1");
			nvram_set("wl_ifname", "eth1");
		}
		break;
	case MODEL_F7D3301:
	case MODEL_F7D3302:
	case MODEL_F7D4301:
	case MODEL_F7D4302:
	case MODEL_F5D8235v3:
		mfr = "Belkin";
		features = SUP_SES | SUP_80211N;
		switch (model) {
		case MODEL_F7D3301:
			name = "Share Max N300 (F7D3301/F7D7301) v1";
			break;
		case MODEL_F7D3302:
			name = "Share N300 (F7D3302/F7D7302) v1";
			break;
		case MODEL_F7D4301:
			name = "Play Max / N600 HD (F7D4301/F7D8301) v1";
			break;
		case MODEL_F7D4302:
			name = "Play N600 (F7D4302/F7D8302) v1";
			break;
		case MODEL_F5D8235v3:
			name = "N F5D8235-4 v3";
			break;
		}
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1 eth2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("landevs", "vlan1 wl0 wl1");
			nvram_set("wandevs", "vlan2");
		}
		break;
#ifdef TCONFIG_BLINK /* RTN/RTAC */
	case MODEL_E1000v2:
#endif
	case MODEL_WRT160Nv3:
		/* same as M10, M20, WRT310Nv2, E1000v1 */
		mfr = "Linksys";
		name = nvram_safe_get("boot_hw_model");
#ifdef TCONFIG_BLINK /* RTN/RTAC */
		ver = nvram_safe_get("boot_hw_ver");
		if (nvram_match("boot_hw_model", "E100")) {
			name = "E1000";
		}
		if (nvram_match("boot_hw_model", "M10") || nvram_match("boot_hw_model", "M20")) {
			mfr = "Cisco";
		}
#else
		if (strcmp(name, "E100") == 0)
			name = "E1000 v1";
		if (strcmp(name, "WRT310N") == 0)
			name = "WRT310N v2";
#endif
		features = SUP_SES | SUP_80211N | SUP_WHAM_LED;
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifname", "eth1");
		}
		break;
	case MODEL_WRT320N:
		mfr = "Linksys";
		name = nvram_match("boardrev", "0x1307") ? "E2000" : "WRT320N";
		features = SUP_SES | SUP_80211N | SUP_WHAM_LED | SUP_1000ET;
		if (!nvram_match("t_fix1", (char *)name)) {
			if (nvram_match("boardrev", "0x1307")) { /* to be check if WRT320N also need this */
				nvram_set("lan_invert", "1");
			}
			nvram_set("lan_ifnames", "vlan1 eth1");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifname", "eth1");
		}
		break;
	case MODEL_WRT610Nv2:
		mfr = "Linksys";
		name = nvram_match("boot_hw_model", "E300") ? "E3000" : "WRT610N v2";
		features = SUP_SES | SUP_80211N | SUP_WHAM_LED | SUP_1000ET;
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1 eth2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifname", "eth1");
		}
		break;
	case MODEL_E4200:
		mfr = "Linksys";
		name = "E4200 v1";
		features = SUP_SES | SUP_80211N | SUP_1000ET;
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1 eth2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifname", "eth1");

#ifdef TCONFIG_BLINK /* RTN/RTAC */
			/* fix MAC addresses */
			strlcpy(s, nvram_safe_get("et0macaddr"), sizeof(s));	/* get et0 MAC address for LAN */
			inc_mac(s, +2);						/* MAC + 1 will be for WAN */
			nvram_set("sb/1/macaddr", s);				/* fix WL mac for 2,4G */
			inc_mac(s, +4);						/* do not overlap with VIFs */
			nvram_set("pci/1/1/macaddr", s);			/* fix WL mac for 5G */

			struct nvram_tuple e4200_pci_1_1_params[] = {
				/* power settings for 2,4 and 5 GHz WiFi */
				{"pa2gw0a0", "0xfe8c", 0},
				{"pa2gw1a0", "0x1b20", 0},
				{"pa2gw2a0", "0xf98c", 0},
				{"pa2gw0a1", "0xfe98", 0},
				{"pa2gw1a1", "0x19ae", 0},
				{"pa2gw2a1", "0xf9ab", 0},

				{"pa5gw0a0", "0xfe52", 0},
				{"pa5gw1a0", "0x163e", 0},
				{"pa5gw2a0", "0xfa59", 0},
				{"pa5gw0a1", "0xfe63", 0},
				{"pa5gw1a1", "0x1584", 0},
				{"pa5gw2a1", "0xfa92", 0},
				{"pa5gw0a2", "0xfe7c", 0},
				{"pa5gw1a2", "0x1720", 0},
				{"pa5gw2a2", "0xfa4a", 0},

				{"pa5ghw0a0", "0xfe6a", 0},
				{"pa5ghw1a0", "0x163c", 0},
				{"pa5ghw2a0", "0xfa69", 0},
				{"pa5ghw0a1", "0xfe67", 0},
				{"pa5ghw1a1", "0x160e", 0},
				{"pa5ghw2a1", "0xfa6a", 0},
				{"pa5ghw0a2", "0xfe76", 0},
				{"pa5ghw1a2", "0x1766", 0},
				{"pa5ghw2a2", "0xfa2c", 0},

				{"pa05gidx", "5", 0},
				{"pa05glidx", "0", 0},
				{"pa05ghidx", "7", 0},
				{"pa15gidx", "0", 0},
				{"pa15glidx", "0", 0},
				{"pa15ghidx", "3", 0},
				{"pa25gidx", "5", 0},
				{"pa25glidx", "0", 0},
				{"pa25ghidx", "9", 0},

				/* adjust cfe wifi country settings part1 */
				{"ccode", "ALL", 0},
				{"regrev", "0", 0 },
				{0, 0, 0}
			};

			set_extra_param(e4200_pci_1_1_params, "pci/1/1/%s");

			/* adjust cfe wifi country settings part2 */
			nvram_set("sb/1/ccode", "ALL");
			nvram_set("sb/1/regrev", "0");

			/* default wifi settings/channels */
			nvram_set("wl0_country_code", "SG");
			nvram_set("wl0_channel", "6");
			nvram_set("wl0_nbw", "40");
			nvram_set("wl0_nbw_cap", "1");
			nvram_set("wl0_nctrlsb", "upper");
			nvram_set("wl1_country_code", "SG");
			nvram_set("wl1_channel", "36");
			nvram_set("wl1_nbw", "40");
			nvram_set("wl1_nbw_cap", "1");
			nvram_set("wl1_nctrlsb", "lower");
#endif /* TCONFIG_BLINK */
		}
		break;
	case MODEL_WL330GE:
		mfr = "Asus";
		name = "WL-330gE";
		/* The 330gE has only one wired port which can act either as WAN or LAN.
		 * Failsafe mode is to have it start as a LAN port so you can get an IP
		 * address via DHCP and access the router config page.
		 */
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("wl_ifname", "eth1");
			nvram_set("lan_ifnames", "eth1");
			nvram_set("wan_ifnameX", "eth0");
			nvram_set("wan_islan", "1");
			nvram_set("wan_proto", "disabled");
		}
		break;
	case MODEL_WL500GPv2:
		mfr = "Asus";
		name = "WL-500gP v2";
		features = SUP_SES;
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		break;
	case MODEL_WL520GU:
		mfr = "Asus";
		name = "WL-520GU";
		features = SUP_SES;
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		break;
	case MODEL_WL500GD:
		mfr = "Asus";
		name = "WL-500g Deluxe";
		/* features = SUP_SES; */
#ifdef TCONFIG_USB
		nvram_set("usb_ohci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("wl_ifname", "eth1");
			nvram_set("lan_ifnames", "vlan0 eth1");
			nvram_set("wan_ifnameX", "vlan1");
			nvram_unset("wl0gpio0");
		}
		break;
	case MODEL_DIR320:
		mfr = "D-Link";
		name = "DIR-320";
		features = SUP_SES;
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("wan_ifnameX", "vlan1");
			nvram_set("wl_ifname", "eth1");
		}
		break;
	case MODEL_H618B:
		mfr = "ZTE";
		name = "ZXV10 H618B";
		features = SUP_SES | SUP_AOSS_LED;
#ifdef TCONFIG_BLINK /* RTN/RTAC */
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan0 eth1");
			nvram_set("wan_ifname", "vlan1");
			nvram_set("wan_ifnames", "vlan1");
			nvram_set("wan_ifnameX", "vlan1");
			nvram_set("wl_ifname", "eth1");
		}
#endif /* TCONFIG_BLINK */
		break;
	case MODEL_WL1600GL:
		mfr = "Ovislink";
		name = "WL1600GL";
		features = SUP_SES;
		break;


#ifdef TCONFIG_BLINK /* RTN/RTAC */

	case MODEL_RTN10U:
		mfr = "Asus";
		name = "RT-N10U";
		features = SUP_SES | SUP_80211N;
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan0 eth1");
			nvram_set("wan_ifnameX", "vlan1");
			nvram_set("wl_ifname", "eth1");
		}
		break;
	case MODEL_L600N:
		mfr = "Rosewill";
		name = "L600N";
		features = SUP_SES | SUP_80211N;
		if (!nvram_match("t_fix1", (char *)name)) {
#ifdef TCONFIG_USBAP
			nvram_set("wl1_hwaddr", nvram_safe_get("0:macaddr"));
			nvram_set("ehciirqt", "3");
			nvram_set("qtdc_pid", "48407");
			nvram_set("qtdc_vid", "2652");
			nvram_set("qtdc0_ep", "4");
			nvram_set("qtdc0_sz", "0");
			nvram_set("qtdc1_ep", "18");
			nvram_set("qtdc1_sz", "10");
			nvram_set("lan_ifnames", "vlan1 eth1 eth2");
			nvram_set("landevs", "vlan1 wl0 wl1");
			nvram_set("wl0_ifname", "wl0");
			nvram_set("wl1_ifname", "wl1");
#else
			nvram_set("lan_ifnames", "vlan1 eth1");
			nvram_set("landevs", "vlan1 wl0");
#endif
			nvram_set("wl_ifname", "eth1");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wandevs", "vlan2");
		}
		break;
	case MODEL_DIR620C1:
		mfr = "D-Link";
		name = "Dir-620 C1";
		features = SUP_SES | SUP_80211N;
		if (!nvram_match("t_fix1", (char *)name)) {
#ifdef TCONFIG_USBAP
			nvram_set("lan_ifnames", "vlan1 eth1 eth2");
			nvram_set("landevs", "vlan1 wl0 wl1");
			nvram_set("wl0_ifname", "eth1");
			nvram_set("wl1_ifname", "eth2");
#else
			nvram_set("lan_ifnames", "vlan1 eth1");
			nvram_set("landevs", "vlan1 wl0");
#endif
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifname", "eth1");

		}
		break;
	case MODEL_CW5358U:
		mfr = "Catchtech";
		name = "CW-5358U";
		features = SUP_SES | SUP_80211N;
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1");
			nvram_set("wan_ifname", "vlan2");
			nvram_set("wan_ifnames", "vlan2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifname", "eth1");
		}
		break;
	case MODEL_HG320:
		mfr = "FiberHome";
		name = "HG320";
		features = SUP_SES | SUP_80211N;
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1");
			nvram_set("wan_ifname", "vlan2");
			nvram_set("wan_ifnames", "vlan2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifname", "eth1");
		}
		break;
	case MODEL_RG200E_CA:
		mfr = "ChinaNet";
		name = "RG200E-CA";
		features = SUP_SES | SUP_80211N;
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1");
			nvram_set("wan_ifname", "vlan2");
			nvram_set("wan_ifnames", "vlan2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifname", "eth1");
		}
		break;
	case MODEL_H218N:
		mfr = "ZTE";
		name = "H218N";
		features = SUP_SES | SUP_80211N;
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1");
			nvram_set("wan_ifname", "vlan2");
			nvram_set("wan_ifnames", "vlan2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifname", "eth1");
		}
		break;
	case MODEL_TDN80:
		mfr = "Tenda";
		name = "N80";
		features = SUP_SES | SUP_80211N | SUP_1000ET | SUP_80211AC;
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1 eth2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wan_ifnames", "vlan2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifnames", "eth1 eth2");
			nvram_set("wl_ifname", "eth1");
			nvram_set("wl0_ifname", "eth2");
			nvram_set("wl1_ifname", "eth1");
			nvram_set("wl0_bw_cap", "7");
			nvram_set("wl0_chanspec", "36/80");
			nvram_set("wl1_bw_cap", "3");
			nvram_set("wl1_chanspec", "1l");
			nvram_set("blink_wl", "1"); /* Enable WLAN LED if wireless interface is enabled, and turn on blink */
			//nvram_set("landevs", "vlan1 wl0 wl1");
			//nvram_set("wandevs", "vlan2");

			/* fix WL mac's */
			nvram_set("wl0_hwaddr", nvram_safe_get("pci/1/1/macaddr"));
			nvram_set("wl1_hwaddr", nvram_safe_get("pci/2/1/macaddr"));

			/* fix ssid according to 5G(eth2) and 2.4G(eth1) */
			nvram_set("wl_ssid", "FreshTomato50");
			nvram_set("wl0_ssid", "FreshTomato50");
			nvram_set("wl1_ssid", "FreshTomato24");
		}
		break;
	case MODEL_TDN60:
		mfr = "Tenda";
		name = "N60";
		features = SUP_SES | SUP_80211N | SUP_1000ET;
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1 eth2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wan_ifnames", "vlan2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifnames", "eth1 eth2");
			nvram_set("wl_ifname", "eth1");
			nvram_set("wl0_ifname", "eth1");
			nvram_set("wl1_ifname", "eth2");
			nvram_set("wl0_bw_cap", "3");
			nvram_set("wl0_chanspec", "1l");
			nvram_set("wl1_bw_cap", "7");
			nvram_set("wl1_chanspec", "36/80");
			nvram_set("blink_wl", "1"); /* Enable WLAN LED if wireless interface is enabled, and turn on blink */
			//nvram_set("landevs", "vlan1 wl0 wl1");
			//nvram_set("wandevs", "vlan2");

			/* fix WL mac`s */
			nvram_set("wl0_hwaddr", nvram_safe_get("sb/1/macaddr"));
			nvram_set("wl1_hwaddr", nvram_safe_get("0:macaddr"));
		}
		break;
	case MODEL_TDN6:
		mfr = "Tenda";
		name = "N6";
		features = SUP_SES | SUP_80211N;
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1 eth2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wan_ifnames", "vlan2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifnames", "eth1 eth2");
			nvram_set("wl_ifname", "eth1");
			nvram_set("wl0_ifname", "eth1");
			nvram_set("wl1_ifname", "eth2");
			nvram_set("wl0_bw_cap", "3");
			nvram_set("wl0_chanspec", "1l");
			nvram_set("wl1_bw_cap", "7");
			nvram_set("wl1_chanspec", "36/80");
			nvram_set("blink_wl", "1"); /* Enable WLAN LED if wireless interface is enabled, and turn on blink */

		/* fix WL mac`s */
		nvram_set("wl0_hwaddr", nvram_safe_get("sb/1/macaddr"));
		nvram_set("wl1_hwaddr", nvram_safe_get("0:macaddr"));
		}
		break;
	case MODEL_RTN12B1:
		mfr = "Asus";
		name = "RT-N12 B1";
		features = SUP_SES | SUP_80211N;
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan0 eth1");
			nvram_set("wan_ifnameX", "vlan1");
			nvram_set("wl_ifname", "eth1");

			nvram_set("sb/1/ledbh5", "2"); /* WL_LED_ACTIVITY; WiFi LED - active HIGH */
		}
		break;
	case MODEL_RTN12C1:
		mfr = "Asus";
		name = "RT-N12 C1";
		features = SUP_SES | SUP_80211N;
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan0 eth1");
			nvram_set("wan_ifnameX", "vlan1");
			nvram_set("wl_ifname", "eth1");

			nvram_set("sb/1/ledbh4", "2");  /* WL_LED_ACTIVITY; WiFi LED - active HIGH */
			nvram_set("sb/1/ledbh5", "11"); /* WL_LED_INACTIVE, not used ... */
			nvram_set("sb/1/ledbh6", "11"); /* WL_LED_INACTIVE, not used ... */
		}
		break;
	case MODEL_RTN12D1:
	case MODEL_RTN12VP:
	case MODEL_RTN12K:
		mfr = "Asus";
		name = "RT-N12 D1";
		features = SUP_SES | SUP_80211N;
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan0 eth1");
			nvram_set("wan_ifnameX", "vlan1");
			nvram_set("wl_ifname", "eth1");

			struct nvram_tuple rtn12_sb_1_params[] = {
				/* WL_LED_ACTIVITY; WiFi LED - active HIGH */
				{"ledbh5", "2", 0},

				/* adjust power settings for 2,4 GHz WiFi (according to / from Asus SRC) */
				{"maxp2ga0", "0x52", 0},
				{"maxp2ga1", "0x52", 0},
				{"cck2gpo", "0x0", 0},
				{"ofdm2gpo0", "0x2000", 0},
				{"ofdm2gpo1", "0x6442", 0},
				{"ofdm2gpo", "0x64422000", 0},
				{"mcs2gpo0", "0x2200", 0},
				{"mcs2gpo1", "0x6644", 0},
				{"mcs2gpo2", "0x2200", 0},
				{"mcs2gpo3", "0x6644", 0},
				{"mcs2gpo4", "0x4422", 0},
				{"mcs2gpo5", "0x8866", 0},
				{"mcs2gpo6", "0x4422", 0},
				{"mcs2gpo7", "0x8866", 0},
				{0, 0, 0}
			};

			set_extra_param(rtn12_sb_1_params, "sb/1/%s");
		}
		break;
	case MODEL_RTN12HP:
		mfr = "Asus";
		name = "RT-N12 HP";
		features = SUP_SES | SUP_80211N;
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan0 eth1");
			nvram_set("wan_ifnameX", "vlan1");
			nvram_set("wl_ifname", "eth1");
			nvram_set("sb/1/ledbh5", "2"); /* WL_LED_ACTIVITY; WiFi LED - active HIGH */
		}
		break;
	case MODEL_RTN15U:
		mfr = "Asus";
		name = "RT-N15U";
		features = SUP_SES | SUP_80211N | SUP_1000ET;
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1");
			nvram_set("wan_iface", "vlan2");
			nvram_set("wan_ifname", "vlan2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wan_ifnames", "vlan2");
			nvram_set("wl_ifname", "eth1");
		}
			nvram_set("blink_wl", "1"); /* Enable WLAN LED if wireless interface is enabled, and turn on blink */
		break;
	case MODEL_RTN53:
	case MODEL_RTN53A1:
		mfr = "Asus";
		name = nvram_match("boardrev", "0x1446") ? "RT-N53 A1" : "RT-N53";
		features = SUP_SES | SUP_80211N;
#ifdef TCONFIG_USBAP
		if (nvram_get_int("usb_storage") == 1)
			nvram_set("usb_storage", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
#ifdef TCONFIG_USBAP
			nvram_set("wl1_hwaddr", nvram_safe_get("0:macaddr"));
			nvram_set("ehciirqt", "3");
			nvram_set("qtdc_pid", "48407");
			nvram_set("qtdc_vid", "2652");
			nvram_set("qtdc0_ep", "4");
			nvram_set("qtdc0_sz", "0");
			nvram_set("qtdc1_ep", "18");
			nvram_set("qtdc1_sz", "10");
			nvram_set("lan_ifnames", "vlan2 eth1 eth2");
			nvram_set("landevs", "vlan2 wl0 wl1");
			nvram_set("wl1_ifname", "eth2");
#else
			nvram_set("lan_ifnames", "vlan2 eth1");
			nvram_set("landevs", "vlan2 wl0");
#endif
			nvram_set("lan_ifname", "br0");
			nvram_set("wl_ifname", "eth1");
			nvram_set("wl0_ifname", "eth1");
			nvram_set("wan_ifnameX", "vlan1");
			nvram_set("wandevs", "vlan1");
			nvram_unset("vlan0ports");
		}
		break;
	case MODEL_RTN66U:
		mfr = "Asus";
#ifdef TCONFIG_AC66U
		name = "RT-AC66U";
		features = SUP_SES | SUP_80211N | SUP_1000ET | SUP_80211AC;
#else
		name = "RT-N66U";
		features = SUP_SES | SUP_80211N | SUP_1000ET;
#ifdef TCONFIG_MICROSD
		if (nvram_get_int("usb_mmc") == -1)
			nvram_set("usb_mmc", "1");
#endif
#endif

#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1 eth2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifnames", "eth1 eth2");
			nvram_set("wl_ifname", "eth1");
			nvram_set("wl0_ifname", "eth1");
			nvram_set("wl1_ifname", "eth2");
			nvram_set("landevs", "vlan1 wl0 wl1");
			nvram_set("wandevs", "vlan2");

			/* fix MAC addresses for N66U and AC66U */
			strlcpy(s, nvram_safe_get("et0macaddr"), sizeof(s));	/* get et0 MAC address for LAN */
			inc_mac(s, +2);
			nvram_set("pci/1/1/macaddr", s);			/* fix WL mac for 2,4G */
			nvram_set("wl0_hwaddr", s);
			inc_mac(s, +4);						/* do not overlap with VIFs */
			nvram_set("pci/2/1/macaddr", s);			/* fix WL mac for 5G */
			nvram_set("wl1_hwaddr", s);

			/* wifi settings/channels */
#ifdef TCONFIG_BCMWL6
			nvram_set("wl0_bw_cap", "3");
			nvram_set("wl0_chanspec", "6u");
#endif
			nvram_set("wl0_channel", "6");
			nvram_set("wl0_nbw", "40");
			nvram_set("wl0_nctrlsb", "upper");
#ifdef TCONFIG_AC66U
			nvram_set("wl1_bw_cap", "7");
			nvram_set("wl1_chanspec", "36/80");
			nvram_set("wl1_channel", "36");
			nvram_set("wl1_nbw", "80");
			nvram_set("wl1_nbw_cap", "3");
			nvram_set("wl1_nctrlsb", "lower");
#else /* SDK5 OR SDK6 for RT-N66U */
#ifdef TCONFIG_BCMWL6
			nvram_set("wl1_bw_cap", "3");
			nvram_set("wl1_chanspec", "36l");
#endif
			nvram_set("wl1_channel", "36");
			nvram_set("wl1_nbw", "40");
			nvram_set("wl1_nbw_cap", "1");
			nvram_set("wl1_nctrlsb", "lower");
#endif /* TCONFIG_AC66U */

#ifndef TCONFIG_AC66U
#ifdef TCONFIG_USB
			nvram_set("usb_noled", "1-1.4"); /* SD/MMC Card */
#endif
#else
			/* wifi country settings SDK6 */
			nvram_set("pci/1/1/regrev", "12");
			nvram_set("pci/2/1/regrev", "12");
			nvram_set("pci/1/1/ccode", "SG");
			nvram_set("pci/2/1/ccode", "SG");

			nvram_set("blink_wl", "1"); /* Enable WLAN LED if wireless interface is enabled, and turn on blink */

			struct nvram_tuple rtn66u_pci_2_1_params[] = {
				/* bcm4360ac_defaults */
				{"aa2g", "0", 0},
				{"aa5g", "7", 0},
				{"aga0", "71", 0},
				{"aga1", "71", 0},
				{"aga2", "71", 0},
				{"agbg0", "133", 0},
				{"agbg1", "133", 0},
				{"agbg2", "133", 0},
				{"antswitch", "0", 0},
				{"cckbw202gpo", "0", 0},
				{"cckbw20ul2gpo", "0", 0},
				{"dot11agofdmhrbw202gpo", "0", 0},
				{"femctrl", "3", 0},
				{"papdcap2g", "0", 0},
				{"tworangetssi2g", "0", 0},
				{"pdgain2g", "4", 0},
				{"epagain2g", "0", 0},
				{"tssiposslope2g", "1", 0},
				{"gainctrlsph", "0", 0},
				{"papdcap5g", "0", 0},
				{"tworangetssi5g", "0", 0},
				{"pdgain5g", "4", 0},
				{"epagain5g", "0", 0},
				{"tssiposslope5g", "1", 0},
				{"maxp2ga0", "76", 0},
				{"maxp2ga1", "76", 0},
				{"maxp2ga2", "76", 0},
				{"mcsbw202gpo", "0", 0},
				{"mcsbw402gpo", "0", 0},
				{"measpower", "0x7f", 0},
				{"measpower1", "0x7f", 0},
				{"measpower2", "0x7f", 0},
				{"noiselvl2ga0", "31", 0},
				{"noiselvl2ga1", "31", 0},
				{"noiselvl2ga2", "31", 0},
				{"noiselvl5gha0", "31", 0},
				{"noiselvl5gha1", "31", 0},
				{"noiselvl5gha2", "31", 0},
				{"noiselvl5gla0", "31", 0},
				{"noiselvl5gla1", "31", 0},
				{"noiselvl5gla2", "31", 0},
				{"noiselvl5gma0", "31", 0},
				{"noiselvl5gma1", "31", 0},
				{"noiselvl5gma2", "31", 0},
				{"noiselvl5gua0", "31", 0},
				{"noiselvl5gua1", "31", 0},
				{"noiselvl5gua2", "31", 0},
				{"ofdmlrbw202gpo", "0", 0},
				{"pa2ga0", "0xfe72,0x14c0,0xfac7", 0},
				{"pa2ga1", "0xfe80,0x1472,0xfabc", 0},
				{"pa2ga2", "0xfe82,0x14bf,0xfad9", 0},
				{"pcieingress_war", "15", 0},
				{"phycal_tempdelta", "255", 0},
				{"rawtempsense", "0x1ff", 0},
				{"rxchain", "7", 0},
				{"rxgainerr2g", "0xffff", 0},
				{"rxgainerr5g", "0xffff,0xffff,0xffff,0xffff", 0},
				{"rxgains2gelnagaina0", "0", 0},
				{"rxgains2gelnagaina1", "0", 0},
				{"rxgains2gelnagaina2", "0", 0},
				{"rxgains2gtrelnabypa0", "0", 0},
				{"rxgains2gtrelnabypa1", "0", 0},
				{"rxgains2gtrelnabypa2", "0", 0},
				{"rxgains2gtrisoa0", "0", 0},
				{"rxgains2gtrisoa1", "0", 0},
				{"rxgains2gtrisoa2", "0", 0},
				{"sar2g", "18", 0},
				{"sar5g", "15", 0},
				{"sromrev", "11", 0},
				{"subband5gver", "0x4", 0},
				{"tempcorrx", "0x3f", 0},
				{"tempoffset", "255", 0},
				{"temps_hysteresis", "15", 0},
				{"temps_period", "15", 0},
				{"tempsense_option", "0x3", 0},
				{"tempsense_slope", "0xff", 0},
				{"tempthresh", "255", 0},
				{"txchain", "7", 0},
				{"ledbh0", "2", 0},
				{"ledbh1", "5", 0},
				{"ledbh2", "4", 0},
				{"ledbh3", "11", 0},
				{"ledbh10", "7",0},
				{0, 0, 0}
			};

			set_extra_param(rtn66u_pci_2_1_params, "pci/2/1/%s");

			/* Only for USA router/version: adjust some wireless default values for US to enable 80 MHz channels */
			if (nvram_match("regulation_domain", "US")) { /* check 2.4 GHz domain only */
				set_regulation(0, "US", "0"); /* 2.4 GHz country and rev setting */
				set_regulation(1, "US", "0"); /* 5 GHz country and rev setting */
				nvram_set("regulation_domain_5G", "US"); /* keep it easy: follow 2.4 GHz wireless */
			}
#endif /* !TCONFIG_AC66U */

		}
		break;

#ifdef CONFIG_BCMWL6
	case MODEL_DIR865L:
		mfr = "D-Link";
		name = "DIR-865L";
		features = SUP_SES | SUP_80211N | SUP_1000ET | SUP_80211AC;
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		/* default nvram setting extracted from original router */
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1 eth2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifnames", "eth1 eth2");
			nvram_set("wl_ifname", "eth1");
			nvram_set("wl0_ifname", "eth1");
			nvram_set("wl1_ifname", "eth2");
			nvram_set("wl0_bw_cap", "7");
			nvram_set("wl0_chanspec", "36/80");
			nvram_set("wl1_bw_cap", "3");
			nvram_set("wl1_chanspec", "1l");
			nvram_set("blink_5g_interface", "eth1");
			//nvram_set("landevs", "vlan1 wl0 wl1");
			//nvram_set("wandevs", "vlan2");

			struct nvram_tuple dir865l_pci_2_1_params[] = {
				/* init wireless power DIR865L defaults */
				/* 2.4 G */
				{"maxp2ga0", "0x64", 0},
				{"maxp2ga1", "0x64", 0},
				{"maxp2ga2", "0x64", 0},
				{"cckbw202gpo", "0x7777", 0},
				{"cckbw20ul2gpo", "0x7777", 0},
				{"legofdmbw202gpo", "0x77777777", 0},
				{"legofdmbw20ul2gpo", "0x77777777", 0},
				{"mcsbw202gpo", "0x77777777", 0},
				{"mcsbw20ul2gpo", "0x77777777", 0},
				{"mcsbw402gpo", "0x99999999", 0},

				{"pa2gw0a0", "0xFE61", 0},
				{"pa2gw1a0", "0x1E65", 0},
				{"pa2gw2a0", "0xF89E", 0},
				{"pa2gw0a1", "0xFE5F", 0},
				{"pa2gw1a1", "0x1DA5", 0},
				{"pa2gw2a1", "0xF8C3", 0},
				{"pa2gw0a2", "0xFE50", 0},
				{"pa2gw1a2", "0x1D68", 0},
				{"pa2gw2a2", "0xF8B7", 0},
				{0, 0, 0}
			};

			struct nvram_tuple dir865l_pci_1_1_params[] = {
				/* 5 G */
				{"devid", "0x43a2", 0},
				{"sromrev", "11", 0},
				{"boardrev", "0x1307", 0},
				{"boardflags", "0x10000000", 0},
				{"boardflags2", "0x300002", 0},
				{"boardtype", "0x621", 0},
				{"boardflags3", "0x300030", 0},
				{"boardnum", "0", 0},
				//{"macaddr", "00:90:4c:d4:00:00", 0},
				//{"ccode", "0", 0},
				//{"regrev", "0", 0},
				{"aa2g", "0", 0},
				{"aa5g", "7", 0},
				{"agbg0", "71", 0},
				{"agbg1", "71", 0},
				{"agbg2", "133", 0},
				{"aga0", "71", 0},
				{"aga1", "133", 0},
				{"aga2", "133", 0},
				{"txchain", "7", 0},
				{"rxchain", "7", 0},
				{"antswitch", "0", 0},
				{"tssiposslope2g", "1", 0},
				{"epagain2g", "0", 0},
				{"pdgain2g", "4", 0},
				{"tworangetssi2g", "0", 0},
				{"papdcap2g", "0", 0},
				{"femctrl", "3", 0},
				{"tssiposslope5g", "1", 0},
				{"epagain5g", "0", 0},
				{"pdgain5g", "4", 0},
				{"tworangetssi5g", "0", 0},
				{"papdcap5g", "0", 0},
				{"gainctrlsph", "0", 0},
				{"tempthresh", "255", 0},
				{"tempoffset", "255", 0},
				{"rawtempsense", "0x1ff", 0},
				{"measpower", "0x7f", 0},
				{"tempsense_slope", "0xff", 0},
				{"tempcorrx", "0x3f", 0},
				{"tempsense_option", "0x3", 0},
				{"phycal_tempdelta", "255", 0},
				{"temps_period", "15", 0},
				{"temps_hysteresis", "15", 0},
				{"measpower1", "0x7f", 0},
				{"measpower2", "0x7f", 0},
				{"pdoffset40ma0", "0", 0},
				{"pdoffset40ma1", "0", 0},
				{"pdoffset40ma2", "0", 0},
				{"pdoffset80ma0", "0", 0},
				{"pdoffset80ma1", "0", 0},
				{"pdoffset80ma2", "0", 0},
				{"subband5gver", "0x4", 0},
				{"cckbw202gpo", "0", 0},
				{"cckbw20ul2gpo", "0", 0},
				{"mcsbw202gpo", "0", 0},
				{"mcsbw402gpo", "0", 0},
				{"dot11agofdmhrbw202gpo", "0", 0},
				{"ofdmlrbw202gpo", "0", 0},
				{"mcsbw205glpo", "572662306", 0},
				{"mcsbw405glpo", "572662306", 0},
				{"mcsbw805glpo", "572662306", 0},
				{"mcsbw1605glpo", "0", 0},
				{"mcsbw205gmpo", "572662306", 0},
				{"mcsbw405gmpo", "572662306", 0},
				{"mcsbw805gmpo", "572662306", 0},
				{"mcsbw1605gmpo", "0", 0},
				{"mcsbw205ghpo", "572662306", 0},
				{"mcsbw405ghpo", "572662306", 0},
				{"mcsbw805ghpo", "572662306", 0},
				{"mcsbw1605ghpo", "0", 0},
				{"mcslr5glpo", "0", 0},
				{"mcslr5gmpo", "0", 0},
				{"mcslr5ghpo", "0", 0},
				{"sb20in40hrrpo", "0", 0},
				{"sb20in80and160hr5glpo", "0", 0},
				{"sb40and80hr5glpo", "0", 0},
				{"sb20in80and160hr5gmpo", "0", 0},
				{"sb40and80hr5gmpo", "0", 0},
				{"sb20in80and160hr5ghpo", "0", 0},
				{"sb40and80hr5ghpo", "0", 0},
				{"sb20in40lrpo", "0", 0},
				{"sb20in80and160lr5glpo", "0", 0},
				{"sb40and80lr5glpo", "0", 0},
				{"sb20in80and160lr5gmpo", "0", 0},
				{"sb40and80lr5gmpo", "0", 0},
				{"sb20in80and160lr5ghpo", "0", 0},
				{"sb40and80lr5ghpo", "0", 0},
				{"dot11agduphrpo", "0", 0},
				{"dot11agduplrpo", "0", 0},
				{"pcieingress_war", "15", 0},
				{"sar2g", "18", 0},
				{"sar5g", "15", 0},
				{"noiselvl2ga0", "31", 0},
				{"noiselvl2ga1", "31", 0},
				{"noiselvl2ga2", "31", 0},
				{"noiselvl5gla0", "31", 0},
				{"noiselvl5gla1", "31", 0},
				{"noiselvl5gla2", "31", 0},
				{"noiselvl5gma0", "31", 0},
				{"noiselvl5gma1", "31", 0},
				{"noiselvl5gma2", "31", 0},
				{"noiselvl5gha0", "31", 0},
				{"noiselvl5gha1", "31", 0},
				{"noiselvl5gha2", "31", 0},
				{"noiselvl5gua0", "31", 0},
				{"noiselvl5gua1", "31", 0},
				{"noiselvl5gua2", "31", 0},
				{"rxgainerr2g", "0xffff", 0},
				{"rxgainerr5g", "0xffff,0xffff,0xffff,0xffff", 0},
				{"maxp2ga0", "76", 0},
				{"pa2ga0", "0xfe72,0x14c0,0xfac7", 0},
				{"rxgains5gmelnagaina0", "2", 0},
				{"rxgains5gmtrisoa0", "5", 0},
				{"rxgains5gmtrelnabypa0", "1", 0},
				{"rxgains5ghelnagaina0", "2", 0},
				{"rxgains5ghtrisoa0", "5", 0},
				{"rxgains5ghtrelnabypa0", "1", 0},
				{"rxgains2gelnagaina0", "0", 0},
				{"rxgains2gtrisoa0", "0", 0},
				{"rxgains2gtrelnabypa0", "0", 0},
				{"rxgains5gelnagaina0", "1", 0},
				{"rxgains5gtrisoa0", "7", 0},
				{"rxgains5gtrelnabypa0", "1", 0},
				{"maxp5ga0", "92,92,92,92", 0},
				{"pa5ga0", "0xff26,0x188e,0xfcf0,0xff2a,0x18ee,0xfcec,0xff21,0x18b4,0xfcec,0xff23,0x1930,0xfcdd", 0},
				{"maxp2ga1", "76", 0},
				{"pa2ga1", "0xfe80,0x1472,0xfabc", 0},
				{"rxgains5gmelnagaina1", "2", 0},
				{"rxgains5gmtrisoa1", "4", 0},
				{"rxgains5gmtrelnabypa1", "1", 0},
				{"rxgains5ghelnagaina1", "2", 0},
				{"rxgains5ghtrisoa1", "4", 0},
				{"rxgains5ghtrelnabypa1", "1", 0},
				{"rxgains2gelnagaina1", "0", 0},
				{"rxgains2gtrisoa1", "0", 0},
				{"rxgains2gtrelnabypa1", "0", 0},
				{"rxgains5gelnagaina1", "1", 0},
				{"rxgains5gtrisoa1", "6", 0},
				{"rxgains5gtrelnabypa1", "1", 0},
				{"maxp5ga1", "92,92,92,92", 0},
				{"pa5ga1", "0xff35,0x1a3c,0xfccc,0xff31,0x1a06,0xfccf,0xff2b,0x1a54,0xfcc5,0xff30,0x1ad5,0xfcb9", 0},
				{"maxp2ga2", "76", 0},
				{"pa2ga2", "0xfe82,0x14bf,0xfad9", 0},
				{"rxgains5gmelnagaina2", "3", 0},
				{"rxgains5gmtrisoa2", "4", 0},
				{"rxgains5gmtrelnabypa2", "1", 0},
				{"rxgains5ghelnagaina2", "3", 0},
				{"rxgains5ghtrisoa2", "4", 0},
				{"rxgains5ghtrelnabypa2", "1", 0},
				{"rxgains2gelnagaina2", "0", 0},
				{"rxgains2gtrisoa2", "0", 0},
				{"rxgains2gtrelnabypa2", "0", 0},
				{"rxgains5gelnagaina2", "1", 0},
				{"rxgains5gtrisoa2", "5", 0},
				{"rxgains5gtrelnabypa2", "1", 0},
				{"maxp5ga2", "92,92,92,92", 0},
				{"pa5ga2", "0xff2e,0x197b,0xfcd8,0xff2d,0x196e,0xfcdc,0xff30,0x1a7d,0xfcc2,0xff2e,0x1ac6,0xfcb4", 0},
				{0, 0, 0}
			};

			set_extra_param(dir865l_pci_1_1_params, "pci/1/1/%s");
			set_extra_param(dir865l_pci_2_1_params, "pci/2/1/%s");

			/* fix WL mac's */
			//strlcpy(s, nvram_safe_get("et0macaddr"), sizeof(s));
			//nvram_set("wl0_hwaddr", nvram_safe_get("0:macaddr"));
			//nvram_set("wl1_hwaddr", nvram_safe_get("1:macaddr"));
			strlcpy(s, nvram_safe_get("et0macaddr"), sizeof(s));
			inc_mac(s, +2);
			nvram_set("wl0_hwaddr", s);
			nvram_set("pci/1/1/macaddr", s);
			inc_mac(s, +1);
			nvram_set("wl1_hwaddr", s);
			nvram_set("pci/2/1/macaddr", s);


			/* fix ssid according to 5G(eth1) and 2.4G(eth2) */
			//nvram_set("wl_ssid", "FreshTomato50");
			nvram_set("wl0_ssid", "FreshTomato50");
			nvram_set("wl1_ssid", "FreshTomato24");
		}
		break; /* DIR-865L */

	case MODEL_W1800R:
		mfr = "Tenda";
		name = "W1800R";
		features = SUP_SES | SUP_80211N | SUP_1000ET | SUP_80211AC;
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1 eth2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifnames", "eth1 eth2");
			nvram_set("wl_ifname", "eth1");
			nvram_set("wl0_ifname", "eth2");
			nvram_set("wl1_ifname", "eth1");
			nvram_set("wl0_bw_cap", "7");
			nvram_set("wl0_chanspec", "36/80");
			nvram_set("wl1_bw_cap", "3");
			nvram_set("wl1_chanspec", "1l");
			nvram_set("blink_wl", "1"); /* Enable WLAN LED if wireless interface is enabled, and turn on blink */
			//nvram_set("landevs", "vlan1 wl0 wl1");
			//nvram_set("wandevs", "vlan2");

			/* fix WL mac's */
			strlcpy(s, nvram_safe_get("et0macaddr"), sizeof(s));
			nvram_set("wl0_hwaddr", nvram_safe_get("0:macaddr"));
			nvram_set("wl1_hwaddr", nvram_safe_get("1:macaddr"));

			/* fix ssid according to 5G(eth2) and 2.4G(eth1) */
			nvram_set("wl_ssid", "FreshTomato50");
			nvram_set("wl0_ssid", "FreshTomato50");
			nvram_set("wl1_ssid", "FreshTomato24");
		}
		break;
	case MODEL_D1800H:
		mfr = "Buffalo";
		if (nvram_match("product", "WLI-H4-D1300")) {
			name = "WLI-H4-D1300";
		}
		else if (nvram_match("product", "WZR-D1100H")) {
			name = "WZR-D1100H";
		}
		else {
			name = "WZR-D1800H";
		}
		features = SUP_SES | SUP_AOSS_LED | SUP_80211N | SUP_1000ET | SUP_80211AC;
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1 eth2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifnames", "eth1 eth2");
			nvram_set("wl_ifname", "eth1");
			nvram_set("wl0_ifname", "eth2");
			nvram_set("wl1_ifname", "eth1");
			nvram_set("wl0_bw_cap", "7");
			nvram_set("wl0_chanspec", "36/80");
			nvram_set("wl1_bw_cap", "3");
			nvram_set("wl1_chanspec", "1l");
			nvram_set("blink_wl", "1"); /* Enable WLAN LED if wireless interface is enabled, and turn on blink */

			/* fix WL mac's */
			strlcpy(s, nvram_safe_get("et0macaddr"), sizeof(s));
			trimstr(s);
			unsigned int i;
			for (i = 0; i < strlen(s); i ++)
				if (s[i] == '-')
					s[i] = ':';
			nvram_set("et0macaddr",s);
			inc_mac(s, +2);
			nvram_set("wl0_hwaddr", s);
			inc_mac(s, +1);
			nvram_set("wl1_hwaddr", s);

			/* fix ssid according to 5G(eth2) and 2.4G(eth1) */
			nvram_set("wl_ssid", "FreshTomato50");
			nvram_set("wl0_ssid", "FreshTomato50");
			nvram_set("wl1_ssid", "FreshTomato24");


			struct nvram_tuple d1800h_pci_2_1_params[] = {
				{"maxp2ga0", "0x70", 0},
				{"maxp2ga1", "0x70", 0},
				{"maxp2ga2", "0x70", 0},
				{"maxp5ga0", "0x6A", 0},
				{"maxp5ga1", "0x6A", 0},
				{"maxp5ga2", "0x6A", 0},
				{"cckbw202gpo", "0x5555", 0},
				{"cckbw20ul2gpo", "0x5555", 0},
				{"legofdmbw202gpo", "0x97555555", 0},
				{"legofdmbw20ul2gpo", "0x97555555", 0},
				{"mcsbw202gpo", "0xDA755555", 0},
				{"mcsbw20ul2gpo", "0xDA755555", 0},
				{"mcsbw402gpo", "0xFC965555", 0},
				{"cckbw205gpo", "0x5555", 0},
				{"cckbw20ul5gpo", "0x5555", 0},
				{"legofdmbw205gpo", "0x97555555", 0},
				{"legofdmbw20ul5gpo", "0x97555555", 0},
				{"legofdmbw205gmpo", "0x77777777", 0},
				{"legofdmbw20ul5gmpo", "0x77777777", 0},
				{"legofdmbw205ghpo", "0x77777777", 0},
				{"legofdmbw20ul5ghpo", "0x77777777", 0},
				{"mcsbw205ghpo", "0x77777777", 0},
				{"mcsbw20ul5ghpo", "0x77777777", 0},
				{"mcsbw205gpo", "0xDA755555", 0},
				{"mcsbw20ul5gpo", "0xDA755555", 0},
				{"mcsbw405gpo", "0xFC965555", 0},
				{"mcsbw405ghpo", "0x77777777", 0},
				{"mcsbw405ghpo", "0x77777777", 0},
				{"mcs32po", "0x7777", 0},
				{"legofdm40duppo", "0x0000", 0},
				{0, 0, 0}
			};

			struct nvram_tuple d1800h_pci_1_1_params[] = {
				{"maxp5ga0", "104,104,104,104", 0},
				{"maxp5ga1", "104,104,104,104", 0},
				{"maxp5ga2", "104,104,104,104", 0},
				{"mcsbw205glpo", "0xBB975311", 0},
				{"mcsbw405glpo", "0xBB975311", 0},
				{"mcsbw805glpo", "0xBB975311", 0},
				{"mcsbw205gmpo", "0xBB975311", 0},
				{"mcsbw405gmpo", "0xBB975311", 0},
				{"mcsbw805gmpo", "0xBB975311", 0},
				{"mcsbw205ghpo", "0xBB975311", 0},
				{"mcsbw405ghpo", "0xBB975311", 0},
				{"mcsbw805ghpo", "0xBB975311", 0},
				{0, 0, 0}
			};

			set_extra_param(d1800h_pci_2_1_params, "pci/2/1/%s");
			set_extra_param(d1800h_pci_1_1_params, "pci/1/1/%s");

			/* force US country for 5G eth1 */
			nvram_set("pci/1/1/ccode", "US");
			nvram_set("wl1_country_code", "US");
			nvram_set("regulation_domain_5G", "US");
		}
		break;
	case MODEL_R6300V1:
		mfr = "Netgear";
		name = "R6300 V1";
		features = SUP_SES | SUP_AOSS_LED | SUP_80211N | SUP_1000ET | SUP_80211AC;
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1 eth2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifnames", "eth1 eth2");
			nvram_set("wl_ifname", "eth1");
			/* 4331, 2.4G */
			nvram_set("wl0_ifname", "eth1");
			nvram_set("wl0_bw_cap", "3");
			nvram_set("wl0_chanspec", "1l");
			nvram_set("wl0_country_code", "US");
			/* 4360, 2.4G/5G */
			nvram_set("wl1_ifname", "eth2");
			nvram_set("wl1_bw_cap", "7");
			nvram_set("wl1_chanspec", "36/80");
			nvram_set("blink_wl", "1"); /* Enable WLAN LED if wireless interface is enabled, and turn on blink */
			nvram_set("landevs", "vlan1 wl0 wl1");
			nvram_set("wandevs", "vlan2");
			nvram_set("lan_invert", "1");

			nvram_set("wl0_hwaddr", nvram_safe_get("pci/1/1/macaddr"));
			nvram_set("wl1_hwaddr", nvram_safe_get("pci/2/1/macaddr"));

			struct nvram_tuple r6300_pci_1_1_params[] = {
				{"aa2g", "7", 0},
				{"ag0", "0", 0},
				{"ag1", "0", 0},
				{"ag2", "0", 0},
				{"antswctl2g", "0", 0},
				{"antswitch", "0", 0},
				{"boardflags", "0x80003200", 0},
				{"boardflags2", "0x4000000", 0},
				{"boardtype", "0x59b", 0},
				{"boardvendor", "0x14e4", 0},
				{"cckbw202gpo", "0x0000", 0},
				{"cckbw20ul2gpo", "0x0000", 0},
				{"devid", "0x4332", 0},
				{"elna2g", "2", 0},
				{"extpagain2g", "3", 0},
				{"ledbh0", "11", 0},
				{"ledbh1", "11", 0},
				{"ledbh12", "11", 0},
				{"ledbh2", "11", 0},
				{"ledbh3", "11", 0},
				{"leddc", "0xFFFF", 0},
				{"legofdm40duppo", "0x0", 0},
				{"legofdmbw202gpo", "0x88000000", 0},
				{"legofdmbw20ul2gpo", "0x88000000", 0},
				{"maxp2ga0", "0x62", 0},
				{"maxp2ga1", "0x62", 0},
				{"maxp2ga2", "0x62", 0},
				{"mcs32po", "0xA", 0},
				{"mcsbw202gpo", "0x88800000", 0},
				{"mcsbw20ul2gpo", "0x88800000", 0},
				{"mcsbw402gpo", "0x0x88800000", 0},
				{"pa2gw0a0", "0xFE56", 0},
				{"pa2gw0a1", "0xFEB3", 0},
				{"pa2gw0a2", "0xFE6A", 0},
				{"pa2gw1a0", "0x1D7C", 0},
				{"pa2gw1a1", "0x1F79", 0},
				{"pa2gw1a2", "0x1D58", 0},
				{"pa2gw2a0", "0xF8A1", 0},
				{"pa2gw2a1", "0xF8BF", 0},
				{"pa2gw2a2", "0xF8DA", 0},
				{"pdetrange2g", "3", 0},
				{"rxchain", "7", 0},
				{"sromrev", "9", 0},
				{"tempoffset", "0", 0},
				{"tempthresh", "120", 0},
				{"triso2g", "3", 0},
				{"tssipos2g", "1", 0},
				{"txchain", "7", 0},
				{"venid", "0x14e4", 0},
				{0, 0, 0}
			};

			struct nvram_tuple r6300_pci_2_1_params[] = {
				{"aa2g", "0", 0},
				{"aa5g", "7", 0},
				{"ag0", "0", 0},
				{"ag1", "0", 0},
				{"ag2", "0", 0},
				{"aga0", "71", 0},
				{"aga1", "133", 0},
				{"aga2", "133", 0},
				{"agbg0", "71", 0},
				{"agbg1", "71", 0},
				{"agbg2", "133", 0},
				{"antswitch", "0", 0},
				{"boardflags", "0x10000000", 0},
				{"boardflags2", "0x300002", 0},
				{"boardflags3", "0x300030", 0},
				{"boardnum", "21059", 0},
				{"boardrev", "0x1307", 0},
				{"boardtype", "0x621", 0},
				{"boardvendor", "0x14e4", 0},
				{"cckbw202gpo", "0", 0},
				{"cckbw20ul2gpo", "0", 0},
				{"devid", "0x43a0", 0},
				{"dot11agduphrpo", "0", 0},
				{"dot11agduplrpo", "0", 0},
				{"dot11agofdmhrbw202gpo", "0", 0},
				{"epagain2g", "0", 0},
				{"epagain5g", "0", 0},
				{"femctrl", "3", 0},
				{"gainctrlsph", "0", 0},
				{"maxp2ga0", "76", 0},
				{"maxp2ga1", "76", 0},
				{"maxp2ga2", "76", 0},
				{"maxp5ga0", "92,96,96,96", 0},
				{"maxp5ga1", "92,96,96,96", 0},
				{"maxp5ga2", "92,96,96,96", 0},
				{"maxp5gb0a0", "0x60", 0},
				{"maxp5gb0a1", "0x60", 0},
				{"maxp5gb0a2", "0x60", 0},
				{"maxp5gb1a0", "0x64", 0},
				{"maxp5gb1a1", "0x64", 0},
				{"maxp5gb1a2", "0x64", 0},
				{"maxp5gb2a0", "0x64", 0},
				{"maxp5gb2a1", "0x64", 0},
				{"maxp5gb2a2", "0x64", 0},
				{"maxp5gb3a0", "0x64", 0},
				{"maxp5gb3a1", "0x64", 0},
				{"maxp5gb3a2", "0x64", 0},
				{"mcsbw1605ghpo", "0", 0},
				{"mcsbw1605glpo", "0", 0},
				{"mcsbw1605gmpo", "0", 0},
				{"mcsbw202gpo", "0", 0},
				{"mcsbw205ghpo", "3429122848", 0},
				{"mcsbw205glpo", "3999687200", 0},
				{"mcsbw205gmpo", "4001780768", 0},
				{"mcsbw402gpo", "0", 0},
				{"mcsbw405ghpo", "3429122848", 0},
				{"mcsbw405glpo", "3999687200", 0},
				{"mcsbw405gmpo", "4001780768", 0},
				{"mcsbw805ghpo", "3429122848", 0},
				{"mcsbw805glpo", "3999687200", 0},
				{"mcsbw805gmpo", "4001780768", 0},
				{"mcslr5ghpo", "0", 0},
				{"mcslr5glpo", "0", 0},
				{"mcslr5gmpo", "0", 0},
				{"measpower", "0x7f", 0},
				{"measpower1", "0x7f", 0},
				{"measpower2", "0x7f", 0},
				{"noiselvl2ga0", "31", 0},
				{"noiselvl2ga1", "31", 0},
				{"noiselvl2ga2", "31", 0},
				{"noiselvl5gha0", "31", 0},
				{"noiselvl5gha1", "31", 0},
				{"noiselvl5gha2", "31", 0},
				{"noiselvl5gla0", "31", 0},
				{"noiselvl5gla1", "31", 0},
				{"noiselvl5gla2", "31", 0},
				{"noiselvl5gma0", "31", 0},
				{"noiselvl5gma1", "31", 0},
				{"noiselvl5gma2", "31", 0},
				{"noiselvl5gua0", "31", 0},
				{"noiselvl5gua1", "31", 0},
				{"noiselvl5gua2", "31", 0},
				{"ofdmlrbw202gpo", "0", 0},
				{"pa2ga0", "0xfe72,0x14c0,0xfac7", 0},
				{"pa2ga1", "0xfe80,0x1472,0xfabc", 0},
				{"pa2ga2", "0xfe82,0x14bf,0xfad9", 0},
				{"pa5ga0", "0xff39,0x1a55,0xfcc7,0xff38,0x1a7f,0xfcc3,0xff33,0x1a66,0xfcc4,0xff36,0x1a7b,0xfcc2", 0},
				{"pa5ga1", "0xff3a,0x1a0b,0xfcd3,0xff38,0x1a37,0xfccd,0xff37,0x1aa1,0xfcc0,0xff37,0x1a6f,0xfcc4", 0},
				{"pa5ga2", "0xff3a,0x1a28,0xfccd,0xff38,0x1a2a,0xfcce,0xff35,0x1a93,0xfcc1,0xff38,0x1aab,0xfcbe", 0},
				{"papdcap2g", "0", 0},
				{"papdcap5g", "0", 0},
				{"pcieingress_war", "15", 0},
				{"pdgain2g", "4", 0},
				{"pdgain5g", "4", 0},
				{"phycal_tempdelta", "255", 0},
				{"rawtempsense", "0x1ff", 0},
				{"rxchain", "7", 0},
				{"rxgainerr2g", "0xffff", 0},
				{"rxgainerr5g", "0xffff,0xffff,0xffff,0xffff", 0},
				{"rxgains2gelnagaina0", "0", 0},
				{"rxgains2gelnagaina1", "0", 0},
				{"rxgains2gelnagaina2", "0", 0},
				{"rxgains2gtrelnabypa0", "0", 0},
				{"rxgains2gtrelnabypa1", "0", 0},
				{"rxgains2gtrelnabypa2", "0", 0},
				{"rxgains2gtrisoa0", "0", 0},
				{"rxgains2gtrisoa1", "0", 0},
				{"rxgains2gtrisoa2", "0", 0},
				{"rxgains5gelnagaina0", "1", 0},
				{"rxgains5gelnagaina1", "1", 0},
				{"rxgains5gelnagaina2", "1", 0},
				{"rxgains5ghelnagaina0", "2", 0},
				{"rxgains5ghelnagaina1", "2", 0},
				{"rxgains5ghelnagaina2", "3", 0},
				{"rxgains5ghtrelnabypa0", "1", 0},
				{"rxgains5ghtrelnabypa1", "1", 0},
				{"rxgains5ghtrelnabypa2", "1", 0},
				{"rxgains5ghtrisoa0", "5", 0},
				{"rxgains5ghtrisoa1", "4", 0},
				{"rxgains5ghtrisoa2", "4", 0},
				{"rxgains5gmelnagaina0", "2", 0},
				{"rxgains5gmelnagaina1", "2", 0},
				{"rxgains5gmelnagaina2", "3", 0},
				{"rxgains5gmtrelnabypa0", "1", 0},
				{"rxgains5gmtrelnabypa1", "1", 0},
				{"rxgains5gmtrelnabypa2", "1", 0},
				{"rxgains5gmtrisoa0", "5", 0},
				{"rxgains5gmtrisoa1", "4", 0},
				{"rxgains5gmtrisoa2", "4", 0},
				{"rxgains5gtrelnabypa0", "1", 0},
				{"rxgains5gtrelnabypa1", "1", 0},
				{"rxgains5gtrelnabypa2", "1", 0},
				{"rxgains5gtrisoa0", "7", 0},
				{"rxgains5gtrisoa1", "6", 0},
				{"rxgains5gtrisoa2", "5", 0},
				{"sar", "0x0F12", 0},
				{"sar2g", "18", 0},
				{"sar5g", "15", 0},
				{"sb20in40hrrpo", "0", 0},
				{"sb20in40lrpo", "0", 0},
				{"sb20in80and160hr5ghpo", "0", 0},
				{"sb20in80and160hr5glpo", "0", 0},
				{"sb20in80and160hr5gmpo", "0", 0},
				{"sb20in80and160lr5ghpo", "0", 0},
				{"sb20in80and160lr5glpo", "0", 0},
				{"sb20in80and160lr5gmpo", "0", 0},
				{"sb40and80hr5ghpo", "0", 0},
				{"sb40and80hr5glpo", "0", 0},
				{"sb40and80hr5gmpo", "0", 0},
				{"sb40and80lr5ghpo", "0", 0},
				{"sb40and80lr5glpo", "0", 0},
				{"sb40and80lr5gmpo", "0", 0},
				{"sromrev", "11", 0},
				{"subband5gver", "0x4", 0},
				{"tempcorrx", "0x3f", 0},
				{"tempoffset", "255", 0},
				{"tempsense_option", "0x3", 0},
				{"tempsense_slope", "0xff", 0},
				{"temps_hysteresis", "15", 0},
				{"temps_period", "15", 0},
				{"tempthresh", "255", 0},
				{"tssiposslope2g", "1", 0},
				{"tssiposslope5g", "1", 0},
				{"tworangetssi2g", "0", 0},
				{"tworangetssi5g", "0", 0},
				{"txchain", "7", 0},
				{"venid", "0x14e4", 0},
				{"xtalfreq", "40000", 0},
				{0, 0, 0}
			};

			set_extra_param(r6300_pci_1_1_params, "pci/1/1/%s");
			set_extra_param(r6300_pci_2_1_params, "pci/2/1/%s");

			if (nvram_match("wl0_country_code", "US"))
				set_regulation(0, "US", "0");
			else if (nvram_match("wl0_country_code", "Q2"))
				set_regulation(0, "US", "0");
			else if (nvram_match("wl0_country_code", "TW"))
				set_regulation(0, "TW", "13");
			else if (nvram_match("wl0_country_code", "CN"))
				set_regulation(0, "CN", "1");
			else
				set_regulation(0, "DE", "0");

			if (nvram_match("wl1_country_code", "Q2"))
				set_regulation(1, "US", "0");
			else if (nvram_match("wl1_country_code", "EU"))
				set_regulation(1, "EU", "13");
			else if (nvram_match("wl1_country_code", "TW"))
				set_regulation(1, "TW", "13");
			else if (nvram_match("wl1_country_code", "CN"))
				set_regulation(1, "CN", "1");
			else
				set_regulation(1, "US", "0");

		}
		break;
	case MODEL_WNDR4500:
		mfr = "Netgear";
		name = "WNDR4500 V1";
		features = SUP_SES | SUP_AOSS_LED | SUP_80211N | SUP_1000ET | SUP_80211AC;
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1 eth2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifnames", "eth1 eth2");
			nvram_set("wl_ifname", "eth1");
			/* 4331, 2.4G */
			nvram_set("wl0_ifname", "eth1");
			nvram_set("wl0_bw_cap", "3");
			nvram_set("wl0_chanspec", "1l");
			nvram_set("wl0_country_code", "US");
			/* 4330, 5G */
			nvram_set("wl1_ifname", "eth2");
			nvram_set("wl1_bw_cap", "7");
			nvram_set("wl1_chanspec", "36/80");
			nvram_set("wl1_country_code", "US");
			nvram_set("blink_wl", "1"); /* Enable WLAN LED if wireless interface is enabled, and turn on blink */
			nvram_set("landevs", "vlan1 wl0 wl1");
			nvram_set("wandevs", "vlan2");
			nvram_set("lan_invert", "1");

			nvram_set("wl0_hwaddr", nvram_safe_get("pci/1/1/macaddr"));
			nvram_set("wl1_hwaddr", nvram_safe_get("pci/2/1/macaddr"));

			/* fix ssid according to 5G(eth2) and 2.4G(eth1) */
			//nvram_set("wl_ssid", "FreshTomato50");
			//nvram_set("wl0_ssid", "FreshTomato50");
			//nvram_set("wl1_ssid", "FreshTomato24");

			struct nvram_tuple wndr4500_pci_1_1_params[] = {
				{"pa2gw1a0", "0x1DFC", 0},
				{"pa2gw1a1", "0x1FF9", 0},
				{"pa2gw1a2", "0x1E58", 0},
				{"ledbh12", "11", 0},
				{"legofdmbw202gpo", "0x88000000", 0},
				{"ag0", "0", 0},
				{"ag1", "0", 0},
				{"ag2", "0", 0},
				{"legofdmbw20ul2gpo", "0x88000000", 0},
				{"rxchain", "7", 0},
				{"cckbw202gpo", "0x0000", 0},
				{"mcsbw20ul2gpo", "0x88800000", 0},
				{"pa2gw0a0", "0xFE56", 0},
				{"pa2gw0a1", "0xFEB3", 0},
				{"pa2gw0a2", "0xFE6A", 0},
				{"boardflags", "0x80003200", 0},
				{"tempoffset", "0", 0},
				{"boardvendor", "0x14e4", 0},
				{"triso2g", "3", 0},
				{"sromrev", "9", 0},
				{"extpagain2g", "3", 0},
				{"venid", "0x14e4", 0},
				{"maxp2ga0", "0x62", 0},
				{"maxp2ga1", "0x62", 0},
				{"maxp2ga2", "0x62", 0},
				{"boardtype", "0x59b", 0},
				{"boardflags2", "0x4000000", 0},
				{"tssipos2g", "1", 0},
				{"ledbh0", "11", 0},
				{"ledbh1", "11", 0},
				{"ledbh2", "11", 0},
				{"ledbh3", "11", 0},
				{"mcs32po", "0xA", 0},
				{"legofdm40duppo", "0x0", 0},
				{"antswctl2g", "0", 0},
				{"txchain", "7", 0},
				{"elna2g", "2", 0},
				{"antswitch", "0", 0},
				{"aa2g", "7", 0},
				{"cckbw20ul2gpo", "0x0000", 0},
				{"leddc", "0xFFFF", 0},
				{"pa2gw2a0", "0xF886", 0},
				{"pa2gw2a1", "0xF8AA", 0},
				{"pa2gw2a2", "0xF8A7", 0},
				{"pdetrange2g", "3", 0},
				{"devid", "0x4332", 0},
				{"tempthresh", "120", 0},
				{"mcsbw402gpo", "0x0x88800000", 0},
				{"mcsbw202gpo", "0x88800000", 0},
				{0, 0, 0}
			};

			struct nvram_tuple wndr4500_pci_2_1_params[] = {
				{"leddc", "0xFFFF", 0},
				{"txchain", "7", 0},
				{"maxp5gla0", "0x60", 0},
				{"elna5g", "1", 0},
				{"maxp5gla1", "0x60", 0},
				{"maxp5gla2", "0x60", 0},
				{"maxp5gha0", "0x72", 0},
				{"maxp5gha1", "0x72", 0},
				{"maxp5gha2", "0x72", 0},
				{"pa5gw0a0", "0xFE6C", 0},
				{"pa5gw0a1", "0xFE72", 0},
				{"pa5gw0a2", "0xFE75", 0},
				{"mcsbw20ul5gmpo", "0x22200000", 0},
				{"extpagain5g", "3", 0},
				{"pa5glw2a0", "0xFFFF", 0},
				{"boardflags", "0x90000200", 0},
				{"pa5glw2a1", "0xFFFF", 0},
				{"pa5glw2a2", "0xFFFF", 0},
				{"triso5g", "3", 0},
				{"tempoffset", "0", 0},
				{"mcsbw205gmpo", "0x22200000", 0},
				{"devid", "0x4333", 0},
				{"aa5g", "7", 0},
				{"pa5ghw2a0", "0xF8C5", 0},
				{"pa5ghw2a1", "0xF8D6", 0},
				{"pa5ghw2a2", "0xF8DA", 0},
				{"mcsbw20ul5glpo", "0x0", 0},
				{"pa5glw1a0", "0xFFFF", 0},
				{"pa5glw1a1", "0xFFFF", 0},
				{"pa5glw1a2", "0xFFFF", 0},
				{"mcsbw205glpo", "0x0", 0},
				{"mcsbw20ul5ghpo", "0x88800000", 0},
				{"legofdmbw205gmpo", "0x22000000", 0},
				{"ledbh12", "11", 0},
				{"mcsbw205ghpo", "0x88800000", 0},
				{"pa5ghw1a0", "0x1DD1", 0},
				{"pa5ghw1a1", "0x1DFF", 0},
				{"parefldovoltage", "35", 0},
				{"pa5ghw1a2", "0x1D76", 0},
				{"pa5gw2a0", "0xF8E9", 0},
				{"mcsbw405gmpo", "0x22200000", 0},
				{"pa5gw2a1", "0xF907", 0},
				{"pa5gw2a2", "0xF8ED", 0},
				{"boardtype", "0x5a9", 0},
				{"ledbh0", "11", 0},
				{"ledbh1", "11", 0},
				{"ledbh2", "11", 0},
				{"legofdmbw20ul5gmpo", "0x22000000", 0},
				{"ledbh3", "11", 0},
				{"rxchain", "7", 0},
				{"pdetrange5g", "4", 0},
				{"legofdm40duppo", "0x0", 0},
				{"maxp5ga0", "0x66", 0},
				{"pa5glw0a0", "0xFFFF", 0},
				{"maxp5ga1", "0x66", 0},
				{"pa5glw0a1", "0xFFFF", 0},
				{"maxp5ga2", "0x66", 0},
				{"pa5glw0a2", "0xFFFF", 0},
				{"legofdmbw205glpo", "0x0", 0},
				{"venid", "0x14e4", 0},
				{"boardvendor", "0x14e4", 0},
				{"legofdmbw205ghpo", "0x88000000", 0},
				{"antswitch", "0", 0},
				{"tempthresh", "120", 0},
				{"pa5ghw0a0", "0xFE74", 0},
				{"pa5ghw0a1", "0xFE7F", 0},
				{"sromrev", "9", 0},
				{"pa5ghw0a2", "0xFE72", 0},
				{"antswctl5g", "0", 0},
				{"pa5gw1a0", "0x1D5E", 0},
				{"mcsbw405glpo", "0x0", 0},
				{"pa5gw1a1", "0x1D3D", 0},
				{"pa5gw1a2", "0x1DA8", 0},
				{"legofdmbw20ul5glpo", "0x0", 0},
				{"ag0", "0", 0},
				{"ag1", "0", 0},
				{"ag2", "0", 0},
				{"mcsbw405ghpo", "0x88800000", 0},
				{"boardflags2", "0x4200000", 0},
				{"legofdmbw20ul5ghpo", "0x88000000", 0},
				{"mcs32po", "0x9", 0},
				{"tssipos5g", "1", 0},
				{0, 0, 0}
			};

			set_extra_param(wndr4500_pci_1_1_params, "pci/1/1/%s");
			set_extra_param(wndr4500_pci_2_1_params, "pci/2/1/%s");

			if (nvram_match("wl0_country_code", "US"))
				set_regulation(0, "US", "0");
			else if (nvram_match("wl0_country_code", "Q2"))
				set_regulation(0, "US", "0");
			else if (nvram_match("wl0_country_code", "TW"))
				set_regulation(0, "TW", "13");
			else if (nvram_match("wl0_country_code", "CN"))
				set_regulation(0, "CN", "1");
			else
				set_regulation(0, "DE", "0");

			if (nvram_match("wl1_country_code", "Q2"))
				set_regulation(1, "US", "0");
			else if (nvram_match("wl1_country_code", "EU"))
				set_regulation(1, "EU", "13");
			else if (nvram_match("wl1_country_code", "TW"))
				set_regulation(1, "TW", "13");
			else if (nvram_match("wl1_country_code", "CN"))
				set_regulation(1, "CN", "1");
			else
				set_regulation(1, "US", "0");

		}
		break;
	case MODEL_WNDR4500V2:
		mfr = "Netgear";
		name = "WNDR4500 V2";
		features = SUP_SES | SUP_AOSS_LED | SUP_80211N | SUP_1000ET | SUP_80211AC;
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1 eth2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifnames", "eth1 eth2");
			nvram_set("wl_ifname", "eth1");
			/* 4331, 2.4G */
			nvram_set("wl0_ifname", "eth1");
			nvram_set("wl0_bw_cap", "3");
			nvram_set("wl0_chanspec", "1l");
			nvram_set("wl0_country_code", "US");
			/* 4331, 5G */
			nvram_set("wl1_ifname", "eth2");
			nvram_set("wl0_bw_cap", "7");
			nvram_set("wl0_chanspec", "36/80");
			nvram_set("blink_wl", "1"); /* Enable WLAN LED if wireless interface is enabled, and turn on blink */
			nvram_set("landevs", "vlan1 wl0 wl1");
			nvram_set("wandevs", "vlan2");
			nvram_set("lan_invert", "1");

			nvram_set("wl0_hwaddr", nvram_safe_get("pci/1/1/macaddr"));
			nvram_set("wl1_hwaddr", nvram_safe_get("pci/2/1/macaddr"));

			struct nvram_tuple wndr4500v2_pci_1_1_params[] = {
				{"pa2gw1a0", "0x1791", 0},
				{"pa2gw1a1", "0x189B", 0},
				{"pa2gw1a2", "0x173E", 0},
				{"ledbh12", "11", 0},
				{"legofdmbw202gpo", "0xECA64200", 0},
				{"ag0", "0", 0},
				{"ag1", "0", 0},
				{"ag2", "0", 0},
				{"legofdmbw20ul2gpo", "0xECA64200", 0},
				{"rxchain", "7", 0},
				{"cckbw202gpo", "0x0000", 0},
				{"mcsbw20ul2gpo", "0xECA64200", 0},
				{"pa2gw0a0", "0xFE90", 0},
				{"pa2gw0a1", "0xFE9F", 0},
				{"pa2gw0a2", "0xFE8B", 0},
				{"boardflags", "0x80003200", 0},
				{"tempoffset", "0", 0},
				{"boardvendor", "0x14e4", 0},
				{"triso2g", "3", 0},
				{"sromrev", "9", 0},
				{"extpagain2g", "1", 0},
				{"venid", "0x14e4", 0},
				{"maxp2ga0", "0x5E", 0},
				{"maxp2ga1", "0x5E", 0},
				{"maxp2ga2", "0x5E", 0},
				{"boardtype", "0x59b", 0},
				{"boardflags2", "0x4100000", 0},
				{"tssipos2g", "1", 0},
				{"ledbh0", "11", 0},
				{"ledbh1", "11", 0},
				{"ledbh2", "11", 0},
				{"ledbh3", "11", 0},
				{"mcs32po", "0xA", 0},
				{"legofdm40duppo", "0x0", 0},
				{"antswctl2g", "0", 0},
				{"txchain", "7", 0},
				{"elna2g", "2", 0},
				{"antswitch", "0", 0},
				{"aa2g", "7", 0},
				{"cckbw20ul2gpo", "0x0000", 0},
				{"leddc", "0xFFFF", 0},
				{"pa2gw2a0", "0xFA5C", 0},
				{"pa2gw2a1", "0xFA22", 0},
				{"pa2gw2a2", "0xFA7A", 0},
				{"pdetrange2g", "3", 0},
				{"devid", "0x4332", 0},
				{"tempthresh", "120", 0},
				{"mcsbw402gpo", "0xECAAAAAA", 0},
				{"mcsbw202gpo", "0xECA64200", 0},
				{0, 0, 0}
			};

			struct nvram_tuple wndr4500v2_pci_2_1_params[] = {
				{"leddc", "0xFFFF", 0},
				{"txchain", "7", 0},
				{"maxp5gla0", "0x64", 0},
				{"elna5g", "1", 0},
				{"maxp5gla1", "0x64", 0},
				{"maxp5gla2", "0x64", 0},
				{"maxp5gha0", "0x5E", 0},
				{"maxp5gha1", "0x5E", 0},
				{"maxp5gha2", "0x5E", 0},
				{"pa5gw0a0", "0xFEB2", 0},
				{"pa5gw0a1", "0xFE7D", 0},
				{"pa5gw0a2", "0xFE78", 0},
				{"mcsbw20ul5gmpo", "0x42000000", 0},
				{"extpagain5g", "3", 0},
				{"pa5glw2a0", "0xF98F", 0},
				{"boardflags", "0x90000200", 0},
				{"pa5glw2a1", "0xF9C1", 0},
				{"pa5glw2a2", "0xF99D", 0},
				{"triso5g", "3", 0},
				{"tempoffset", "0", 0},
				{"mcsbw205gmpo", "0x42000000", 0},
				{"devid", "0x4333", 0},
				{"aa5g", "7", 0},
				{"pa5ghw2a0", "0xF9DC", 0},
				{"pa5ghw2a1", "0xFA04", 0},
				{"pa5ghw2a2", "0xF9EE", 0},
				{"mcsbw20ul5glpo", "0x42000000", 0},
				{"pa5glw1a0", "0x1A5D", 0},
				{"pa5glw1a1", "0x1962", 0},
				{"pa5glw1a2", "0x19EC", 0},
				{"mcsbw205glpo", "0x20000000", 0},
				{"mcsbw20ul5ghpo", "0xECA64200", 0},
				{"legofdmbw205gmpo", "0x42000000", 0},
				{"ledbh12", "11", 0},
				{"mcsbw205ghpo", "0xECA64200", 0},
				{"pa5ghw1a0", "0x1896", 0},
				{"pa5ghw1a1", "0x1870", 0},
				{"parefldovoltage", "35", 0},
				{"pa5ghw1a2", "0x1883", 0},
				{"pa5gw2a0", "0xF93C", 0},
				{"mcsbw405gmpo", "0x42000000 ", 0},
				{"pa5gw2a1", "0xF99B", 0},
				{"pa5gw2a2", "0xF995", 0},
				{"boardtype", "0x5a9", 0},
				{"ledbh0", "11", 0},
				{"ledbh1", "11", 0},
				{"ledbh2", "11", 0},
				{"legofdmbw20ul5gmpo", "0x42000000", 0},
				{"ledbh3", "11", 0},
				{"rxchain", "7", 0},
				{"pdetrange5g", "4", 0},
				{"legofdm40duppo", "0x0", 0},
				{"maxp5ga0", "0x4A", 0},
				{"pa5glw0a0", "0xFE7F", 0},
				{"maxp5ga1", "0x4A", 0},
				{"pa5glw0a1", "0xFE66", 0},
				{"maxp5ga2", "0x4A", 0},
				{"pa5glw0a2", "0xFE6B", 0},
				{"legofdmbw205glpo", "0x20000000", 0},
				{"venid", "0x14e4", 0},
				{"boardvendor", "0x14e4", 0},
				{"legofdmbw205ghpo", "0xECA64200", 0},
				{"antswitch", "0", 0},
				{"tempthresh", "120", 0},
				{"pa5ghw0a0", "0xFE53", 0},
				{"pa5ghw0a1", "0xFE68", 0},
				{"sromrev", "9", 0},
				{"pa5ghw0a2", "0xFE5D", 0},
				{"antswctl5g", "0", 0},
				{"pa5gw1a0", "0x1C6A", 0},
				{"mcsbw405glpo", "0x42000000", 0},
				{"pa5gw1a1", "0x1A47", 0},
				{"pa5gw1a2", "0x1A39", 0},
				{"legofdmbw20ul5glpo", "0x42000000", 0},
				{"ag0", "0", 0},
				{"ag1", "0", 0},
				{"ag2", "0", 0},
				{"mcsbw405ghpo", "0xECA64200", 0},
				{"boardflags2", "0x4200000", 0},
				{"legofdmbw20ul5ghpo", "0xECA64200", 0},
				{"mcs32po", "0x9", 0},
				{"tssipos5g", "1", 0},
				{0, 0, 0}
			};

			set_extra_param(wndr4500v2_pci_1_1_params, "pci/1/1/%s");
			set_extra_param(wndr4500v2_pci_2_1_params, "pci/2/1/%s");

			if (nvram_match("wl0_country_code", "US"))
				set_regulation(0, "US", "0");
			else if (nvram_match("wl0_country_code", "Q2"))
				set_regulation(0, "US", "0");
			else if (nvram_match("wl0_country_code", "TW"))
				set_regulation(0, "TW", "13");
			else if (nvram_match("wl0_country_code", "CN"))
				set_regulation(0, "CN", "1");
			else
				set_regulation(0, "DE", "0");

			if (nvram_match("wl1_country_code", "Q2"))
				set_regulation(1, "US", "0");
			else if (nvram_match("wl1_country_code", "EU"))
				set_regulation(1, "EU", "13");
			else if (nvram_match("wl1_country_code", "TW"))
				set_regulation(1, "TW", "13");
			else if (nvram_match("wl1_country_code", "CN"))
				set_regulation(1, "CN", "1");
			else
				set_regulation(1, "US", "0");

		}
		break;
	case MODEL_EA6500V1:
		mfr = "Linksys";
		name = "EA6500v1";
		features = SUP_SES | SUP_80211N | SUP_1000ET | SUP_80211AC;
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1 eth2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifnames", "eth1 eth2");
			nvram_set("wl_ifname", "eth1");
			nvram_set("wl0_ifname", "eth1");
			nvram_set("wl1_ifname", "eth2");
			nvram_set("wl0_bw_cap", "7");
			nvram_set("wl0_chanspec", "36/80");
			nvram_set("wl1_bw_cap", "3");
			nvram_set("wl1_chanspec","1l");
			nvram_set("blink_wl", "1"); /* Enable WLAN LED if wireless interface is enabled, and turn on blink */

			/* fix ssid according to 5G(eth1) and 2.4G(eth2) */
			nvram_set("wl_ssid", "FreshTomato50");
			nvram_set("wl0_ssid", "FreshTomato50");
			nvram_set("wl1_ssid", "FreshTomato24");

			/* force US country for 5G eth1 */
			nvram_set("pci/1/1/ccode", nvram_safe_get("ccode"));
			nvram_set("regulation_domain_5G", nvram_safe_get("ccode"));
		}
		break;
#endif /* CONFIG_BCMWL6 */
	case MODEL_WNR3500LV2:
		mfr = "Netgear";
		name = "WNR3500L v2";
		features = SUP_SES | SUP_AOSS_LED | SUP_80211N | SUP_1000ET;
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifname", "eth1");
		}
		break;
	case MODEL_F9K1102:
		mfr = "Belkin";
		features = SUP_SES | SUP_80211N;
		name = "N600 DB Wireless N+";
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("vlan1hwname", "et0");
			nvram_set("vlan2hwname", "et0");
			nvram_set("wan_ifnameX", "vlan1");
			nvram_set("wandevs", "vlan1");
			nvram_set("wan_ifname", "vlan1");
			nvram_set("wl0_ifname", "eth1");
#ifdef TCONFIG_USBAP
			nvram_set("ehciirqt", "3");
			nvram_set("qtdc_pid", "48407");
			nvram_set("qtdc_vid", "2652");
			nvram_set("qtdc0_ep", "4");
			nvram_set("qtdc0_sz", "5");
			nvram_set("qtdc1_ep", "18");
			nvram_set("qtdc1_sz", "10");
			nvram_set("br0_ifnames", "vlan2 eth1 eth2");
			nvram_set("lan_ifnames", "vlan2 eth1 eth2");
			nvram_set("landevs", "vlan2 wl0 wl1");
			nvram_set("wl1_ifname", "eth2");
			nvram_set("wl1_channel", "36");
			nvram_set("wl1_nbw", "40");
			nvram_set("wl1_nbw_cap", "1");
			nvram_set("wl1_nctrlsb", "lower");
#endif
		}
		break;
	case MODEL_E900:
	case MODEL_E1500:
		mfr = "Linksys";
		name = nvram_safe_get("boot_hw_model");
		ver = nvram_safe_get("boot_hw_ver");
		features = SUP_SES | SUP_80211N;
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifname", "eth1");
		}
		break;
	case MODEL_E1550:
		mfr = "Linksys";
		name = nvram_safe_get("boot_hw_model");
		ver = nvram_safe_get("boot_hw_ver");
		features = SUP_SES | SUP_80211N;
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifname", "eth1");
		}
		break;
	case MODEL_E2500:
		mfr = "Linksys";
		name = "E2500 v1/v2/v3";
		/* NOTE: E2500v1 & v2 have 8 MB flash, no external USB
		 * E2500v3 has 16 MB flash, external USB
		 * all three have the same boot_hw_ver
		 */
		features = SUP_SES | SUP_80211N;
		if (!nvram_match("t_fix1", (char *)name)) {
#ifdef TCONFIG_USBAP
			nvram_set("wl1_hwaddr", nvram_safe_get("0:macaddr"));
			nvram_set("ehciirqt", "3");
			nvram_set("qtdc_pid", "48407");
			nvram_set("qtdc_vid", "2652");
			nvram_set("qtdc0_ep", "4");
			nvram_set("qtdc0_sz", "0");
			nvram_set("qtdc1_ep", "18");
			nvram_set("qtdc1_sz", "10");
			nvram_set("lan_ifnames", "vlan1 eth1 eth2");
			nvram_set("landevs", "vlan1 wl0 wl1");
			nvram_set("wl0_ifname", "eth1");
			nvram_set("wl1_ifname", "eth2");
#else
			nvram_set("lan_ifnames", "vlan1 eth1");
			nvram_set("landevs", "vlan1 wl0");
#endif
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifname", "eth1");

		}
		break;
	case MODEL_E3200:
		mfr = "Linksys";
		name = nvram_safe_get("boot_hw_model");
		ver = nvram_safe_get("boot_hw_ver");
		features = SUP_SES | SUP_80211N | SUP_1000ET;
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
#ifdef TCONFIG_USBAP
			nvram_set("wl1_hwaddr", nvram_safe_get("usb/0xBD17/macaddr"));
			nvram_set("ehciirqt", "3");
			nvram_set("qtdc_pid", "48407");
			nvram_set("qtdc_vid", "2652");
			nvram_set("qtdc0_ep", "4");
			nvram_set("qtdc0_sz", "0");
			nvram_set("qtdc1_ep", "18");
			nvram_set("qtdc1_sz", "10");
			nvram_set("lan_ifnames", "vlan1 eth1 eth2");
			nvram_set("landevs", "vlan1 wl0 wl1");
			nvram_set("wl0_ifname", "eth1");
			nvram_set("wl1_ifname", "eth2");
#else
			nvram_set("lan_ifnames", "vlan1 eth1");
			nvram_set("landevs", "vlan1 wl0");
#endif
			nvram_set("wl_ifname", "eth1");
			nvram_set("wan_ifnameX", "vlan2");
		}
		break;
	case MODEL_WNDR4000:
		mfr = "Netgear";
		name = "WNDR4000";
		features = SUP_SES | SUP_80211N | SUP_1000ET;
		/* Don't auto-start blink, as shift register causes other LED's to blink slightly because of this.
		 * Rather, turn on in startup script if desired ... so disable the line below
		 */
		//nvram_set("blink_wl", "1");
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1 eth2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifname", "eth1");
		}

		/* Set Key Parameters for Wireless Interfaces: SB (Southbridge) and PCI, to configure HW (as Netgear intends)
		 * Credit for the nvram_tuple approach below goes to DD-WRT (borrowed here for simplicity)!
		 * Parameters optimized based on clean Netgear build (NVRAM extracted and checked vs. Tomato 30/30/30 Reset version of NVRAM)
		 */
		struct nvram_tuple wndr4000_sb_1_params[] = {
			{"cck2gpo", "0x1111", 0},
			//{"ccode", "EU", 0},
			{"cddpo", "0x1111", 0},
			{"extpagain2g", "3", 0},
			{"maxp2ga0", "0x56", 0},
			{"maxp2ga1", "0x56", 0},
			{"mcs2gpo0", "0x1000", 0},
			{"mcs2gpo1", "0x7531", 0},
			{"mcs2gpo2", "0x2111", 0},
			{"mcs2gpo3", "0xA864", 0},
			{"mcs2gpo4", "0x3333", 0},
			{"mcs2gpo5", "0x9864", 0},
			{"mcs2gpo6", "0x3333", 0},
			{"mcs2gpo7", "0xB975", 0},
			{"ofdm2gpo", "0x75331111", 0},
			{"pa2gw0a0", "0xFEA6", 0},
			{"pa2gw0a1", "0xFE9E", 0},
			{"pa2gw1a0", "0x191D", 0},
			{"pa2gw1a1", "0x1809", 0},
			{"pa2gw2a0", "0xFA18", 0},
			{"pa2gw2a1", "0xFA4B", 0},
			//{"regrev", "15", 0},
			{"stbcpo", "0x1111", 0},
			{0, 0, 0}
		};

		struct nvram_tuple wndr4000_pci_1_1_params[] = {
			{"boardflags2", "0x04000000", 0},
			//{"ccode", "EU", 0},
			{"extpagain2g", "0", 0},
			{"extpagain5g", "0", 0},
			{"legofdm40duppo", "0x2222", 0},
			{"legofdmbw205ghpo", "0x88642100", 0},
			{"legofdmbw205gmpo", "0x33221100", 0},
			{"legofdmbw20ul5ghpo", "0x88642100", 0},
			{"legofdmbw20ul5gmpo", "0x33221100", 0},
			{"maxp5ga0", "0x4E", 0},
			{"maxp5ga1", "0x4E", 0},
			{"maxp5ga2", "0x4E", 0},
			{"maxp5gha0", "0x4E", 0},
			{"maxp5gha1", "0x4E", 0},
			{"maxp5gha2", "0x4E", 0},
			{"maxp5gla0", "0x48", 0},
			{"maxp5gla1", "0x48", 0},
			{"maxp5gla2", "0x48", 0},
			{"mcs32po", "0x2222", 0},
			{"mcsbw205ghpo", "0x88642100", 0},
			{"mcsbw205glpo", "0x11000000", 0},
			{"mcsbw205gmpo", "0x44221100", 0},
			{"mcsbw20ul5ghpo", "0x88642100", 0},
			{"mcsbw20ul5glpo", "0x11000000", 0},
			{"mcsbw20ul5gmpo", "0x44221100", 0},
			{"mcsbw405ghpo", "0x99875310", 0},
			{"mcsbw405glpo", "0x33222222", 0},
			{"mcsbw405gmpo", "0x66443322", 0},
			{"pa5ghw1a1", "0x155F", 0},
			{"pa5ghw2a1", "0xFAB0", 0},
			//{"regrev", "15", 0},
			{0, 0, 0}
		};

		set_extra_param(wndr4000_sb_1_params, "sb/1/%s");
		set_extra_param(wndr4000_pci_1_1_params, "pci/1/1/%s");
		break;
	case MODEL_WNDR3700v3:
		mfr = "Netgear";
		name = "WNDR3700v3";
		features = SUP_SES | SUP_80211N | SUP_1000ET;
		/* Don't auto-start blink, as shift register causes other LED's to blink slightly because of this.
		 * Rather, turn on in startup script if desired ... so disable the line below
		 */
		//nvram_set("blink_wl", "1");
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1 eth2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifname", "eth1");
		}

		/* Set Key Parameters for Wireless Interfaces: SB (Southbridge) and PCI, to configure HW (as Netgear intends)
		 * Credit for the nvram_tuple approach below goes to DD-WRT (borrowed here for simplicity)!
		 * Parameters optimized based on clean Netgear build (NVRAM extracted and checked vs. Tomato 30/30/30 Reset version of NVRAM)
		 */
		struct nvram_tuple wndr3700v3_sb_1_params[] = {
			{"cck2gpo", "0x1111", 0},
			//{"ccode", "EU", 0},
			{"cddpo", "0x1111", 0},
			{"extpagain2g", "3", 0},
			{"maxp2ga0", "0x56", 0},
			{"maxp2ga1", "0x56", 0},
			{"mcs2gpo0", "0x1000", 0},
			{"mcs2gpo1", "0x7531", 0},
			{"mcs2gpo2", "0x2111", 0},
			{"mcs2gpo3", "0xA864", 0},
			{"mcs2gpo4", "0x3333", 0},
			{"mcs2gpo5", "0x9864", 0},
			{"mcs2gpo6", "0x3333", 0},
			{"mcs2gpo7", "0xB975", 0},
			{"ofdm2gpo", "0x75331111", 0},
			{"pa2gw0a0", "0xFEA6", 0},
			{"pa2gw0a1", "0xFE9E", 0},
			{"pa2gw1a0", "0x191D", 0},
			{"pa2gw1a1", "0x1809", 0},
			{"pa2gw2a0", "0xFA18", 0},
			{"pa2gw2a1", "0xFA4B", 0},
			//{"regrev", "15", 0},
			{"stbcpo", "0x1111", 0},
			{0, 0, 0}
		};

		struct nvram_tuple wndr3700v3_pci_1_1_params[] = {
			{"boardflags2", "0x04000000", 0},
			//{"ccode", "EU", 0},
			{"extpagain2g", "0", 0},
			{"extpagain5g", "0", 0},
			{"legofdm40duppo", "0x2222", 0},
			{"legofdmbw205ghpo", "0x88642100", 0},
			{"legofdmbw205gmpo", "0x33221100", 0},
			{"legofdmbw20ul5ghpo", "0x88642100", 0},
			{"legofdmbw20ul5gmpo", "0x33221100", 0},
			{"maxp5ga0", "0x4E", 0},
			{"maxp5ga1", "0x4E", 0},
			{"maxp5ga2", "0x4E", 0},
			{"maxp5gha0", "0x4E", 0},
			{"maxp5gha1", "0x4E", 0},
			{"maxp5gha2", "0x4E", 0},
			{"maxp5gla0", "0x48", 0},
			{"maxp5gla1", "0x48", 0},
			{"maxp5gla2", "0x48", 0},
			{"mcs32po", "0x2222", 0},
			{"mcsbw205ghpo", "0x88642100", 0},
			{"mcsbw205glpo", "0x11000000", 0},
			{"mcsbw205gmpo", "0x44221100", 0},
			{"mcsbw20ul5ghpo", "0x88642100", 0},
			{"mcsbw20ul5glpo", "0x11000000", 0},
			{"mcsbw20ul5gmpo", "0x44221100", 0},
			{"mcsbw405ghpo", "0x99875310", 0},
			{"mcsbw405glpo", "0x33222222", 0},
			{"mcsbw405gmpo", "0x66443322", 0},
			{"pa5ghw1a1", "0x155F", 0},
			{"pa5ghw2a1", "0xFAB0", 0},
			//{"regrev", "15", 0},
			{0, 0, 0}
		};

		set_extra_param(wndr3700v3_sb_1_params, "sb/1/%s");
		set_extra_param(wndr3700v3_pci_1_1_params, "pci/1/1/%s");
		break;
	case MODEL_WNDR3400:
		mfr = "Netgear";
		name = "WNDR3400";
		features = SUP_SES | SUP_80211N;
		/* Don't auto-start blink, as shift register causes other LED's to blink slightly because of this.
		 * Rather, turn on in startup script if desired ... so disable the line below
		 */
		//nvram_set("blink_wl", "1");
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1 eth2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifname", "eth1");
		}

		/* Set Key Parameters for Wireless Interfaces: SB (Southbridge) and PCI, to configure HW (as Netgear intends)
		 * Credit for the nvram_tuple approach below goes to DD-WRT (borrowed here for simplicity)!
		 * Parameters optimized based on clean Netgear build (NVRAM extracted and checked vs. Tomato 30/30/30 Reset version of NVRAM)
		 */
		struct nvram_tuple wndr3400_sb_1_params[] = {
			{"aa2g", "3", 0},
			{"ag0", "2", 0},
			{"ag1", "2", 0},
			{"antswctl2g", "2", 0},
			{"antswitch", "0", 0},
			{"bw40po", "0", 0},
			{"bwduppo", "0", 0},
			{"cck2gpo", "0x0000", 0},
			{"ccode", "US", 0},
			{"cddpo", "0", 0},
			{"extpagain2g", "2", 0},
			{"itt2ga0", "0x20", 0},
			{"itt2ga1", "0x20", 0},
			{"ledbh0", "2", 0},
			{"ledbh1", "11", 0},
			{"ledbh2", "11", 0},
			{"ledbh3", "11", 0},
			{"leddc", "0xffff", 0},
			{"maxp2ga0", "0x56", 0},
			{"maxp2ga1", "0x56", 0},
			{"mcs2gpo0", "0x2222", 0},
			{"mcs2gpo1", "0xa642", 0},
			{"mcs2gpo2", "0x6666", 0},
			{"mcs2gpo3", "0xa866", 0},
			{"mcs2gpo4", "0x8888", 0},
			{"mcs2gpo5", "0xa888", 0},
			{"mcs2gpo6", "0x8888", 0},
			{"mcs2gpo7", "0xcc88", 0},
			{"ofdm2gpo", "0x54400000", 0},
			{"pa2gw0a0", "0xfeca", 0},
			{"pa2gw0a1", "0xfebd", 0},
			{"pa2gw1a0", "0x17dd", 0},
			{"pa2gw1a1", "0x16ba", 0},
			{"pa2gw2a0", "0xfa8e", 0},
			{"pa2gw2a1", "0xfab1", 0},
			{"pdetrange2g", "2", 0},
			{"regrev", "39", 0},
			{"rxchain", "3", 0},
			{"sromrev", "8", 0},
			{"stbcpo", "0", 0},
			{"triso2g", "3", 0},
			{"tssipos2g", "1", 0},
			{"txchain", "3", 0},
			{0, 0, 0}
		};

		struct nvram_tuple wndr3400_pci_1_1_params[] = {
			{"aa5g", "3", 0},
			{"ag0", "2", 0},
			{"ag1", "2", 0},
			{"antswctl2g", "0", 0},
			{"antswctl5g", "0", 0},
			{"antswitch", "0", 0},
			{"bw405ghpo/bw405glpo/bw405gpo/bw402gpo", "0x2", 0},
			{"bw40po", "0", 0},
			{"bwduppo", "0", 0},
			{"ccode", "US", 0},
			{"cdd5ghpo/cdd5glpo/cdd5gpo/cdd2gpo", "0x0", 0},
			{"cddpo", "0", 0},
			{"extpagain5g", "2", 0},
			{"itt5ga0", "0x3e", 0},
			{"itt5ga1", "0x3e", 0},
			{"ledbh0", "0", 0},
			{"ledbh1", "0xffff", 0},
			{"ledbh2", "0xffff", 0},
			{"ledbh3", "0xffff", 0},
			{"leddc", "0xffff", 0},
			{"maxp5ga0", "0x4E", 0},
			{"maxp5ga1", "0x4E", 0},
			{"maxp5gha0", "0x4A", 0},
			{"maxp5gha1", "0x4A", 0},
			{"maxp5gla0", "0x3E", 0},
			{"maxp5gla1", "0x3E", 0},
			{"mcs5ghpo0", "0x4200", 0},
			{"mcs5ghpo1", "0x6664", 0},
			{"mcs5ghpo2", "0x4200", 0},
			{"mcs5ghpo3", "0x6664", 0},
			{"mcs5ghpo4", "0x4200", 0},
			{"mcs5ghpo5", "0x6664", 0},
			{"mcs5ghpo6", "0x4200", 0},
			{"mcs5ghpo7", "0x6664", 0},
			{"mcs5glpo0", "0x0000", 0},
			{"mcs5glpo1", "0x2200", 0},
			{"mcs5glpo2", "0x0000", 0},
			{"mcs5glpo3", "0x2200", 0},
			{"mcs5glpo4", "0x0000", 0},
			{"mcs5glpo5", "0x2200", 0},
			{"mcs5glpo6", "0x0000", 0},
			{"mcs5glpo7", "0x2200", 0},
			{"mcs5gpo0", "0x4200", 0},
			{"mcs5gpo1", "0x6664", 0},
			{"mcs5gpo2", "0x4200", 0},
			{"mcs5gpo3", "0x6664", 0},
			{"mcs5gpo4", "0x4200", 0},
			{"mcs5gpo5", "0x6664", 0},
			{"mcs5gpo6", "0x4200", 0},
			{"mcs5gpo7", "0x6664", 0},
			{"ofdm5ghpo0", "0x0000", 0},
			{"ofdm5ghpo1", "0x2000", 0},
			{"ofdm5glpo0", "0x0000", 0},
			{"ofdm5glpo1", "0x0000", 0},
			{"ofdm5gpo0", "0x0000", 0},
			{"ofdm5gpo1", "0x2000", 0},
			{"pa5ghw0a0", "0xfe98", 0},
			{"pa5ghw0a1", "0xfead", 0},
			{"pa5ghw1a0", "0x15c0", 0},
			{"pa5ghw1a1", "0x1539", 0},
			{"pa5ghw2a0", "0xfa9c", 0},
			{"pa5ghw2a1", "0xfab9", 0},
			{"pa5glw0a0", "0xfe87", 0},
			{"pa5glw0a1", "0xfe9a", 0},
			{"pa5glw1a0", "0x1637", 0},
			{"pa5glw1a1", "0x1591", 0},
			{"pa5glw2a0", "0xfa8e", 0},
			{"pa5glw2a1", "0xfabc", 0},
			{"pa5gw0a0", "0xfe9b", 0},
			{"pa5gw0a1", "0xfe9b", 0},
			{"pa5gw1a0", "0x153f", 0},
			{"pa5gw1a1", "0x1576", 0},
			{"pa5gw2a0", "0xfaae", 0},
			{"pa5gw2a1", "0xfaa5", 0},
			{"pdetrange5g", "4", 0},
			{"regrev", "39", 0},
			{"rxchain", "3", 0},
			{"sromrev", "8", 0},
			{"stbc5ghpo/stbc5glpo/stbc5gpo/stbc2gpo", "0x0", 0},
			{"stbcpo", "0", 0},
			{"triso5g", "3", 0},
			{"tssipos5g", "1", 0},
			{"txchain", "3", 0},
			{"wdup405ghpo/wdup405glpo/wdup405gpo/wdup402gpo", "0x0", 0},
			{0, 0, 0}
		};

		set_extra_param(wndr3400_sb_1_params, "sb/1/%s");
		set_extra_param(wndr3400_pci_1_1_params, "pci/1/1/%s");
		break;
	case MODEL_WNDR3400v2:
		mfr = "Netgear";
		name = "WNDR3400v2";
		features = SUP_SES | SUP_80211N;
		/* Don't auto-start blink, as shift register causes other LED's to blink slightly because of this.
		 * Rather, turn on in startup script if desired ... so disable the line below
		 */
		//nvram_set("blink_wl", "1");
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1 eth2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifname", "eth1");
		}
		xstart("gpio", "disable", "16"); /* turn on Power LED (active LOW); GPIO 16 controls state (on/off) and GPIO 14 controls color, see led.c */
		xstart("gpio", "enable", "21"); /* turn on USB supply (active HIGH) */
		break;
	case MODEL_WNDR3400v3:
		mfr = "Netgear";
		name = "WNDR3400v3";
		features = SUP_SES | SUP_80211N;
		/* Don't auto-start blink, as shift register causes other LED's to blink slightly because of this.
		 * Rather, turn on in startup script if desired ... so disable the line below
		 */
		//nvram_set("blink_wl", "1");
#ifdef TCONFIG_USB
		nvram_set("usb_uhci", "-1");
#endif
		if (!nvram_match("t_fix1", (char *)name)) {
			nvram_set("lan_ifnames", "vlan1 eth1 eth2");
			nvram_set("wan_ifnameX", "vlan2");
			nvram_set("wl_ifname", "eth1");
		}

		nvram_set("boardflags", "0x80001710");
		nvram_set("boardflags2", "0x1000");

		/* Set Key Parameters for Wireless Interfaces: SB (Southbridge) and PCI, to configure HW (as Netgear intends) */
		struct nvram_tuple wndr3400v3_sb_1_params[] = {
			{"aa2g", "0x3", 0},
			{"ag0", "0x0", 0},
			{"ag1", "0x0", 0},
			{"ag2", "0x2", 0},
			{"ag3", "0xff", 0},
			{"antswctl2g", "0x1", 0},
			{"antswitch", "0x0", 0},
			{"boardflags", "0x80001710", 0},
			{"boardflags2", "0x1000", 0},
			{"bw40po", "0x0", 0},
			{"bwduppo", "0x0", 0},
			{"bxa2g", "0x3", 0},
			{"cck2gpo", "0x0", 0},
			{"ccode", "Q1", 0},
			{"cddpo", "0x0", 0},
			{"devid", "0x4329", 0},
			{"elna2g", "2", 0},
			{"extpagain2g", "0x2", 0},
			{"itt2ga0", "0x20", 0},
			{"itt2ga1", "0x20", 0},
			{"ledbh0", "11", 0},
			{"ledbh1", "11", 0},
			{"ledbh2", "11", 0},
			{"ledbh3", "11", 0},
			{"leddc", "0xffff", 0},
			{"maxp2ga0", "0x4C", 0},
			{"maxp2ga1", "0x4C", 0},
			{"mcs2gpo0", "0x0", 0},
			{"mcs2gpo1", "0x5300", 0},
			{"mcs2gpo2", "0x0", 0},
			{"mcs2gpo3", "0x5300", 0},
			{"mcs2gpo4", "0x6666", 0},
			{"mcs2gpo5", "0x6666", 0},
			{"mcs2gpo6", "0x6666", 0},
			{"mcs2gpo7", "0x6666", 0},
			{"ofdm2gpo", "0x30000000", 0},
			{"opo", "0x0", 0},
			{"pa2gw0a0", "0xFEE2", 0},
			{"pa2gw0a1", "0xFEE4", 0},
			{"pa2gw1a0", "0x150F", 0},
			{"pa2gw1a1", "0x141C", 0},
			{"pa2gw2a0", "0xFACA", 0},
			{"pa2gw2a1", "0xFAEB", 0},
			{"pdetrange2g", "0x2", 0},
			{"regrev", "50", 0},
			{"rssisav2g", "0x7", 0},
			{"rssismc2g", "0xf", 0},
			{"rssismf2g", "0xf", 0},
			{"rxchain", "0x3", 0},
			{"rxpo2g", "0xff", 0},
			{"sromrev", "8", 0},
			{"stbcpo", "0x0", 0},
			{"temps_hysteresis", "5", 0},
			{"temps_period", "5", 0},
			{"tempthresh", "120", 0},
			{"tri2g", "0xff", 0},
			{"triso2g", "0x3", 0},
			{"tssipos2g", "0x1", 0},
			{"txchain", "0x3", 0},
			{"txq_len", "1024", 0},
			{0, 0, 0}
		};

		struct nvram_tuple wndr3400v3_pci_1_1_params[] = {
			{"ag0", "0x1", 0},
			{"ag1", "0x1", 0},
			{"antswctl2g", "0", 0},
			{"antswctl5g", "0", 0},
			{"antswitch", "0", 0},
			{"bw405ghpo/bw405glpo/bw405gpo/bw402gpo", "0x2", 0},
			{"bw40po", "0", 0},
			{"bwduppo", "0", 0},
			{"ccode", "0", 0},
			{"cdd5ghpo/cdd5glpo/cdd5gpo/cdd2gpo", "0x0", 0},
			{"cddpo", "0", 0},
			{"devid", "0x432d", 0},
			{"extpagain5g", "2", 0},
			{"itt5ga0", "0x3e", 0},
			{"itt5ga1", "0x3e", 0},
			{"maxp5ga0", "0x4E", 0},
			{"maxp5ga1", "0x4E", 0},
			{"maxp5gha0", "0x4E", 0},
			{"maxp5gha1", "0x4E", 0},
			{"maxp5gla0", "0x3E", 0},
			{"maxp5gla1", "0x3E", 0},
			{"mcs5ghpo0", "0x0000", 0},
			{"mcs5ghpo1", "0xa820", 0},
			{"mcs5ghpo2", "0x2222", 0},
			{"mcs5ghpo3", "0xa822", 0},
			{"mcs5ghpo4", "0x0000", 0},
			{"mcs5ghpo5", "0xa820", 0},
			{"mcs5ghpo6", "0x2222", 0},
			{"mcs5ghpo7", "0xa822", 0},
			{"mcs5glpo0", "0x0000", 0},
			{"mcs5glpo1", "0x2000", 0},
			{"mcs5glpo2", "0x0000", 0},
			{"mcs5glpo3", "0x2000", 0},
			{"mcs5glpo4", "0x0000", 0},
			{"mcs5glpo5", "0x2000", 0},
			{"mcs5glpo6", "0x0000", 0},
			{"mcs5glpo7", "0x2000", 0},
			{"mcs5gpo0", "0x0000", 0},
			{"mcs5gpo1", "0xa820", 0},
			{"mcs5gpo2", "0x2222", 0},
			{"mcs5gpo3", "0xa822", 0},
			{"mcs5gpo4", "0x0000", 0},
			{"mcs5gpo5", "0xa820", 0},
			{"mcs5gpo6", "0x2222", 0},
			{"mcs5gpo7", "0xa822", 0},
			{"ofdm5ghpo0", "0x0000", 0},
			{"ofdm5ghpo1", "0x2000", 0},
			{"ofdm5glpo0", "0x0000", 0},
			{"ofdm5glpo1", "0x0000", 0},
			{"ofdm5gpo0", "0x0000", 0},
			{"ofdm5gpo1", "0x2000", 0},
			{"pa5ghw0a0", "0xfeda", 0},
			{"pa5ghw0a1", "0xff18", 0},
			{"pa5ghw1a0", "0x1612", 0},
			{"pa5ghw1a1", "0x1661", 0},
			{"pa5ghw2a0", "0xfabe", 0},
			{"pa5ghw2a1", "0xfafe", 0},
			{"pa5glw0a0", "0xFEF9", 0},
			{"pa5glw0a1", "0xFF31", 0},
			{"pa5glw1a0", "0x154B", 0},
			{"pa5glw1a1", "0x1517", 0},
			{"pa5glw2a0", "0xFAFD", 0},
			{"pa5glw2a1", "0xFB2F", 0},
			{"pa5gw0a0", "0xFEF9", 0},
			{"pa5gw0a1", "0xff31", 0},
			{"pa5gw1a0", "0x164B", 0},
			{"pa5gw1a1", "0x1697", 0},
			{"pa5gw2a0", "0xFADD", 0},
			{"pa5gw2a1", "0xfb08", 0},
			{"pdetrange5g", "4", 0},
			{"regrev", "0", 0},
			{"rxchain", "3", 0},
			{"sromrev", "8", 0},
			{"stbc5ghpo/stbc5glpo/stbc5gpo/stbc2gpo", "0x0", 0},
			{"stbcpo", "0", 0},
			{"triso5g", "3", 0},
			{"tssipos5g", "1", 0},
			{"txchain", "3", 0},
			{"wdup405ghpo/wdup405glpo/wdup405gpo/wdup402gpo", "0x0", 0},
			{0, 0, 0}
		};

		set_extra_param(wndr3400v3_sb_1_params, "sb/1/%s");
		set_extra_param(wndr3400v3_pci_1_1_params, "pci/1/1/%s");

		xstart("gpio", "disable", "16"); /* turn on Power LED (active LOW); GPIO 16 controls state (on/off) and GPIO 14 controls color, see led.c */
		xstart("gpio", "enable", "21"); /* turn on USB supply (active HIGH) */
		break;

#endif /* TCONFIG_BLINK */

	/*
	 * add here MIPS models if WL_BSS_INFO_VERSION >= 108
	 */

#endif /* WL_BSS_INFO_VERSION >= 108 */
	} /* switch (model) */

	if (name) {
		nvram_set("t_fix1", name);
		if (ver && strcmp(ver, "")) {
			snprintf(s, sizeof(s), "%s %s v%s", mfr, name, ver);
		}
		else {
			snprintf(s, sizeof(s), "%s %s", mfr, name);
		}
	}
	else {
		snprintf(s, sizeof(s), "%s %d/%s/%s/%s/%s", mfr, check_hw_type(), nvram_safe_get("boardtype"), nvram_safe_get("boardnum"), nvram_safe_get("boardrev"), nvram_safe_get("boardflags"));
		s[64] = 0;
	}
	nvram_set("t_model_name", s);
	nvram_set("pa0maxpwr", "400"); /* allow Tx power up tp 400 mW, needed for ND only */

	snprintf(s, sizeof(s), "0x%lX", features);
	nvram_set("t_features", s);

	/* note: set wan_ifnameX if wan_ifname needs to be overriden */
	if (nvram_is_empty("wan_ifnameX")) {
		nvram_set("wan_ifnameX", ((strtoul(nvram_safe_get("boardflags"), NULL, 0) & BFL_ENETVLAN) || (check_hw_type() == HW_BCM4712)) ? "vlan1" : "eth1");
	}

	nvram_set("wan_get_dns", "");
	nvram_set("wan_get_domain", "");
	nvram_set("wan_ppp_get_ip", "0.0.0.0");
	nvram_set("action_service", "");
	nvram_set("jffs2_format", "0");
	nvram_set("rrules_radio", "-1");
	nvram_unset("https_crt_gen");
	nvram_unset("log_wmclear");
#ifdef TCONFIG_IPV6
	nvram_set("ipv6_get_dns", "");
#endif
#ifdef TCONFIG_MEDIA_SERVER
	nvram_unset("ms_rescan");
#endif
	if (nvram_get_int("http_id_gen") == 1)
		nvram_unset("http_id");

	nvram_unset("sch_rboot_last");
	nvram_unset("sch_rcon_last");
	nvram_unset("sch_c1_last");
	nvram_unset("sch_c2_last");
	nvram_unset("sch_c3_last");

	nvram_set("brau_state", "");
	if ((features & SUP_BRAU) == 0)
		nvram_set("script_brau", "");
	if ((features & SUP_SES) == 0)
		nvram_set("sesx_script", "");
	if ((features & SUP_1000ET) == 0)
		nvram_set("jumbo_frame_enable", "0");

	/* compatibility with old versions */
	if (nvram_match("wl_net_mode", "disabled")) {
		nvram_set("wl_radio", "0");
		nvram_set("wl_net_mode", "mixed");
	}

	return 0;
}

#ifndef TCONFIG_BCMARM
/* Get the special files from nvram and copy them to disc.
 * These were files saved with "nvram setfile2nvram <filename>".
 * Better hope that they were saved with full pathname.
 */
static void load_files_from_nvram(void)
{
	char *name, *cp;
	int ar_loaded = 0;
	char buf[NVRAM_SPACE];

	if (nvram_getall(buf, sizeof(buf)) != 0)
		return;

	for (name = buf; *name; name += strlen(name) + 1) {
		if (strncmp(name, "FILE:", 5) == 0) { /* this special name marks a file to get */
			if ((cp = strchr(name, '=')) == NULL)
				continue;
			*cp = 0;
			logmsg(LOG_INFO, "loading file '%s' from nvram", name + 5);
			nvram_nvram2file(name, name + 5);
			if (memcmp(".autorun", cp - 8, 9) == 0) 
				++ar_loaded;
		}
	}
	/* start any autorun files that may have been loaded into one of the standard places. */
	if (ar_loaded != 0)
		run_nvscript(".autorun", NULL, 3);
}
#endif

static inline void set_jumbo_frame(void)
{
	int enable = nvram_get_int("jumbo_frame_enable");

	/*
	 * 0x40 JUMBO frame page
	 * JUMBO Control Register
	 * 0x01 REG_JUMBO_CTRL (Port Mask (bit i == port i enabled), bit 24 == GigE always enabled)
	 * 0x05 REG_JUMBO_SIZE
	 */
#ifdef TCONFIG_BCMARM
	eval("et", "robowr", "0x40", "0x01", enable ? "0x010001ff" : "0x00", "4"); /* set enable flag for arm (32 bit) */
#else
	/* at mips branch we set the enable flag arleady at bcmrobo.c --> so nothing to do here right now */
	//eval("et", "robowr", "0x40", "0x01", enable ? "0x1f" : "0x00"); /* set enable flag for mips */
#endif
	if (enable) {
		eval("et", "robowr", "0x40", "0x05", nvram_safe_get("jumbo_frame_size")); /* set the packet size */
	}
}

static inline void set_kernel_panic(void)
{
	/* automatically reboot after a kernel panic */
	f_write_string("/proc/sys/kernel/panic", "3", 0, 0);
	f_write_string("/proc/sys/kernel/panic_on_oops", "3", 0, 0);
}

static inline void set_kernel_memory(void)
{
	f_write_string("/proc/sys/vm/overcommit_memory", "2", 0, 0); /* Linux kernel will not overcommit memory */
	f_write_string("/proc/sys/vm/overcommit_ratio", "100", 0, 0); /* allow userspace to commit up to 100% of total memory */
}

#ifdef TCONFIG_USB
static inline void tune_min_free_kbytes(void)
{
	struct sysinfo info;

	memset(&info, 0, sizeof(struct sysinfo));
	sysinfo(&info);
	if (info.totalram >= (TOMATO_RAM_HIGH_END * 1024)) { /* Router with 256 MB RAM and more */
		f_write_string("/proc/sys/vm/min_free_kbytes", "20480", 0, 0);  /* 20 MByte */
	}
	else if (info.totalram >= (TOMATO_RAM_MID_END * 1024)) { /* Router with 128 MB RAM */
		f_write_string("/proc/sys/vm/min_free_kbytes", "14336", 0, 0); /* 14 MByte */
	}
	else if (info.totalram >= (TOMATO_RAM_LOW_END * 1024)) { /* Router with 64 MB RAM */
		f_write_string("/proc/sys/vm/min_free_kbytes", "8192", 0, 0); /* 8 MByte */
	}
	else if (info.totalram >= (TOMATO_RAM_VLOW_END * 1024)) {
		/* If we have 32MB+ RAM, tune min_free_kbytes
		 * to reduce page allocation failure errors.
		 */
		f_write_string("/proc/sys/vm/min_free_kbytes", "1024", 0, 0); /* 1 MByte */
	}
}
#endif

static void sysinit(void)
{
	static int noconsole = 0;
	static const time_t tm = 0;
	int hardware;
	unsigned int i;
	DIR *d;
	struct dirent *de;
	char s[256];
	char t[256];

	mount("proc", "/proc", "proc", 0, NULL);
	mount("tmpfs", "/tmp", "tmpfs", 0, NULL);
	mount("devfs", "/dev", "tmpfs", MS_MGC_VAL | MS_NOATIME, NULL);
	mknod("/dev/null", S_IFCHR | 0666, makedev(1, 3));
	mknod("/dev/console", S_IFCHR | 0600, makedev(5, 1));
	mount("sysfs", "/sys", "sysfs", MS_MGC_VAL, NULL);
	mkdir("/dev/shm", 0777);
	mkdir("/dev/pts", 0777);
	mknod("/dev/pts/ptmx", S_IRWXU|S_IFCHR, makedev(5, 2));
	mknod("/dev/pts/0", S_IRWXU|S_IFCHR, makedev(136, 0));
	mknod("/dev/pts/1", S_IRWXU|S_IFCHR, makedev(136, 1));
	mount("devpts", "/dev/pts", "devpts", MS_MGC_VAL, NULL);

	if (console_init())
		noconsole = 1;

	stime(&tm);

	static const char *mkd[] = { "/tmp/etc", "/tmp/var", "/tmp/home", "/tmp/mnt", "/tmp/splashd",
	                             "/tmp/share", "/tmp/var/wwwext", "/tmp/var/wwwext/cgi-bin",
	                             "/var/webmon", "/var/log", "/var/run", "/var/tmp", "/var/lib", "/var/lib/misc",
	                             "/var/spool", "/var/spool/cron", "/var/spool/cron/crontabs",
	                             NULL
	};

	umask(0);
	for (i = 0; mkd[i]; ++i) {
		mkdir(mkd[i], 0755);
	}

	mkdir("/var/lock", 0777);
	mkdir("/var/tmp/dhcp", 0777);
	mkdir("/home/root", 0700);
	chmod("/tmp", 0777);
	f_write("/etc/hosts", NULL, 0, 0, 0644); /* blank */
	f_write("/etc/fstab", NULL, 0, 0, 0644); /* blank */
	simple_unlock("cron");
	simple_unlock("firewall");
	simple_unlock("restrictions");
	umask(022);

	if ((d = opendir("/rom/etc")) != NULL) {
		while ((de = readdir(d)) != NULL) {
			if (de->d_name[0] == '.')
				continue;

			snprintf(s, sizeof(s), "%s/%s", "/rom/etc", de->d_name);
			snprintf(t, sizeof(t), "%s/%s", "/etc", de->d_name);
			symlink(s, t);
		}
		closedir(d);
	}
	symlink("/proc/mounts", "/etc/mtab");

#ifdef TCONFIG_SAMBASRV
	if ((d = opendir("/usr/codepages")) != NULL) {
		while ((de = readdir(d)) != NULL) {
			if (de->d_name[0] == '.')
				continue;

			snprintf(s, sizeof(s), "/usr/codepages/%s", de->d_name);
			snprintf(t, sizeof(t), "/usr/share/%s", de->d_name);
			symlink(s, t);
		}
		closedir(d);
	}
#endif

	static const char *dn[] = { "null", "zero", "random", "urandom", "full", "ptmx", "nvram", NULL };
	for (i = 0; dn[i]; ++i) {
		snprintf(s, sizeof(s), "/dev/%s", dn[i]);
		chmod(s, 0666);
	}
	chmod("/dev/gpio", 0660);

	set_action(ACT_IDLE);

	for (i = 0; defenv[i]; ++i) {
		putenv(defenv[i]);
	}

	eval("hotplug2", "--coldplug");
	start_hotplug2();

	if (!noconsole) {
		printf("\n\nHit ENTER for console...\n\n");
		run_shell(1, 0);
	}

	check_bootnv();

#ifdef TCONFIG_IPV6
	/* disable IPv6 by default on all interfaces */
	f_write_procsysnet("ipv6/conf/default/disable_ipv6", "1");
#endif

	for (i = 0; i < sizeof(fatalsigs) / sizeof(fatalsigs[0]); i++) {
		signal(fatalsigs[i], handle_fatalsigs);
	}
	signal(SIGCHLD, handle_reap);

	/* ctf must be loaded prior to any other modules */
	if (nvram_invmatch("ctf_disable", "1"))
		modprobe("ctf");

#ifdef TCONFIG_BCMNAT
	if (nvram_invmatch("bcmnat_disable", "1"))
		modprobe("bcm_nat");
#endif

#ifdef TCONFIG_EMF
	modprobe("emf");
	modprobe("igs");
#endif

	switch (hardware = check_hw_type()) {
	case HW_BCM4785:
		modprobe("bcm57xx");
		break;
	default:
		modprobe("et");
		break;
	}

	restore_defaults(); /* restore default if necessary */
	init_nvram();

	set_jumbo_frame(); /* enable or disable jumbo_frame and set jumbo frame size */

	/* load after init_nvram */
#ifdef TCONFIG_USBAP
	load_wl(); /* for non USBAP see function start_lan() */
#endif

	//config_loopback(); /* see function start_lan() */

	klogctl(8, NULL, nvram_get_int("console_loglevel"));

#ifdef TCONFIG_USB
	tune_min_free_kbytes();
#endif

	set_kernel_panic(); /* reboot automatically when the kernel panics and set waiting time */
	set_kernel_memory(); /* set overcommit_memory and overcommit_ratio */

	setup_conntrack();
	set_host_domain_name();

	set_tz();

	eval("buttons");

#if defined(TCONFIG_BLINK) || defined(TCONFIG_BCMARM) /* RT-N+ */
	/* enable LED for LAN / Bridge */
	eval("blink_br");
#endif

	if (!noconsole)
		xstart("console");

	/* startup LED setup, see GUI (admin-buttons.asp) */
	i = nvram_get_int("sesx_led");
	led(LED_AMBER, (i & 1) != 0);
	led(LED_WHITE, (i & 2) != 0);
	led(LED_AOSS, (i & 4) != 0);
	led(LED_BRIDGE, (i & 8) != 0);
	led(LED_DIAG, LED_ON);
}

int init_main(int argc, char *argv[])
{
	unsigned int i;
	int state;
	sigset_t sigset;

	/* failsafe? */
	nvram_unset("debug_rc_svc");

	sysinit();

	sigemptyset(&sigset);
	for (i = 0; i < sizeof(initsigs) / sizeof(initsigs[0]); i++) {
		sigaddset(&sigset, initsigs[i]);
	}
	sigprocmask(SIG_BLOCK, &sigset, NULL);

#ifdef DEBUG_NOISY
	nvram_set("debug_logeval", "1");
	nvram_set("debug_cprintf", "1");
	nvram_set("debug_cprintf_file", "1");
	nvram_set("debug_ddns", "1");
#endif

	/* disable QoS and BWL when CTF is ON */
	if (nvram_invmatch("ctf_disable", "1")) {
		if (nvram_get_int("qos_enable"))
			nvram_set("qos_enable", "0");
		if (nvram_get_int("bwl_enable"))
			nvram_set("bwl_enable", "0");
	}

#ifdef TCONFIG_BCMNAT
	/* disable BWL when bcm_nat is ON */
	if (nvram_invmatch("bcmnat_disable", "1")) {
		if (nvram_get_int("bwl_enable"))
			nvram_set("bwl_enable", "0");
	}
#endif

	/* reset ntp status */
	nvram_set("ntp_ready", "0");

	start_jffs2();

	/* set unique system id */
	if (!f_exists("/etc/machine-id"))
		system("echo $(nvram get lan_hwaddr) | md5sum | cut -b -32 > /etc/machine-id");

	state = SIGUSR2; /* START */

	for (;;) {
		switch (state) {
		case SIGUSR1: /* USER1: service handler */
			exec_service();
			break;

		case SIGHUP:  /* RESTART */
		case SIGINT:  /* STOP */
		case SIGQUIT: /* HALT */
		case SIGTERM: /* REBOOT */
			led(LED_DIAG, LED_ON);
			unlink("/var/notice/sysup");

			if (nvram_match( "webmon_bkp", "1" )) {
				xstart( "/usr/sbin/webmon_bkp", "hourly" ); /* make a copy before halt/reboot router */
			}

			run_nvscript("script_shut", NULL, 10);

			stop_services();
			stop_wan();
			stop_arpbind();
			stop_lan();
			stop_vlan();

			if ((state == SIGTERM /* REBOOT */) ||
			    (state == SIGQUIT /* HALT */)) {
				remove_storage_main(1);
				stop_usb();
				stop_syslog();

				shutdn(state == SIGTERM /* REBOOT */);
				sync(); sync(); sync();
				exit(0);
			}
			if (state == SIGINT) { /* STOP */
				break;
			}

			/* SIGHUP (RESTART) falls through */

			//nvram_set("wireless_restart_req", "1"); /* restart wifi twice to make sure all is working ok! not needed right now M_ars */
			logmsg(LOG_INFO, "FreshTomato RESTART ...");

		case SIGUSR2: /* START */
			stop_syslog();
			start_syslog();

#ifndef TCONFIG_BCMARM
			load_files_from_nvram();
#endif

			int fd = -1;
			fd = file_lock("usb"); /* hold off automount processing */
			start_usb();

			xstart("/usr/sbin/mymotd", "init");
			run_nvscript("script_init", NULL, 2);

			file_unlock(fd); /* allow to process usb hotplug events */
#ifdef TCONFIG_USB
			/*
			 * On RESTART some partitions can stay mounted if they are busy at the moment.
			 * In that case USB drivers won't unload, and hotplug won't kick off again to
			 * remount those drives that actually got unmounted. Make sure to remount ALL
			 * partitions here by simulating hotplug event.
			 */
			if (state == SIGHUP) /* RESTART */
				add_remove_usbhost("-1", 1);
#endif

			log_segfault();
			create_passwd();
			start_vlan();
			start_lan();
			start_arpbind();
			mwan_state_files();
			start_services();

			if (restore_defaults_fb /*|| nvram_match("wireless_restart_req", "1")*/) {
				logmsg(LOG_INFO, "%s: FreshTomato WiFi restarting ... (restore defaults)", nvram_safe_get("t_model_name"));
				restore_defaults_fb = 0; /* reset */
				//nvram_set("wireless_restart_req", "0");
				restart_wireless();
			}
			else {
				start_wl();

				/* If a virtual SSID is disabled, it requires two initialisations */
				if (foreach_wif(1, NULL, disabled_wl)) {
					logmsg(LOG_INFO, "%s: FreshTomato WiFi restarting ... (virtual SSID disabled)", nvram_safe_get("t_model_name"));
					restart_wireless();
				}
			}
			/*
			 * last one as ssh telnet httpd samba etc can fail to load until start_wan_done
			 */
			start_wan();

			if (wds_enable()) {
				/* Restart NAS one more time - for some reason without
				 * this the new driver doesn't always bring WDS up.
				 */
				stop_nas();
				start_nas();
			}

			logmsg(LOG_INFO, "%s: FreshTomato %s", nvram_safe_get("t_model_name"), tomato_version);

			led(LED_DIAG, LED_OFF);

#ifndef TCONFIG_BCMARM
			switch(get_model()) {
#ifdef TCONFIG_BLINK /* RTN/RTAC */
				case MODEL_WTR54GS:
					gpio_write(1 << 2, 1); /* clear power light blinking */
					break;
				case MODEL_WNDR3400:
				case MODEL_WNDR3700v3:
				case MODEL_WNDR4000:
					led(LED_WHITE, LED_ON);
					led(LED_AOSS, LED_ON);
					break;
				case MODEL_R6300V1:
					gpio_write(1 << 1, 0); /* turn on left half of LOGO light */
					gpio_write(1 << 9, 0); /* turn on right half of LOGO light */
					gpio_write(1 << 2, 0); /* turn on power light (green) */
					break;
#endif /* TCONFIG_BLINK */
				case MODEL_E4200:
					led(LED_DIAG, LED_ON); /* turn on cisco LOGO light (again) */
					break;
			}
#endif /* !TCONFIG_BCMARM */

			notice_set("sysup", "");
			break;
		}

		if (!g_upgrade) {
			chld_reap(0); /* periodically reap zombies. */
			check_services();
		}

		sigwait(&sigset, &state);
	}

	return 0;
}

int reboothalt_main(int argc, char *argv[])
{
	int reboot = (strstr(argv[0], "reboot") != NULL);
	int def_reset_wait = 30;

	cprintf(reboot ? "Rebooting...\n" : "Shutting down...\n");
	kill(1, reboot ? SIGTERM : SIGQUIT);

	int wait = nvram_get_int("reset_wait") ? : def_reset_wait;
	/* In the case we're hung, we'll get stuck and never actually reboot.
	 * The only way out is to pull power.
	 * So after 'reset_wait' seconds (default: 30), forcibly crash & restart.
	 */
	if (fork() == 0) {
		if ((wait < 10) || (wait > 120))
			wait = 10;

		f_write("/proc/sysrq-trigger", "s", 1, 0 , 0); /* sync disks */
		sleep(wait);
		cprintf("Still running... Doing machine reset.\n");
#ifdef TCONFIG_USB
		remove_usb_module();
#endif
		f_write("/proc/sysrq-trigger", "s", 1, 0 , 0); /* sync disks */
		sleep(1);
		f_write("/proc/sysrq-trigger", "b", 1, 0 , 0); /* machine reset */
	}

	return 0;
}
