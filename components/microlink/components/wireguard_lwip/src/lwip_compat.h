






#ifndef LWIP_COMPAT_H
#define LWIP_COMPAT_H

#include "lwip/ip_addr.h"

// Detect lwIP version
#if LWIP_IPV4 && LWIP_IPV6
    // Dual stack - newer lwIP uses u_addr union
    #define LWIP_USES_U_ADDR 1
#else
    #define LWIP_USES_U_ADDR 0
#endif

// Compatibility macros for IP address access
#if LWIP_USES_U_ADDR
    // New lwIP (v5.3+): ip_addr_t has u_addr union
    #define IP_ADDR_GET_IP4_U32(ipaddr)     ((ipaddr)->u_addr.ip4.addr)
    #define IP_ADDR_SET_IP4_U32(ipaddr, val) do { (ipaddr)->u_addr.ip4.addr = (val); (ipaddr)->type = IPADDR_TYPE_V4; } while(0)
    #define IP_ADDR_TO_IP4(ipaddr)          (&((ipaddr)->u_addr.ip4))
    #define IP_ADDR_TO_IP4_CONST(ipaddr)    ((const ip4_addr_t*)(&((ipaddr)->u_addr.ip4)))

    static inline int ip_addr_netcmp_dual(const ip_addr_t *addr1, const ip_addr_t *addr2, const ip_addr_t *mask) {
#if LWIP_IPV6
        if (IP_IS_V6(addr1) && IP_IS_V6(addr2)) {
            if (IP_IS_V6(mask)) {
                const ip6_addr_t *a1 = ip_2_ip6(addr1);
                const ip6_addr_t *a2 = ip_2_ip6(addr2);
                const ip6_addr_t *m = ip_2_ip6(mask);
                return ((a1->addr[0] & m->addr[0]) == (a2->addr[0] & m->addr[0])) &&
                       ((a1->addr[1] & m->addr[1]) == (a2->addr[1] & m->addr[1])) &&
                       ((a1->addr[2] & m->addr[2]) == (a2->addr[2] & m->addr[2])) &&
                       ((a1->addr[3] & m->addr[3]) == (a2->addr[3] & m->addr[3]));
            }
            return ip6_addr_cmp(ip_2_ip6(addr1), ip_2_ip6(addr2));
        }
#endif
#if LWIP_IPV4
        if (IP_IS_V4(addr1) && IP_IS_V4(addr2)) {
            return ip4_addr_netcmp(ip_2_ip4(addr1), ip_2_ip4(addr2), IP_ADDR_TO_IP4_CONST(mask));
        }
#endif
        return 0;
    }
    #define IP_ADDR_NETCMP_COMPAT(addr1, addr2, mask) ip_addr_netcmp_dual(addr1, addr2, mask)
#else
    // Old lwIP: ip_addr_t has direct addr field
    #define IP_ADDR_GET_IP4_U32(ipaddr)     ((ipaddr)->addr)
    #define IP_ADDR_SET_IP4_U32(ipaddr, val) ((ipaddr)->addr = (val))
    #define IP_ADDR_TO_IP4(ipaddr)          ((ip4_addr_t*)(ipaddr))
    #define IP_ADDR_TO_IP4_CONST(ipaddr)    ((const ip4_addr_t*)(ipaddr))

    // Old lwIP doesn't need special handling
    #define IP_ADDR_NETCMP_COMPAT(addr1, addr2, mask) \
        ip_addr_netcmp(addr1, addr2, mask)
#endif

#endif /* LWIP_COMPAT_H */
