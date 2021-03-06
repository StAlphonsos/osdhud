/*
 * Copyright (c) 2004 Markus Friedl <markus@openbsd.org>
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
 *
 * Portions of this code were also taken from
 * /usr/src/usr.bin/systat/systat.h, which has a BSD 3-clause
 * license as of OpenBSD 5.5 preceded by the following copyright:
 * Copyright (c) 1980, 1989, 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Portions of this code were also taken from
 * /usr/src/usr.bin/top/machine.c, which has a BSD 3-clause
 * license as of OpenBSD 5.5 preceded by the following copyright:
 * Copyright (c) 1994 Thorsten Lockert <tholo@sigmasoft.com>
 * All rights reserved.
 *
 * Portions of this code were also taken from
 * /usr/src/usr.bin/vmstat/dkstats.c, which has the following
 * copyright and license:
 * Copyright (c) 1996 John M. Vinopal
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
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed for the NetBSD Project
 *      by John M. Vinopal.
 * 4. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Implement the probe_xxx() functions used by osdhud.c for OpenBSD.
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
#include <sys/param.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/sensors.h>
#include <sys/vmmeter.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/route.h>
#include <sys/sockio.h>
#include <sys/ioctl.h>
#include <sys/swap.h>
#include <machine/apmvar.h>
#include <sys/disk.h>
#include <xosd.h>
#include <Judy.h>
#include "movavg.h"
#include "osdhud.h"

#define APM_DEV "/dev/apm"
#define LOG1024        10
#define pagetok(size,ps) ((size) << ps)

/* lifted from systat ca. openbsd 5.5 */

struct ifcount {
	u_int64_t           ifc_ib;         /* input bytes */
	u_int64_t           ifc_ip;         /* input packets */
	u_int64_t           ifc_ie;         /* input errors */
	u_int64_t           ifc_ob;         /* output bytes */
	u_int64_t           ifc_op;         /* output packets */
	u_int64_t           ifc_oe;         /* output errors */
	u_int64_t           ifc_co;         /* collisions */
	int                 ifc_flags;      /* up / down */
	int                 ifc_state;      /* link state */
};

struct ifstat {
	char                ifs_name[IFNAMSIZ]; /* interface name */
	struct ifcount      ifs_cur;
	struct ifcount      ifs_old;
	struct ifcount      ifs_now;
	char                ifs_flag;
	int                 ifs_speed;
};

struct openbsd_data {
	int		    pageshift;
	int                 nifs;	/* number of interfaces */
	struct ifstat      *ifstats;	/* that many ifstat structs */
	struct timeval      boottime;
	struct swapent     *swap_devices;
	int                 ncpus;
	Pvoid_t             groups;	/* judy str->ptr hash for if groups */
	int                 ndrive;
	struct diskstats   *drive_stats;
	char              **drive_names;    /* points into drive_names_raw */
	char               *drive_names_raw;
	size_t              drive_names_raw_size;
	struct temp_sensor *temp_sensor;
};

/*
 * This is my first attempt at a table that maps media flags to
 * maximum interface speeds.  I gleaned most of these clues from
 * if_media.h, if.h, the netintro(4) and ifmedia(4) man pages and a
 * few other places in the system source.  It definitely needs some
 * work; many of the mbit/sec numbers in the table are probably not
 * quite right but I don't have Internet access as I write this so I
 * can't look things up easily...
 */

#define M_ETHER(ttt)    IFM_ETHER|IFM_##ttt
#define M_FDDI(ttt)     IFM_FDDI|IFM_FDDI_##ttt
#define M_WIFI(ttt)     IFM_IEEE80211|IFM_IEEE80211_##ttt

