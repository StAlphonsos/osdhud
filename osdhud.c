/*
 * Copyright (C) 2014,2015 by attila <attila@stalphonsos.com>
 *
 * Permission to use, copy, modify, and distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * osdhud - xosd-based heads up system status display
 */

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stdint.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <net/if.h>
#include <sys/un.h>
#include <xosd.h>
#include <Judy.h>
#include <err.h>
#include "config.h"
#include "version.h"
#include "movavg.h"
#include "osdhud.h"

volatile sig_atomic_t interrupted = 0;	/* got a SIGINT */
volatile sig_atomic_t restart_req = 0;	/* got a SIGHUP */
#ifdef SIGINFO
volatile sig_atomic_t bang_bang = 0;	/* got a SIGINFO */
#endif

#define WHITESPACE " \t\n\r"

#define safe_percent(a,b) b? a/b: 0;	/* don't divide by zero */
#define ipercent(v) (int)(100 * (v))	/* 0.05 -> 5 */
/*#define ENABLE_ALERTS 1*/
    
char *
err_str(struct osdhud_state *state, int err)
{
	state->errbuf[0] = 0;
	(void) strerror_r(err,state->errbuf,sizeof(state->errbuf));
	return state->errbuf;
}

void
die(struct osdhud_state *state, char *msg)
{
	if (state->foreground)
		err(1,"%s",NULLS(msg));
	syslog(LOG_ERR,"FATAL: %s",msg);
	closelog();
	exit(1);
}

void
update_net_statistics(struct osdhud_state *state, u_int64_t delta_ibytes,
		      u_int64_t delta_obytes, u_int64_t delta_ipackets,
		      u_int64_t delta_opackets)
{
	if (state->delta_t) {
		float dt = (float)state->delta_t / 1000.0;

		state->net_ikbps =
			(movavg_add(state->ikbps_ma,delta_ibytes) / dt)/KILO;
		state->net_okbps =
			(movavg_add(state->okbps_ma,delta_obytes) / dt)/KILO;
		state->net_ipxps =
			movavg_add(state->ipxps_ma,delta_ipackets) / dt;
		state->net_opxps =
			movavg_add(state->opxps_ma,delta_opackets) / dt;

		DSPEW("net %s bytes in  += %llu -> %.2f / %f secs => %.2f",
		      state->net_iface,delta_ibytes,movavg_val(state->ikbps_ma),
		      dt,state->net_ikbps);
		DSPEW("net %s bytes out += %llu -> %.2f / %f secs => %.2f",
		      state->net_iface,delta_obytes,movavg_val(state->okbps_ma),
		      dt,state->net_okbps);
		DSPEW("net %s packets   in  += %llu -> %.2f / %f secs => %.2f",
		      state->net_iface,delta_ipackets,
		      movavg_val(state->ipxps_ma),dt,state->net_ipxps);
		DSPEW("net %s packets   out += %llu -> %.2f / %f secs => %.2f",
		      state->net_iface,delta_opackets,
		      movavg_val(state->opxps_ma),dt,state->net_opxps);

	}
}

void
clear_net_statistics(struct osdhud_state *state)
{
	state->net_ikbps = state->net_ipxps =
		state->net_okbps = state->net_opxps = 0;
	state->net_tot_ibytes = state->net_tot_obytes =
		state->net_tot_ipackets = state->net_tot_opackets = 0;
	state->net_peak_kbps = state->net_peak_pxps = 0;
	movavg_clear(state->ikbps_ma);
	movavg_clear(state->okbps_ma);
	movavg_clear(state->ipxps_ma);
	movavg_clear(state->opxps_ma);
}

void
update_disk_statistics(struct osdhud_state *state, u_int64_t delta_rbytes,
		       u_int64_t delta_wbytes, u_int64_t delta_reads,
		       u_int64_t delta_writes)
{
	if (state->delta_t) {
		float dt = (float)state->delta_t / 1000.0;

		state->disk_rkbps =
			(movavg_add(state->rbdisk_ma,delta_rbytes)/dt)/KILO;
		state->disk_wkbps =
			(movavg_add(state->wbdisk_ma,delta_wbytes)/dt)/KILO;
		state->disk_rxps =
			movavg_add(state->rxdisk_ma,delta_reads) / dt;
		state->disk_wxps =
			movavg_add(state->wxdisk_ma,delta_writes) / dt;
	}
}

unsigned long
time_in_microseconds(void)
{
	struct timeval tv = { .tv_sec=0, .tv_usec=0 };

	if (gettimeofday(&tv,NULL)) {
		perror("gettimeofday");
		exit(1);
	}
	return (tv.tv_sec * 1000000) + tv.tv_usec;
}

unsigned long
time_in_milliseconds(void)
{
	unsigned long usecs = time_in_microseconds();

	return usecs / 1000;
}

/*
 * Turn a number of seconds elapsed into a human-readable string.
 * e.g. "10 days 1 hour 23 mins 2 secs".  We have an snprintf-style
 * API: buf,bufsiz specify a buffer and its size.  If the resulting
 * string's length is less than bufsiz then buf was big enough and
 * contains a NUL-terminatd string whose length is our return value.
 * If our return value is greater than bufsiz then buf was too small.
 * Sadly our return value is not useful for determining how large to
 * make buf, but in practice anything over 35 bytes should be large
 * enough.
 */
