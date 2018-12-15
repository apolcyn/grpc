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
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define SOCKET_CACHE_SIZE 5

typedef struct socket_cache_entry {
  int s;
  pthread_mutex_t mu;
} socket_cache_entry;

static socket_cache_entry g_ipv4_socket_cache[SOCKET_CACHE_SIZE];
static socket_cache_entry g_ipv6_socket_cache[SOCKET_CACHE_SIZE];

static socket_cache_entry* get_socket_cache_entry(int address_family) {
  size_t hash = (size_t)&address_family;
  hash = (hash >> 5) % SOCKET_CACHE_SIZE;
  switch (address_family) {
    case AF_INET:
      return &g_ipv4_socket_cache[hash];
      break;
    case AF_INET6:
      return &g_ipv6_socket_cache[hash];
      break;
    default:
      return NULL;
  }
}

static bool posix_source_addr_factory_get_source_addr(
    address_sorting_source_addr_factory* factory,
    const address_sorting_address* dest_addr,
    address_sorting_address* source_addr) {
  socket_cache_entry *cache_entry =
      get_socket_cache_entry(((struct sockaddr*)dest_addr)->sa_family);
  if (cache_entry == NULL) {
    return false;
  }
  bool source_addr_exists = false;
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
    close(g_ipv4_socket_cache[i].s);
    pthread_mutex_destroy(&g_ipv4_socket_cache[i].mu);
  }
  for (int i = 0; i < SOCKET_CACHE_SIZE; i++) {
    close(g_ipv6_socket_cache[i].s);
    pthread_mutex_destroy(&g_ipv6_socket_cache[i].mu);
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
  for (int i = 0; i < SOCKET_CACHE_SIZE; i++) {
    // Android sets SOCK_CLOEXEC. Don't set this here for portability.
    g_ipv4_socket_cache[i].s = socket(AF_INET, SOCK_DGRAM, 0);
    fcntl(g_ipv4_socket_cache[i].s, F_SETFL, O_NONBLOCK);
    pthread_mutex_init(&g_ipv4_socket_cache[i].mu, NULL);
  }
  for (int i = 0; i < SOCKET_CACHE_SIZE; i++) {
    g_ipv6_socket_cache[i].s = socket(AF_INET6, SOCK_DGRAM, 0);
    fcntl(g_ipv6_socket_cache[i].s, F_SETFL, O_NONBLOCK);
    pthread_mutex_init(&g_ipv6_socket_cache[i].mu, NULL);
  }
  memset(factory, 0, sizeof(address_sorting_source_addr_factory));
  factory->vtable = &posix_source_addr_factory_vtable;
  return factory;
}

#endif  // defined(ADDRESS_SORTING_POSIX)
