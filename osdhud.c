/* -*- mode:c; c-basic-offset:4; tab-width:4; indent-tabs-mode:nil -*- */

/**
 * @file osdhud.c
 * @brief xosd-based heads up display
 *
 * This command should be bound to some key in your window manager.
 * When invoked it brings up a heads-up display overlaid on the screen
 * via libosd.  It stays up for some configurable duration during
 * which it updates in real time, then disappears.  The default is for
 * the display to stay up for 2 seconds and update every 100
 * milliseconds.  The display includes load average, memory
 * utilization, swap utilization, network utilization, battery
 * lifetime and uptime.
 */

/* ISC LICENSE:
 *
 * Copyright (C) 2014 by attila <attila@stalphonsos.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for
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

#include "osdhud.h"

int interrupted = 0;                    /* got a SIGINT */
int restart_req = 0;                    /* got a SIGHUP */
#ifdef SIGINFO
int bang_bang = 0;                      /* got a SIGINFO */
#endif

#define WHITESPACE " \t\n\r"
#define ipercent(v) ((int)(100 * (v)))
/*#define ENABLE_ALERTS 1*/

/*
 * Maintain moving averages.
 */

struct movavg *movavg_new(
    int                 wsize)
{
    struct movavg *ma;

    assert((wsize > 1) || (wsize <= MAX_WSIZE));
    ma = malloc(sizeof(*ma));
    assert(ma);
    ma->window_size = wsize;
    ma->off = ma->count = 0;
    ma->sum = 0;
    ma->window = (float *)calloc(wsize,sizeof(float));
    assert(ma->window);
    return ma;
}

void movavg_free(
    struct movavg      *ma)
{
    if (ma) {
        free(ma->window);
        free(ma);
    }
}

void movavg_clear(
    struct movavg      *ma)
{
    if (ma) {
        int i;

        for (i = 0; i < ma->window_size; i++)
            ma->window[i] = 0;
        ma->off = ma->count = 0;
        ma->sum = 0;
    }
}

float movavg_add(
    struct movavg      *ma,
    float               val)
{
    float result = 0;

    if (ma) {
        if (ma->count < ma->window_size)
            ma->count++;
        else {
            ma->off %= ma->window_size;
            ma->sum -= ma->window[ma->off]; /* the "moving" part */
        }
        ma->sum += val;
        ma->window[ma->off++] = val;
        assert(ma->count);
        result = ma->sum / ma->count;
    }
    return result;
}

float movavg_val(
    struct movavg      *ma)
{
    return (ma && ma->count) ? (ma->sum / ma->count) : 0;
}

/*
 * Utilities
 */
    
char *err_str(
    osdhud_state_t     *state,
    int                 err)
{
    state->errbuf[0] = 0;
    (void) strerror_r(err,state->errbuf,sizeof(state->errbuf));
    return state->errbuf;
}

static void die(
    osdhud_state_t     *state,
    char               *msg)
{
    if (state->foreground)
        fprintf(stderr,"[%s] ERROR: %s\n",state->argv0,msg);
    else
        syslog(LOG_ERR,"ERROR: %s",msg);
    exit(1);
}

void update_net_statistics(
    osdhud_state_t     *state,
    unsigned long       delta_ibytes,
    unsigned long       delta_obytes,
    unsigned long       delta_ipax,
    unsigned long       delta_opax)
{
    if (state->delta_t) {
        float dt = (float)state->delta_t / 1000.0;

        state->net_ikbps = (movavg_add(state->ikbps_ma,delta_ibytes) / dt)/KILO;
        state->net_okbps = (movavg_add(state->okbps_ma,delta_obytes) / dt)/KILO;
        state->net_ipxps = movavg_add(state->ipxps_ma,delta_ipax) / dt;
        state->net_opxps = movavg_add(state->opxps_ma,delta_opax) / dt;

        DSPEW("net %s bytes in  += %lu -> %.2f / %f secs => %.2f",state->net_iface,delta_ibytes,movavg_val(state->ikbps_ma),dt,state->net_ikbps);
        DSPEW("net %s bytes out += %lu -> %.2f / %f secs => %.2f",state->net_iface,delta_obytes,movavg_val(state->okbps_ma),dt,state->net_okbps);
        DSPEW("net %s pax   in  += %lu -> %.2f / %f secs => %.2f",state->net_iface,delta_ipax,movavg_val(state->ipxps_ma),dt,state->net_ipxps);
        DSPEW("net %s pax   out += %lu -> %.2f / %f secs => %.2f",state->net_iface,delta_opax,movavg_val(state->opxps_ma),dt,state->net_opxps);

    }
}

