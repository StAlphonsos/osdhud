/* -*- mode:c; c-basic-offset:4; tab-width:4; indent-tabs-mode:nil -*- */

/**
 * @file openbsd.c
 * @brief probe functions for osdhud on OpenBSD
 *
 * Implement the probe_xxx() functions used by osdhud.c for OpenBSD.
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
#include <net/sockio.h>
#include <sys/ioctl.h>

#define LOG1024        10
#define pagetok(size) ((size) << pageshift)

static int pageshift;

void probe_init(
    void)
{
    int pagesize;

    /* taken from /usr/src/usr.bin/top/machine.c as of OpenBSD 5.5 */
    pagesize = getpagesize();
    pageshift = 0;
    while (pagesize > 1) {
        pageshift++;
        pagesize >>= 1;
    }
    pageshift -= LOG1024;
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
}

void probe_net(
    osdhud_state_t     *state)
{
    struct if_msghdr ifm;
    char *buf, *next;
    size_t need;
    int i;
    int mib[6] = { CTL_NET, AF_ROUTE, 0, 0, NET_RT_IFLIST, 0 };

    /*
     * This logic lifted directly from fetchifstat() in
     * /usr/src/usr.bin/systat/if.c as of OpenBSD 5.5.  At first I
     * thought it was ugly and strange but after reading the man pages
     * and the source I've come to the conclusion that it is as it
     * should be... just needed to bring myself up to date from my
     * 1990's level of clue.  The commentary is mine, mainly to help
     * me keep it straight vs. FreeBSD.
     */

    /* Probe how many entries we need in the array */
    if (sysctl(mib,ARRAY_SIZE(mib),NULL,&need,NULL,0) < 0) {
        SPEWE("sysctl(IFLIST)");
        return;
    }
    /* Can't use calloc because they aren't fixed-sized entries */
    if ((buf = malloc(need)) == NULL) {
        SPEWE("malloc failed for interface list buffer");
        return;
    }
    if (sysctl(mib,ARRAY_SIZE(mib),buf,&need,NULL,0) < 0) {
        SPEWE("sysctl(IFLIST#2)");
        return;
    }
    /* lim points past the end */
    lim = buf + need;
    for (next = buf; next < lim; next += ifm.ifm_msglen) {
        struct ifstat *ifs;

        bcopy(next,&ifm,sizeof(ifm));
        /* We might get back all kinds of things; filter for just the
         * ones we want to examine.
         */
        if (ifm.ifm_version != RTM_VERSION ||
            ifm.ifm_type != RTM_IFINFO ||
            !(ifm.ifm_addrs & RTA_IFP))
            continue;
        if (ifm.ifm_index >= state->nifs) {
            struct ifstat *newstats = realloc(
                state->ifstats,(ifm.ifm_index + 4) * sizeof(strut ifstat)
            );
            assert(newstats);
            state->ifstats = newstats;
            for (; state->nifs < ifm.ifm_index + 4; state->nifs++)
                bzero(&state->ifstats[state->nifs], sizeof(*state->ifstats));
        }
        ifs = &state->ifstats[ifm.ifm_index];
        if (ifs->ifs_name[0] == '\0') {
            struct sockaddr *info[RTAX_MAX];

            bzero(&info,sizeof(info));
        }
    }
    /* XXX */
}

void probe_battery(
    osdhud_state_t     *state)
{
}

void probe_uptime(
    osdhud_state_t     *state)
{
}
