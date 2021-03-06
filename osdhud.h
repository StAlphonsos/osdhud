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

/* Data structures and macros for osdhud */

#if defined(__OpenBSD__) || defined(__FreeBSD__)
# define HAVE_SETPROCTITLE 1
#endif

#define SECSPERMIN      60
#define SECSPERHOUR     (SECSPERMIN*60)
#define SECSPERDAY      (SECSPERHOUR*24)

#define SIZEOF_F "%lu"			/* printf fmt for sizeof() */
#define SIZE_T_F SIZEOF_F		/* printf fmt for size_t */

#define ARRAY_SIZE(aa) (sizeof(aa)/sizeof(aa[0]))
#define NULLS(_x_) ((_x_) ? (_x_) : "NULL")
#define MAX_ALERTS_SIZE 1024

#define NLINES 16

/*
 * Application state
 */
struct osdhud_state {
	int		 kill_server:1;
	int		 down_hud:1;
	int		 up_hud:1;
	int		 stick_hud:1;
	int		 unstick_hud:1;
	int		 foreground:1;
	int		 hud_is_up:1;
	int		 server_quit:1;
	int		 stuck:1;
	int		 debug:1;
	int		 countdown:1;
	int		 quiet_at_start:1;
	int		 toggle_mode:1;
	int		 alerts_mode:1;
	int		 cancel_alerts:1;
	char		*argv0;
	char		 hostname[128];
	int		 pid;
	char		*sock_path;
	struct		 sockaddr_un addr;
	int		 sock_fd;
	char		*font;
	char		*net_iface;
	int		 net_speed_mbits;
	char		*time_fmt;
	char		*temp_sensor_name;
	double		 temperature;
	int		 nswap;
	int		 min_battery_life;
	float		 max_load_avg;
	float		 max_mem_used;
	float		 max_temperature;
	u_int64_t	 net_tot_ipackets;
	u_int64_t	 net_tot_ierr;
	u_int64_t	 net_tot_opackets;
	u_int64_t	 net_tot_oerr;
	u_int64_t	 net_tot_ibytes;
	u_int64_t	 net_tot_obytes;
	int		 delta_t;
	int		 pos_x;
	int		 pos_y;
	int		 nlines;
	int		 line_height;
	int		 width;
	int		 display_msecs;
	int		 duration_msecs;
	int		 t0_msecs;
	int		 short_pause_msecs;
	int		 long_pause_msecs;
	int		 net_movavg_wsize;
	int		 verbose;
	float		 load_avg;
	void		*per_os_data;
	struct		 movavg *ikbps_ma;
	float		 net_ikbps;
	struct		 movavg *okbps_ma;
	float		 net_okbps;
	struct		 movavg *ipxps_ma;
	float		 net_ipxps;
	struct		 movavg *opxps_ma;
	float		 net_opxps;
	float		 net_peak_kbps;
	float		 net_peak_pxps;
	struct		 movavg *rxdisk_ma;
	float		 disk_rkbps;
	struct		 movavg *wxdisk_ma;
	float		 disk_wkbps;
	struct		 movavg *rbdisk_ma;
	float		 disk_rxps;
	struct		 movavg *wbdisk_ma;
	float		 disk_wxps;
	float		 mem_used_percent;
	float		 swap_used_percent;
	int		 battery_missing:1;
	int		 battery_life;
	char		 battery_state[32];
	int		 battery_time;
	time_t		 uptime_secs;
        time_t		 last_t;
	time_t		 first_t;
	time_t		 sys_uptime;
	int		 message_seen:1;
	char		 message[MAX_ALERTS_SIZE];
	xosd		*osds[NLINES];
	int		 disp_line;
	xosd	        *osd_bot;
	char		 errbuf[1024];
};

#define KILO 1024
#define MEGA (KILO*KILO)
#define OSDHUD_MAX_MSG_SIZE 2048
/* XXX this introduces a dep on fonts/terminus; default should be in base */
#define DEFAULT_FONT "-xos4-terminus-medium-r-normal--32-320-72-72-c-160-iso8859-1"
/*#define DEFAULT_FONT "-adobe-helvetica-bold-r-normal-*-*-320-*-*-p-*-*-*"*/
#define DEFAULT_POS_X 10
#define DEFAULT_POS_Y 48
#define DEFAULT_LINE_HEIGHT 36
#define DEFAULT_WIDTH 50
#define DEFAULT_DISPLAY 2000
#define DEFAULT_SHORT_PAUSE 80
/*#define DEFAULT_LONG_PAUSE 1800*/
#define DEFAULT_LONG_PAUSE DEFAULT_SHORT_PAUSE
#define DEFAULT_TIME_FMT "%Y-%m-%d %H:%M:%S"
#define DEFAULT_NET_MOVAVG_WSIZE 6
#define DEFAULT_NSWAP 1
#define DEFAULT_MIN_BATTERY_LIFE 10
#define DEFAULT_MAX_LOAD_AVG 0.0
#define DEFAULT_MAX_MEM_USED 0.9
#define DEFAULT_MAX_TEMPERATURE 120

