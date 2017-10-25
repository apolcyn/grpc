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
 */

#include <grpc/support/port_platform.h>
#if GRPC_ARES == 1 && !defined(GRPC_UV)

#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_wrapper.h"
#include "src/core/lib/iomgr/sockaddr.h"
#include "src/core/lib/iomgr/socket_utils_posix.h"

#include <string.h>
#include <sys/types.h>
#include <errno.h>
#include <netinet/in.h>

#include <ares.h>
#include <grpc/support/alloc.h>
#include <grpc/support/host_port.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/time.h>
#include <grpc/support/useful.h>

#include "src/core/ext/filters/client_channel/parse_address.h"
#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_ev_driver.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/iomgr/executor.h"
#include "src/core/lib/iomgr/iomgr_internal.h"
#include "src/core/lib/iomgr/nameser.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"
#include "src/core/lib/support/string.h"

grpc_tracer_flag grpc_trace_cares_address_sorting = GRPC_TRACER_INITIALIZER(false, "cares_address_sorting");

struct sortable_address {
  grpc_lb_address lb_addr;
  struct sockaddr_in6 dest_addr;
  struct sockaddr_in6 source_addr;
  size_t original_index;
  bool src_addr_exists;
};

struct rfc_6724_table_entry {
  uint8_t prefix[16];
  size_t prefix_len;
  int precedence;
  int label;
};

rfc_6724_table_entry rfc_6724_default_policy_table[9] = {
  {
    { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1},
    128,
    50,
    0,
  },
  {
    { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0},
    0,
    40,
    1,
  },
  {
    { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xff, 0xff, 0xff, 0xff, 0x0, 0x0},
    96,
    35,
    4,
  },
  {
    { 0x20, 0x02, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0},
    16,
    30,
    2,
  },
  {
    { 0x20, 0x01, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0},
    32,
    5,
    5,
  },
  {
    { 0xfc, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0},
    7,
    3,
    13,
  },
  {
    { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0},
    96,
    1,
    3,
  },
  {
    { 0xfe, 0xc0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0},
    10,
    1,
    11,
  },
  {
    { 0x3f, 0xfe, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0},
    16,
    1,
    12,
  },
};

static rfc_6724_table_entry *rfc_6724_policy_table = rfc_6724_default_policy_table;
static size_t rfc_6724_policy_table_size = 9;

