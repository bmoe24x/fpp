/*
 * This file is part of the Falcon Player (FPP) and is Copyright (C)
 * 2013-2022 by the Falcon Player Developers.
 *
 * The Falcon Player (FPP) is free software, and is covered under
 * multiple Open Source licenses.  Please see the included 'LICENSES'
 * file for descriptions of what files are covered by each license.
 *
 * This source file is covered under the LGPL v2.1 as described in the
 * included LICENSE.LGPL file.
 */

#include "fpp-pch.h"

#ifndef PLATFORM_OSX
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#endif
#include <net/if.h>
#include <netinet/in.h>
#include <sys/types.h>

#include "NetworkMonitor.h"

NetworkMonitor NetworkMonitor::INSTANCE;

void NetworkMonitor::Init(std::map<int, std::function<bool(int)>>& callbacks) {
#ifndef PLATFORM_OSX
    int nl_socket = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (nl_socket < 0) {
        LogWarn(VB_GENERAL, "Could not create NETLINK socket.\n");
    }

    struct sockaddr_nl addr;
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = getpid();
    addr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR;

    if (bind(nl_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LogWarn(VB_GENERAL, "Could not bind NETLINK socket.\n");
    }
    callbacks[nl_socket] = [nl_socket, this](int i) {
        int status;
        int ret = 0;
        char buf[4096];
        struct iovec iov = { buf, sizeof buf };
        struct sockaddr_nl snl;
        struct msghdr msg = { (void*)&snl, sizeof snl, &iov, 1, NULL, 0, 0 };
        struct nlmsghdr* h;
        char name[IF_NAMESIZE + 1];

        status = recvmsg(nl_socket, &msg, MSG_DONTWAIT);
        while (status > 0) {
            bool OK = true;
            for (h = (struct nlmsghdr*)buf; OK && NLMSG_OK(h, (unsigned int)status); h = NLMSG_NEXT(h, status)) {
                //Finish reading
                switch (h->nlmsg_type) {
                case NLMSG_DONE:
                    OK = false;
                    break;
                case NLMSG_ERROR:
                    //error - not sure what to do, just bail
                    OK = false;
                    break;
                case RTM_NEWLINK:
                case RTM_DELLINK: {
                    struct ifinfomsg* ifi = (ifinfomsg*)NLMSG_DATA(h);
                    std::string strName;
                    if (if_indextoname(ifi->ifi_index, name)) {
                        strName = name;
                    }
                    callCallbacks(h->nlmsg_type == RTM_NEWLINK ? NetEventType::NEW_LINK : NetEventType::DEL_LINK,
                                  (ifi->ifi_flags & IFF_RUNNING) ? 1 : 0, strName);
                } break;
                case RTM_NEWADDR:
                case RTM_DELADDR: {
                    struct ifaddrmsg* ifi = (ifaddrmsg*)NLMSG_DATA(h);
                    if (ifi->ifa_family == AF_INET) {
                        std::string strName;
                        if (if_indextoname(ifi->ifa_index, name)) {
                            strName = name;
                        }
                        callCallbacks(h->nlmsg_type == RTM_NEWADDR ? NetEventType::NEW_ADDR : NetEventType::DEL_ADDR,
                                      h->nlmsg_type == RTM_NEWADDR ? 1 : 0, strName);
                    }
                } break;
                default:
                    //printf("NETLINK: %d   Uknown\n", h->nlmsg_type);
                    break;
                }
            }
            status = recvmsg(nl_socket, &msg, MSG_DONTWAIT);
        }
        return false;
    };
#endif    
}
void NetworkMonitor::callCallbacks(NetEventType nl, int up, const std::string& n) {
    for (auto& cb : callbacks) {
        cb.second(nl, up, n);
    }
}

int NetworkMonitor::registerCallback(std::function<void(NetEventType, int, const std::string&)>& callback) {
    int id = curId++;
    callbacks[id] = callback;
    return id;
}

void NetworkMonitor::removeCallback(int id) {
    callbacks.erase(id);
}