int
elapsed(char *buf, int bufsiz, unsigned long secs)
{
	unsigned long days;
	unsigned long hours;
	unsigned long mins;
	size_t nleft = bufsiz;
	size_t off = 0;

	days = secs / SECSPERDAY;
	secs %= SECSPERDAY;

	hours = secs / SECSPERHOUR;
	secs %= SECSPERHOUR;

	mins = secs / SECSPERMIN;
	secs %= SECSPERMIN;

#define catsup(var)                                                     \
        if (var) {                                                      \
		size_t _catsup_n;					\
		char _catsup_u[10] = #var;				\
		if (off) {						\
			if (nleft <= 2) {				\
				off = bufsiz+1;				\
				goto DONE;				\
			}						\
			(void) strlcat(&buf[off]," ",nleft);		\
			nleft -= 1;					\
			off += 1;					\
		}							\
		if (var == 1) _catsup_u[strlen(#var)-1] = 0;		\
		_catsup_n = snprintf(					\
			&buf[off],nleft,"%lu %s",var,_catsup_u);	\
		if (_catsup_n >= nleft) {				\
			off = bufsiz+1;					\
			goto DONE;					\
		}							\
		off += _catsup_n;					\
		nleft -= _catsup_n;					\
        }

	catsup(days);
	catsup(hours);
	catsup(mins);
	catsup(secs);
#undef catsup

DONE:
	return off;
}

/*
 * Probe data and gather statistics
 *
 * This function invokes probe_xxx() routines defined in the per-OS
 * modules, e.g. openbsd.c, freebsd.c
 */
void
probe(struct osdhud_state *state)
{
	unsigned long now = time_in_milliseconds();

	state->delta_t = now - state->last_t;
	state->last_t = now;
	probe_load(state);
	probe_mem(state);
	probe_swap(state);
	probe_net(state);
	probe_disk(state);
	probe_battery(state);
	probe_temperature(state);
	probe_uptime(state);
}

/*
 * Display Routines
 */

char *
reading_to_color(float percent)
{
	static char *colors[] = {
		"green", "yellow", "orange", "red", "violet"
	};
	int severity;

	if (percent <= 0.25)
		severity = 0;
	else if (percent <= 0.5)
		severity = 1;
	else if (percent <= 0.75)
		severity = 2;
	else if (percent <= 1.0)
		severity = 3;
	else
		severity = 4;
	return colors[severity];
}

xosd *
osd_to_use(struct osdhud_state *state, int do_color, float reading)
{
	int off;
	xosd *osd;

	off = state->disp_line++;
	assert(off < state->nlines);
	osd = state->osds[off];
	if (do_color) {
		char *color;

		color = reading_to_color(reading);
		if (xosd_set_colour(osd, color))
			syslog(LOG_WARNING,
			       "could not set osd[%d] color to %s (%f)",
				off,color,reading);
	}
	return osd;
}

void
display_load(struct osdhud_state *state)
{
	float percent = safe_percent(state->load_avg,state->max_load_avg);

	xosd_display(osd_to_use(state,1,percent),0,XOSD_printf,"load: %.2f",
		     state->load_avg);
	if (state->max_load_avg)
		xosd_display(osd_to_use(state,1,percent),0,XOSD_percentage,
			     ipercent(percent));
}

void
display_mem(struct osdhud_state *state)
{
	xosd_display(osd_to_use(state,1,state->mem_used_percent),0,
		     XOSD_printf,"mem: %d%%",ipercent(state->mem_used_percent));
	xosd_display(osd_to_use(state,1,state->mem_used_percent),0,
		     XOSD_percentage,ipercent(state->mem_used_percent));
}

void
display_swap(struct osdhud_state *state)
{
	if (!state->nswap)
		return;
	xosd_display(osd_to_use(state,1,state->swap_used_percent),0,XOSD_printf,
		     "swap: %d%%",ipercent(state->swap_used_percent));
	xosd_display(osd_to_use(state,1,state->swap_used_percent),0,
		     XOSD_percentage,ipercent(state->swap_used_percent));
}

void
display_net(struct osdhud_state *state)
{
	char *iface = state->net_iface? state->net_iface: "-";
	int left, off, n;
	char label[256];
	char details[1024];
	float net_kbps = state->net_ikbps + state->net_okbps;
	float net_pxps = state->net_ipxps + state->net_opxps;
	char unit = 'k';
	float unit_div = 1.0;
	float max_kbps = ((float)state->net_speed_mbits / 8.0) * KILO;
	float raw_percent = safe_percent(net_kbps,max_kbps);
	int percent = ipercent(raw_percent);

	VSPEW("display_net %s net_speed_mbits %d max_kbps %f",
	      iface,state->net_speed_mbits,max_kbps);
	memset(label,0,sizeof(label));
	memset(details,0,sizeof(details));
	/*
	 * If there are gigabytes or megabytes flying by then
	 * switch to the appropriate unit.
	 */
	if (net_kbps > state->net_peak_kbps)
		state->net_peak_kbps = net_kbps;
	if (net_pxps > state->net_peak_pxps)
		state->net_peak_pxps = net_pxps;
	if (net_kbps > MEGA) {
		unit = 'g';
		unit_div = MEGA;
	} else if (net_kbps > KILO) {
		unit = 'm';
		unit_div = KILO;
	}
	/* Put together the label */
	if (!max_kbps)
		assert_snprintf(label,"net (%s):",iface);
	else
		assert_snprintf(label,"net (%s %dmb/s):",iface,
				state->net_speed_mbits);
	/* Put together the details string, as short as possible */
	if ((unsigned long)net_kbps) {
		left = sizeof(details);
		off = 0;
		if (max_kbps && percent) {
			if (percent <= 100)
				n = snprintf(&details[off],left,"%3d%% ",
					     percent);
			else
				/*
				 * This must be because max_kbps is
				 * wrong, which can happen if my guess
				 * is wrong or if the user gives us a
				 * value for -X that is wrong.
				 */
				n = snprintf(&details[off],left,"> 100%%(!) ");
			assert(n < left);
			left -= n;
			off += n;
		}
		n = snprintf(&details[off],left,"%lu %cB/s (%lu px/s)",
			     (unsigned long)(net_kbps/unit_div),unit,
			     (unsigned long)net_pxps);
		assert(n < left);
		left -= n;
		off += n;
	} else {
		assert_strlcpy(details,TXT__QUIET_);
	}

	xosd_display(osd_to_use(state,1,raw_percent),0,XOSD_printf,"%s %s",
		     label,details);
	if (max_kbps)
		xosd_display(osd_to_use(state,1,raw_percent),0,XOSD_percentage,
			     percent);
}

void
display_disk(struct osdhud_state *state)
{
}

void
display_battery(struct osdhud_state *state)
{
	char *charging = state->battery_state[0] ?
		state->battery_state : TXT__UNKNOWN_;
	char mins[128] = { 0 };
	float battery_used;

	if (state->battery_missing)
		return;
	if (state->battery_time < 0) {
		assert_strlcpy(mins,TXT_TIME_UNKNOWN);
	} else {
		assert_elapsed(mins,state->battery_time*60);
	}
	/* We want the color based on the percentage used, not remaining: */
	battery_used = 1.0 - ((float)state->battery_life / 100.0);
	xosd_display(osd_to_use(state,1,battery_used),0,XOSD_printf,
		     "battery: %s, %d%% charged (%s)",charging,
		     state->battery_life,mins);
	xosd_display(osd_to_use(state,1,battery_used),0,XOSD_percentage,
		     state->battery_life);
}

void
display_temperature(struct osdhud_state *state)
{
}

void
display_uptime(struct osdhud_state *state)
{
	if (state->sys_uptime) {
		unsigned long secs = state->sys_uptime;
		char upbuf[64] = { 0 };

		assert_elapsed(upbuf,secs);
		xosd_display(osd_to_use(state,0,0),0,XOSD_printf,
			     "%s up %s",state->hostname,upbuf);
	}
}

void
display_message(struct osdhud_state *state)
{
	if (!state->message_seen && state->message[0]) {
		xosd_display(osd_to_use(state,0,0),0,XOSD_printf,"%s",
			     state->message);
		state->message_seen = 1;
	} else {
		xosd_display(osd_to_use(state,0,0),0,XOSD_printf,"");
	}
}

void
display_hudmeta(struct osdhud_state *state)
{
	unsigned int now = time_in_milliseconds();
	unsigned int dt = now - state->t0_msecs;
	unsigned int left = (dt < state->duration_msecs) ?
		state->duration_msecs - dt : 0;
	unsigned int left_secs = (left + 500) / 1000;
	xosd *osd = state->osd_bot;
	char now_str[512] = { 0 };
	char left_s[512] = { 0 };

	if (state->time_fmt) {
		time_t now = time(NULL);
		struct tm ltime = {
			.tm_sec = 0, .tm_min = 0, .tm_hour = 0, .tm_mday = 0,
			.tm_mon = 0, .tm_year = 0, .tm_wday = 0, .tm_yday = 0,
			.tm_isdst = 0, .tm_gmtoff = 0, .tm_zone = NULL
		};

		(void) localtime_r(&now,&ltime);
		assert(strftime(now_str,sizeof(now_str),state->time_fmt,
				&ltime) > 0);
	}

	if (state->stuck) {
		char *txt = (state->message[0] && state->alerts_mode) ?
			TXT__ALERT_ : TXT__STUCK_;

		assert_strlcpy(left_s,txt);
	} else if (state->countdown) {
		if (left_secs)
			assert_snprintf(left_s,"hud down in %d",left_secs);
		else
			assert_strlcpy(left_s,TXT__BLINK_);
	}
	if (state->time_fmt)
		xosd_display(osd,0,XOSD_printf,"%s%s%s%s",now_str,
			     left_s[0]? " [": "",left_s,left_s[0]? "]":"");
	else if (left_s[0])
		xosd_display(osd,0,XOSD_printf,"[%s]",left_s);
}

/*
 * Display the HUD
 */
void
display(struct osdhud_state *state)
{
	state->disp_line = 0;
	display_uptime(state);
	display_load(state);
	display_mem(state);
	display_swap(state);
	display_net(state);
	display_disk(state);
	display_battery(state);
	display_temperature(state);
	display_message(state);
	display_hudmeta(state);
}

#define OSDHUD_OPTIONS "d:p:P:vf:s:i:T:X:knDUSNFCwhgaAt?"
#define USAGE_MSG "usage: %s [-vgtkFDUSNCwh?] [-d msec] [-p msec] [-P msec]\n\
              [-f font] [-s path] [-i iface]\n\
   -v verbose      | -k kill server | -F run in foreground\n\
   -D down HUD     | -U up HUD      | -S stick HUD | -N unstick HUD\n\
   -g debug mode   | -t toggle mode | -w don't show swap\n\
   -n don't show HUD on startup     | -C display HUD countdown\n\
   -h,-? display this\n\
   -T fmt   show time using strftime fmt (def: %%Y-%%m-%%d %%H:%%M:%%S)\n\
   -d msec  leave HUD visible for millis (def: 2000)\n\
   -p msec  millis between sampling when HUD is up (def: 100)\n\
   -P msec  millis between sampling when HUD is down (def: 100)\n\
   -f font  (def: "DEFAULT_FONT")\n\
   -s path  path to Unix-domain socket (def: ~/.%s_%s.sock)\n\
   -i iface network interface to watch\n\
   -X mb/s  fix max net link speed in mbit/sec (def: query interface)\n"

int
usage(struct osdhud_state *state, char *msg)
{
	int fail = 0;

	if (!state->argv0) {
		syslog(LOG_WARNING,"client message error: %s",msg);
		fail = 1;
	} else {
		if (msg)
			fprintf(stderr,"%s ERROR: %s\n",state->argv0,msg);
		else {
			fprintf(stderr,"%s %s: system status HUD\n",
				state->argv0,VERSION);
			fprintf(stderr,USAGE_MSG,state->argv0,
				state->argv0,VERSION);
		}
		exit(1);
	}
	return fail;
}

/*
 * Parse command-line arguments into struct osdhud_state structure
 */
int
parse(struct osdhud_state *state, int argc, char **argv)
{
	int fail = 0;
	int ch = -1;

	if (!state->argv0) {
		optreset = 1;                   /* c.f. man getopt(3) */
		optind = 1;
	}
	DBG2("parse: argc=%d argv@%p",argc,argv);
	while ((ch = getopt(argc,argv,OSDHUD_OPTIONS)) != -1) {
		DBG1("option ch: %c",ch);
		switch (ch) {
		case 'd':		/* duration in milliseconds */
			if (sscanf(optarg,"%d",&state->display_msecs) != 1)
				fail = usage(state,"bad value for -d");
			DBG2("parsed -%c %d",ch,state->display_msecs);
			break;
		case 'p':      /* inter-probe pause in milliseconds */
			if (sscanf(optarg,"%d",&state->short_pause_msecs) != 1)
				fail = usage(state,"bad value for -p");
			DBG2("parsed -%c %d",ch,state->short_pause_msecs);
			break;
		case 'P':
			if (sscanf(optarg,"%d",&state->long_pause_msecs) != 1)
				fail = usage(state,"bad value for -P");
			DBG2("parsed -%c %d",ch,state->long_pause_msecs);
			break;
		case 'T':
			state->time_fmt = strdup(optarg);
			DBG2("parsed -%c %s",ch,state->time_fmt);
			break;
		case 'v':                       /* verbose */
			state->verbose++;
			DBG2("parsed -%c => %d",ch,state->verbose);
			break;
		case 'f':                       /* font */
			state->font = strdup(optarg);
			DBG2("parsed -%c %s",ch,state->font);
			break;
		case 's':                       /* path to unix socket */
			state->sock_path = strdup(optarg);
			DBG2("parsed -%c %s",ch,state->sock_path);
			break;
		case 'i':
			/* network iface of interest */
			state->net_iface = strdup(optarg);
			DBG2("parsed -%c %s",ch,state->net_iface);
			break;
		case 'X':
			if (sscanf(optarg,"%d",&state->net_speed_mbits) != 1)
				fail = usage(state,"bad value for -X");
			DBG2("parsed -%c %d",ch,state->net_speed_mbits);
			break;
		case 'k':
			state->kill_server = 1;
			DBG1("parsed -%c",ch);
			break;
		case 'D':
			state->down_hud = 1;
			DBG1("parsed -%c",ch);
			break;
		case 'U':
			state->up_hud = 1;
			DBG1("parsed -%c",ch);
			break;
		case 'S':
			state->stick_hud = 1;
			DBG1("parsed -%c",ch);
			break;
		case 'N':
			state->unstick_hud = 1;
			DBG1("parsed -%c",ch);
			break;
		case 'F':
			state->foreground = 1;
			DBG1("parsed -%c",ch);
			break;
		case 'g':
			state->debug = 1;
			DBG1("parsed -%c",ch);
			break;
		case 'C':
			state->countdown = 1;
			DBG1("parsed -%c",ch);
			break;
		case 'w':
			state->nswap = 0;
			DBG1("parsed -%c",ch);
			break;
		case 'n':
			state->quiet_at_start = 1;
			DBG1("parsed -%c",ch);
			break;
		case 't':
			state->toggle_mode = 1;
			DBG1("parsed -%c",ch);
			break;
		case 'a':
			state->alerts_mode = 1;
			DBG1("parsed -%c",ch);
			break;
		case 'A':
			state->cancel_alerts = 1;
			DBG1("parsed -%c",ch);
			break;
		case '?':                       /* help */
		case 'h':                       /* ditto */
			fail = usage(state,NULL);
			break;
		default:                        /* similar */
			fail = usage(state,"unknown option");
			break;
		}
		if (fail)
			break;
	}
	return fail;
}

void
init_state(struct osdhud_state *state, char *argv0)
{
	int i;

	if (!argv0)
		state->argv0 = NULL;
	else {
		char *base = rindex(argv0,'/');

		state->argv0 = base ? base+1 : argv0;
	}
	state->kill_server = state->down_hud = state->up_hud =
		state->countdown = state->stick_hud = state->unstick_hud =
		state->foreground = state->hud_is_up = state->server_quit =
		state->stuck = state->verbose = state->debug =
		state->toggle_mode = state->alerts_mode =
		state->cancel_alerts = 0;
	state->pid = 0;
	state->sock_fd = -1;
	state->sock_path = NULL;
#ifdef DEFAULT_TIME_FMT
	state->time_fmt = strdup(DEFAULT_TIME_FMT);
#endif
	memset((void *)&state->addr,0,sizeof(state->addr));
	state->font = NULL;
	state->net_iface = NULL;
	state->net_speed_mbits = 0;
	state->net_tot_ipackets = state->net_tot_ierr =
		state->net_tot_opackets = state->net_tot_oerr =
		state->net_tot_ibytes = state->net_tot_obytes = 0;
	state->delta_t = 0;
	state->pos_x = DEFAULT_POS_X;
	state->pos_y = DEFAULT_POS_Y;
	state->nlines = 0;
	state->line_height = DEFAULT_LINE_HEIGHT;
	state->width = DEFAULT_WIDTH;
	state->display_msecs = DEFAULT_DISPLAY;
	state->t0_msecs = 0;
	state->nswap = DEFAULT_NSWAP;
	state->min_battery_life = DEFAULT_MIN_BATTERY_LIFE;
	state->max_load_avg = DEFAULT_MAX_LOAD_AVG;
	state->max_mem_used = DEFAULT_MAX_MEM_USED;
	state->short_pause_msecs = DEFAULT_SHORT_PAUSE;
	state->long_pause_msecs = DEFAULT_LONG_PAUSE;
	state->net_movavg_wsize = DEFAULT_NET_MOVAVG_WSIZE;
	state->load_avg = state->mem_used_percent =
		state->swap_used_percent = 0;
	state->per_os_data = NULL;
	state->net_ikbps = state->net_ipxps =
		state->net_okbps = state->net_opxps = 0;
	state->net_peak_kbps = state->net_peak_pxps = 0;
	state->ikbps_ma = state->ipxps_ma =
		state->okbps_ma = state->opxps_ma = NULL;
	state->rxdisk_ma = state->wxdisk_ma =
		state->rbdisk_ma = state->wbdisk_ma = NULL;
	state->disk_rkbps = state->disk_wkbps =
		state->disk_rxps = state->disk_wxps = 0;
	state->battery_missing = 0;
	state->battery_life = 0;
	memset(state->battery_state,0,sizeof(state->battery_state));
	state->battery_time = 0;
	state->last_t = 0;
	state->first_t = 0;
	state->sys_uptime = 0;
	for (i = 0; i < ARRAY_SIZE(state->osds); i++)
		state->osds[i] = NULL;
	state->osd_bot = NULL;
	state->disp_line = 0;
	memset(state->message,0,sizeof(state->message));
	state->message_seen = 0;
	memset(state->errbuf,0,sizeof(state->errbuf));
}

struct osdhud_state *
create_state(struct osdhud_state *state)
{
	struct osdhud_state *new_state =
		(struct osdhud_state *)malloc(sizeof(struct osdhud_state));

	if (new_state) {
		init_state(new_state,NULL);

#define set_field(fld) new_state->fld = state->fld
#define dup_field(fld) new_state->fld = state->fld? strdup(state->fld): NULL
#define cpy_field(fld)					\
		memcpy((void *)&new_state->fld,		\
		       (void *)&state->fld,		\
			sizeof(new_state->fld))

		set_field(kill_server);
		set_field(down_hud);
		set_field(up_hud);
		set_field(stick_hud);
		set_field(unstick_hud);
		set_field(hud_is_up);
		set_field(server_quit);
		set_field(stuck);
		set_field(debug);
		set_field(toggle_mode);
		set_field(alerts_mode);
		set_field(cancel_alerts);
		set_field(countdown);
		dup_field(sock_path);
		cpy_field(addr);
		dup_field(font);
		dup_field(net_iface);
		set_field(net_speed_mbits);
		dup_field(time_fmt);
		set_field(pos_x);
		set_field(pos_y);
		set_field(width);
		set_field(display_msecs);
		set_field(short_pause_msecs);
		set_field(long_pause_msecs);

#undef cpy_field
#undef dup_field
#undef set_field
	}
	return new_state;
}

void
cleanup_state(struct osdhud_state *state)
{
	int i;

	if (state) {
		if (state->osds[0]) {
			for (i = 0; i < state->nlines; i++) {
				xosd_destroy(state->osds[i]);
				state->osds[i] = NULL;
			}
			xosd_destroy(state->osd_bot);
		}
		probe_cleanup(state);
		if (state->time_fmt) {
			free(state->time_fmt);
			state->time_fmt = NULL;
		}
		free(state->sock_path);
		state->sock_path = NULL;
		free(state->font);
		state->font = NULL;
		free(state->net_iface);
		state->net_iface = NULL;
		movavg_free(state->ikbps_ma);
		state->ikbps_ma = NULL;
		movavg_free(state->okbps_ma);
		state->okbps_ma = NULL;
		movavg_free(state->ipxps_ma);
		state->ipxps_ma = NULL;
		movavg_free(state->opxps_ma);
		state->opxps_ma = NULL;
		movavg_free(state->rxdisk_ma);
		state->rxdisk_ma = NULL;
		movavg_free(state->wxdisk_ma);
		state->wxdisk_ma = NULL;
		movavg_free(state->rbdisk_ma);
		state->rbdisk_ma = NULL;
		movavg_free(state->wbdisk_ma);
		state->wbdisk_ma = NULL;
	}
}

void
free_state(struct osdhud_state *dispose)
{
	cleanup_state(dispose);
	free(dispose);
}

/*
 * Split str into words delimited by whitespace; returns number of words
 */
int
split(char *str, char ***out_words)
{
	char *copy = NULL;
	char *toke = NULL;
	char *splitz[100];
	char **splits = NULL;
	int ntoke = 0;
	int i = 0;

	*out_words = NULL;
	if (!str)
		return 0;
	copy = strdup(str);
	assert(copy);
	toke = strsep(&copy,WHITESPACE);
	while (toke && (ntoke < ARRAY_SIZE(splitz))) {
		splitz[ntoke++] = toke;
		toke = strsep(&copy,WHITESPACE);
	}
	if (toke)
		syslog(LOG_WARNING,"split too many tokens (> "SIZEOF_F") '%s'",
		       ARRAY_SIZE(splitz),str);
	splits = (char **)calloc(1+ntoke,sizeof(char *));
	assert(splits);
	splits[0] = strdup("osdhud");       /* getopt(3) */
	for (i = 0; i < ntoke; i++)
		splits[i+1] = strdup(splitz[i]);
	free(copy);
	*out_words = splits;
	return ntoke+1;
}

/*
 * Free the results of calling split()
 */
void
free_split(int argc, char **argv)
{
	if (argv) {
		int i = 0;

		for (i = 0; i < argc; i++)
			free(argv[i]);
		free(argv);
	}
}

void
cleanup_daemon(struct osdhud_state *state)
{
	if (state->sock_fd >= 0) {
		close(state->sock_fd);
		if (unlink(state->sock_path))
			syslog(LOG_ERR,"could not unlink socket %s: %s (#%d)",
			       state->sock_path,err_str(state,errno),errno);
		state->sock_fd = -1;
		cleanup_state(state);
	}
	closelog();
}

void
clear_net_info(struct osdhud_state *state)
{
	clear_net_statistics(state);
	state->net_speed_mbits = 0;
}

/*
 * Attempt to receive a message via our control socket and act on it
 */
int
handle_message(struct osdhud_state *state)
{
	int client = -1;
	int retval = 0;
	struct sockaddr_un cli;
	socklen_t cli_sz = sizeof(cli);

	if (state->verbose)
		syslog(LOG_WARNING,"accepting conn on sock # %d, HUD is %s",
			state->sock_fd,state->hud_is_up ? "UP": "DOWN");
	client = accept(state->sock_fd,(struct sockaddr *)&cli,&cli_sz);
	if (client < 0)
		syslog(LOG_WARNING,"accept(#%d) failed: %s (#%d)",
		       state->sock_fd, err_str(state,errno), errno);
	else {
		/* the client just sends its command-line args to the daemon */
		FILE *stream = fdopen(client, "r");
		char msgbuf[OSDHUD_MAX_MSG_SIZE+1] = { 0 };
		char *msg = fgets(msgbuf,OSDHUD_MAX_MSG_SIZE,stream);

		if (!msg)
			syslog(LOG_WARNING,"error reading client: %s (#%d)",
				err_str(state,errno),errno);
		else {
			int argc = 0;
			char **argv = NULL;
			struct osdhud_state *foo = create_state(state);
			size_t msglen = strlen(msg);

			/* The message is just command-line args */
			if (msglen && (msg[msglen-1] == '\n'))
				msg[msglen-1] = 0;
			argc = split(msg,&argv);
			if (argc < 1) {
				syslog(LOG_ERR,"too many args in "
				       SIZE_T_F" bytes: '%.50s%s'",msglen,
				       msg,(msglen>50)? "...": "");
				cleanup_daemon(state);
				exit(1);
			}
			if (state->verbose) {
				int i = 0;

				for (i = 0; i < argc; i++)
					syslog(LOG_WARNING,"msg arg#%d: '%s'",
					       i,argv[i]);
			}
			if (!argc)
				syslog(LOG_WARNING,"malformed msg buf |%.*s|",
					OSDHUD_MAX_MSG_SIZE,msgbuf);
			else if (parse(foo,argc,argv))
				syslog(LOG_WARNING,"parse error for '%s'",msg);
			else {
				/* Successfully parsed msg */

#define setparam(nn,ff)							\
				do {					\
					if (state->verbose)		\
						syslog(LOG_WARNING,	\
						       #nn" "ff" => "ff, \
						       state->nn,foo->nn); \
					state->nn = foo->nn;		\
				} while(0)
#define setstrparam(nn)							\
				do {					\
					if (state->verbose)		\
						syslog(LOG_WARNING,	\
						       #nn" %s => %s",	\
						       NULLS(state->nn),\
						       NULLS(foo->nn));	\
					free(state->nn);		\
					state->nn = foo->nn ?		\
						strdup(foo->nn) : NULL; \
				} while (0)
#define is_different(nn) (((state->nn && foo->nn) &&			\
			   strcmp(state->nn,foo->nn)) ||		\
			  (state->nn && !foo->nn) ||			\
			  (!state->nn && foo->nn))
#define maybe_setstrparam(nn)						\
				do {					\
					if (is_different(nn)) {		\
						setstrparam(nn);	\
					}				\
				} while (0)
#define maybe_setstrparam2(nn,cc)					\
				do {					\
					if (is_different(nn)) {		\
						setstrparam(nn);	\
						cc;			\
					}				\
				} while (0)
				
				/* -k trumps all else */
				if (foo->kill_server) {
					state->server_quit = retval = 1;
					goto DONE;
				}
				setparam(display_msecs,"%d");
				if (!state->hud_is_up || state->toggle_mode)
					retval = 1;
				else
					/* hud is up: bump duration */
					state->duration_msecs +=
						state->display_msecs;
				setparam(long_pause_msecs,"%d");
				maybe_setstrparam(font);
				maybe_setstrparam(time_fmt);
				maybe_setstrparam2(net_iface,
						   clear_net_info(state));

#undef maybe_setstrparam2
#undef maybe_setstrparam
#undef is_different
#undef setstrparam
#undef setparam
				if (foo->toggle_mode) {
					/* -t overrides -S/-N */
					foo->stick_hud = foo->unstick_hud = 0;
					retval = 1;
					state->stuck = !state->stuck;
				} else if (foo->up_hud || foo->stick_hud) {
					retval = !state->hud_is_up;
					state->stuck = foo->stick_hud ? 1 : 0;
				} else if (foo->down_hud)
					retval = state->hud_is_up ? 1 : 0;
				else if (foo->unstick_hud)
					state->stuck = 0;
				state->countdown = foo->countdown;
				if (foo->cancel_alerts)
					state->alerts_mode = 0;
				else if (foo->alerts_mode)
					state->alerts_mode = 1;
				if (foo->net_speed_mbits)
					state->net_speed_mbits = 
						foo->net_speed_mbits;
			}
		DONE:
			free_state(foo);
			free_split(argc,argv);
		}
		fclose(stream);
		close(client);
		if (state->verbose)
			syslog(LOG_WARNING,"done handling client");
	}
	if (state->verbose)
		syslog(LOG_WARNING,"handle_message => %d, is_up:%d",
		       retval,state->hud_is_up);
	return retval;
}

