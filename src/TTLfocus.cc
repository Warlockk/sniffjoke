/*
 *   SniffJoke is a software able to confuse the Internet traffic analysis,
 *   developed with the aim to improve digital privacy in communications and
 *   to show and test some securiy weakness in traffic analysis software.
 *   
 *   Copyright (C) 2010 vecna <vecna@delirandom.net>
 *                      evilaliv3 <giovanni.pellerano@evilaliv3.org>
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "TTLfocus.h"
#include "hardcoded-defines.h"
#include "Utils.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>

TTLFocus::TTLFocus(unsigned int destip) :
	daddr(destip),
	expiring_ttl(0),
        min_working_ttl(0xff),
	synack_ttl(0),
	sent_probe(0),
	received_probe(0),
	puppet_port(htons((random() % 15000) + 1100)),
	rand_key(random()),
	status(TTL_BRUTALFORCE)
{
	clock_gettime(CLOCK_REALTIME, &next_probe_time);
}

TTLFocus::TTLFocus(const TTLFocus& cpy) :
	daddr(cpy.daddr),
	expiring_ttl(cpy.expiring_ttl),
	min_working_ttl(cpy.min_working_ttl),
	synack_ttl(cpy.synack_ttl),
	sent_probe(cpy.sent_probe),
	received_probe(cpy.received_probe),
	puppet_port(cpy.puppet_port),
	rand_key(cpy.rand_key),
	status(cpy.status)
{
	memcpy(&next_probe_time, &(cpy.next_probe_time), sizeof(struct timespec));
}

TTLFocus::TTLFocus(const struct ttlfocus_cache_record& cpy) :
	daddr(cpy.daddr),
	expiring_ttl(cpy.expiring_ttl),
	min_working_ttl(cpy.min_working_ttl),
	synack_ttl(cpy.synack_ttl),
	sent_probe(cpy.sent_probe),
	received_probe(cpy.received_probe),
	puppet_port(cpy.puppet_port),
	rand_key(cpy.rand_key),
	status(cpy.status)
{
	clock_gettime(CLOCK_REALTIME, &next_probe_time);
}

void TTLFocus::scheduleNextProbe()
{
    if(TTLPROBEINTERVAL > 1000000000 - next_probe_time.tv_nsec) {
        next_probe_time.tv_sec++;
        next_probe_time.tv_nsec = next_probe_time.tv_nsec + TTLPROBEINTERVAL - 1000000000;
    } else
        next_probe_time.tv_nsec = next_probe_time.tv_nsec + TTLPROBEINTERVAL;
}

bool TTLFocus::isProbeIntervalPassed(const struct timespec& now)
{
    if(now.tv_sec > next_probe_time.tv_sec)
        return true;

    else if(now.tv_sec == next_probe_time.tv_sec && now.tv_nsec > next_probe_time.tv_nsec)
        return true;

    return false;
}

void TTLFocusMap::load()
{
	FILE *loadfd;
	int i = 0;
	int ret;
	struct ttlfocus_cache_record tmp;

	internal_log(NULL, VERBOSE_LEVEL, "loading ttlfocusmap from %s",  TTLFOCUSMAP_FILE);
	
	loadfd = fopen(TTLFOCUSMAP_FILE, "r");
	if(loadfd == NULL) {
		internal_log(NULL, ALL_LEVEL, "unable to access %s: sniffjoke will start without a ttl cache", TTLFOCUSMAP_FILE);
        	return;
        }

	while( (ret = fread(&tmp, sizeof(struct ttlfocus_cache_record), 1, loadfd)) == 1 ) {
		i++;
		TTLFocus *ttlfocus = new TTLFocus(tmp);
		insert(pair<int, TTLFocus>(ttlfocus->daddr, *ttlfocus));
	}

	fclose(loadfd);

	if(ret != 0) {
		unlink(TTLFOCUSMAP_FILE);
		internal_log(NULL, ALL_LEVEL, "unable to read ttlfocus from %s: %s",
			TTLFOCUSMAP_FILE, strerror(errno)
		);
		check_call_ret("reading ttlfocus file", errno, (ret - 1), false);
	}
	internal_log(NULL, VERBOSE_LEVEL, "ttlfocusmap load completed: %d records loaded", i);
}


void TTLFocusMap::dump()
{
	FILE *dumpfd;
	int i = 0;
	int ret;
	TTLFocus* tmp;
	struct ttlfocus_cache_record cache_record;

	internal_log(NULL, VERBOSE_LEVEL, "dumping ttlfocusmap to %s",  TTLFOCUSMAP_FILE);

	dumpfd = fopen(TTLFOCUSMAP_FILE, "w");
        if(dumpfd == NULL) {
                internal_log(NULL, ALL_LEVEL, "unable to access %s: sniffjoke will not dump ttl cache", TTLFOCUSMAP_FILE);
                return;
        }


	for ( TTLFocusMap::iterator it = this->begin(); it != this->end(); it++ ) {
		i++;
		tmp = &(it->second);
		cache_record.daddr = tmp->daddr;
		cache_record.expiring_ttl = tmp->expiring_ttl;
		cache_record.min_working_ttl = tmp->min_working_ttl;
		cache_record.synack_ttl = tmp->synack_ttl;
		cache_record.sent_probe = tmp->sent_probe;
		cache_record.received_probe = tmp->received_probe;
		cache_record.puppet_port = tmp->puppet_port;
		cache_record.rand_key = tmp->rand_key;
		cache_record.status = tmp->status;

		ret = fwrite(&cache_record, sizeof(struct ttlfocus_cache_record), 1, dumpfd);
		if(ret != 1)
		{
			fclose(dumpfd);
			unlink(TTLFOCUSMAP_FILE);
			internal_log(NULL, ALL_LEVEL, "unable to write ttlfocus to %s: %s",
				TTLFOCUSMAP_FILE, strerror(errno)
			);
			check_call_ret("writing ttlfocus file", errno, (ret - 1), false);
			return;
		}
	}
	fclose(dumpfd);

	internal_log(NULL, VERBOSE_LEVEL, "ttlfocusmap dump completed: %d records dumped", i);
}

void TTLFocus::selflog(const char *func, const char *umsg) 
{
	const char *status_name;

	switch(status) {
		case TTL_KNOWN: status_name = "TTL known"; break;
		case TTL_BRUTALFORCE: status_name = "BRUTALFORCE running"; break;
		case TTL_UNKNOWN: status_name = "TTL UNKNOWN"; break;
		default: status_name = "badly unset TTL status"; break;
	}

	internal_log(NULL, SESSION_DEBUG, 
		"%s [%s] m_sent %d, m_recv %d m_expiring %d [%s]",
		func, status_name, sent_probe, received_probe, expiring_ttl, umsg
	);

	memset(debugbuf, 0x00, LARGEBUF);
}