static struct { int bits; int mbit_sec; } media_speeds[] = {
	{ M_ETHER(10_T),              10 },
	{ M_ETHER(10_2),              10 },
	{ M_ETHER(10_5),              10 },
	{ M_ETHER(100_TX),           100 },
	{ M_ETHER(100_FX),           100 },
	{ M_ETHER(100_T4),           100 },
	{ M_ETHER(100_VG),           100 },
	{ M_ETHER(100_T2),           100 },
	{ M_ETHER(1000_SX),         1000 },
	{ M_ETHER(10_STP),            10 },
	{ M_ETHER(10_FL),             10 },
	{ M_ETHER(1000_LX),         1000 },
	{ M_ETHER(1000_CX),         1000 },
	{ M_ETHER(HPNA_1),             1 },
	{ M_ETHER(10G_LR),            10 },
	{ M_ETHER(10G_SR),            10 },
	{ M_ETHER(10G_CX4),           10 },
	{ M_ETHER(2500_SX),         2500 },
	{ M_ETHER(10G_T),           1000 },
	{ M_ETHER(10G_SFP_CU),      1000 },

	/* FDDI ?  Don't know speeds, no net access now, just guessing XXX */

	{ M_FDDI(SMF),               100 },
	{ M_FDDI(MMF),               100 },
	{ M_FDDI(UTP),               100 },

	/* 802.11xxx */

	{ M_WIFI(FH1),                 1 },
	{ M_WIFI(FH2),                 2 },
	{ M_WIFI(DS2),                 2 },
	{ M_WIFI(DS5),                 5 },
	{ M_WIFI(DS11),               11 },
	{ M_WIFI(DS1),                 1 },
	{ M_WIFI(DS22),               22 },
	{ M_WIFI(OFDM6),               6 },
	{ M_WIFI(OFDM9),               9 },
	{ M_WIFI(OFDM12),             12 },
	{ M_WIFI(OFDM18),             18 },
	{ M_WIFI(OFDM24),             24 },
	{ M_WIFI(OFDM48),             48 },
	{ M_WIFI(OFDM54),             54 },
	{ M_WIFI(OFDM72),             72 },

	/* XXX add more later */

	{ 0,                          10 }, /* default for the 1st world :-) */
};

/* For the temperature probe */

SLIST_HEAD(, temp_sensor) temp_sensors;
struct temp_sensor {
	SLIST_ENTRY(temp_sensor) entries;
	char name[128];
	int mib[5];
	char desc[128];
	double val;
};
int n_temp_sensors;

static void
load_temperature_sensors()
{
	int mib[5] = { CTL_HW, HW_SENSORS, 0, 0, 0 };
	int dev;
	struct sensordev snsrdev;
	size_t sdlen = sizeof(snsrdev);
	int numt;
	struct sensor snsr;
	size_t slen = sizeof(snsr);
	struct temp_sensor *tail;

	SLIST_INIT(&temp_sensors);
	n_temp_sensors = 0;
	tail = NULL;
	for (dev = 0; ; dev++) {
		mib[2] = dev;
		if (sysctl(mib, 3, &snsrdev, &sdlen, NULL, 0) == -1) {
			if (errno == ENXIO)
				continue;
			if (errno == ENOENT)
				break;
		}
		mib[3] = SENSOR_TEMP;
		for (numt = 0; numt < snsrdev.maxnumt[mib[3]]; numt++){
			mib[4] = numt;
			if (sysctl(mib, 5, &snsr, &slen, NULL, 0) == -1)
				continue;
			if (slen && !(snsr.flags & SENSOR_FINVALID)) {
				struct temp_sensor *s;

				s = malloc(sizeof(struct temp_sensor));
				assert_snprintf(s->name,"hw.sensors.%s.temp%d",
						snsrdev.xname, numt);
				s->mib[0] = mib[0];
				s->mib[1] = mib[1];
				s->mib[2] = mib[2];
				s->mib[3] = mib[3];
				s->mib[4] = mib[4];
				s->desc[0] = 0;
				assert_strlcpy(s->desc, snsr.desc);
				s->val = (snsr.value - 273150000) / 1000000.0;
				if (tail == NULL) {
					SLIST_INSERT_HEAD(&temp_sensors, s,
							  entries);
					tail = s;
				} else {
					SLIST_INSERT_AFTER(tail, s, entries);
					tail = s;
				}
				n_temp_sensors++;
			}
		}
	}
}

static struct temp_sensor *
find_temperature_sensor(char *name)
{
	struct temp_sensor *s;

	SLIST_FOREACH(s, &temp_sensors, entries)
		if (!strcmp(s->name, name))
			return s;
	return NULL;
}