#ifdef ENABLE_ALERTS
int
check_alerts(struct osdhud_state *state)
{
	int nalerts = 0;

	memset(state->message,0,sizeof(state->message));
	state->message_seen = 0;

#define catmsg(xx)						\
	do {							\
		if (state->message[0])				\
			assert_strlcat(state->message,", ");	\
		assert_strlcat(state->message,xx);		\
		nalerts++;					\
	} while (0);

	if (!state->battery_missing &&
	    (state->battery_life<state->min_battery_life))
		catmsg(TXT_ALERT_BATTERY_LOW);
	if (state->max_load_avg &&
	    (ipercent(state->load_avg/state->max_load_avg)>40))
		catmsg(TXT_ALERT_LOAD_HIGH);
	if (state->max_mem_used &&
	    (state->mem_used_percent > state->max_mem_used))
		catmsg(TXT_ALERT_MEM_LOW);

#undef catmsg

	return nalerts;
}
#endif /* ENABLE_ALERTS */

/*
 * Pause for the appropriate amount of time given our state
 *
 * If we are displaying the HUD then pause for the short inter-sample
 * time (usually 100msec).  If we are not displaying the HUD then
 * pause for the long inter-sample time (1 second).  We use select(2)
 * to also watch for events on the control socket.
 *
 * Our return value decides whether or not we exit the loop we are in:
 * either the HUD's-Up short-time loop or the HUD's-Down long-time
 * sampling loop.  In the HUD's-Up case we return true if the duration
 * of display has expired.  In the HUD's-Down case we return true if
 * we receive a message on the control socket telling us to bring up
 * the HUD.  If we receive a message telling us to shut down then we
 * set state->server_quit to 1 and return true: this shuts us down
 * cleanly.
 */
