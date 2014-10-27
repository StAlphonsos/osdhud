/* -*- mode:c; c-basic-offset:4; tab-width:4; indent-tabs-mode:nil -*- */

/**
 * @file openbsd.c
 * @brief probe functions for osdhud on OpenBSD
 *
 * Implement the probe_xxx() functions used by osdhud.c for OpenBSD.
 *
 * Much of this code is lifted liberally from the OpenBSD systat
 * command whose source code can be found in /usr/src/usr.bin/systat
 * in the OpenBSD source tree as of version 5.5.  XXX not sure if I
 * should cut and paste the licenses from if.c, systat.h, swap.c and
 * w.c here or if pointing at them and waving a BSD flag is enough...
 */

/* LICENSE:
 *
 * Copyright (C) 1999-2014 by attila <attila@stalphonsos.com>
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
#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <sys/sockio.h>
#include <sys/ioctl.h>
#include <sys/swap.h>

#define LOG1024        10
#define pagetok(size) ((size) << pageshift)

/* lifted from systat ca. openbsd 5.5 */

struct ifcount {
    u_int64_t    ifc_ib;            /* input bytes */
    u_int64_t    ifc_ip;            /* input packets */
    u_int64_t    ifc_ie;            /* input errors */
    u_int64_t    ifc_ob;            /* output bytes */
    u_int64_t    ifc_op;            /* output packets */
    u_int64_t    ifc_oe;            /* output errors */
    u_int64_t    ifc_co;            /* collisions */
    int        ifc_flags;        /* up / down */
    int        ifc_state;        /* link state */
};

struct ifstat {
    char        ifs_name[IFNAMSIZ];    /* interface name */
    char        ifs_description[IFDESCRSIZE];
    struct ifcount    ifs_cur;
    struct ifcount    ifs_old;
    struct ifcount    ifs_now;
    char        ifs_flag;
};

struct openbsd_data {
    int                 nifs;
    struct ifstat      *ifstats;
    struct timeval      boottime;
    struct swapent     *swap_devices;
};

static int pageshift;

void
rt_getaddrinfo(struct sockaddr *sa, int addrs, struct sockaddr **info)
{
    int i;

    for (i = 0; i < RTAX_MAX; i++) {
        if (addrs & (1 << i)) {
            info[i] = sa;
            sa = (struct sockaddr *) ((char *)(sa) +
                roundup(sa->sa_len, sizeof(long)));
        } else
            info[i] = NULL;
    }
}

void probe_init(
    osdhud_state_t     *state)
{
    int mib[2] = { CTL_KERN, KERN_BOOTTIME };
    int pagesize;
    struct openbsd_data *obsd;
    size_t size = 0;

    /* taken from /usr/src/usr.bin/top/machine.c as of OpenBSD 5.5 */
    pagesize = getpagesize();
    pageshift = 0;
    while (pagesize > 1) {
        pageshift++;
        pagesize >>= 1;
    }
    pageshift -= LOG1024;

    obsd = (struct openbsd_data *)malloc(sizeof(struct openbsd_data));
    assert(obsd);
    obsd->nifs = 0;
    obsd->ifstats = NULL;

    size = sizeof(obsd->boottime);
    assert(!sysctl(mib,2,&obsd->boottime,&size,NULL,0));

    if (!state->nswap)
        obsd->swap_devices = NULL;
    else {
        obsd->swap_devices =
            (struct swapent *)calloc(state->nswap,sizeof(struct swapent));
        assert(obsd->swap_devices);
    }

    state->per_os_data = (void *)obsd;
}

void probe_cleanup(
    osdhud_state_t     *state)
{
    if (state->per_os_data) {
        struct openbsd_data *obsd = (struct openbsd_data *)state->per_os_data;

        free(obsd->swap_devices);
        free(obsd->ifstats);
        obsd->ifstats = NULL;
        obsd->nifs = 0;
        free(obsd);
    }
}

