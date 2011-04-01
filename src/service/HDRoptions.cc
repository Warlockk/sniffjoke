/*
 *   SniffJoke is a software able to confuse the Internet traffic analysis,
 *   developed with the aim to improve digital privacy in communications and
 *   to show and test some securiy weakness in traffic analysis software.
 *   
 * Copyright (C) 2010, 2011 vecna <vecna@delirandom.net>
 *                          evilaliv3 <giovanni.pellerano@evilaliv3.org>
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

/*
 * Handling randomized ip/tcp options.. WHAT dirty job!
 * 
 * good ipoptions mean options that don't cause the discarging of packets,
 * they need to exist in order to avoid arbitrary discrimination. 
 *
 * the future focus of those routine is to integrate the choosing of be
 * a bad or a good ipoptions analyzing the remote OS.
 *
 * - rules for adding: check the link :
 *   http://www.iana.org/assignments/ip-parameters 
 *   test versus Linux/BSD/win/lose, submit to us, we are happy to add
 *   every bit of randomization available.
 *
 * I've based a lot of consideration on:
 * http://lxr.oss.org.cn/source/net/ipv4/ip_options.c?v=2.6.36#L250
 *
 * but checking:
 * http://www.faqs.org/rfcs/rfc1812.html
 * seems that some weird ipoptions will cause a packet to be discarded
 * on the route, without ever reach the server. we aim to create 
 * ipoptions accepted by the router, and discarded from the remote host.
 */

#include "HDRoptions.h"
#include "IPTCPoptApply.h"
#include "Utils.h"

/* Now start the implementation of HDRoptions member */
HDRoptions::HDRoptions(injector_t t, Packet &pkt, TTLFocus &ttlfocus) :
type(t),
pkt(pkt),
ttlfocus(ttlfocus),
corruptRequest(false),
corruptNow(false),
corruptDone(false),
nextPlannedInj(SJ_NULL_OPT)
{
    optionLoader optConfigData;
    optionImplement *usableOption;

    if(optConfigData.isFileLoaded == false)
        RUNTIME_EXCEPTION("Invalid use of HDRoptions: is not configured optionLoader statis objs");

    switch (type)
    {
    case IPOPTS_INJECTOR:

        /* initialization of header and index */
        oD.actual_opts_len = pkt.iphdrlen - sizeof (struct iphdr);
        oD.target_opts_len = oD.actual_opts_len;
        oD.available_opts_len = MAXIPOPTIONS - oD.actual_opts_len;

        oD.optshdr.resize(MAXIPOPTIONS, IPOPT_EOL);
        memcpy((void *) &oD.optshdr[0], (uint8_t *) pkt.ip + sizeof (struct iphdr), oD.actual_opts_len);

        /* initialized the vector using the pointer of the static private data
         * in optionLoaded::loadedOptions */
        for( optConfigData.getInitializedOpts(IPPROTO_IP); (usableOption = optConfigData.getNextOpts()) != NULL; )
        {
            /* specific per-classes initialization need to be called here */
            if(usableOption->sjOptIndex == SJ_IPOPT_TIMESTOVERFLOW)
                (reinterpret_cast<Io_TIMESTOVERFLOW *>(usableOption))->setupTTLFocus(&ttlfocus);

            availOpts.push_back( usableOption );
        }

        protD.NOP_code = IPOPT_NOOP;
        protD.END_code = IPOPT_END;
        protD.protoName = "IP";

        break;

    case TCPOPTS_INJECTOR:
        oD.actual_opts_len = pkt.tcphdrlen - sizeof (struct tcphdr);
        oD.target_opts_len = oD.actual_opts_len;
        oD.available_opts_len = MAXTCPOPTIONS - oD.actual_opts_len;

        oD.optshdr.resize(MAXTCPOPTIONS, TCPOPT_EOL);
        memcpy((void *) &oD.optshdr[0], (uint8_t *) pkt.tcp + sizeof (struct tcphdr), oD.actual_opts_len);

        /* initialized the vector using the pointer of the static private data
         * in optionLoaded::loadedOptions */
        for( optConfigData.getInitializedOpts(IPPROTO_TCP); (usableOption = optConfigData.getNextOpts()) != NULL; )
            availOpts.push_back( usableOption );

        /* fix the appropriate protocol specification */
        protD.NOP_code = TCPOPT_NOP;
        protD.END_code = TCPOPT_EOL;
        protD.protoName = "TCP";

        break;
    }

    acquirePresentOptions();
}

