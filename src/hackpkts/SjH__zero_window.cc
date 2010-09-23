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
#include "sj_hackpkts.h"
SjH__zero_window::SjH__zero_window(Packet pkt) : HackPacket(pkt) {
	debug_info = (char *)"zero_ window";
	prescription_probability = 93;
	hack_frequency = 5;
}

bool SjH__zero_window::condition(const Packet &pkt)
{
	return true;
}

void SjH__zero_window::hack()
{
	resizePayload(0);

	tcp->syn = tcp->fin = tcp->rst = 1;
	tcp->psh = tcp->ack = 0;
	tcp->window = 0;
}
