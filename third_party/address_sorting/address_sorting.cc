/*	$NetBSD: getaddrinfo.c,v 1.82 2006/03/25 12:09:40 rpaulo Exp $	*/
/*	$KAME: getaddrinfo.c,v 1.29 2000/08/31 17:26:57 itojun Exp $	*/
/*
 * Copyright (C) 1995, 1996, 1997, and 1998 WIDE Project.
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
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

/*
 * This is an adaptation of Android's implementation of RFC 6724
 * (in Android's getaddrinfo.c). It is written in C++ and has other cosmetic
 * differences from Android's getaddrinfo.c, but Android's getaddrinfo.c was
 * used as a guide or example of a way to implement the RFC 6724 spec when
 * this was written.
 */

#include "address_sorting.h"
#include <errno.h>
#include <grpc/support/alloc.h>
#include <grpc/support/port_platform.h>
#include <grpc/support/useful.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/types.h>
#include <algorithm>
#include <climits>
#include <vector>
#include "src/core/ext/filters/client_channel/lb_policy_factory.h"
#include "src/core/lib/iomgr/sockaddr.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"
#include "third_party/address_sorting/address_sorting.h"

using std::vector;
using std::sort;

namespace address_sorting {

// Scope values increase with increase in scope.
const int kIpv6AddrScopeLinkLocal = 1;
const int kIpv6AddrScopeSiteLocal = 2;
const int kIpv6AddrScopeGlobal = 3;

class DefaultSocketFactory : public SocketFactory {
 public:
  DefaultSocketFactory() : SocketFactory() {}
  ~DefaultSocketFactory() {}
  int Socket(int domain, int type, int protocol) {
    return socket(domain, type, protocol);
  }

  int Connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    return connect(sockfd, addr, addrlen);
  }

  int GetSockName(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    return getsockname(sockfd, addr, addrlen);
  }

  int Close(int sockfd) { return close(sockfd); }
};

SocketFactory *g_current_socket_factory = nullptr;

int Socket(int domain, int type, int protocol) {
  return g_current_socket_factory->Socket(domain, type, protocol);
}

int Connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
  return g_current_socket_factory->Connect(sockfd, addr, addrlen);
}

int GetSockName(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
  return g_current_socket_factory->GetSockName(sockfd, addr, addrlen);
}

int Close(int sockfd) { return g_current_socket_factory->Close(sockfd); }

static int ipv6_prefix_match_length(const sockaddr_in6 *sa,
                                    const sockaddr_in6 *sb) {
  unsigned char *a = (unsigned char *)&sa->sin6_addr;
  unsigned char *b = (unsigned char *)&sb->sin6_addr;
  int cur_bit = 0;
  while (cur_bit < 128) {
    int a_val = a[cur_bit / CHAR_BIT] & (1 << (cur_bit % CHAR_BIT));
    int b_val = b[cur_bit / CHAR_BIT] & (1 << (cur_bit % CHAR_BIT));
    if (a_val == b_val) {
      cur_bit++;
    } else {
      break;
    }
  }
  return cur_bit;
}

static int in6_is_addr_6to4(const in6_addr *s_addr) {
  uint8_t *bytes = (uint8_t *)s_addr;
  return bytes[0] == 0x20 && bytes[1] == 0x02;
}

static int in6_is_addr_ula(const in6_addr *s_addr) {
  uint8_t *bytes = (uint8_t *)s_addr;
  return (bytes[0] & 0xfe) == 0xfc;
}

static int in6_is_addr_toredo(const in6_addr *s_addr) {
  uint8_t *bytes = (uint8_t *)s_addr;
  return bytes[0] == 0x20 && bytes[1] == 0x02 && bytes[2] == 0x00 &&
         bytes[3] == 0x00;
}

static int in6_is_addr_6bone(const in6_addr *s_addr) {
  uint8_t *bytes = (uint8_t *)s_addr;
  return bytes[0] == 0x3f && bytes[1] == 0xfe;
}

static int get_label_value(const grpc_resolved_address *resolved_addr) {
  if (grpc_sockaddr_get_family(resolved_addr) == AF_INET) {
    return 4;
  } else if (grpc_sockaddr_get_family(resolved_addr) != AF_INET6) {
    gpr_log(GPR_INFO, "Address is not AF_INET or AF_INET6.");
    return 1;
  }
  sockaddr_in6 *ipv6_addr = (sockaddr_in6 *)&resolved_addr->addr;
  if (IN6_IS_ADDR_LOOPBACK(&ipv6_addr->sin6_addr)) {
    return 0;
  } else if (IN6_IS_ADDR_V4MAPPED(&ipv6_addr->sin6_addr)) {
    return 4;
  } else if (in6_is_addr_6to4(&ipv6_addr->sin6_addr)) {
    return 2;
  } else if (in6_is_addr_toredo(&ipv6_addr->sin6_addr)) {
    return 5;
  } else if (in6_is_addr_ula(&ipv6_addr->sin6_addr)) {
    return 13;
  } else if (IN6_IS_ADDR_V4COMPAT(&ipv6_addr->sin6_addr)) {
    return 3;
  } else if (IN6_IS_ADDR_SITELOCAL(&ipv6_addr->sin6_addr)) {
    return 11;
  } else if (in6_is_addr_6bone(&ipv6_addr->sin6_addr)) {
    return 12;
  }
  return 1;
}

