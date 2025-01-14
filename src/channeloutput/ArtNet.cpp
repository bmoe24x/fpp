/*
 * This file is part of the Falcon Player (FPP) and is Copyright (C)
 * 2013-2022 by the Falcon Player Developers.
 *
 * The Falcon Player (FPP) is free software, and is covered under
 * multiple Open Source licenses.  Please see the included 'LICENSES'
 * file for descriptions of what files are covered by each license.
 *
 * This source file is covered under the GPL v2 as described in the
 * included LICENSE.GPL file.
 */

#include "fpp-pch.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "ArtNet.h"
#include "ChannelOutputSetup.h"
#include "FPD.h"
#include "channeloutputthread.h"
#include "e131bridge.h"
#include "fpp.h"

#define MAX_ARTNET_UNIVERSE_COUNT 512
#define ARTNET_HEADER_LENGTH 18
#define ARTNET_SYNC_PACKET_LENGTH 14

#define ARTNET_DEST_PORT 6454

#define ARTNET_SEQUENCE_INDEX 12
#define ARTNET_UNIVERSE_INDEX 14
#define ARTNET_LENGTH_INDEX 16

#define ARTNET_TYPE_BROADCAST 2
#define ARTNET_TYPE_UNICAST 3

const char ArtNetHeader[] = {
    'A', 'r', 't', '-', 'N', 'e', 't', 0x00, // 8-byte ID
    0x00,                                    // Opcode Low
    0x50,                                    // Opcode High
    0x00,                                    // Protocol Version High
    0x0E,                                    // Protocol Version Low
    0x00,                                    // Sequence #
    0x00,                                    // Physical #
    0x00,                                    // Universe Number Low
    0x00,                                    // Universe Number High (7 bits)
    0x00,                                    // Length High (length always an even number)
    0x00                                     // Length Low
};

const char ArtNetSyncPacket[] = {
    'A', 'r', 't', '-', 'N', 'e', 't', 0x00, // 8-byte ID
    0x00,                                    // Opcode Low
    0x52,                                    // Opcode High
    0x00,                                    // Protocol Version High
    0x0E,                                    // Protocol Version Low
    0x00,                                    // Aux1
    0x00                                     // Aux2
};

static struct iovec ArtNetSyncIovecs = { (void*)ArtNetSyncPacket, ARTNET_SYNC_PACKET_LENGTH };
static struct sockaddr_in ArtNetSyncAddress;

static const std::string ARTNETTYPE = "ArtNet";

const std::string& ArtNetOutputData::GetOutputTypeString() const {
    return ARTNETTYPE;
}

ArtNetOutputData::ArtNetOutputData(const Json::Value& config) :
    UDPOutputData(config),
    sequenceNumber(1),
    universeCount(1) {
    memset((char*)&anAddress, 0, sizeof(sockaddr_in));
    anAddress.sin_family = AF_INET;
    anAddress.sin_port = htons(ARTNET_DEST_PORT);

    memset((char*)&ArtNetSyncAddress, 0, sizeof(sockaddr_in));
    ArtNetSyncAddress.sin_family = AF_INET;
    ArtNetSyncAddress.sin_port = htons(ARTNET_DEST_PORT);
    ArtNetSyncAddress.sin_addr.s_addr = inet_addr("255.255.255.255");

    universe = config["id"].asInt();
    priority = config["priority"].asInt();
    if (config.isMember("universeCount")) {
        universeCount = config["universeCount"].asInt();
    }
    if (universeCount < 1) {
        universeCount = 1;
    }

    switch (type) {
    case ARTNET_TYPE_BROADCAST: // Multicast
        ipAddress = "";
        break;
    case 3: //UnicastAddress
        ipAddress = config["address"].asString();
        break;
    }

    if (type == ARTNET_TYPE_BROADCAST) {
        anAddress.sin_addr.s_addr = inet_addr("255.255.255.255");
    } else {
        anAddress.sin_addr.s_addr = toInetAddr(ipAddress, valid);
        if (!valid && active) {
            WarningHolder::AddWarning("Could not resolve host name " + ipAddress + " - disabling output");
            active = false;
        }
    }

    anIovecs.resize(universeCount * 2);
    anHeaders.resize(universeCount);
    for (int x = 0; x < universeCount; x++) {
        unsigned char* anBuffer = (unsigned char*)malloc(ARTNET_HEADER_LENGTH);
        anHeaders[x] = anBuffer;

        memcpy(anBuffer, ArtNetHeader, ARTNET_HEADER_LENGTH);

        int uni = universe + x;

        anBuffer[ARTNET_UNIVERSE_INDEX] = (char)(uni % 256);
        anBuffer[ARTNET_UNIVERSE_INDEX + 1] = (char)(uni / 256);

        anBuffer[ARTNET_LENGTH_INDEX] = (char)(channelCount / 256);
        anBuffer[ARTNET_LENGTH_INDEX + 1] = (char)(channelCount % 256);

        // use scatter/gather for the packet.   One IOV will contain
        // the header, the second will point into the raw channel data
        // and will be set at output time.   This avoids any memcpy.
        anIovecs[x * 2].iov_base = anBuffer;
        anIovecs[x * 2].iov_len = ARTNET_HEADER_LENGTH;
        anIovecs[x * 2 + 1].iov_base = nullptr;
        anIovecs[x * 2 + 1].iov_len = channelCount;
    }
}