void probe_load(
    osdhud_state_t     *state)
{
    double avgs[1] = { 0.0 };

    if (getloadavg(avgs,ARRAY_SIZE(avgs)) < 0) {
        SPEWE("getloadavg");
        exit(1);
    }
    state->load_avg = avgs[0];
}

void probe_mem(
    osdhud_state_t     *state)
{
    static int vmtotal_mib[] = {CTL_VM, VM_METER};
    struct vmtotal vmtotal;
    size_t size = sizeof(vmtotal);
    float tot_kbytes, act_kbytes;

    size = sizeof(vmtotal);
    if (sysctl(vmtotal_mib,ARRAY_SIZE(vmtotal_mib),&vmtotal,&size,NULL,0)<0) {
        SPEWE("sysctl");
        bzero(&vmtotal,sizeof(vmtotal));
    }
    tot_kbytes = (float)pagetok(vmtotal.t_rm);
    act_kbytes = (float)pagetok(vmtotal.t_arm);
    state->mem_used_percent = act_kbytes / tot_kbytes;
}

void probe_swap(
    osdhud_state_t     *state)
{
    struct openbsd_data *obsd = (struct openbsd_data *)state->per_os_data;
    int i, used, xsize;

    if (!state->nswap)
        return;
    assert(
        swapctl(SWAP_STATS,(void *)obsd->swap_devices,state->nswap) ==
        state->nswap
    );
    used = xsize = 0;
    for (i = 0; i < state->nswap; i++) {
        used += obsd->swap_devices[i].se_inuse;
        xsize += obsd->swap_devices[i].se_nblks;
    }
    state->swap_used_percent = (float)used / (float)xsize;
}