/*
 *    returns true if injection is possible, false instead;
 *    in addition it registers the presence of some options.
 */
bool HDRoptions::acquirePresentOptions(void)
{
    for (uint8_t i = 0; i < oD.actual_opts_len;)
    {
        uint8_t * const option = &oD.optshdr[i];

        /* NOP_code will be IPOPT_NOOP or TCPOPT_NOP either */
        if (*option == protD.NOP_code )
        {
            i++;
            continue;
        }

        if (*option == protD.END_code)
            break;

        uint8_t option_len = (uint8_t) oD.optshdr[i + 1];
        if (option_len == 0 || option_len > (oD.actual_opts_len - i))
        {
            /*
             * the packet contains invalid options
             * we avoid injection regardless of the corrupt value.
             *
             * REMIND: 
             * will this became a vulnerability if we check incoming packet 
             */
            RUNTIME_EXCEPTION("invalid %s opt: option|%02x option_len|%u residual|%u", 
                              protD.protoName, *option, option_len, (oD.actual_opts_len - i));
        }

        bool identified = false;
        for (vector<optionImplement *>::iterator it = availOpts.begin(); it != availOpts.end(); ++it)
        {
            optionImplement *underVerify = *it;

            if (*option == underVerify->info.optValue)
            {
                identified = true;
                registerOptOccurrence(underVerify->sjOptIndex, i, option_len);
                break;
            }
        }

        if (!identified)
        {
            /*
             * analysis: will we make a malformed and stripping an option we don't know ?
             * I belive is better to return false if the code is running here, but I prefer
             * support every IP options available in the optMap[].
             * for this reason, in the beta and < 1.0 release the previous message
             * will be used for debug & progress pourposes.
             */
            LOG_PACKET("INFO: a non trapped %s-options: %02x", protD.protoName, *option);
        }

        i += option_len;
    }

    return true;
}

/* return false if the condition doesn't fit */
bool HDRoptions::checkCondition(optionImplement *isUsable)
{
    corruptNow = corruptRequest;

    /*
     * 1st global check: can we use this option ?
     * at the time a global enabled variable is used to permit selective testing
     */
    if (isUsable->info.enabled == false)
        return false;

    /*
     * 2nd global check: at which state of the injection are we?
     * we avoid corrupt options if we have just corrupted the packet
     * and we also alter the probability for the first injection
     * in favour of good injection.
     */
    if (corruptNow && (corruptDone || ((oD.actual_opts_len < 4) && RANDOM_PERCENT(40))))
        corruptNow = false;

    if (corruptNow)
    {
        /* if we have decided to corrupt we must avoid only NOT_CORRUPT options */
        if (isUsable->info.availableUsage != NOT_CORRUPT)
            return true;
    }
    else
    {
        /* if we have decided to no corrupt we must avoid ONESHOT and repeated options */
        if ((isUsable->info.availableUsage != ONESHOT) && !optTrack[isUsable->sjOptIndex].size())
        {
            nextPlannedInj = SJ_NULL_OPT;
            return true;
        }
    }

    return false;
}

void HDRoptions::registerOptOccurrence(uint8_t sjOptIndex, uint8_t offset, uint8_t len)
{
    struct option_occurrence occ;
    occ.off = offset;
    occ.len = len;
    optTrack[sjOptIndex].push_back(occ);
}

struct optionImplement * HDRoptions::updateCorruptAlign(struct optionImplement *oDesc, uint8_t addedLength)
{
    /* the first segment of code update the segments */
    oD.actual_opts_len += addedLength;
    oD.available_opts_len = oD.target_opts_len - oD.actual_opts_len;

    if(oDesc->info.availableUsage == ONESHOT)
        corruptDone = true;

    /* TWOSHOT management */
    if(oDesc->info.availableUsage == TWOSHOT)
    {
        if( optTrack[oDesc->info.optValue].size())
            corruptDone = true;

        if (corruptRequest && !corruptDone)
            return oDesc;
    }

    return NULL;
}