int
check(struct osdhud_state *state)
{
	int done = 0;
	int quit_loop = 0;
	int pause_msecs = state->hud_is_up ? state->short_pause_msecs :
		state->long_pause_msecs;

	if (state->verbose > 1)
		syslog(LOG_WARNING,"check: pause is %d, HUD is %s",
			pause_msecs,state->hud_is_up ? "UP": "DOWN");
	do {
		struct timeval tout;
		int x;
		unsigned long b4;
		int pause_secs;
		int pause_usecs;
		fd_set rfds;
		int have_alerts;

		FD_ZERO(&rfds);
		FD_SET(state->sock_fd,&rfds);
		/* set up our timeout */
		pause_secs = pause_msecs / 1000;
		pause_usecs = (pause_msecs - (pause_secs*1000)) * 1000;
		tout.tv_sec = pause_secs;
		tout.tv_usec = pause_usecs;
		/* wait for I/O on the socket or a timeout */
		b4 = time_in_milliseconds();
		x = select(state->sock_fd+1,&rfds,NULL,NULL,&tout);
		if (x < 0) {                    /* error */
			syslog(LOG_ERR,"select() => %s (#%d)",
			       err_str(state,errno),errno);
			cleanup_daemon(state);
			exit(1);
		} else if (x > 0) {
			/* command */
			quit_loop = handle_message(state);
			if (!quit_loop) {
				/* client didn't tell us to quit so continue */
				int dt = time_in_milliseconds() - b4;

				/* whittle down the time left */
				if (dt >= pause_msecs)
					done = 1;
				else
					pause_msecs -= dt;
			} /* else message told us to quit loop */
		} else {
                        /* timeout */
			done = 1;
			if (state->hud_is_up && !state->toggle_mode) {
				/* if hud is up, see if it is time to down it */
				int now = time_in_milliseconds();
				int delta_d = (now - state->t0_msecs);

				if (!state->stuck &&
				    (delta_d >= state->duration_msecs))
					quit_loop = 1;
			} /* else done=1 will force sample and pause again */
		}
		/* Deal with signals-based flags */
		if (interrupted) {
			syslog(LOG_WARNING,"interrupted - bailing out");
			done = quit_loop = state->server_quit = 1;
		}
		if (restart_req)
			syslog(LOG_WARNING,
			       "restart requested - not doing anything");
		interrupted = restart_req = 0;
#ifdef SIGINFO
		if (bang_bang) {
			syslog(LOG_WARNING,"bang, bang");
			done = 1;
			if (!state->hud_is_up)
				quit_loop = 1;
			else
				state->duration_msecs += state->display_msecs;
		}
		bang_bang = 0;
#endif
#ifdef ENABLE_ALERTS
		have_alerts = check_alerts(state);
#else
		have_alerts = 0;
#endif /* ENABLE_ALERTS */
		if (have_alerts && state->alerts_mode && !state->hud_is_up) {
			quit_loop = done = 1;
			state->stuck = 1; /* alerts force them to unstick...? */
		}
	} while (!done && !quit_loop);
	return quit_loop;
}