static void
update_temperature_sensors()
{
	struct temp_sensor *s;
	struct sensor snsr;
	size_t slen = sizeof(snsr);

	SLIST_FOREACH(s, &temp_sensors, entries) {
		if (sysctl(s->mib, 5, &snsr, &slen, NULL, 0) == -1)
			continue;
		s->val = (snsr.value - 273150000) / 1000000.0;
	}
}

void
print_temperature_sensors()
{
	struct temp_sensor *s;

	if (!n_temp_sensors)
		load_temperature_sensors();
	printf("Valid temperature sensors and their current values:\n");
	SLIST_FOREACH(s, &temp_sensors, entries)
		printf("%s = %.2f degC%s%s%s\n", s->name, s->val,
		       s->desc[0] ? " (": "", s->desc, s->desc[0]? ")": "");
}

/* also from systat/if.c */

static void
rt_getaddrinfo(struct sockaddr *sa, int addrs, struct sockaddr **info)
{
	int i;

	for (i = 0; i < RTAX_MAX; i++) {
		if (addrs & (1 << i)) {
			info[i] = sa;
			sa = (struct sockaddr *)
				((char *)(sa) +
				 roundup(sa->sa_len, sizeof(long)));
		} else
			info[i] = NULL;
	}
}

void
probe_init(struct osdhud_state *state)
{
	int mib[2] = { CTL_KERN, KERN_BOOTTIME };
	int pagesize;
	struct openbsd_data *obsd;
	size_t size = 0;


	obsd = (struct openbsd_data *)malloc(sizeof(struct openbsd_data));
	assert(obsd);
	obsd->nifs = 0;
	obsd->ifstats = NULL;

	/* adapted from /usr/src/usr.bin/top/machine.c as of OpenBSD 5.5 */
	pagesize = getpagesize();
	obsd->pageshift = 0;
	while (pagesize > 1) {
		obsd->pageshift++;
		pagesize >>= 1;
	}
	obsd->pageshift -= LOG1024;

	size = sizeof(obsd->boottime);
	assert(!sysctl(mib,2,&obsd->boottime,&size,NULL,0));

	if (!state->nswap)
		obsd->swap_devices = NULL;
	else {
		obsd->swap_devices = (struct swapent *)calloc(
			state->nswap,sizeof(struct swapent));
		assert(obsd->swap_devices);
	}

	mib[0] = CTL_HW;
	mib[1] = HW_NCPU;
	size = sizeof(obsd->ncpus);
	obsd->ncpus = 0;
	assert(!sysctl(mib,2,&obsd->ncpus,&size,NULL,0));
	if (!state->max_load_avg)
		state->max_load_avg = 2.0 * (float)obsd->ncpus; /* xxx 2? */
	VSPEW("ncpus=%d, max load avg=%f",obsd->ncpus,state->max_load_avg);
	obsd->groups = NULL;

	obsd->ndrive = 0;
	obsd->drive_stats = NULL;
	obsd->drive_names = NULL;
	obsd->drive_names_raw = NULL;
	obsd->drive_names_raw_size = 0;

	obsd->temp_sensor = NULL;
	load_temperature_sensors();
	if (n_temp_sensors) {
		struct temp_sensor *tsens;

		tsens = SLIST_FIRST(&temp_sensors);
		if (state->temp_sensor_name != NULL) {
			tsens = find_temperature_sensor(
				state->temp_sensor_name);
			if (tsens == NULL) {
				tsens = SLIST_FIRST(&temp_sensors);
				syslog(LOG_ERR, "invalid temp sensor '%s'"
				       " - using '%s' instead",
				       state->temp_sensor_name, tsens->name);
			}
		}
		obsd->temp_sensor = tsens;
		free(state->temp_sensor_name);
		state->temp_sensor_name = strdup(tsens->name);
	}

	state->per_os_data = (void *)obsd;
}

