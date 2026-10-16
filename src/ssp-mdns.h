/*
obs-ssp
 Copyright (C) 2019-2020 Yibai Zhang

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; If not, see <https://www.gnu.org/licenses/>
*/

#ifndef OBS_SSP_SSP_MDNS_H
#define OBS_SSP_SSP_MDNS_H
#include <mdns.h>

#include <string>
#include <map>
#include <vector>

struct ssp_device_item {
	std::string device_name;
	std::string ip_address;
};

struct mdns_record {
	bool has_ptr;
	std::string ptr_record;
	bool has_a;
	sockaddr_in a_record;
	bool has_aaaa;
	sockaddr_in6 aaaa_record;
	uint64_t last_available;
};

class SspMDnsIterator {
public:
	SspMDnsIterator();
	~SspMDnsIterator();
	bool hasNext();
	ssp_device_item *next();

private:
	/* A snapshot of the live records taken in the ctor; iteration never holds
	   the global lock. */
	std::vector<ssp_device_item> snapshot_;
	size_t index_ = 0;
};

void create_mdns_loop();
void stop_mdns_loop();

/* Ask the discovery thread to send its next PTR query at once instead of
   waiting out the 1 s interval. The dock's manual "rescan" uses this so a
   freshly powered-on camera is queried immediately rather than on the next
   scheduled tick. Safe to call from the OBS frontend thread at any time. */
void ssp_mdns_request_refresh();
#endif //OBS_SSP_SSP_MDNS_H