uint32_t HDRoptions::alignOpthdr()
{

    uint8_t alignBytes = (oD.actual_opts_len % 4) ? 4 - (oD.actual_opts_len % 4) : 0;
    if (alignBytes)
    {
        oD.optshdr[oD.actual_opts_len] = protD.END_code;

        oD.actual_opts_len += alignBytes;

        LOG_PACKET("*+ aligned to %u for %d bytes", oD.actual_opts_len, alignBytes);
    }

    return oD.actual_opts_len;
}

void HDRoptions::copyOpthdr(uint8_t * dst)
{
    memcpy(dst, &oD.optshdr[0], oD.actual_opts_len);
}

bool HDRoptions::isGoalAchieved()
{
    return corruptRequest == corruptDone;
}

bool HDRoptions::prepareInjection(bool corrupt, bool strip_previous)
{
    uint16_t freespace = MTU - pkt.pbuf.size();

    if (strip_previous)
    {
        freespace += oD.actual_opts_len;
        oD.actual_opts_len = 0;
    }

    // ip/tcp hdrlen must be a multiple of 4, we decrement by the modulus keeping count of MTU
    freespace -= freespace % 4;
    if (freespace == 0)
        return false;

    oD.target_opts_len = (type == IPOPTS_INJECTOR ? MAXIPOPTIONS : MAXTCPOPTIONS);
    if (freespace < oD.target_opts_len)
        oD.target_opts_len = freespace;

    oD.available_opts_len = oD.target_opts_len - oD.actual_opts_len;

    corruptRequest = corrupt;

    return true;
}

void HDRoptions::completeInjection()
{
    /* we cant put those info in protocolSpec because under pkt is
     * called a different method for IP than TCP */
    if (type == IPOPTS_INJECTOR)
    {
        pkt.iphdrResize(sizeof (struct iphdr) +oD.actual_opts_len);
        copyOpthdr((uint8_t *) pkt.ip + sizeof (struct iphdr));
    }
    else
    {
        pkt.tcphdrResize(sizeof (struct tcphdr) +oD.actual_opts_len);
        copyOpthdr((uint8_t *) pkt.tcp + sizeof (struct tcphdr));
    }
}

void HDRoptions::injector(uint8_t optIndex)
{
    optionImplement *requested = NULL;

    for (vector<optionImplement *>::iterator it = availOpts.begin(); it != availOpts.end(); ++it)
    {
        optionImplement *underVerify = *it;

        if(requested->sjOptIndex == optIndex)
            requested = underVerify;
    }

    if(requested == NULL)
        RUNTIME_EXCEPTION("Invalid index %u in registered protocol %s", optIndex, protD.protoName);

    LOG_PACKET("*1 %s option: total_opt_len|%u target_opt_len|%u (avail %u) goal|%s",
               protD.protoName, oD.actual_opts_len, oD.target_opts_len, 
               oD.available_opts_len, corruptRequest ? "CORRUPT" : "NOT CORRUPT");

    if (checkCondition(requested))
    {
        uint8_t ret = requested->optApply(&oD);

        if (ret)
        {
            registerOptOccurrence(requested->sjOptIndex, oD.actual_opts_len, ret);

            /* the planned option is used when a TWOSHOT define the second shot */
            nextPlannedInj = updateCorruptAlign(requested, ret);

            if (nextPlannedInj != SJ_NULL_OPT)
            {
                ret = nextPlannedInj->optApply(&oD);

                if (ret)
                {
                    registerOptOccurrence(nextPlannedInj->info.optValue, oD.actual_opts_len, ret);

                    if(updateCorruptAlign(nextPlannedInj, ret) != SJ_NULL_OPT)
                        RUNTIME_EXCEPTION("Invalid implementation of option #%d", nextPlannedInj->sjOptIndex);
                }
                else
                {
                    /* TWOSHOT FAIL: and now ?! no problem!
                     *
                     *  1) an other injection tries will follow;
                     *  2) if all the tries will fail probably a scramble downgrade will
                     *     solve the problem with no inconvenience.
                     */
                }

                nextPlannedInj = SJ_NULL_OPT;
            }

            alignOpthdr();
        }
    }

    LOG_PACKET("*2 %s option: total_opt_len|%u target_opt_len|%u (avail %u) goal|%s ",
               protD.protoName, oD.actual_opts_len, oD.target_opts_len, 
               oD.available_opts_len, isGoalAchieved() ? "ACHIEVED" : "NOT ACHIEVED");
}