static void clear_net_statistics(
    osdhud_state_t     *state)
{
    state->net_ikbps = state->net_ipxps =
        state->net_okbps = state->net_opxps = 0;
    state->net_tot_ibytes = state->net_tot_obytes =
        state->net_tot_ipax = state->net_tot_opax = 0;
    state->net_peak_kbps = state->net_peak_pxps = 0;
    movavg_clear(state->ikbps_ma);
    movavg_clear(state->okbps_ma);
    movavg_clear(state->ipxps_ma);
    movavg_clear(state->opxps_ma);
}

static unsigned long time_in_microseconds(
    void)
{
    struct timeval tv = { .tv_sec=0, .tv_usec=0 };

    if (gettimeofday(&tv,NULL)) {
        perror("gettimeofday");
        exit(1);
    }
    return (tv.tv_sec * 1000000) + tv.tv_usec;
}

static unsigned long time_in_milliseconds(
    void)
{
    unsigned long usecs = time_in_microseconds();

    return usecs / 1000;
}

/**
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
int elapsed(
    char               *buf,
    int                 bufsiz,
    unsigned long       secs)
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
            size_t _catsup_n;                                           \
            char _catsup_u[10] = #var;                                  \
            if (off) {                                                  \
                if (nleft <= 2) {                                       \
                    off = bufsiz+1;                                     \
                    goto DONE;                                          \
                }                                                       \
                (void) strlcat(&buf[off]," ",nleft);                    \
                nleft -= 1;                                             \
                off += 1;                                               \
            }                                                           \
            if (var == 1) _catsup_u[strlen(#var)-1] = 0;                \
            _catsup_n = snprintf(                                       \
                &buf[off],nleft,"%lu %s",var,_catsup_u);                \
            if (_catsup_n >= nleft) {                                   \
                off = bufsiz+1;                                         \
                goto DONE;                                              \
            }                                                           \
            off += _catsup_n;                                           \
            nleft -= _catsup_n;                                         \
        }

    catsup(days);
    catsup(hours);
    catsup(mins);
    catsup(secs);
#undef catsup

 DONE:
    return off;
}

/**
 * Probe data and gather statistics
 *
 * This function invokes probe_xxx() routines defined in the per-OS
 * modules, e.g. openbsd.c, freebsd.c
 */
static void probe(
    osdhud_state_t     *state)
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
    probe_uptime(state);
}

/*
 * Display Routines
 */

static void display_load(
    osdhud_state_t     *state)
{
    xosd_display(
        state->osd,state->disp_line++,XOSD_printf,"load: %.2f",state->load_avg
    );
    if (state->max_load_avg) {
        int percent = ipercent(state->load_avg/state->max_load_avg);

        xosd_display(state->osd,state->disp_line++,XOSD_percentage,percent);
    }
}

static void display_mem(
    osdhud_state_t     *state)
{
    int percent = (int)(100 * state->mem_used_percent);

    xosd_display(state->osd,state->disp_line++,XOSD_printf,"mem: %d%%",percent);
    xosd_display(state->osd,state->disp_line++,XOSD_percentage,percent);
}

static void display_swap(
    osdhud_state_t     *state)
{
    int percent = (int)(100 * state->swap_used_percent);

    if (!state->nswap)
        return;
    xosd_display(
        state->osd,state->disp_line++,XOSD_printf,"swap: %d%%",percent
    );
    xosd_display(state->osd,state->disp_line++,XOSD_percentage,percent);
}

