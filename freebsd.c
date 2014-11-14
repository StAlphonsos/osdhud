/* -*- mode:c; c-basic-offset:4; tab-width:4; indent-tabs-mode:nil -*- */

/**
 * @file freebsd.c
 * @brief osdhud probe functions for FreeBSD
 *
 * Implementations of osdhud's probe_xxx() functions for FreeBSD
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
#include <sys/sysctl.h>
#include <vm/vm_param.h>
#include <sys/vmmeter.h>
#include <net/if_mib.h>
#include <net/if_var.h>
#include <net/if_types.h>

# define DO_SYSCTL(dd,nn,mm,vv,zz,nv,nz) do {                           \
        if (sysctl(nn,mm,vv,zz,nv,nz)) { SPEWE(dd); exit(1); }          \
    } while (0);

void probe_init(
    osdhud_state_t     *state)
{
    /* nothing */
}

void probe_cleanup(
    osdhud_state_t     *state)
{
    /* nothing */
}

void probe_load(
    osdhud_state_t     *state)
{
    int mib[2] = { CTL_VM, VM_LOADAVG };
    struct loadavg avgs;
    size_t len = sizeof(avgs);

    DO_SYSCTL("vm.loadavg",mib,2,&avgs,&len,NULL,0);
    state->load_avg = (float)avgs.ldavg[0] / (float)avgs.fscale;
}

void probe_mem(
    osdhud_state_t     *state)
{
    int mib[2] = { CTL_HW, HW_PAGESIZE };
    int pgsz = 0;
    size_t len = sizeof(pgsz);
    struct vmtotal vmtot;

    DO_SYSCTL("hw.pagesize",mib,2,&pgsz,&len,NULL,0);

    mib[0] = CTL_VM;
    mib[1] = VM_TOTAL;
    len = sizeof(vmtot);
    DO_SYSCTL("vm.total",mib,2,&vmtot,&len,NULL,0);

    state->mem_used_percent = (float)vmtot.t_avm / (float)vmtot.t_vm;
}

void probe_swap(
    osdhud_state_t     *state)
{
}

void probe_net(
    osdhud_state_t     *state)
{
    int mib[6] = {CTL_NET,PF_LINK,NETLINK_GENERIC,IFMIB_SYSTEM,IFMIB_IFCOUNT,0};
    int ifcount = 0;
    size_t len = sizeof(ifcount);
    int i = 0;

    DO_SYSCTL("ifcount",mib,5,&ifcount,&len,NULL,0);
    mib[3] = IFMIB_IFDATA;
    mib[5] = IFDATA_GENERAL;
    for (i = 1; i < ifcount; i++) {
        struct ifmibdata ifmd;
        struct if_data *d = NULL;
        int delta_in = 0;
        int delta_out = 0;
        int delta_pin = 0;
        int delta_pout = 0;

        mib[4] = i;
        len = sizeof(ifmd);
        DO_SYSCTL("ifmib",mib,6,&ifmd,&len,NULL,0);
        /* If an interface name was specified and this isn't it, skip it */
        if (state->net_iface && strcmp(ifmd.ifmd_name,state->net_iface))
            continue;
        /* If no interface name was specified and this one is down, skip it */
        if (!state->net_iface && !(ifmd.ifmd_flags & IFF_UP))
            continue;
        d = &ifmd.ifmd_data;
        if (state->verbose)
            syslog(LOG_DEBUG,"#%2d/%2d: %s flags=0x%x ipax=%lu ierr=%lu opax=%lu oerr=%lu recv=%lu sent=%lu\n",i,ifcount,ifmd.ifmd_name,ifmd.ifmd_flags,d->ifi_ipackets,d->ifi_ierrors,d->ifi_opackets,d->ifi_oerrors,d->ifi_ibytes,d->ifi_obytes);
        if (!state->net_iface) {
            state->net_iface = strdup(ifmd.ifmd_name);
            if (state->verbose)
                syslog(
                    LOG_WARNING,"chose network interface: %s",state->net_iface
                );
        }
        state->net_tot_ierr += d->ifi_ierrors;
        state->net_tot_oerr += d->ifi_oerrors;

        delta_in = d->ifi_ibytes - state->net_tot_ibytes;
        state->net_tot_ibytes += delta_in;

        delta_out = d->ifi_obytes - state->net_tot_obytes;
        state->net_tot_obytes += delta_out;

        delta_pin = d->ifi_ipackets - state->net_tot_ipax;
        state->net_tot_ipax += delta_pin;

        delta_pout = d->ifi_opackets - state->net_tot_opax;
        state->net_tot_opax += delta_pout;

        update_net_statistics(state,delta_in,delta_out,delta_pin,delta_pout);
        break;
    }
    if (i == ifcount)
        syslog(LOG_WARNING,"no useful network interfaces / %d seen",ifcount);
}

void probe_disk(
    osdhud_state_t     *state)
{
}

void probe_battery(
    osdhud_state_t     *state)
{
}

void probe_uptime(
    osdhud_state_t     *state)
{
}