#define DBG1(fmt,arg1)                                                  \
    if (state->debug) {                                                 \
        if (state->foreground)                                          \
            printf(fmt,arg1);                                           \
        else                                                            \
            syslog(LOG_WARNING,fmt,arg1);                               \
    }

#define DBG2(fmt,arg1,arg2)                                             \
    if (state->debug) {                                                 \
        if (state->foreground)                                          \
            printf(fmt,arg1,arg2);                                      \
        else                                                            \
            syslog(LOG_WARNING,fmt,arg1,arg2);                          \
    }

#define SPEW(fmt,mem)                                                   \
    if (state->verbose) {                                               \
        if (state->foreground)                                          \
            printf(fmt"\n",state->mem);                                 \
        else                                                            \
            syslog(LOG_WARNING,fmt,state->mem);                         \
    }

#define VSPEW(fmt,...)                                                  \
    if (state->verbose) {                                               \
        if (state->foreground)                                          \
            printf(fmt "\n",##__VA_ARGS__);                             \
        else                                                            \
            syslog(LOG_WARNING,fmt,##__VA_ARGS__);                      \
    }

#define DSPEW(fmt,...)                                                  \
    if (state->debug) {                                                 \
        if (state->foreground)                                          \
            printf(fmt "\n",##__VA_ARGS__);                             \
        else                                                            \
            syslog(LOG_WARNING,fmt,##__VA_ARGS__);                      \
    }

#define SPEWE(msg)                                                      \
    if (state->verbose) {                                               \
        if (state->foreground)                                          \
            perror(msg);                                                \
        else                                                            \
            syslog(LOG_ERR,"%s",msg);                                   \
    }

/*
 * Shorthands for common idioms.  The _z versions take the size as an
 * argument, the non _z versions use sizeof(xx)
 */

#define assert_strlcpy_z(xx,yy,zz)	assert(strlcpy(xx,yy,zz) < zz)
#define assert_strlcpy(xx,yy)		assert_strlcpy_z(xx,yy,sizeof(xx))
#define assert_strlcat_z(xx,yy,zz)	assert(strlcat(xx,yy,zz) < zz)
#define assert_strlcat(xx,yy)		assert_strlcat_z(xx,yy,sizeof(xx))
#define assert_snprintf_z(xx,zz,ff,...)                                 \
    assert(snprintf(xx,zz,ff,##__VA_ARGS__) < zz)
#define assert_snprintf(xx,ff,...)                                      \
    assert(snprintf(xx,sizeof(xx),ff,##__VA_ARGS__) < sizeof(xx))
#define assert_elapsed(xx,ss)                                           \
    assert(elapsed(xx,sizeof(xx),ss) < sizeof(xx))

/*
 * TXT_xxx constants, should probably just internationalize properly
 */

#define TXT__QUIET_             "-quiet-"
#define TXT_TIME_UNKNOWN        "time unknown"
#define TXT__UNKNOWN_           "-unknown-"
#define TXT__STUCK_             "-stuck-"
#define TXT__ALERT_             "-alert-"
#ifdef BLINK
# define TXT__BLINK_            "-blink-"
#else
# define TXT__BLINK_		"hud down in 0"
#endif
#define TXT_ALERT_BATTERY_LOW   "BATTERY LOW"
#define TXT_ALERT_LOAD_HIGH     "HIGH LOAD"
#define TXT_ALERT_MEM_LOW       "MEMORY PRESSURE"

/*
 * Per-OS modules call in to these functions to report their
 * statistics for network and disk
 */

void update_net_statistics(struct osdhud_state *state, u_int64_t delta_ibytes,
			   u_int64_t delta_obytes, u_int64_t delta_ipackets,
			   u_int64_t delta_opackets);

void update_disk_statistics(struct osdhud_state *state, u_int64_t delta_rbytes,
			    u_int64_t delta_wbytes, u_int64_t delta_reads,
			    u_int64_t delta_writes);

/*
 * probe_xxx() function prototypes; each os-specific module
 * implements these, e.g. openbsd.c, freebsd.c.
 */

void probe_init(struct osdhud_state *);
void probe_cleanup(struct osdhud_state *);
void probe_load(struct osdhud_state *);
void probe_mem(struct osdhud_state *);
void probe_swap(struct osdhud_state *);
void probe_net(struct osdhud_state *);
/*void probe_disk(struct osdhud_state *);*/ /* XXX not yet */
void probe_battery(struct osdhud_state *);
void probe_temperature(struct osdhud_state *);
void probe_uptime(struct osdhud_state *);

void print_temperature_sensors(void); /* exported from per-os as well */

/*
 * Local variables:
 * mode: c
 * c-file-style: "bsd"
 * tab-width: 8
 * indent-tabs-mode: t
 * End:
 */