/*
 * Turn state into equivalent command-line options to send to running instance
 */
int
pack_message(struct osdhud_state *state, char **out_msg)
{
	int len = 1, off = 0, left = 0;
	char *packed = NULL;

	if (state->kill_server)
		len += 3;
	if (state->down_hud)
		len += 3;
	if (state->up_hud)
		len += 3;
	if (state->unstick_hud)
		len += 3;
	if (state->stick_hud)
		len += 3;
	if (state->font)
		len += 4 + strlen(state->font);
	if (state->net_iface)
		len += 4 + strlen(state->net_iface);
	if (state->pos_x != DEFAULT_POS_X)
		len += 10;
	if (state->pos_y != DEFAULT_POS_Y)
		len += 10;
	if (state->width != DEFAULT_WIDTH)
		len += 10;
	if (state->display_msecs)
		len += 10;
	if (state->short_pause_msecs)
		len += 10;
	if (state->long_pause_msecs)
		len += 10;
	packed = (char *)malloc(len);
	memset((void *)packed,0,len);
	off = 0;
	left = len;

#define lead (!off ? "": " ")
#define single_opt(f,o)                                                 \
	if (state->f) {							\
		int x = snprintf(&packed[off],left,"%s-%s",lead,o);	\
		if (x < 0)						\
			die(state,"pack: " o " failed !?");		\
		off += x;						\
		left -= x;						\
	}
#define integer_opt(f,o)                                                \
	do  {								\
		int x=snprintf(&packed[off],left,"%s-%s %d",lead,o,state->f); \
		if (x < 0)						\
			die(state,"pack: " o " failed !?");		\
		off += x;						\
		left -= x;						\
	} while (0);
#define string_opt(f,o)                                                 \
	if (state->f) {							\
		int x=snprintf(&packed[off],left,"%s-%s %s",lead,o,state->f); \
		if (x < 0)						\
			die(state,"pack: " o " failed !?");		\
		off += x;						\
		left -= x;						\
	}

	single_opt(verbose,"v");
	single_opt(debug,"g");
	single_opt(kill_server,"k");
	single_opt(down_hud,"D");
	single_opt(up_hud,"U");
	single_opt(stick_hud,"S");
	single_opt(unstick_hud,"N");
	single_opt(toggle_mode,"t");
	single_opt(alerts_mode,"a");
	single_opt(cancel_alerts,"A");
	single_opt(countdown,"C");
	string_opt(font,"f");
	string_opt(net_iface,"i");
	if (state->net_speed_mbits) {
		integer_opt(net_speed_mbits,"X");
	}
	integer_opt(display_msecs,"d");
	integer_opt(short_pause_msecs,"p");
	integer_opt(long_pause_msecs,"P");

#undef string_opt
#undef integer_opt
#undef single_opt

	assert_strlcat_z(&packed[off],"\n",left);

	*out_msg = packed;
	return strlen(packed);
}