void
probe_cleanup(struct osdhud_state *state)
{
	if (state->per_os_data) {
		struct openbsd_data *obsd =
			(struct openbsd_data *)state->per_os_data;
		Word_t *jvp = NULL;
		int rc = 0;
		uint8_t group[IFNAMSIZ] = { 0 };

		free(obsd->drive_stats);
		free(obsd->drive_names);
		free(obsd->drive_names_raw);
		free(obsd->swap_devices);
		free(obsd->ifstats);
		obsd->ifstats = NULL;
		obsd->nifs = 0;
		/* for each group name */
		JSLF(jvp,obsd->groups,group);
		while (jvp) {
			/* get the set of interfaces */
			Pvoid_t ifaces = *(Pvoid_t *)jvp;

			JSLFA(rc,ifaces);           /* free the set */
			JSLN(jvp,obsd->groups,group); /* next group */
		}
		JSLFA(rc,obsd->groups);         /* now free the groups hash */
		obsd->groups = NULL;
		free(obsd);
	}
}

void
probe_load(struct osdhud_state *state)
{
	double avgs[1] = { 0.0 };

	if (getloadavg(avgs,ARRAY_SIZE(avgs)) < 0) {
		SPEWE("getloadavg");
		exit(1);
	}
	state->load_avg = avgs[0];
}

void
probe_mem(struct osdhud_state *state)
{
	struct openbsd_data *obsd = (struct openbsd_data *)state->per_os_data;
	static int vmtotal_mib[] = {CTL_VM, VM_METER};
	struct vmtotal vmtotal;
	size_t size = sizeof(vmtotal);
	float tot_kbytes, act_kbytes;

	size = sizeof(vmtotal);
	if (sysctl(vmtotal_mib,ARRAY_SIZE(vmtotal_mib),&vmtotal,&size,NULL,0)) {
		SPEWE("sysctl");
		memset(&vmtotal,0,sizeof(vmtotal));
	}
	tot_kbytes = (float)pagetok(vmtotal.t_rm,obsd->pageshift);
	act_kbytes = (float)pagetok(vmtotal.t_arm,obsd->pageshift);
	state->mem_used_percent = tot_kbytes ? act_kbytes / tot_kbytes : 0;
}

void
probe_swap(struct osdhud_state *state)
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

static int
get_speed(int sock,char *name,struct osdhud_state *state)
{
	struct ifmediareq media;
	int act, mbit_sec, i;

	memset(&media,0,sizeof(media));
	assert_strlcpy(media.ifm_name,name);
	mbit_sec = 0;
	if (ioctl(sock,SIOCGIFMEDIA,&media) != 0)
		goto DONE;
	act = media.ifm_active & 0xff; /* low 8 bits */
	for (i = 0; i < ARRAY_SIZE(media_speeds); i++)
		if (media_speeds[i].bits == act) {
			mbit_sec = media_speeds[i].mbit_sec;
			break;
		}
	if (i == ARRAY_SIZE(media_speeds))
		/* Last entry is default */
		mbit_sec = media_speeds[i-1].mbit_sec;
	VSPEW("iface %s media cur 0x%llx mask 0x%llx status 0x%llx "
	      "active 0x%llx count=%d: %d mbit/sec",name,media.ifm_current,
	      media.ifm_mask,media.ifm_status,media.ifm_active,
	      media.ifm_count,mbit_sec);
DONE:
	return mbit_sec;
}

static void
suss_groups(int sock,char *name,struct osdhud_state *state)
{
	struct openbsd_data *os_data =
		(struct openbsd_data *)state->per_os_data;
	struct ifgroupreq groups;
	int ngroups, i;

	memset(&groups,0,sizeof(groups));
	assert_strlcpy(groups.ifgr_name,name);
	if (ioctl(sock,SIOCGIFGROUP,&groups)) {
		perror("ioctl(SIOCGIFGROUP) 1st time");
		return;
	}
	ngroups = groups.ifgr_len;
	groups.ifgr_groups =
		(struct ifg_req *)calloc(ngroups,sizeof(struct ifg_req));
	assert(groups.ifgr_groups);
	if (ioctl(sock,SIOCGIFGROUP,&groups)) {
		perror("ioctl(SIOCGIFGROUP) 2nd time");
		goto DONE;
	}
	/* We build a hash of hashes (effectively):
	 *    { group_name -> { iface_name -> 1, ... } }
	 */
	for (i = 0; i < groups.ifgr_len; i++) {
		char *group = groups.ifgr_groups[i].ifgrq_group;
		Pvoid_t ifaces = NULL;
		Word_t *jvp = NULL, *jvp2 = NULL;

		VSPEW("%s group#%d/%d %s",name,i,
		      groups.ifgr_len,group);
		JSLG(jvp,os_data->groups,group);
		ifaces = jvp ? *(Pvoid_t *)jvp : NULL;
		JSLG(jvp2,ifaces,name);
		if (!jvp2) {
			JSLI(jvp2,ifaces,name);
			assert(jvp2);
			*jvp2 = 1;
		}
		if (!jvp)
			JSLI(jvp,os_data->groups,group);
		assert(jvp);
		*jvp = (Word_t)ifaces;
	}
DONE:
	free(groups.ifgr_groups);
}