void HDRoptions::randomInjector()
{
    LOG_PACKET("*1 %s option: total_opt_len|%u target_opt_len|%u (avail %u) goal|%s",
               protD.protoName, oD.actual_opts_len, oD.target_opts_len, 
               oD.available_opts_len, corruptRequest ? "CORRUPT" : "NOT CORRUPT");

    random_shuffle( availOpts.begin(), availOpts.end() );

    for (vector<optionImplement *>::iterator it = availOpts.begin(); it != availOpts.end(); ++it)
    {
        optionImplement *randOpt = *it;

        if (!checkCondition(randOpt))
            continue;

        uint8_t ret = randOpt->optApply(&oD);

        if (ret)
        {
            registerOptOccurrence(randOpt->sjOptIndex, oD.actual_opts_len, ret);

            /* the planned option is used when a TWOSHOT define the second shot */
            nextPlannedInj = updateCorruptAlign(randOpt, ret);

            /* the planned option is used when a TWOSHOT define the second shot */
            if (nextPlannedInj != SJ_NULL_OPT)
            {
                ret = nextPlannedInj->optApply(&oD);

                if (ret)
                {
                    registerOptOccurrence(nextPlannedInj->info.optValue, oD.actual_opts_len, ret);

                    if(updateCorruptAlign(nextPlannedInj, ret) != SJ_NULL_OPT)
                        RUNTIME_EXCEPTION("Invalid implementation of option #%d", nextPlannedInj->sjOptIndex);
                }
                else
                {
                    /* TWOSHOT FAIL: and now ?! no problem!
                     *
                     *  1) an other injection tries will follow;
                     *  2) if all the tries will fail probably a scramble downgrade will
                     *     solve the problem with no inconvenience.
                     */
                }

                nextPlannedInj = SJ_NULL_OPT;
            }
        }
    }

    alignOpthdr();

    LOG_PACKET("*2 %s option: total_opt_len|%u target_opt_len|%u (avail %u) goal|%s ",
               protD.protoName, oD.actual_opts_len, oD.target_opts_len, 
               oD.available_opts_len, isGoalAchieved() ? "ACHIEVED" : "NOT ACHIEVED");
}

bool HDRoptions::injectOpt(bool corrupt, bool strip_previous, uint8_t optIndex)
{
    bool goalAchieved = false;

    if (optIndex >= SUPPORTED_OPTIONS)
        RUNTIME_EXCEPTION("Invalid use of optcode index");

    pkt.SELFLOG("before %d opcode %s injection %sstrip iphdrlen|%u tcphdrlen|%u optshdr|%u pktlen|%u",
                optIndex, type == IPOPTS_INJECTOR ? "IP" : "TCP",
                strip_previous ? "" : "NOT ", 
                pkt.iphdrlen, pkt.tcphdrlen, oD.optshdr.size(), pkt.pbuf.size());

    if (prepareInjection(corrupt, strip_previous))
    {
        injector(optIndex);

        if((goalAchieved = isGoalAchieved()) == true)
            completeInjection();
    }

    pkt.SELFLOG("after %d opcode %s injection %sstrip iphdrlen|%u tcphdrlen|%u optshdr|%u pktlen|%u",
                optIndex, type == IPOPTS_INJECTOR ? "IP" : "TCP",
                strip_previous ? "" : "NOT ", 
                pkt.iphdrlen, pkt.tcphdrlen, oD.optshdr.size(), pkt.pbuf.size());

    return goalAchieved;
}

bool HDRoptions::injectRandomOpts(bool corrupt, bool strip_previous)
{
    bool goalAchieved = false;

    pkt.SELFLOG("before random %s injection %sstrip iphdrlen|%u tcphdrlen|%u optshdr|%u pktlen|%u",
                type == IPOPTS_INJECTOR ? "IP" : "TCP",
                strip_previous ? "" : "NOT ", 
                pkt.iphdrlen, pkt.tcphdrlen, oD.optshdr.size(), pkt.pbuf.size());

    if (prepareInjection(corrupt, strip_previous))
    {
        randomInjector();

        goalAchieved = isGoalAchieved();
        if (goalAchieved)
            completeInjection();
    }

    pkt.SELFLOG("after random %s injection %sstrip iphdrlen|%u tcphdrlen|%u optshdr|%u pktlen|%u",
                type == IPOPTS_INJECTOR ? "IP" : "TCP",
                strip_previous ? "" : "NOT ", 
                pkt.iphdrlen, pkt.tcphdrlen, oD.optshdr.size(), pkt.pbuf.size());

    return goalAchieved;
}