static void display_net(
    osdhud_state_t     *state)
{
    char *iface = state->net_iface? state->net_iface: "-";
    int left, off, n;
    char label[128] = { 0 };
    char details[128] = { 0 };
    float net_kbps = state->net_ikbps + state->net_okbps;
    float net_pxps = state->net_ipxps + state->net_opxps;
    char unit = 'k';
    float unit_div = 1.0;
    float max_kbps = (state->net_speed_mbits / 8.0) * KILO;
    int percent = max_kbps ? (int)(100 * (net_kbps / max_kbps)) : 0;

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
        assert_snprintf(label,"net (%s %dmb/s):",iface,state->net_speed_mbits);
    /* Put together the details string, as short as possible */
    if ((unsigned long)net_kbps) {
        left = sizeof(details);
        off = 0;
        if (max_kbps && percent) {
            n = snprintf(&details[off],left,"%3d%% ",percent);
            assert(n < left);
            left -= n;
            off += n;
        }
        n = snprintf(
            &details[off],left,"%lu %cB/s (%lu px/s)",
            (unsigned long)(net_kbps/unit_div),unit,
            (unsigned long)net_pxps
        );
        assert(n < left);
        left -= n;
        off += n;
    } else {
        assert_strlcpy(details,TXT__QUIET_);
    }
    xosd_display(
        state->osd,state->disp_line++,XOSD_printf,"%s %s",label,details
    );
    if (max_kbps)
        xosd_display(state->osd,state->disp_line++,XOSD_percentage,percent);
}

static void display_disk(
    osdhud_state_t     *state)
{
}

static void display_battery(
    osdhud_state_t     *state)
{
    char *charging = state->battery_state[0] ?
        state->battery_state : TXT__UNKNOWN_;
    char mins[128] = { 0 };

    if (state->battery_missing)
        return;
    if (state->battery_time < 0) {
        assert_strlcpy(mins,TXT_TIME_UNKNOWN);
    } else {
        assert_elapsed(mins,state->battery_time*60);
    }
    xosd_display(
        state->osd,state->disp_line++,XOSD_printf,
        "battery: %s, %d%% charged (%s)",charging,state->battery_life,mins
    );
    xosd_display(
        state->osd,state->disp_line++,XOSD_percentage,state->battery_life
    );
}

static void display_uptime(
    osdhud_state_t     *state)
{
    if (state->sys_uptime) {
        unsigned long secs = state->sys_uptime;
        char upbuf[64] = { 0 };

        assert_elapsed(upbuf,secs);
        xosd_display(state->osd,state->disp_line++,XOSD_printf,"up %s",upbuf);
    }
}

static void display_message(
    osdhud_state_t     *state)
{
    if (!state->message_seen && state->message[0]) {
        xosd_display(
            state->osd,state->disp_line++,XOSD_printf,"%s",state->message
        );
        state->message_seen = 1;
    } else {
        xosd_display(state->osd,state->disp_line++,XOSD_printf,"");
    }
}

static void display_hudmeta(
    osdhud_state_t     *state)
{
    unsigned int now = time_in_milliseconds();
    unsigned int dt = now - state->t0_msecs;
    unsigned int left = (dt < state->duration_msecs) ?
        state->duration_msecs - dt : 0;
    unsigned int left_secs = (left + 500) / 1000;
#ifdef USE_TWO_OSDS
    xosd *osd = state->osd2;
    int line = 0;
#else
    xosd *osd = state->osd;
    int line = state->disp_line++;
#endif
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
        if (!strftime(now_str,sizeof(now_str),state->time_fmt,&ltime)) {
            syslog(
                LOG_ERR,"time fmt '%s' produces > "SIZEOF_F" bytes - disabled",
                state->time_fmt,sizeof(now_str)
            );
            free(state->time_fmt);
            state->time_fmt = NULL;
        }
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
#ifndef USE_TWO_OSDS
    line = ++state->disp_line;
#endif /* USE_TWO_OSDS */
    if (state->time_fmt)
        xosd_display(
            osd,line,XOSD_printf,"%s%s%s%s",now_str,
            left_s[0]? " [": "",left_s,left_s[0]? "]":""
        );
    else if (left_s[0])
        xosd_display(osd,line,XOSD_printf,"[%s]",left_s);
}

/**
 * Display the HUD
 */
static void display(
    osdhud_state_t     *state)
{
    state->disp_line = 0;
    display_uptime(state);
    display_load(state);
    display_mem(state);
    display_swap(state);
    display_net(state);
    display_disk(state);
    display_battery(state);
    display_message(state);
    display_hudmeta(state);
}