void
probe_net(struct osdhud_state *state)
{
	char *buf, *next, *lim;
	size_t need;
	int i;
	int mib[6] = { CTL_NET, AF_ROUTE, 0, 0, NET_RT_IFLIST, 0 };
	struct openbsd_data *os_data =
		(struct openbsd_data *)state->per_os_data;
	struct ifstat *ifstats = os_data->ifstats;
	int nifs = os_data->nifs;
	struct if_msghdr ifm;
	struct sockaddr *info[RTAX_MAX];
	struct sockaddr_dl *sdl;
	int num_ifs = 0;
	struct ifstat *ifs;
	unsigned long tot_delta_in_b = 0;
	unsigned long tot_delta_out_b = 0;
	unsigned long tot_delta_in_p = 0;
	unsigned long tot_delta_out_p = 0;
	int interest;

	/*
	 * This logic lifted directly from fetchifstat() in
	 * /usr/src/usr.bin/systat/if.c ca. OpenBSD 5.5.
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
		memcpy(&ifm,next,sizeof(ifm));
		/* We might get back all kinds of things; filter for just the
		 * ones we want to examine.
		 */
		if (ifm.ifm_version != RTM_VERSION ||
		    ifm.ifm_type != RTM_IFINFO ||
		    !(ifm.ifm_addrs & RTA_IFP))
			continue;
		/* Expand array of ifs as needed so it includes ifm_index */
		if (ifm.ifm_index >= nifs) {
			struct ifstat *newstats =
				realloc(ifstats,(ifm.ifm_index + 4) *
					sizeof(struct ifstat));
			assert(newstats);
			ifstats = newstats;
			for (; nifs < ifm.ifm_index + 4; nifs++)
				memset(&ifstats[nifs],0,sizeof(*ifstats));
		}
		ifs = &ifstats[ifm.ifm_index];
		if (ifs->ifs_name[0] == '\0') { /* index not seen yet */
			memset(&info,0,sizeof(info));
			/* This gets a bunch of metadata,
			 * only one bit of which we want
			 */
			rt_getaddrinfo(
				(struct sockaddr *)((struct if_msghdr *)next+1),
				ifm.ifm_addrs, info);
			sdl = (struct sockaddr_dl *)info[RTAX_IFP];
			if (sdl && sdl->sdl_family == AF_LINK &&
			    sdl->sdl_nlen > 0) {
				/* This is an actual interface with a name */
				int s;

				/* Query interface media and group membership */
				memcpy(ifs->ifs_name,sdl->sdl_data,
				       sdl->sdl_nlen);
				ifs->ifs_name[sdl->sdl_nlen] = '\0';
				s = socket(AF_INET, SOCK_DGRAM, 0);
				assert(s >= 0);
				ifs->ifs_speed =
					get_speed(s,ifs->ifs_name,state);
				suss_groups(s,ifs->ifs_name,state);
				close(s);
			}
			if (ifs->ifs_name[0] == '\0') /* was not interesting */
				continue;
		}
		num_ifs++;
		/* Am not using all of these yet... */
#define ifi_x(x) ifm.ifm_data.ifi_##x
		ifs->ifs_cur.ifc_flags = ifm.ifm_flags;
		ifs->ifs_cur.ifc_state = ifm.ifm_data.ifi_link_state;
		ifs->ifs_flag++;
		/*
		 * If no interface specification was given we pick the first
		 * non-loopback interface we find as the one we care about.
		 * Arbitrary.
		 */
		if (!state->net_iface && strncmp(ifs->ifs_name,"lo",2)) {
			/* first non-loopback interface */
			state->net_iface = strdup(ifs->ifs_name);
			VSPEW("choosing first non-loopback interface: %s",
			      state->net_iface);
		}
		interest = state->net_iface &&
			!strcmp(state->net_iface,ifs->ifs_name);
		if (state->net_iface && !interest) {
			/* Is the name they gave us a group name? */
			Word_t *jvp = (Word_t *)0;

			/* Use -i value as key into group hash */
			JSLG(jvp,os_data->groups,state->net_iface);
			if (jvp) {
				Pvoid_t group_ifaces = *(Pvoid_t *)jvp;

				/* Now check for this in that group */
				jvp = NULL;
				JSLG(jvp,group_ifaces,ifs->ifs_name);
				interest = !!jvp;
			}
		}
		/* after all is said and done, are we interested? */
		if (interest) {
#define delta_x(nn) ifi_x(nn) - state->net_tot_##nn
			unsigned long delta_in_b = delta_x(ibytes);
			unsigned long delta_out_b = delta_x(obytes);
			unsigned long delta_in_p = delta_x(ipackets);
			unsigned long delta_out_p = delta_x(opackets);
#undef delta_x
			if (!state->net_speed_mbits) {
				VSPEW("%s net_speed_mbits = %d",
				      ifs->ifs_name,ifs->ifs_speed);
				state->net_speed_mbits = ifs->ifs_speed;
			}
			tot_delta_in_b += delta_in_b;
			tot_delta_out_b += delta_out_b;
			tot_delta_in_p += delta_in_p;
			tot_delta_out_p += delta_out_p;
			state->net_tot_ibytes = ifi_x(ibytes);
			state->net_tot_obytes = ifi_x(obytes);
			state->net_tot_ipackets = ifi_x(ipackets);
			state->net_tot_opackets = ifi_x(opackets);
		}
#undef ifi_x
	}
	update_net_statistics(state,tot_delta_in_b,tot_delta_out_b,
			      tot_delta_in_p,tot_delta_out_p);
	/* remove unreferenced interfaces */
	for (i = 0; i < nifs; i++) {
		ifs = &ifstats[i];
		if (ifs->ifs_flag)
			ifs->ifs_flag = 0;
		else
			ifs->ifs_name[0] = '\0';
	}
	free(buf);
	os_data->ifstats = ifstats;
	os_data->nifs = nifs;
}