static int get_precedence_value(const grpc_resolved_address *resolved_addr) {
  if (grpc_sockaddr_get_family(resolved_addr) == AF_INET) {
    return 35;
  } else if (grpc_sockaddr_get_family(resolved_addr) != AF_INET6) {
    gpr_log(GPR_INFO, "Address is not AF_INET or AF_INET6.");
    return 1;
  }
  sockaddr_in6 *ipv6_addr = (sockaddr_in6 *)&resolved_addr->addr;
  if (IN6_IS_ADDR_LOOPBACK(&ipv6_addr->sin6_addr)) {
    return 50;
  } else if (IN6_IS_ADDR_V4MAPPED(&ipv6_addr->sin6_addr)) {
    return 35;
  } else if (in6_is_addr_6to4(&ipv6_addr->sin6_addr)) {
    return 30;
  } else if (in6_is_addr_toredo(&ipv6_addr->sin6_addr)) {
    return 5;
  } else if (in6_is_addr_ula(&ipv6_addr->sin6_addr)) {
    return 3;
  } else if (IN6_IS_ADDR_V4COMPAT(&ipv6_addr->sin6_addr) ||
             IN6_IS_ADDR_SITELOCAL(&ipv6_addr->sin6_addr) ||
             in6_is_addr_6bone(&ipv6_addr->sin6_addr)) {
    return 1;
  }
  return 40;
}

static int sockaddr_get_scope(const grpc_resolved_address *resolved_addr) {
  if (grpc_sockaddr_get_family(resolved_addr) == AF_INET) {
    return kIpv6AddrScopeGlobal;
  } else if (grpc_sockaddr_get_family(resolved_addr) == AF_INET6) {
    sockaddr_in6 *ipv6_addr = (sockaddr_in6 *)&resolved_addr->addr;
    if (IN6_IS_ADDR_LOOPBACK(&ipv6_addr->sin6_addr) ||
        IN6_IS_ADDR_LINKLOCAL(&ipv6_addr->sin6_addr)) {
      return kIpv6AddrScopeLinkLocal;
    }
    if (IN6_IS_ADDR_SITELOCAL(&ipv6_addr->sin6_addr)) {
      return kIpv6AddrScopeSiteLocal;
    }
    return kIpv6AddrScopeGlobal;
  }
  gpr_log(GPR_ERROR, "Unknown socket family %d.", grpc_sockaddr_get_family(resolved_addr));
  return 0;
}

class SortableAddress final {
 public:
  SortableAddress(grpc_lb_address lb_addr, grpc_resolved_address dest_addr,
                  grpc_resolved_address source_addr, size_t original_index,
                  bool source_addr_exists)
      : lb_addr_(lb_addr),
        dest_addr_(dest_addr),
        source_addr_(source_addr),
        original_index_(original_index),
        source_addr_exists_(source_addr_exists) {}

  bool operator<(const SortableAddress &other) const {
    int out = 0;
    if ((out = CompareSourceAddrExists(other))) {
      return out < 0;
    } else if ((out = CompareSourceDestScopeMatches(other))) {
      return out < 0;
    } else if ((out = CompareSourceDestLabelsMatch(other))) {
      return out < 0;
      // TODO: avoid deprecated addresses
      // TODO: avoid temporary addresses
    } else if ((out = CompareDestPrecedence(other))) {
      return out < 0;
      // TODO: prefer native transport
    } else if ((out = CompareDestScope(other))) {
      return out < 0;
    } else if ((out = CompareSourceDestPrefixMatchLengths(other))) {
      return out < 0;
    }
    // Prefer that the sort be stable otherwise
    return original_index_ < other.original_index_;
  }

  grpc_lb_address lb_addr() { return lb_addr_; }

 private:
  grpc_lb_address lb_addr_;
  grpc_resolved_address dest_addr_;
  grpc_resolved_address source_addr_;
  size_t original_index_;
  bool source_addr_exists_;

  int CompareSourceAddrExists(const SortableAddress &other) const {
    if (source_addr_exists_ != other.source_addr_exists_) {
      return source_addr_exists_ ? -1 : 1;
    }
    return 0;
  }