bool HDRoptions::removeOption(uint8_t opt)
{
    if (opt >= SUPPORTED_OPTIONS)
        RUNTIME_EXCEPTION("Invalid use of optcode index");

    /* if an option is request to be deleted, we need to check if it exists! */
    if (optTrack[opt].size() == false)
        return false;

    for (vector<option_occurrence>::iterator it = optTrack[opt].begin(); it != optTrack[opt].end(); it = optTrack[opt].erase(it))
    {
        vector<unsigned char>::iterator start = oD.optshdr.begin() + it->off;
        vector<unsigned char>::iterator end = start + it->len;
        oD.optshdr.erase(start, end);

        oD.target_opts_len -= it->len;
        oD.actual_opts_len -= it->len;
    }

    alignOpthdr();

    completeInjection();

    return true;
}

/* all the derived classes implemented in IPTCPoptApply call this constructor */
optionImplement::optionImplement(bool enable, uint8_t sjI, const char *n, uint8_t proto, uint8_t opcode, corruption_t c)
{
    sjOptIndex = sjI;

    info.enabled = enable;
    info.availableUsage = c;
    info.optValue = opcode;
    info.optProtocol = proto;
    info.optName = strdup(n);
}

optionImplement::~optionImplement()
{
    free( (void *)info.optName );
}

uint8_t optionImplement::optApply(struct optHdrData *xxx)
{
    RUNTIME_EXCEPTION("Invalid used of class optionImplement (optApply must be treated as virtual)");
}

/* this is the utility function used by the single option adder to calculate the best fit size for an option */
uint8_t optionImplement::getBestRandsize(struct optHdrData *oD, uint8_t fixedLen, uint8_t minRblks, uint8_t maxRblks, uint8_t blockSize)
{
    uint8_t minComputed = fixedLen + (minRblks * blockSize);
    uint8_t maxComputed = fixedLen + (maxRblks * blockSize);

    if (oD->available_opts_len == minComputed || oD->available_opts_len == maxComputed)
        return oD->available_opts_len;

    if (oD->available_opts_len < minComputed)
        return 0;

    if (oD->available_opts_len > maxComputed)
    {
        return (((random() % (maxRblks - minRblks + 1)) + minRblks) * blockSize) +fixedLen;
    }
    else /* should try the best filling of memory and the NOP fill after */
    {
        uint8_t blockNumber = (oD->available_opts_len - fixedLen) / blockSize;
        return (blockNumber * blockSize) + fixedLen;
    }
}

/* the optionLoader class work as inizializator for the HDRoptions */
optionImplement * optionLoader::loadedOptions[SUPPORTED_OPTIONS];
bool optionLoader::isFileLoaded;
uint8_t optionLoader::settedProto;
uint8_t optionLoader::counter;

void optionLoader::getInitializedOpts(uint8_t reqProto)
{
    /* static variables */
    settedProto = reqProto;
    counter = 0;
}

optionImplement * optionLoader::getNextOpts(void)
{
    ++counter;

    if( counter == SUPPORTED_OPTIONS || loadedOptions[counter] == NULL)
        return NULL;

    if(loadedOptions[counter]->info.optProtocol == settedProto)
        return loadedOptions[counter];
    else
        return getNextOpts();
}
optionImplement * optionLoader::getSingleOption(uint8_t sjOptIndex)
{
    return (loadedOptions[sjOptIndex]);
}

/* overloading of the constructor */
optionLoader::optionLoader(void)
{
    if(!isFileLoaded)
        RUNTIME_EXCEPTION("Request IP/TCP option loaded before file initialization");
}