/*
 * Try to kick an existing instance of ourselves
 *
 * If we can make contact with an existing instance of ourselves via
 * the control socket then send a message to it and return true.
 */
int kicked(struct osdhud_state *state)
{
	int sock_fd = -1;
	struct stat sock_stat;

	if (state->foreground)
		/* run in foreground - don't even try */
		return 0;
	sock_fd = socket(PF_UNIX,SOCK_STREAM,0);
	if (sock_fd < 0) {
		perror("socket");
		exit(1);
	}
	if (!connect(sock_fd, (struct sockaddr *)&state->addr,
		     sizeof(state->addr))) {
		int len = 0;
		char *msg = NULL;
		int nw = -1;

		/* we use command-line args as our rpc format */
		len = pack_message(state,&msg);
		nw = write(sock_fd,msg,len);
		if (nw != len) {
			if (nw < 0)
				perror("write to server");
			else
				fprintf(stderr,
					"%s: short write to %s (%d != %d)\n",
					state->argv0,state->sock_path,nw,len);
			exit(1);
		}
		free(msg);
		close(sock_fd);
		return 1;
	}
	if ((errno == ECONNREFUSED) && !stat(state->sock_path,&sock_stat)) {
		/* Connection refused but socket exists - daemon died */
		if (unlink(state->sock_path)) {
			perror("unlink stale socket");
			exit(1);
		}
		/* Now we continue as normal and daemonize */
	} else if ((errno != ENOENT) && state->verbose)
		perror(state->sock_path);
	return 0;
}

