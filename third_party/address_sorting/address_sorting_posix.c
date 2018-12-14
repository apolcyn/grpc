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
 * (in Android's getaddrinfo.c). It has some cosmetic differences
 * from Android's getaddrinfo.c, but Android's getaddrinfo.c was
 * used as a guide or example of a way to implement the RFC 6724 spec when
 * this was written.
 */

#include "address_sorting_internal.h"

#if defined(ADDRESS_SORTING_POSIX)

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>

#define SOCKET_CACHE_SIZE 2

pthread_mutex_t g_socket_cache_mu;

typedef struct socket_cache_entry{
  int s;
  pthread_mutex_t mu;
} socket_cache_entry;

socket_cache_entry g_socket_cache[SOCKET_CACHE_SIZE];

static int get_socket_cache_index(int address_family) {
  switch(address_family) {
    case AF_INET:
      return 0;
      break;
    case AF_INET6:
      return 1;
      break;
    default:
      return -1;
  }
}

static bool posix_source_addr_factory_get_source_addr(
    address_sorting_source_addr_factory* factory,
    const address_sorting_address* dest_addr,
    address_sorting_address* source_addr) {
  int cache_index = get_socket_cache_index(((struct sockaddr*)dest_addr)->sa_family);
  if (cache_index < 0) {
    return false;
  }
  bool source_addr_exists = false;
  socket_cache_entry *cache_entry = &g_socket_cache[cache_index];
  pthread_mutex_lock(&cache_entry->mu);
  int s = cache_entry->s;
  if (s != -1) {
    if (connect(s, (const struct sockaddr*)&dest_addr->addr,
                (socklen_t)dest_addr->len) != -1) {
      address_sorting_address found_source_addr;
      memset(&found_source_addr, 0, sizeof(found_source_addr));
      found_source_addr.len = sizeof(found_source_addr.addr);
      if (getsockname(s, (struct sockaddr*)&found_source_addr.addr,
                      (socklen_t*)&found_source_addr.len) != -1) {
        source_addr_exists = true;
        *source_addr = found_source_addr;
      }
    }
  }
  pthread_mutex_unlock(&cache_entry->mu);
  return source_addr_exists;
}

static void posix_source_addr_factory_destroy(
    address_sorting_source_addr_factory* self) {
  for (int i = 0; i < SOCKET_CACHE_SIZE; i++) {
    close(g_socket_cache[i].s);
    pthread_mutex_destroy(&g_socket_cache[i].mu);
  }
  free(self);
}

static const address_sorting_source_addr_factory_vtable
    posix_source_addr_factory_vtable = {
        posix_source_addr_factory_get_source_addr,
        posix_source_addr_factory_destroy,
};

address_sorting_source_addr_factory*
address_sorting_create_source_addr_factory_for_current_platform() {
  address_sorting_source_addr_factory* factory =
      malloc(sizeof(address_sorting_source_addr_factory));
  // Android sets SOCK_CLOEXEC. Don't set this here for portability.
  int ipv4_index = get_socket_cache_index(AF_INET);
  g_socket_cache[ipv4_index].s = socket(AF_INET, SOCK_DGRAM, 0);
  fcntl(g_socket_cache[ipv4_index].s, F_SETFL, O_NONBLOCK);
  pthread_mutex_init(&g_socket_cache[ipv4_index].mu, NULL);
  int ipv6_index = get_socket_cache_index(AF_INET6);
  g_socket_cache[ipv6_index].s = socket(AF_INET6, SOCK_DGRAM, 0);
  fcntl(g_socket_cache[ipv6_index].s, F_SETFL, O_NONBLOCK);
  pthread_mutex_init(&g_socket_cache[ipv6_index].mu, NULL);
  memset(factory, 0, sizeof(address_sorting_source_addr_factory));
  factory->vtable = &posix_source_addr_factory_vtable;
  return factory;
}

#endif  // defined(ADDRESS_SORTING_POSIX)