corruption_t optionLoader::lineParser(FILE *flow, uint8_t optLooked)
{
    corruption_t retval = UNASSIGNED_VALUE;
    char line[MEDIUMBUF];
    int32_t linecnt = 0;

    do
    {
        int32_t readedIndex, readedCorruption;

        fgets(line, MEDIUMBUF, flow);
        linecnt++;

        if(feof(flow))
            break;

        if( strlen(line) < 2 || line[0] == '#' )
            continue;

        sscanf(line, "%d,%d", &readedIndex, &readedCorruption);

        if(readedIndex < 1 || readedIndex > (SUPPORTED_OPTIONS -1) )
            RUNTIME_EXCEPTION("In option file invalid index at line %d", linecnt);

        if(readedIndex == optLooked)
            retval = (corruption_t)readedCorruption;
        else
            RUNTIME_EXCEPTION("Found index %d instead of the expected %d (line %d)", 
                              readedIndex, optLooked, linecnt);

    } while(retval == UNASSIGNED_VALUE);

    if( retval == UNASSIGNED_VALUE )
        RUNTIME_EXCEPTION("Unable to found option index %d in the option config file", optLooked);

    LOG_VERBOSE("option index %d found value corruption value of %d", optLooked, (uint8_t)retval);

    return retval;
}