/*
 * Try to fork a daemon child to run the HUD
 *
 * We return true if we are running in the daemonized child process
 * and false otherwise ala fork(2).
 */
int
forked(struct osdhud_state *state)
{
	int child = -1;
	int fd = -1;

	if (state->foreground)
		return 1;
	child = fork();
	if (child) {
		state->pid = child;
		return 0;
	}
	fd = open("/dev/tty",O_RDWR|O_NOCTTY);
	if (fd >= 0) {
		(void) ioctl(fd,TIOCNOTTY,NULL);
		close(fd);
	}
	(void) setsid();
	freopen("/dev/null","r",stdin);
	freopen("/dev/null","w",stdout);
	freopen("/dev/null","w",stderr);
	return 1;
}

xosd *
create_big_osd(struct osdhud_state *state, char *font)
{
	xosd *osd;

	osd = xosd_create(1);
	if (!osd) {
		SPEWE("could not create osd display");
		exit(1);
	}
	xosd_set_font(osd,font);
	xosd_set_outline_offset(osd,1);
	xosd_set_shadow_offset(osd,4);
	xosd_set_outline_colour(osd,"black");
	xosd_set_align(osd,XOSD_left);
	xosd_set_pos(osd,XOSD_top);
	xosd_set_horizontal_offset(osd,state->pos_x);
	xosd_set_vertical_offset(osd,state->pos_y +
				 (state->line_height * state->nlines++));
	xosd_set_bar_length(osd,state->width);

	return osd;
}

xosd *
create_small_osd(struct osdhud_state *state, char *font)
{
	xosd *osd = xosd_create(1);

	if (!osd) {
		SPEWE("could not create second osd display");
		exit(1);
	}
	xosd_set_font(osd,font);
	xosd_set_outline_offset(osd,1);
	xosd_set_shadow_offset(osd,4);
	xosd_set_outline_colour(osd,"black");
	xosd_set_align(osd,XOSD_right);
	xosd_set_pos(osd,XOSD_bottom);

	return osd;
}

void
hud_up(struct osdhud_state *state)
{
	char *font = state->font ? state->font : DEFAULT_FONT;
	int i;

	if (state->verbose > 1)
		syslog(LOG_WARNING,"HUD coming up");

	if (!state->osds[0]) {
		for (i = 0; i < ARRAY_SIZE(state->osds); i++) {
			state->osds[i] = create_big_osd(state,font);
			assert(state->osds[i]);
			xosd_hide(state->osds[i]);
		}
		state->osd_bot = create_small_osd(state,font);
		xosd_hide(state->osd_bot);
	}
	for (i = 0; i < state->nlines; i++)
		if (xosd_show(state->osds[i])) {
			syslog(LOG_ERR,"xosd_show failed #%d: %s",i,xosd_error);
			exit(1);
		}
	if (xosd_show(state->osd_bot)) {
		syslog(LOG_ERR,"xosd_show failed (#2): %s",xosd_error);
		exit(1);
	}

	state->hud_is_up = 1;
	state->t0_msecs = time_in_milliseconds();
	state->duration_msecs = state->display_msecs;
}