  int CompareSourceDestScopeMatches(const SortableAddress &other) const {
    int this_src_dst_scope_matches = false;
    if (sockaddr_get_scope(&dest_addr_) == sockaddr_get_scope(&source_addr_)) {
      this_src_dst_scope_matches = true;
    }
    int other_src_dst_scope_matches = false;
    if (sockaddr_get_scope(&other.dest_addr_) ==
        sockaddr_get_scope(&other.source_addr_)) {
      other_src_dst_scope_matches = true;
    }
    if (this_src_dst_scope_matches != other_src_dst_scope_matches) {
      return this_src_dst_scope_matches ? -1 : 1;
    }
    return 0;
  }

  int CompareSourceDestLabelsMatch(const SortableAddress &other) const {
    int this_label_matches = false;
    if (get_label_value(&dest_addr_) == get_label_value(&source_addr_)) {
      this_label_matches = true;
    }
    int other_label_matches = false;
    if (get_label_value(&other.dest_addr_) ==
        get_label_value(&other.source_addr_)) {
      other_label_matches = true;
    }
    if (this_label_matches != other_label_matches) {
      return this_label_matches ? -1 : 1;
    }
    return 0;
  }

  int CompareDestPrecedence(const SortableAddress &other) const {
    if (get_precedence_value(&dest_addr_) !=
        get_precedence_value(&other.dest_addr_)) {
      return get_precedence_value(&other.dest_addr_) -
             get_precedence_value(&dest_addr_);
    }
    return 0;
  }

  int CompareDestScope(const SortableAddress &other) const {
    if (sockaddr_get_scope(&dest_addr_) !=
        sockaddr_get_scope(&other.dest_addr_)) {
      return sockaddr_get_scope(&dest_addr_) -
             sockaddr_get_scope(&other.dest_addr_);
    }
    return 0;
  }

  int CompareSourceDestPrefixMatchLengths(const SortableAddress &other) const {
    if (source_addr_exists_ || other.source_addr_exists_) {
    } else {
    }
    if (source_addr_exists_ &&
        grpc_sockaddr_get_family(&source_addr_) == AF_INET6 &&
        other.source_addr_exists_ &&
        grpc_sockaddr_get_family(&other.source_addr_) == AF_INET6) {
      int this_match_length = ipv6_prefix_match_length(
          (sockaddr_in6 *)&source_addr_.addr, (sockaddr_in6 *)&dest_addr_.addr);
      int other_match_length =
          ipv6_prefix_match_length((sockaddr_in6 *)&other.source_addr_.addr,
                                   (sockaddr_in6 *)&other.dest_addr_.addr);
      return other_match_length - this_match_length;
    }
    return 0;
  }
};

void OverrideSocketFactoryForTesting(SocketFactory *factory) {
  delete g_current_socket_factory;
  g_current_socket_factory = factory;
}

}  // namespace

void address_sorting_rfc_6724_sort(grpc_lb_addresses *resolved_lb_addrs) {
  auto sortable = std::vector<address_sorting::SortableAddress>();
  for (size_t i = 0; i < resolved_lb_addrs->num_addresses; i++) {
    bool source_addr_exists = false;
    grpc_resolved_address source_addr;
    int address_family =
        grpc_sockaddr_get_family(&resolved_lb_addrs->addresses[i].address);
    int s =
        address_sorting::Socket(address_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s != -1) {
      if (address_sorting::Connect(
              s,
              (struct sockaddr *)&resolved_lb_addrs->addresses[i].address.addr,
              resolved_lb_addrs->addresses[i].address.len) != -1) {
        grpc_resolved_address found_source_addr;
        if (address_sorting::GetSockName(
                s, (struct sockaddr *)&found_source_addr.addr,
                (socklen_t *)&found_source_addr.len) != -1) {
          source_addr_exists = true;
          source_addr = found_source_addr;
        }
      }
      address_sorting::Close(s);
    }
    sortable.emplace_back(address_sorting::SortableAddress(
        resolved_lb_addrs->addresses[i],
        resolved_lb_addrs->addresses[i].address, source_addr, i,
        source_addr_exists));
  }
  GPR_ASSERT(sortable.size() == resolved_lb_addrs->num_addresses);
  std::sort(sortable.begin(), sortable.end());
  grpc_lb_address *sorted_lb_addrs = (grpc_lb_address *)gpr_zalloc(
      resolved_lb_addrs->num_addresses * sizeof(grpc_lb_address));
  for (size_t i = 0; i < resolved_lb_addrs->num_addresses; i++) {
    sorted_lb_addrs[i] = sortable[i].lb_addr();
  }
  gpr_free(resolved_lb_addrs->addresses);
  resolved_lb_addrs->addresses = sorted_lb_addrs;
}

void address_sorting_init() {
  address_sorting::g_current_socket_factory =
      new address_sorting::DefaultSocketFactory();
}

void address_sorting_shutdown() {
  GPR_ASSERT(address_sorting::g_current_socket_factory != nullptr);
  delete address_sorting::g_current_socket_factory;
}