#define OSDHUD_OPTIONS "d:p:P:vf:s:i:T:X:knDUSNFCwhgaAt?"
#define USAGE_MSG "usage: %s [-vgtkFDUSNCwh?] [-d msec] [-p msec] [-P msec]\n\
              [-f font] [-s path] [-i iface]\n\
       -v verbose      | -k kill server | -F run in foreground\n\
       -D down hud     | -U up hud      | -S stick hud | -N unstick hud\n\
       -g debug mode   | -t toggle mode | -w don't show swap\n\
       -n don't display at startup    | -C display hud countdown\n\
                        -h,-? display this\n\
       -T fmt   show time using strftime fmt (def: %%Y-%%m-%%d %%H:%%M:%%S)\n\
       -d msec  leave hud visible for millis (def: 2000)\n\
       -p msec  millis between sampling when hud is up (def: 100)\n\
       -P msec  millis between sampling when hud is down (def: 100)\n\
       -f font  font (def: "DEFAULT_FONT")\n\
       -s path  path to Unix-domain socket (def: ~/.%s_%s.sock)\n\
       -i iface network interface to watch\n\
       -X mb/s  fix max net link speed in mbit/sec (def: query interface)\n"

static int usage(
    osdhud_state_t     *state,
    char               *msg)
{
    int fail = 0;

    if (!state->argv0) {
        syslog(LOG_WARNING,"client message error: %s",msg);
        fail = 1;
    } else {
        if (msg)
            fprintf(stderr,"%s ERROR: %s\n",state->argv0,msg);
        else {
            fprintf(stderr,"%s v%s: %s\n",state->argv0,VERSION,PURPOSE);
            fprintf(stderr,USAGE_MSG,state->argv0,state->argv0,VERSION);
        }
        exit(1);
    }
    return fail;
}

/*
 * Parse command-line arguments into osdhud_state_t structure
 */
static int parse(
    osdhud_state_t     *state,
    int                 argc,
    char              **argv)
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
        case 'd':                       /* duration in milliseconds */
            if (sscanf(optarg,"%d",&state->display_msecs) != 1)
                fail = usage(state,"bad value for -d");
            DBG2("parsed -%c %d",ch,state->display_msecs);
            break;
        case 'p':                       /* inter-probe pause in milliseconds */
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
            state->net_iface = strdup(optarg); /* network iface of interest */
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

static void init_state(
    osdhud_state_t     *state,
    char               *argv0)
{
    if (!argv0)
        state->argv0 = NULL;
    else {
        char *base = rindex(argv0,'/');

        state->argv0 = base ? base+1 : argv0;
    }
    state->kill_server = state->down_hud = state->up_hud = state->countdown =
        state->stick_hud = state->unstick_hud = state->foreground =
        state->hud_is_up = state->server_quit = state->stuck = state->verbose =
        state->debug = state->toggle_mode = state->alerts_mode = 
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
    state->net_tot_ipax = state->net_tot_ierr =
        state->net_tot_opax = state->net_tot_oerr =
        state->net_tot_ibytes = state->net_tot_obytes = 0;
    state->delta_t = 0;
    state->pos_x = DEFAULT_POS_X;
    state->pos_y = DEFAULT_POS_Y;
    state->nlines = DEFAULT_NLINES;
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
    state->load_avg = state->mem_used_percent = state->swap_used_percent = 0;
    state->per_os_data = NULL;
    state->net_ikbps = state->net_ipxps =
        state->net_okbps = state->net_opxps = 0;
    state->net_peak_kbps = state->net_peak_pxps = 0;
    state->ikbps_ma = state->ipxps_ma =
        state->okbps_ma = state->opxps_ma = NULL;
    state->battery_missing = 0;
    state->battery_life = 0;
    memset(state->battery_state,0,sizeof(state->battery_state));
    state->battery_time = 0;
    state->last_t = 0;
    state->first_t = 0;
    state->sys_uptime = 0;
    state->osd = NULL;
    memset(state->message,0,sizeof(state->message));
    state->message_seen = 0;
#ifdef USE_TWO_OSDS
    state->osd2 = NULL;
#endif
    state->disp_line = 0;
    memset(state->errbuf,0,sizeof(state->errbuf));
}