void
hud_down(struct osdhud_state *state)
{
	int i;

	if (state->verbose)
		syslog(LOG_WARNING,"HUD coming down");

	for (i = 0; i < state->nlines; i++)
		xosd_hide(state->osds[i]);
	xosd_hide(state->osd_bot);

	state->hud_is_up = 0;
}

void
handle_signal(int signo, siginfo_t *info, void *ptr)
{
	switch (info->si_signo) {
	case SIGINT:
	case SIGTERM:
		interrupted = 1;
		break;
	case SIGHUP:
		restart_req = 1;
		break;
#ifdef SIGINFO
	case SIGINFO:
		bang_bang = 1;
		break;
#endif
	default:
		syslog(LOG_ERR,"received unexpected signal #%d",info->si_signo);
		break;
	}
}

void
init_signals(struct osdhud_state *state)
{
	struct sigaction sact;

	sact.sa_flags = SA_SIGINFO;
	sigemptyset(&sact.sa_mask);
	sact.sa_sigaction = handle_signal;
	if (sigaction(SIGHUP,&sact,NULL))
		die(state,err_str(state,errno));
	if (sigaction(SIGINT,&sact,NULL))
		die(state,err_str(state,errno));
	if (sigaction(SIGTERM,&sact,NULL))
		die(state,err_str(state,errno));
#ifdef SIGINFO
	if (sigaction(SIGINFO,&sact,NULL))
		die(state,err_str(state,errno));
#endif
}

void
setup_daemon(struct osdhud_state *state)
{
	int syslog_flags = LOG_PID, i;

	if (gethostname(state->hostname,sizeof(state->hostname))) {
		perror("gethostname");
		exit(1);
	}
	for (i = 0; state->hostname[i]; i++)
		if (state->hostname[i] == '.') {
			state->hostname[i] = 0;
			break;
		}
	if (state->foreground)
		syslog_flags |= LOG_PERROR;
	openlog(state->argv0,syslog_flags,LOG_LOCAL0);
	if (state->verbose)
		syslog(LOG_INFO,"server starting; v%s",VERSION);
	state->sock_fd = socket(PF_UNIX,SOCK_STREAM,0);
	if (state->sock_fd < 0) {
		syslog(LOG_ERR,"could not create unix socket: %s (#%d)",
			err_str(state,errno),errno);
		closelog();
		exit(1);
	}
	if (bind(state->sock_fd, (struct sockaddr*)&state->addr,
		sizeof(state->addr))) {
		perror("bind");
		exit(1);
	}
	if (listen(state->sock_fd,100)) {
		perror("listen");
		exit(1);
	}
	if (chmod(state->addr.sun_path,0700)) {
		perror("chmod");
		exit(1);
	}
	init_signals(state);

	state->last_t = state->first_t = time_in_milliseconds();
	state->ikbps_ma = movavg_new(state->net_movavg_wsize);
	state->okbps_ma = movavg_new(state->net_movavg_wsize);
	state->ipxps_ma = movavg_new(state->net_movavg_wsize);
	state->opxps_ma = movavg_new(state->net_movavg_wsize);

	probe_init(state);                  /* per-OS probe init */
}

/*
 * osdhud - heads-up style system status display
 *
 * This command should be bound to some key in your window manager.
 * When invoked it brings up a heads-up display overlaid on the screen
 * via libosd.  It stays up for some configurable duration during
 * which it updates in real time, then disappears.  The default is for
 * the display to stay up for 2 seconds and update every 100
 * milliseconds.  The display includes load average, memory
 * utilization, swap utilization, network utilization, battery
 * lifetime and uptime.
 *
 * The idea is that just running us from a keybinding in the window
 * manager with no arguments should do something reasonable: the HUD
 * appears for a couple of seconds and fades away if nothing else is
 * done.  If we are invoked while the HUD is still up then it will
 * stay up longer.  This is intuitively what I want:
 *     more hit key -> more hud
 *     stop hit key -> no more hud
 * I call this the Caveman Theory of Human/Computer Interaction:
 * PUNCH COMPUTER TO MAKE IT GO.
 */
int
main(int argc, char **argv)
{
	struct osdhud_state state;

	init_state(&state,argv[0]);
	if (parse(&state,argc,argv))
		exit(1);  /* already complained to stderr */
#ifdef HAVE_SETPROCTITLE
	setproctitle("v.%s",VERSION);
#endif
	/* Setup unix-domain socket address for use below */
	if (!state.sock_path) {
		char *home = getenv("HOME");
		char *path = NULL;
		int path_len = 0;

		if (!home)
			usage(&state,"no -s and no homedir - giving up");
		path_len = asprintf(&path,"%s/.%s_%s.sock",home,state.argv0,
				    VERSION);
		if (path_len < 0)
			usage(&state,"could not mk default sock path... why?");
		if (state.verbose)
			fprintf(stderr,"[%s] socket: %s\n",state.argv0,path);
		state.sock_path = path;
	}
	state.addr.sun_family = AF_UNIX;
	assert_strlcpy(state.addr.sun_path,state.sock_path);

	/* Everything out here spews to stdout/stderr via (f)printf */
	if (kicked(&state)) {
		/* Already running: sent existing process a message */
		if (state.verbose)
			printf("%s: kicked existing osdhud\n",state.argv0);
	} else if (forked(&state)) {
		/* Everything in here spews to syslog */
		setup_daemon(&state);           /* init daemon state */
		if (!state.quiet_at_start)
			hud_up(&state);
		do {
			int toggle = 0;

			probe(&state);
			if (state.hud_is_up)
				display(&state);
			toggle = check(&state);
			if (!state.server_quit && toggle) {
				if (state.hud_is_up)
					hud_down(&state);
				else
					hud_up(&state);
			}
		} while (!state.server_quit);   /* told to quit - go away */
		if (state.verbose)
			syslog(LOG_WARNING,"server exiting");
		if (state.hud_is_up)
			hud_down(&state);
		cleanup_daemon(&state);
	} else if (state.verbose && !state.foreground)
		printf("%s: forked daemon pid %d\n",state.argv0,state.pid);
	exit(0);
}

/*
 * Local variables:
 * mode: c
 * c-file-style: "bsd"
 * tab-width: 8
 * indent-tabs-mode: t
 * End:
 */
