#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <linux/ip.h>
#include <linux/if.h>
#include <netlink/netlink.h>
#include <netlink/cache.h>
#include <netlink/msg.h>
#include <netlink/route/link.h>
#include <netlink/route/addr.h>
#include <netlink/route/link/ip6tnl.h>

const int ERROR = -1;
const char LOCAL_IPv6[] = "fd84:c300:ca02:76d2::1";
const char REMOTE_IPv6[] = "fd84:c300:ca02:76d2::2";
const char TUN_NAME[] = "ip6tun0";

/*
* Finds master device ifidx.
*/
int find_master_ifidx(struct nl_sock* socket) {
    struct nl_cache* link_cache = NULL;
    if(rtnl_link_alloc_cache(socket, AF_UNSPEC, &link_cache) < 0) {
        perror("Could not allocate link cache!");
        return ERROR;
    }

    int eth0 = 0;
    if (!(eth0 = rtnl_link_name2i(link_cache, "enp0s3"))) {
        perror("Could not find master device!");
        nl_cache_put(link_cache);
        return ERROR;
    }

    nl_cache_put(link_cache);
    return eth0;
}

int receive_i6addrs(struct nl_msg *msg, void *arg) {
    struct ifaddrmsg* ifaddr = NLMSG_DATA(nlmsg_hdr(msg));

    if(ifaddr->ifa_index != 2) return 0;

    struct rtattr* retrta = IFA_RTA(ifaddr);

    int attlen = IFA_PAYLOAD(nlmsg_hdr(msg));
    char buf[INET6_ADDRSTRLEN];
    
    while RTA_OK(retrta, attlen) {
        if (retrta->rta_type == IFA_ADDRESS) {
            memcpy(arg, RTA_DATA(retrta), sizeof(struct in6_addr));
        }
        retrta = RTA_NEXT(retrta, attlen);
    }

    return 0;
}

/*
* Finds the master INET6 address. Returned structure must be freed after use.
*/
struct in6_addr* find_master_addr(struct nl_sock* socket) {
    struct nl_msg* msg = nlmsg_alloc();

    struct nlmsghdr* hdr = nlmsg_put(
        msg,
        NL_AUTO_PID,
        NL_AUTO_SEQ,
        RTM_GETADDR,
        sizeof(struct ifaddrmsg),
        NLM_F_REQUEST | NLM_F_ROOT
    );

    struct ifaddrmsg addrmsg;
    memset(&addrmsg, 0, sizeof(struct ifaddrmsg));
    addrmsg.ifa_family = AF_INET6;

    memcpy(nlmsg_data(hdr), &addrmsg, sizeof(struct ifaddrmsg));

    struct in6_addr* addr = malloc(sizeof(struct in6_addr));
    memset(addr, 0, sizeof(struct in6_addr));
    nl_send_auto_complete(socket, msg);
    nlmsg_free(msg);

    nl_socket_modify_cb(socket, NL_CB_VALID, NL_CB_CUSTOM, &receive_i6addrs, addr);
    nl_recvmsgs_default(socket);

    return addr;
}

int create_tunnel(struct nl_sock* socket, struct in6_addr* local) {
    struct rtnl_link* tunnel = rtnl_link_ip6_tnl_alloc();

    int eth0 = find_master_ifidx(socket);
    if(eth0 < 0) {
        perror("Error finding master device!");
        rtnl_link_put(tunnel);
        return ERROR;
    }

    rtnl_link_set_name(tunnel, TUN_NAME);
    rtnl_link_set_link(tunnel, eth0); //Set default link as the master device
    rtnl_link_set_flags(tunnel, IFF_UP);
    rtnl_link_ip6_tnl_set_proto(tunnel, IPPROTO_IPV6);

    //Build required addresses
    struct in6_addr* rem = malloc(sizeof(struct in6_addr));
    inet_pton(AF_INET6, REMOTE_IPv6, rem);

    rtnl_link_ip6_tnl_set_local(tunnel, local);
    rtnl_link_ip6_tnl_set_remote(tunnel, rem);

    if(rtnl_link_add(socket, tunnel, NLM_F_CREATE) < 0) {
        perror("Could not create link!");
        rtnl_link_put(tunnel);
        free(rem);
        return ERROR;
    }

    rtnl_link_put(tunnel);
    free(rem);

    printf("Tunnel successfully created!\n");
    return 0;
}

int main() {
    struct nl_sock* socket = nl_socket_alloc();
    nl_connect(socket, NETLINK_ROUTE);

    struct in6_addr* local = find_master_addr(socket);

    create_tunnel(socket, local);

    free(local);
    nl_close(socket);
    nl_socket_free(socket);

    return 0;
}