static osdhud_state_t *create_state(
    osdhud_state_t     *state)
{
    osdhud_state_t *new_state =
        (osdhud_state_t *)malloc(sizeof(osdhud_state_t));

    if (new_state) {
        init_state(new_state,NULL);

#define set_field(fld) new_state->fld = state->fld
#define dup_field(fld) new_state->fld = state->fld? strdup(state->fld): NULL
#define cpy_field(fld)                                                  \
        memcpy(                                                         \
            (void *)&new_state->fld,                                    \
            (void *)&state->fld,                                        \
            sizeof(new_state->fld)                                      \
        )

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
        set_field(nlines);
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

static void cleanup_state(
    osdhud_state_t     *state)
{
    if (state) {
        if (state->osd) {
            xosd_destroy(state->osd);
#ifdef USE_TWO_OSDS
            xosd_destroy(state->osd2);
#endif
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
    }
}

static void free_state(
    osdhud_state_t     *dispose)
{
    cleanup_state(dispose);
    free(dispose);
}

/*
 * Split str into words delimited by whitespace; returns number of words
 */
static int split(
    char               *str,
    char             ***out_words)
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
        syslog(
            LOG_WARNING,"split too many tokens (> "SIZEOF_F") '%s'",
            ARRAY_SIZE(splitz),str
        );
    splits = (char **)calloc(1+ntoke,sizeof(char *));
    assert(splits);
    splits[0] = strdup("osdhud");       /* getopt(3) */
    for (i = 0; i < ntoke; i++)
        splits[i+1] = strdup(splitz[i]);
    free(copy);
    *out_words = splits;
    return ntoke+1;
}

/**
 * Free the results of calling split()
 */
static void free_split(
    int                 argc,
    char              **argv)
{
    if (argv) {
        int i = 0;

        for (i = 0; i < argc; i++)
            free(argv[i]);
        free(argv);
    }
}

static void cleanup_daemon(
    osdhud_state_t     *state)
{
    if (state->sock_fd >= 0) {
        close(state->sock_fd);
        if (unlink(state->sock_path))
            syslog(
                LOG_ERR,"could not unlink socket %s: %s (#%d)",
                state->sock_path,err_str(state,errno),errno
            );
        state->sock_fd = -1;
        cleanup_state(state);
    }
    closelog();
}

/**
 * Attempt to receive a message via our control socket and act on it
 */
static int handle_message(
    osdhud_state_t     *state)
{
    int client = -1;
    int retval = 0;
    struct sockaddr_un cli;
    socklen_t cli_sz = sizeof(cli);

    if (state->verbose)
        syslog(
            LOG_WARNING,"accepting connection on sock # %d, HUD is %s",
            state->sock_fd,state->hud_is_up ? "UP": "DOWN"
        );
    client = accept(state->sock_fd,(struct sockaddr *)&cli,&cli_sz);
    if (client < 0)
        syslog(
            LOG_WARNING,"accept(#%d) failed: %s (#%d)",state->sock_fd,
            err_str(state,errno),errno
        );
    else {
        /* the client just sends its command-line args to the daemon */
        FILE *stream = fdopen(client, "r");
        char msgbuf[OSDHUD_MAX_MSG_SIZE+1] = { 0 };
        char *msg = fgets(msgbuf,OSDHUD_MAX_MSG_SIZE,stream);

        if (!msg)
            syslog(
                LOG_WARNING,"error reading from client: %s (#%d)",
                err_str(state,errno),errno
            );
        else {
            int argc = 0;
            char **argv = NULL;
            osdhud_state_t *foo = create_state(state);
            size_t msglen = strlen(msg);

            /* The message is just command-line args */
            if (msglen && (msg[msglen-1] == '\n'))
                msg[msglen-1] = 0;
            argc = split(msg,&argv);
            if (argc < 1) {
                syslog(
                    LOG_ERR,"too many args in "SIZE_T_F" bytes: '%.50s%s'",
                    msglen,msg,(msglen>50)? "...": ""
                );
                cleanup_daemon(state);
                exit(1);
            }
            if (state->verbose) {
                int i = 0;

                for (i = 0; i < argc; i++)
                    syslog(LOG_WARNING,"msg arg#%d: '%s'",i,argv[i]);
            }
            if (!argc) {
                syslog(
                    LOG_WARNING,"malformed message buffer |%.*s|",
                    OSDHUD_MAX_MSG_SIZE,msgbuf
                );
            } else if (parse(foo,argc,argv)) {
                syslog(LOG_WARNING,"parse error for '%s'",msg);
            } else {
                /* Successfully parsed msg */
                if (foo->kill_server)
                    /* If we were invoked with -k then that trumps all else */
                    state->server_quit = retval = 1;
                else {
                    if (state->verbose)
                        syslog(LOG_WARNING,"merging in params from msg");
                    /*  Merge it in */
                    if (state->verbose)
                        syslog(
                            LOG_WARNING,"display_msecs %d => %d",
                            state->display_msecs,foo->display_msecs
                        );
                    state->display_msecs = foo->display_msecs;
                    if (!state->hud_is_up || state->toggle_mode)
                        retval = 1;
                    else {
                        /* hud is up: increase duration of display */
                        if (state->verbose)
                            syslog(
                                LOG_WARNING,"duration += %d ms -> %d ms",
                                state->display_msecs,
                                state->duration_msecs + state->display_msecs
                            );
                        state->duration_msecs += state->display_msecs;
                    }
                    if (state->verbose)
                        syslog(
                            LOG_WARNING,"short_pause %d => %d",
                            state->short_pause_msecs,foo->short_pause_msecs
                        );
                    state->short_pause_msecs = foo->short_pause_msecs;
                    if (state->verbose)
                        syslog(
                            LOG_WARNING,"long_pause %d => %d",
                            state->long_pause_msecs,foo->long_pause_msecs
                        );
                    state->long_pause_msecs = foo->long_pause_msecs;

                    /* Either both fields are set and differ in content
                     * or only one of the two fields is not NULL:
                     */
#define is_different(f) (                                               \
                        ((state->f && foo->f) && strcmp(state->f,foo->f)) || \
                        (state->f && !foo->f) || (!state->f && foo->f)  \
                    )

                    if (is_different(font)) {
                        if (state->verbose)
                            syslog(
                                LOG_WARNING,"font %s => %s",
                                NULLS(state->font),NULLS(foo->font)
                            );
                        free(state->font);
                        state->font = foo->font ? strdup(foo->font) : NULL;
                    }
                    if (is_different(time_fmt)) {
                        if (state->verbose)
                            syslog(
                                LOG_WARNING,"time_fmt %s => %s",
                                NULLS(state->time_fmt),NULLS(foo->time_fmt)
                            );
                        free(state->time_fmt);
                        state->time_fmt = foo->time_fmt ?
                            strdup(foo->time_fmt) : NULL;
                    }
                    if (is_different(net_iface)) {
                        if (state->verbose)
                            syslog(
                                LOG_WARNING,"net_iface %s => %s",
                                NULLS(state->net_iface),NULLS(foo->net_iface)
                            );
                        clear_net_statistics(state);
                        free(state->net_iface);
                        state->net_iface = foo->net_iface ?
                            strdup(foo->net_iface) : NULL;
                        state->net_speed_mbits = 0;
                    }
#undef is_different
                    if (state->verbose)
                        syslog(
                            LOG_WARNING,"tog:%d up:%d dn:%d stk:%d ustk:%d",
                            foo->toggle_mode,foo->up_hud,foo->down_hud,
                            foo->stick_hud,foo->unstick_hud
                        );
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
                        state->net_speed_mbits = foo->net_speed_mbits;
                }
            }
            if (state->verbose)
                syslog(LOG_WARNING,"done processing command");
            free_state(foo);
            free_split(argc,argv);
        }
        fclose(stream);
        close(client);
        if (state->verbose)
            syslog(LOG_WARNING,"done handling client");
    }
    if (state->verbose)
        syslog(
            LOG_WARNING,"handle_message => %d, is_up:%d",
            retval,state->hud_is_up
        );
    return retval;
}

#ifdef ENABLE_ALERTS
static int check_alerts(
    osdhud_state_t     *state)
{
    int nalerts = 0;

    memset(state->message,0,sizeof(state->message));
    state->message_seen = 0;

#define catmsg(xx)                                                      \
    do {                                                                \
        if (state->message[0])                                          \
            assert_strlcat(state->message,", ");                        \
        assert_strlcat(state->message,xx);                              \
        nalerts++;                                                      \
    } while (0);

    if (!state->battery_missing&&(state->battery_life<state->min_battery_life))
        catmsg(TXT_ALERT_BATTERY_LOW);
    if (state->max_load_avg&&(ipercent(state->load_avg/state->max_load_avg)>40))
        catmsg(TXT_ALERT_LOAD_HIGH);
    if (state->max_mem_used && (state->mem_used_percent > state->max_mem_used))
        catmsg(TXT_ALERT_MEM_LOW);

#undef catmsg

    return nalerts;
}
#endif /* ENABLE_ALERTS */

/**
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
static int check(
    osdhud_state_t     *state)
{
    int done = 0;
    int quit_loop = 0;
    int pause_msecs = state->hud_is_up ? state->short_pause_msecs :
        state->long_pause_msecs;

    if (state->verbose > 1)
        syslog(
            LOG_WARNING,"check: pause is %d, HUD is %s",
            pause_msecs,state->hud_is_up ? "UP": "DOWN"
        );
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
            syslog(LOG_ERR,"select() => %s (#%d)",err_str(state,errno),errno);
            cleanup_daemon(state);
            exit(1);
        } else if (x > 0) {             /* command */
            quit_loop = handle_message(state);
            if (!quit_loop) {
                /* client did not tell us to quit, continue pausing */
                int dt = time_in_milliseconds() - b4;

                /* whittle down the time left */
                if (dt >= pause_msecs)
                    done = 1;
                else
                    pause_msecs -= dt;
            } /* else message told us to quit loop */
        } else {                        /* timeout */
            done = 1;
            if (state->hud_is_up && !state->toggle_mode) {
                /* if hud is up, see if it is time to bring it down */
                int now = time_in_milliseconds();
                int delta_d = (now - state->t0_msecs);

                if (!state->stuck && (delta_d >= state->duration_msecs))
                    quit_loop = 1;
            } /* otherwise done=1 will force us to sample and pause again */
        }
        /* Deal with signals-based flags */
        if (interrupted) {
            syslog(LOG_WARNING,"interrupted - bailing out by force");
            done = quit_loop = state->server_quit = 1;
        }
        if (restart_req)
            syslog(LOG_WARNING,"restart requested - not doing anything");
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
            state->stuck = 1;           /* alerts force them to unstick...? */
        }
    } while (!done && !quit_loop);
    return quit_loop;
}