void probe_net(
    osdhud_state_t     *state)
{
    char *buf, *next, *lim;
    size_t need;
    int i;
    int mib[6] = { CTL_NET, AF_ROUTE, 0, 0, NET_RT_IFLIST, 0 };
    struct openbsd_data *os_data = (struct openbsd_data *)state->per_os_data;
    struct ifstat *ifstats = os_data->ifstats;
    int nifs = os_data->nifs;
    struct if_msghdr ifm;
    struct sockaddr *info[RTAX_MAX];
    struct sockaddr_dl *sdl;
    int num_ifs = 0;
    struct ifstat *ifs;

    /*
     * This logic lifted directly from fetchifstat() in
     * /usr/src/usr.bin/systat/if.c as of OpenBSD 5.5.  At first I
     * thought it was ugly and strange but after reading the man pages
     * and the source I've come to the conclusion that it is as it
     * should be... just needed to bring myself up to date from my
     * 1990's level of clue.  The inline commentary is mine, mainly to
     * help me keep it straight vs. FreeBSD.
     */

    /* Ask how much space will be needed for the whole array */
    if (sysctl(mib,ARRAY_SIZE(mib),NULL,&need,NULL,0) < 0) {
        SPEWE("sysctl(IFLIST)");
        return;
    }
    /* Can't use calloc because they aren't fixed-sized entries */
    if ((buf = malloc(need)) == NULL) {
        SPEWE("malloc failed for interface list buffer");
        return;
    }
    /* Now get them */
    if (sysctl(mib,ARRAY_SIZE(mib),buf,&need,NULL,0) < 0) {
        SPEWE("sysctl(IFLIST#2)");
        return;
    }
    /* lim points past the end */
    lim = buf + need;
    for (next = buf; next < lim; next += ifm.ifm_msglen) {
        bcopy(next,&ifm,sizeof(ifm));
        /* We might get back all kinds of things; filter for just the
         * ones we want to examine.
         */
        if (ifm.ifm_version != RTM_VERSION ||
            ifm.ifm_type != RTM_IFINFO ||
            !(ifm.ifm_addrs & RTA_IFP))
            continue;
        if (ifm.ifm_index >= nifs) {
            struct ifstat *newstats = realloc(
                ifstats,(ifm.ifm_index + 4) * sizeof(struct ifstat)
            );
            assert(newstats);
            ifstats = newstats;
            for (; nifs < ifm.ifm_index + 4; nifs++)
                bzero(&ifstats[nifs], sizeof(*ifstats));
        }
        ifs = &ifstats[ifm.ifm_index];
        if (ifs->ifs_name[0] == '\0') {
            bzero(&info,sizeof(info));
            rt_getaddrinfo(
                (struct sockaddr *)((struct if_msghdr *)next + 1),
                ifm.ifm_addrs, info);
            sdl = (struct sockaddr_dl *)info[RTAX_IFP];
            if (sdl && sdl->sdl_family == AF_LINK &&
                sdl->sdl_nlen > 0) {
                struct ifreq ifrdesc;
                char ifdescr[IFDESCRSIZE];
                int s;

                bcopy(sdl->sdl_data, ifs->ifs_name,
                      sdl->sdl_nlen);
                ifs->ifs_name[sdl->sdl_nlen] = '\0';

                /* Get the interface description */
                memset(&ifrdesc, 0, sizeof(ifrdesc));
                strlcpy(ifrdesc.ifr_name, ifs->ifs_name,
                    sizeof(ifrdesc.ifr_name));
                ifrdesc.ifr_data = (caddr_t)&ifdescr;

                s = socket(AF_INET, SOCK_DGRAM, 0);
                if (s != -1) {
                    if (ioctl(s, SIOCGIFDESCR, &ifrdesc) == 0)
                        strlcpy(ifs->ifs_description,
                            ifrdesc.ifr_data,
                            sizeof(ifs->ifs_description));
                    close(s);
                }
            }
            if (ifs->ifs_name[0] == '\0')
                continue;
        }
        num_ifs++;
#define ifix(nn) ifm.ifm_data.ifi_##nn
        ifs->ifs_cur.ifc_flags = ifm.ifm_flags;
        ifs->ifs_cur.ifc_state = ifm.ifm_data.ifi_link_state;
        ifs->ifs_flag++;
        if (!state->net_iface && strncmp(ifs->ifs_name,"lo",2)) {
            /* first non-loopback interface */
            state->net_iface = strdup(ifs->ifs_name);
            VSPEW("choosing first non-loopback interface: %s",state->net_iface);
        }
        if (state->net_iface && !strcmp(state->net_iface,ifs->ifs_name)) {
            /* this is our boy */
            unsigned long delta_in_b = ifix(ibytes) - state->net_tot_ibytes;
            unsigned long delta_out_b = ifix(obytes) - state->net_tot_obytes;
            unsigned long delta_in_p = ifix(ipackets) - state->net_tot_ipax;
            unsigned long delta_out_p = ifix(opackets) - state->net_tot_opax;

            update_net_statistics(
                state,delta_in_b,delta_out_b,delta_in_p,delta_out_p
            );
            state->net_tot_ibytes = ifix(ibytes);
            state->net_tot_obytes = ifix(obytes);
            state->net_tot_ipax = ifix(ipackets);
            state->net_tot_opax = ifix(opackets);
        }
#undef ifix
    }
    /* remove unreferenced interfaces */
    for (i = 0; i < nifs; i++) {
        ifs = &ifstats[i];
        if (ifs->ifs_flag)
            ifs->ifs_flag = 0;
        else
            ifs->ifs_name[0] = '\0';
    }
    os_data->ifstats = ifstats;
    os_data->nifs = nifs;
}

void probe_battery(
    osdhud_state_t     *state)
{
}

void probe_uptime(
    osdhud_state_t     *state)
{
    time_t now;
    struct openbsd_data *obsd = (struct openbsd_data *)state->per_os_data;

    (void) time(&now);
    state->sys_uptime = now - obsd->boottime.tv_sec;
}