#if 0
/* Many clues taken from /usr/src/usr.bin/vmstat/dkstats.c */
/* XXX incomplete */
void
probe_disk(struct osdhud_state *state)
{
	struct openbsd_data *obsd = (struct openbsd_data *)state->per_os_data;
	int ndrive = 0;
	int mib[2];
	size_t size;

	/* How many drives are there? */
	mib[0] = CTL_HW;
	mib[1] = HW_DISKCOUNT;
	size = sizeof(ndrive);
	assert(!sysctl(mib,2,&ndrive,&size,NULL,0));
	assert(ndrive);                    /* ... otherwise, ? */
	/* Get ready to acquire data */
	if (ndrive != obsd->ndrive) {
		free(obsd->drive_stats);
		free(obsd->drive_names);
		free(obsd->drive_names_raw);
		obsd->drive_stats = NULL;
		obsd->drive_names = NULL;
		obsd->drive_names_raw_size = 0;
		obsd->drive_names_raw = NULL;
		obsd->ndrive = ndrive;
	}
	/* Get drive stats */
	size = obsd->ndrive * sizeof(struct diskstats);
	if (!obsd->drive_stats)
		obsd->drive_stats = malloc(size);
	assert(obsd->drive_stats);
	memset(obsd->drive_stats,0,size);
	mib[1] = HW_DISKSTATS;
	assert(!sysctl(mib,2,obsd->drive_stats,&size,NULL,0));
	/* Get drive names (raw list, comma-sep) */
	size = 0;
	mib[1] = HW_DISKNAMES;
	assert(!sysctl(mib,2,NULL,&size,NULL,0));
	if (obsd->drive_names_raw_size != size)
		obsd->drive_names_raw = realloc(obsd->drive_names_raw,size);
	assert(obsd->drive_names_raw);
	obsd->drive_names_raw_size = size;
	memset(obsd->drive_names_raw,0,size);
	assert(!sysctl(mib,2,obsd->drive_names_raw,&size,NULL,0));
	/* Make sure there is space to store drive name pointers */
	if (!obsd->drive_names)
		obsd->drive_names = calloc(obsd->ndrive,sizeof(char *));
	VSPEW("found %d disks",obsd->ndrive);
	{
		int i;
		char *bufpp = obsd->drive_names_raw;
		char *name;

		for (i = 0; i<obsd->ndrive && (name=strsep(&bufpp,",")); i++) {
			struct diskstats *d = &obsd->drive_stats[i];

			obsd->drive_names[i] = name;
			/* name is something like: sd0:e6149932cc95fda9,... */
			while (*name && *name != ':')
				name++;
			if (*name == ':')
				*name = '\0';
			/* Jump to next name */
			name = strsep(&bufpp,",");
#ifdef DEBUG
#define dd(nn) d->ds_##nn
			VSPEW("disk#%d: %s rxfer=%llu wxfer=%llu seek=%llu"
			      " rbytes=%llu wbyes=%llu",i,obsd->drive_names[i],
			      dd(rxfer),dd(wxfer),dd(seek),dd(rbytes),
			      dd(wbytes));
#undef dd
#endif
		}
	}
}
#endif