/**
 * Turn state into equivalent command-line options to send to running instance
 */
static int pack_message(
    osdhud_state_t     *state,
    char              **out_msg)
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
    if (state->f) {                                                     \
        int x = snprintf(&packed[off],left,"%s-%s",lead,o);             \
        if (x < 0)                                                      \
            die(state,"pack: " o " failed !?");                         \
        off += x;                                                       \
        left -= x;                                                      \
    }
#define integer_opt(f,o)                                                \
    do  {                                                               \
        int x=snprintf(&packed[off],left,"%s-%s %d",lead,o,state->f);   \
        if (x < 0)                                                      \
            die(state,"pack: " o " failed !?");                         \
        off += x;                                                       \
        left -= x;                                                      \
    } while (0);
#define string_opt(f,o)                                                 \
    if (state->f) {                                                     \
        int x=snprintf(&packed[off],left,"%s-%s %s",lead,o,state->f);   \
        if (x < 0)                                                      \
            die(state,"pack: " o " failed !?");                         \
        off += x;                                                       \
        left -= x;                                                      \
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

    assert(strlcat(&packed[off],"\n",left) < left);

    *out_msg = packed;
    return strlen(packed);
}

/**
 * Try to kick an existing instance of ourselves
 *
 * If we can make contact with an existing instance of ourselves via
 * the control socket then send a message to it and return true.
 */