static int ipv6_prefix_match_length(unsigned char *a, unsigned char *b) {
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

rfc_6724_table_entry *lookup_policy_table_match(sockaddr_in6 *s_addr) {
  rfc_6724_table_entry *best_match  = NULL;
  size_t best_match_index = -1;
  for (size_t i = 0; i < rfc_6724_policy_table_size; i++) {
    size_t prefix_match = ipv6_prefix_match_length((unsigned char*)rfc_6724_policy_table[i].prefix, (unsigned char*)&s_addr->sin6_addr.s6_addr);
    if (prefix_match >= rfc_6724_policy_table[i].prefix_len) {
      if (best_match == NULL || rfc_6724_policy_table[i].prefix_len > best_match->prefix_len) {
        best_match = &rfc_6724_policy_table[i];
        best_match_index = i;
      }
    }
  }
  GPR_ASSERT(best_match);
  if (GRPC_TRACER_ON(grpc_trace_cares_address_sorting)) {
    gpr_log(GPR_INFO, "Looked up best match in policy table. Index: %" PRIdPTR ". Label: %d. Precedence: %d", best_match_index, best_match->label, best_match->precedence);
  }
  return best_match;
}

static int get_label_value(sockaddr_in6 *s_addr) {
  rfc_6724_table_entry *entry = lookup_policy_table_match(s_addr);
  GPR_ASSERT(entry != NULL);
  gpr_log(GPR_INFO, "returning label: %d", entry->label);
  return entry->label;
}

static int get_precedence_value(sockaddr_in6 *s_addr) {
  rfc_6724_table_entry *entry = lookup_policy_table_match(s_addr);
  GPR_ASSERT(entry != NULL);
  return entry->precedence;
}

#define IPV6_ADDR_SCOPE_GLOBAL 0x0e
#define IPV6_ADDR_SCOPE_LINKLOCAL 0x02
#define IPV6_ADDR_SCOPE_SITELOCAL 0x05

static int sockaddr_get_scope(sockaddr_in6 *s_addr) {
  switch (s_addr->sin6_family) {
  case AF_INET:
    gpr_log(GPR_INFO, "ipv4 so global scope");
    return IPV6_ADDR_SCOPE_GLOBAL;
  case AF_INET6:
    if (IN6_IS_ADDR_LOOPBACK(&s_addr->sin6_addr) || IN6_IS_ADDR_LINKLOCAL(&s_addr->sin6_addr)) {
      gpr_log(GPR_INFO, "found link local scope");
      return IPV6_ADDR_SCOPE_LINKLOCAL;
    }
    if (IN6_IS_ADDR_SITELOCAL(&s_addr->sin6_addr)) {
      gpr_log(GPR_INFO, "found site local scope");
      return IPV6_ADDR_SCOPE_SITELOCAL;
    }
    gpr_log(GPR_INFO, "found global scope");
    return IPV6_ADDR_SCOPE_GLOBAL;
  default:
    gpr_log(GPR_ERROR, "Unknown socket family %d in grpc_sockaddr_get_port", s_addr->sin6_family);
    return 0;
  }
}

static int compare_src_addr_exists(sortable_address *sa, sortable_address *sb) {
  if (sa->src_addr_exists != sb->src_addr_exists) {
    gpr_log(GPR_INFO, "src addrs not equal");
    return sa->src_addr_exists ? -1 : 1;
  }
  gpr_log(GPR_INFO, "src addrs both not there or there");
  return 0;
}

static int compre_src_dst_scope_matches(sortable_address *sa, sortable_address *sb) {
  int a_src_dst_scope_matches = false;
  if (sockaddr_get_scope((sockaddr_in6*)&sa->dest_addr) == sockaddr_get_scope(&sa->source_addr)) {
    gpr_log(GPR_INFO, "a src and dst scopes match");
    a_src_dst_scope_matches = true;
  }
  int b_src_dst_scope_matches = false;
  if (sockaddr_get_scope((sockaddr_in6*)&sb->dest_addr) == sockaddr_get_scope(&sb->source_addr)) {
    gpr_log(GPR_INFO, "b src and dst scopes match");
    b_src_dst_scope_matches = true;
  }
  if (a_src_dst_scope_matches != b_src_dst_scope_matches) {
    return a_src_dst_scope_matches ? -1 : 1;
  }
  gpr_log(GPR_INFO, "matching of scopes matches");
  return 0;
}

static int compare_src_dst_labels_match(sortable_address *sa, sortable_address *sb) {
  int a_label_matches = false;
  if (get_label_value((sockaddr_in6*)&sa->dest_addr) == get_label_value(&sa->source_addr)) {
    a_label_matches = true;
  }
  int b_label_matches = false;
  if (get_label_value((sockaddr_in6*)&sb->dest_addr) == get_label_value(&sb->source_addr)) {
    b_label_matches = true;
  }
  if (a_label_matches != b_label_matches) {
    gpr_log(GPR_INFO, "labels dont match");
    return a_label_matches ? -1 : 1;
  }
  gpr_log(GPR_INFO, "labels match");
  return 0;
}

static int compare_dst_precedence(sortable_address *sa, sortable_address *sb) {
  if (get_precedence_value(&sa->dest_addr) != get_precedence_value(&sb->dest_addr)) {
    return get_precedence_value(&sb->dest_addr) - get_precedence_value(&sa->dest_addr);
  }
  gpr_log(GPR_INFO, "precedence of both destinations match");
  return 0;
}

static int compare_dst_scope(sortable_address *sa, sortable_address *sb) {
  if (sockaddr_get_scope(&sa->dest_addr) != sockaddr_get_scope(&sb->dest_addr)) {
    return sockaddr_get_scope(&sa->dest_addr) - sockaddr_get_scope(&sb->dest_addr);
  }
  return 0;
}

static int compare_src_dst_prefix_match_lengths(sortable_address *sa, sortable_address *sb) {
  if (grpc_sockaddr_get_family(&sa->lb_addr.address) == grpc_sockaddr_get_family(&sb->lb_addr.address) && grpc_sockaddr_get_family(&sa->lb_addr.address) == AF_INET6) {
    int a_match = ipv6_prefix_match_length((unsigned char*)&sa->source_addr.sin6_addr.s6_addr, (unsigned char*)&sa->dest_addr.sin6_addr.s6_addr);
    int b_match = ipv6_prefix_match_length((unsigned char*)&sb->source_addr.sin6_addr.s6_addr, (unsigned char*)&sb->dest_addr.sin6_addr.s6_addr);
    return b_match - a_match;
  }
  return 0;
}

static int rfc_6724_compare(const void *a, const void *b) {
  sortable_address* sa = (sortable_address*)a;
  sortable_address* sb = (sortable_address*)b;

  int out = 0;
  if ((out = compare_src_addr_exists(sa, sb))) {
    return out;
  } else if ((out = compre_src_dst_scope_matches(sa, sb))) {
    return out;
  } else if ((out = compare_src_dst_labels_match(sa, sb))) {
    return out;
  // TODO: avoid deprecated addresses
  // TODO: avoid temporary addresses
  } else if ((out = compare_dst_precedence(sa, sb))) {
    return out;
  // TODO: prefer native transport
  } else if ((out = compare_dst_scope(sa, sb))) {
    return out;
  } else if ((out = compare_src_dst_prefix_match_lengths(sa, sb))) {
    return out;
  }
  // Prefer that the sort be stable otherwise
  return sa->original_index - sb->original_index;
}

static void update_maybe_v4map(grpc_resolved_address *resolved_addr, sockaddr_in6 *to_update) {
  grpc_resolved_address v4_mapped;
  if (grpc_sockaddr_to_v4mapped(resolved_addr, &v4_mapped)) {
    memcpy(to_update, &v4_mapped.addr, sizeof(struct sockaddr_in6));
  } else {
    memcpy(to_update, &resolved_addr->addr, sizeof(struct sockaddr_in6));
  }
}

static void log_address_sorting_list(grpc_lb_addresses *lb_addrs, const char *input_output_str) {
  for (size_t i = 0; i < lb_addrs->num_addresses; i++) {
    char *addr_str;
    if (grpc_sockaddr_to_string(&addr_str, &lb_addrs->addresses[i].address, true)) {
      gpr_log(GPR_INFO, "C-ares sockaddr address sorting %s index: %" PRIdPTR ". Sockaddr-to-string: %s", input_output_str, i, addr_str);
    } else {
      gpr_log(GPR_INFO, "Failed to convert sockaddr c-ares address sorting %s index: %" PRIdPTR " to string.", input_output_str, i);
    }
  }
}

void grpc_ares_wrapper_rfc_6724_sort(grpc_lb_addresses *resolved_lb_addrs) {
  sortable_address* sortable = (sortable_address*)gpr_zalloc(resolved_lb_addrs->num_addresses * sizeof(sortable_address));
  if (GRPC_TRACER_ON(grpc_trace_cares_address_sorting)) {
    log_address_sorting_list(resolved_lb_addrs, "input");
  }
  for (size_t i = 0; i < resolved_lb_addrs->num_addresses; i++) {
    sortable[i].lb_addr = resolved_lb_addrs->addresses[i];
    sortable[i].src_addr_exists = false;
    sortable[i].original_index = i;
    update_maybe_v4map(&resolved_lb_addrs->addresses[i].address, &sortable[i].dest_addr);
    int address_family = grpc_sockaddr_get_family(&resolved_lb_addrs->addresses[i].address);
    // TODO: reset already-created sockets when possible, if needed
    int s = grpc_ares_wrapper_socket(address_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (address_family == AF_INET6) {
      sockaddr_in6 *s_addr = (sockaddr_in6*)&resolved_lb_addrs->addresses[i].address.addr;
      char ntop_buf[INET6_ADDRSTRLEN + 1];
      ntop_buf[INET6_ADDRSTRLEN] = 0;
      gpr_log(GPR_INFO, "ares wrapper lb addr addr string");
      inet_ntop(AF_INET6, &s_addr->sin6_addr, ntop_buf, sizeof(ntop_buf));
      gpr_log(GPR_INFO, "lb addr str: %s", ntop_buf);
    }
    if (s != -1) {
      struct sockaddr *dest = (struct sockaddr*)resolved_lb_addrs->addresses[i].address.addr;
      if (grpc_ares_wrapper_connect(s, dest, resolved_lb_addrs->addresses[i].address.len) != -1) {
        grpc_resolved_address src_addr;
        if (grpc_ares_wrapper_getsockname(s, (struct sockaddr*)&src_addr.addr, (socklen_t*)&src_addr.len) != -1) {
          sortable[i].src_addr_exists = true;
          update_maybe_v4map(&src_addr, &sortable[i].source_addr);
          // Do logging
          char *dst_str;
          GPR_ASSERT(grpc_sockaddr_to_string(&dst_str, &resolved_lb_addrs->addresses[i].address, true));
          char *src_str;
          GPR_ASSERT(grpc_sockaddr_to_string(&src_str, &src_addr, true));
          gpr_log(GPR_INFO, "Resolved destination %s and found source address candidate %s", dst_str, src_str);
          gpr_free(dst_str);
          gpr_free(src_str);
        } else {
          char *addr_str;
          GPR_ASSERT(grpc_sockaddr_to_string(&addr_str, &resolved_lb_addrs->addresses[i].address, true));
          gpr_log(GPR_INFO, "Resolved destination %s but getsockname after connect failed with %d, so de-prioritizing it", addr_str, errno);
          gpr_free(addr_str);
        }
      } else {
        char *addr_str;
        GPR_ASSERT(grpc_sockaddr_to_string(&addr_str, &resolved_lb_addrs->addresses[i].address, true));
        gpr_log(GPR_INFO, "Resolved destination %s but connect failed with %d, so de-prioritizing it", addr_str, errno);
        gpr_free(addr_str);
      }
      grpc_ares_wrapper_close(s);
    }
  }
  qsort(sortable, resolved_lb_addrs->num_addresses, sizeof(sortable_address), rfc_6724_compare);
  grpc_lb_address *sorted_lb_addrs = (grpc_lb_address*)gpr_zalloc(resolved_lb_addrs->num_addresses * sizeof(grpc_lb_address));
  for (size_t i = 0; i < resolved_lb_addrs->num_addresses; i++) {
    sorted_lb_addrs[i] = sortable[i].lb_addr;
    char *src_str;
    GPR_ASSERT(grpc_sockaddr_to_string(&src_str, &sorted_lb_addrs[i].address, false));
    gpr_log(GPR_INFO, "Adding sorted address: %s", src_str);
    gpr_free(src_str);
  }
  gpr_free(sortable);
  gpr_free(resolved_lb_addrs->addresses);
  resolved_lb_addrs->addresses = sorted_lb_addrs;
  if (GRPC_TRACER_ON(grpc_trace_cares_address_sorting)) {
    log_address_sorting_list(resolved_lb_addrs, "output");
  }
}

#endif /* GRPC_ARES == 1 && !defined(GRPC_UV) */