/* c.f. apm(4) */
void
probe_battery(struct osdhud_state *state)
{
	int apm;
	struct apm_power_info info;

	if (state->battery_missing)
		return;
	apm = open(APM_DEV,O_RDONLY);
	if (apm < 0) {
		perror(APM_DEV);
		return;
	}
	bzero(&info,sizeof(info));
	if (ioctl(apm,APM_IOC_GETPOWER,&info) < 0) {
		perror("APM_IOC_GETPOWER");
		close(apm);
		return;
	}
	if (info.battery_state == APM_BATTERY_ABSENT)
		state->battery_missing = 1;
	else {
		char *bat = NULL;
		char *ac = NULL;

		switch (info.battery_state) {
		case APM_BATT_HIGH:
			bat = "high";
			break;
		case APM_BATT_LOW:
			bat = "low";
			break;
		case APM_BATT_CRITICAL:
			bat = "critical";
			break;
		case APM_BATT_CHARGING:
			bat = "charging";
			break;
		case APM_BATT_UNKNOWN:
			bat = "unk";
			break;
		default:
			bat = "?";
			break;
		}
		switch (info.ac_state) {
		case APM_AC_OFF:
			ac = "no ac";
			break;
		case APM_AC_ON:
			ac = "ac on";
			break;
		case APM_AC_BACKUP:
			ac = "backup";
			break;
		case APM_AC_UNKNOWN:
			ac = "unk";
		default:
			ac = "?";
			break;
		}
		assert_snprintf(state->battery_state,"%s/%s",bat,ac);
		state->battery_life = info.battery_life;
		state->battery_time = info.minutes_left;
	}
	close(apm);
}

void
probe_temperature(struct osdhud_state *state)
{
	struct openbsd_data *obsd = (struct openbsd_data *)state->per_os_data;

	if (!n_temp_sensors)
		return;
	update_temperature_sensors();
	if (strcmp(state->temp_sensor_name, obsd->temp_sensor->name)) {
		/* sensor was changed on the fly... */
		struct temp_sensor *tsens;

		tsens = find_temperature_sensor(state->temp_sensor_name);
		if (tsens)
			obsd->temp_sensor = tsens;
		else {
			syslog(LOG_ERR, "invalid temp sensor name '%s'",
				state->temp_sensor_name);
			free(state->temp_sensor_name);
			state->temp_sensor_name =
				strdup(obsd->temp_sensor->name);
		}
	}
	state->temperature = obsd->temp_sensor->val;
}

void
probe_uptime(struct osdhud_state *state)
{
	time_t now;
	struct openbsd_data *obsd = (struct openbsd_data *)state->per_os_data;

	(void) time(&now);
	state->sys_uptime = now - obsd->boottime.tv_sec;
}

/*
 * Local variables:
 * mode: c
 * c-file-style: "bsd"
 * tab-width: 8
 * indent-tabs-mode: t
 * End:
 */