static int kicked(
    osdhud_state_t     *state)
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
    if (!connect(sock_fd,(struct sockaddr *)&state->addr,sizeof(state->addr))) {
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
                fprintf(
                    stderr,"%s: short write to socket %s (%d != %d)\n",
                    state->argv0,state->sock_path,nw,len
                );
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

/**
 * Try to fork a daemon child to run the HUD
 *
 * We return true if we are running in the daemonized child process
 * and false otherwise ala fork(2).
 */
static int forked(
    osdhud_state_t     *state)
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

static xosd *create_big_osd(
    osdhud_state_t     *state,
    char               *font)
{
    xosd *osd = xosd_create(state->nlines);

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
    xosd_set_vertical_offset(osd,state->pos_y);
    xosd_set_bar_length(osd,state->width);

    return osd;
}

#ifdef USE_TWO_OSDS
static xosd *create_small_osd(
    osdhud_state_t     *state,
    char               *font)
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
#endif

static void hud_up(
    osdhud_state_t     *state)
{
    char *font = state->font ? state->font : DEFAULT_FONT;

    if (state->verbose > 1)
        syslog(LOG_WARNING,"HUD coming up, osd @ %p",state->osd);

#ifdef CREATE_EACH_TIME
    state->osd = create_big_osd(state,font);
# ifdef USE_TWO_OSDS
    state->osd2 = create_small_osd(state,font);
# endif
#else /* !CREATE_EACH_TIME */
    if (!state->osd) {
        state->osd = create_big_osd(state,font);
# ifdef USE_TWO_OSDS
        state->osd2 = create_small_osd(state,font);
# endif
    } else {
        if (xosd_show(state->osd)) {
            syslog(LOG_ERR,"xosd_show failed (big): %s",xosd_error);
            exit(1);
        }
# ifdef USE_TWO_OSDS
        if (xosd_show(state->osd2)) {
            syslog(LOG_ERR,"xosd_show failed (small): %s",xosd_error);
            exit(1);
        }
# endif
    }
#endif
    state->hud_is_up = 1;
    state->t0_msecs = time_in_milliseconds();
    state->duration_msecs = state->display_msecs;
}

static void hud_down(
    osdhud_state_t     *state)
{
    if (state->verbose)
        syslog(LOG_WARNING,"HUD coming down");

#ifdef CREATE_EACH_TIME
    xosd_destroy(state->osd);
    state->osd = NULL;
# ifdef USE_TWO_OSDS
    xosd_destroy(state->osd2);
    state->osd2 = NULL;
# endif
#else /* !CREATE_EACH_TIME */
    if (state->osd) {
        xosd_hide(state->osd);
# ifdef USE_TWO_OSDS
        xosd_hide(state->osd2);
# endif
    }
#endif

    state->hud_is_up = 0;
}

static void handle_signal(
    int                 signo,
    siginfo_t          *info,
    void               *ptr)
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

static void init_signals(
    osdhud_state_t     *state)
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

static void setup_daemon(
    osdhud_state_t     *state)
{
    int syslog_flags = LOG_PID;

    if (state->foreground)
        syslog_flags |= LOG_PERROR;
    openlog(state->argv0,syslog_flags,LOG_LOCAL0);
    if (state->verbose)
        syslog(LOG_INFO,"server starting; v%s",VERSION);
    state->sock_fd = socket(PF_UNIX,SOCK_STREAM,0);
    if (state->sock_fd < 0) {
        syslog(
            LOG_ERR,"could not create unix socket: %s (#%d)",
            err_str(state,errno),errno
        );
        closelog();
        exit(1);
    }
    if(bind(state->sock_fd,(struct sockaddr*)&state->addr,sizeof(state->addr))){
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

/**
 * osdhud - main program
 *
 * The idea is that just running us from a keybinding in the window
 * manager with no arguments should do something reasonable: the
 * HUD appears for a couple of seconds and fades away if nothing else
 * is done.  If we are invoked while the HUD is still up then it will
 * stay up longer.  This is intuitively what I want:
 *     more hit key -> more hud
 *     stop hit key -> no more hud
 * I call this the Caveman Theory of Human/Computer Interaction:
 * PUNCH COMPUTER TO MAKE IT GO.
 */
int main(
    int                 argc,
    char              **argv)
{
    osdhud_state_t state;

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
            usage(&state,"no -s and could not determine homedir - giving up");
        path_len = asprintf(
            &path,"%s/.%s_%s.sock",home,state.argv0,VERSION
        );
        if (path_len < 0)
            usage(&state,"could not cons up default sock path... why?");
        if (state.verbose)
            fprintf(stderr,"[%s] default socket path: %s\n",state.argv0,path);
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