ArtNetOutputData::~ArtNetOutputData() {
    for (auto a : anHeaders) {
        free(a);
    }
}

bool ArtNetOutputData::IsPingable() {
    return type == ARTNET_TYPE_UNICAST;
}

void ArtNetOutputData::PrepareData(unsigned char* channelData, UDPOutputMessages& messages) {
    if (valid && active) {
        //ALL ArtNet messages must go out on the same socket
        //and the socket MUST have the source port of ARTNET_DEST_PORT
        //as per the ArtNet protocol
        if (messages.GetSocket(ARTNET_DEST_PORT) == -1) {
            // we MAY be bridging ArtNet so we need to use that same socket
            messages.ForceSocket(ARTNET_DEST_PORT, CreateArtNetSocket());
        }

        unsigned char* cur = channelData + startChannel - 1;
        int start = 0;
        bool skipped = false;
        bool allSkipped = true;

        std::vector<struct mmsghdr>& msgs = messages[ARTNET_DEST_PORT];
        for (int x = 0; x < universeCount; x++) {
            if (NeedToOutputFrame(channelData, startChannel - 1, start, channelCount)) {
                struct mmsghdr msg;
                memset(&msg, 0, sizeof(msg));
                msg.msg_hdr.msg_name = &anAddress;
                msg.msg_hdr.msg_namelen = sizeof(sockaddr_in);
                msg.msg_hdr.msg_iov = &anIovecs[x * 2];
                msg.msg_hdr.msg_iovlen = 2;
                msg.msg_len = channelCount + ARTNET_HEADER_LENGTH;
                msgs.push_back(msg);

                anHeaders[x][ARTNET_SEQUENCE_INDEX] = sequenceNumber;
                anIovecs[x * 2 + 1].iov_base = (void*)cur;
                allSkipped = false;
            } else {
                skipped = true;
            }
            cur += channelCount;
            start += channelCount;
        }
        sequenceNumber++;
        if (sequenceNumber == 0) {
            sequenceNumber++;
        }
        if (skipped) {
            skippedFrames++;
        }
        if (!allSkipped) {
            SaveFrame(&channelData[startChannel - 1], start);
        }
    }
}
void ArtNetOutputData::PostPrepareData(unsigned char* channelData, UDPOutputMessages& msgs) {
    if (valid && active) {
        for (auto msg : msgs[ARTNET_DEST_PORT]) {
            if (msg.msg_hdr.msg_iov == &ArtNetSyncIovecs) {
                //already added, skip
                return;
            }
        }

        struct mmsghdr msg;
        memset(&msg, 0, sizeof(msg));

        msg.msg_hdr.msg_name = &ArtNetSyncAddress;
        msg.msg_hdr.msg_namelen = sizeof(sockaddr_in);
        msg.msg_hdr.msg_iov = &ArtNetSyncIovecs;
        msg.msg_hdr.msg_iovlen = 1;
        msg.msg_len = ARTNET_SYNC_PACKET_LENGTH;
        msgs[ARTNET_DEST_PORT].push_back(msg);
    }
}

void ArtNetOutputData::GetRequiredChannelRange(int& min, int& max) {
    min = startChannel - 1;
    max = startChannel + (channelCount * universeCount) - 1;
}

void ArtNetOutputData::DumpConfig() {
    LogDebug(VB_CHANNELOUT, "ArtNet Universe: %s   %d:%d:%d:%d:%d:%d  %s\n",
             description.c_str(),
             active,
             universe,
             universeCount,
             startChannel,
             channelCount,
             type,
             ipAddress.c_str());
}