optionLoader::optionLoader(const char *fname)
{
    uint8_t sjI; // SniffJoke Internal Option Indexing value */
    
    memset(loadedOptions, 0, sizeof (optionImplement*)*(SUPPORTED_OPTIONS));

    /* testing modality - all options are loaded without a corruption defined */
    if(fname == NULL && isFileLoaded)
    {
        sjI = SJ_IPOPT_NOOP;
        loadedOptions[sjI] = new Io_NOOP(true, sjI, "IP NOOP", IPPROTO_IP, IPOPT_NOOP, UNASSIGNED_VALUE);

        sjI = SJ_IPOPT_TIMESTAMP;
        loadedOptions[sjI] = new Io_TIMESTAMP(true, sjI, "IP Timestamp", IPPROTO_IP, IPOPT_TIMESTAMP, UNASSIGNED_VALUE);

        sjI = SJ_IPOPT_TIMESTOVERFLOW;
        loadedOptions[sjI] = new Io_TIMESTOVERFLOW(false, sjI, "IP time overflow", IPPROTO_IP, DUMMY_OPCODE, UNASSIGNED_VALUE);

        sjI = SJ_IPOPT_LSRR;
        loadedOptions[sjI] = new Io_LSRR(true, sjI, "Loose source routing", IPPROTO_IP, IPOPT_LSRR, UNASSIGNED_VALUE);

        sjI = SJ_IPOPT_RR;
        loadedOptions[sjI] = new Io_RR(true, sjI, "Record route", IPPROTO_IP, IPOPT_RR, UNASSIGNED_VALUE);

        sjI = SJ_IPOPT_RA;
        loadedOptions[sjI] = new Io_RA(true, sjI, "Router advertising", IPPROTO_IP, IPOPT_RA, UNASSIGNED_VALUE);

        sjI = SJ_IPOPT_CIPSO;
        loadedOptions[sjI] = new Io_CIPSO(true, sjI, "Cipso", IPPROTO_IP, IPOPT_CIPSO, UNASSIGNED_VALUE);

        sjI = SJ_IPOPT_SEC;
        loadedOptions[sjI] = new Io_SEC(true, sjI, "Security", IPPROTO_IP, IPOPT_SEC, UNASSIGNED_VALUE);

        sjI = SJ_IPOPT_SID;
        loadedOptions[sjI] = new Io_SID(true, sjI, "Session ID", IPPROTO_IP, IPOPT_SID, UNASSIGNED_VALUE);

        sjI = SJ_TCPOPT_NOP;
        loadedOptions[sjI] = new To_NOP(true, sjI, "TCP NOOP", IPPROTO_TCP, TCPOPT_NOP, UNASSIGNED_VALUE);

        sjI = SJ_TCPOPT_MD5SIG;
        loadedOptions[sjI] = new To_MD5SIG(false, sjI, "TCP MD5SIG", IPPROTO_TCP, TCPOPT_MD5SIG, UNASSIGNED_VALUE);

        sjI = SJ_TCPOPT_PAWSCORRUPT;
        loadedOptions[sjI] = new To_PAWSCORRUPT(false, sjI, "TCP bad PAWS", IPPROTO_TCP, DUMMY_OPCODE, UNASSIGNED_VALUE);
#if 0
        loadedOptions[SJ_TCPOPT_TIMESTAMP] = 
        loadedOptions[SJ_TCPOPT_MSS] = 
        loadedOptions[SJ_TCPOPT_SACK] = 
#endif
        return;
    }

    if(isFileLoaded)
        RUNTIME_EXCEPTION("Request IP/TCP option loaded before file initialization");

    /* loadinf the configuration file, containings which option bring corruption for your ISP */
    /* NOW - sets with the default used by vecna & evilaliv3 */
    /* THESE DATA HAS TO BE LOADED FROM A Location-SPECIFIC CONFIGUATION FILE */

    corruption_t writUsage;
    FILE *optInput = fopen(fname, "r");

    if(optInput == NULL)
        RUNTIME_EXCEPTION("Unable to open in reading %s: %s", fname, strerror(errno));

    sjI = SJ_IPOPT_NOOP;
    writUsage = lineParser(optInput, sjI);
    loadedOptions[sjI] = new Io_NOOP(true, sjI, "IP NOOP", IPPROTO_IP, IPOPT_NOOP, NOT_CORRUPT);

    sjI = SJ_IPOPT_TIMESTAMP;
    writUsage = lineParser(optInput, sjI);
    loadedOptions[sjI] = new Io_TIMESTAMP(true, sjI, "IP Timestamp", IPPROTO_IP, IPOPT_TIMESTAMP, TWOSHOT);

    sjI = SJ_IPOPT_TIMESTOVERFLOW;
    writUsage = lineParser(optInput, sjI);
    loadedOptions[sjI] = new Io_TIMESTOVERFLOW(false, sjI, "IP time overflow", IPPROTO_IP, DUMMY_OPCODE, ONESHOT);

    sjI = SJ_IPOPT_LSRR;
    writUsage = lineParser(optInput, sjI);
    loadedOptions[sjI] = new Io_LSRR(true, sjI, "Loose source routing", IPPROTO_IP, IPOPT_LSRR, ONESHOT);

    sjI = SJ_IPOPT_RR;
    writUsage = lineParser(optInput, sjI);
    loadedOptions[sjI] = new Io_RR(true, sjI, "Record route", IPPROTO_IP, IPOPT_RR, ONESHOT);

    sjI = SJ_IPOPT_RA;
    writUsage = lineParser(optInput, sjI);
    loadedOptions[sjI] = new Io_RA(true, sjI, "Router advertising", IPPROTO_IP, IPOPT_RA, NOT_CORRUPT);

    sjI = SJ_IPOPT_CIPSO;
    writUsage = lineParser(optInput, sjI);
    loadedOptions[sjI] = new Io_CIPSO(true, sjI, "Cipso", IPPROTO_IP, IPOPT_CIPSO, ONESHOT);

    sjI = SJ_IPOPT_SEC;
    writUsage = lineParser(optInput, sjI);
    loadedOptions[sjI] = new Io_SEC(true, sjI, "Security", IPPROTO_IP, IPOPT_SEC, ONESHOT);

    sjI = SJ_IPOPT_SID;
    writUsage = lineParser(optInput, sjI);
    loadedOptions[sjI] = new Io_SID(true, sjI, "Session ID", IPPROTO_IP, IPOPT_SID, TWOSHOT);

    sjI = SJ_TCPOPT_NOP;
    writUsage = lineParser(optInput, sjI);
    loadedOptions[sjI] = new To_NOP(true, sjI, "TCP NOOP", IPPROTO_TCP, TCPOPT_NOP, NOT_CORRUPT);

    sjI = SJ_TCPOPT_MD5SIG;
    writUsage = lineParser(optInput, sjI);
    loadedOptions[sjI] = new To_MD5SIG(false, sjI, "TCP MD5SIG", IPPROTO_TCP, TCPOPT_MD5SIG, ONESHOT);

    sjI = SJ_TCPOPT_PAWSCORRUPT;
    writUsage = lineParser(optInput, sjI);
    loadedOptions[sjI] = new To_PAWSCORRUPT(false, sjI, "TCP bad PAWS", IPPROTO_TCP, DUMMY_OPCODE, ONESHOT);

    fclose(optInput);
    isFileLoaded = true;

    LOG_DEBUG("option loaded correctly from %s, %d values", fname, sjI + 1);
}
