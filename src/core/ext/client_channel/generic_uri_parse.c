/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "src/core/ext/client_channel/generic_uri_parse.h"

#include <string.h>

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>

#define MAX_HOST_PORT_PARSERS 2

static grpc_host_port_parser *g_all_of_the_host_port_parsers[MAX_HOST_PORT_PARSERS];
static int g_number_of_host_port_parsers = 0;

void grpc_host_port_parser_ref(grpc_host_port_parser* parser) {
  parser->vtable->ref(parser);
}

void grpc_host_port_parser_unref(grpc_host_port_parser* parser) {
  parser->vtable->unref(parser);
}

int grpc_host_port_parser_join_host_port(
    grpc_host_port_parser* parser, char **joined_host_port, const char *host, const char *port) {
  if (parser == NULL) return -1;
  return parser->vtable->join_host_port(parser, joined_host_port, host, port);
}

int grpc_host_port_parser_split_host_port(
    grpc_host_port_parser* parser, const char *joined_host_port, char **host, char **port) {
  if (parser == NULL) return -1;
  return parser->vtable->split_host_port(parser, joined_host_port, host, port);
}

typedef struct grpc_host_port_parser grpc_host_port_parser;
typedef struct grpc_host_port_parser_vtable grpc_host_port_parser_vtable;

void default_host_port_parser_ref(grpc_host_port_parser *parser) {
  gpr_log(GPR_INFO, "hello from default ref");
}

void default_host_port_parser_unref(grpc_host_port_parser *parser) {
  gpr_log(GPR_INFO, "hello from default unref");
}

int default_host_port_parser_join_host_port(grpc_host_port_parser *parser, char **joined_host_port, const char *host, const char *port) {
  gpr_log(GPR_INFO, "hello from default join");
  return 0;
}

int default_host_port_parser_split_host_port(grpc_host_port_parser *parser, const char *joined_host_port, char **host, char **port) {
  gpr_log(GPR_INFO, "hello from default split");
  return 0;
}

static const grpc_host_port_parser_vtable default_host_port_parser_vtable = {
  default_host_port_parser_ref,
  default_host_port_parser_unref,
  default_host_port_parser_join_host_port,
  default_host_port_parser_split_host_port,
  "",
};

void grpc_default_host_port_parser_init(void) {
  gpr_log(GPR_INFO, "hello from grpc_default host port parser init");
}

void grpc_default_host_port_parser_shutdown(void) {
  gpr_log(GPR_INFO, "hello from grpc_default host port parser init");
}

void grpc_register_host_port_parser(grpc_host_port_parser *parser) {
  int i;
  for (i = 0; i < g_number_of_host_port_parsers; i++) {
    GPR_ASSERT(0 != strcmp(parser->vtable->scheme,
                           g_all_of_the_host_port_parsers[i]->vtable->scheme));
  }
  GPR_ASSERT(g_number_of_host_port_parsers != MAX_HOST_PORT_PARSERS);
  grpc_host_port_parser_ref(parser);
  g_all_of_the_host_port_parsers[g_number_of_host_port_parsers++] = parser;
}

static grpc_host_port_parser *lookup_host_port_parser(const char *name) {
  int i;

  for (i = 0; i < g_number_of_host_port_parsers; i++) {
    if (0 == strcmp(name, g_all_of_the_host_port_parsers[i]->vtable->scheme)) {
      return g_all_of_the_host_port_parsers[i];
    }
  }

  return NULL;
}

grpc_host_port_parser *grpc_host_port_parser_lookup(const char *name) {
  grpc_host_port_parser *p = lookup_host_port_parser(name);
  if (p) grpc_host_port_parser_ref(p);
  return p;
}

int grpc_generic_join_host_port(char **joined_host_port, const char *host, const char *port) {
  gpr_log(GPR_INFO, "hello from grpc_generic_join_host_port");
  return 0;
//  grpc_generic_host_port_parser *parser;
//  grpc_uri *parsed_uri;
//  char *out = NULL;
//  char *canonical_target = NULL;
//  int i;
//  int match = 0;
//
//  factory = resolve_factory(host, &parsed_uri, &canonical_target);
//  if (factory == NULL) {
//    gpr_log(GPR_INFO, "no parser for host type");
//    return -1;
//  }
//  grpc_uri_destroy(parsed_uri);
//  for(i = 0; i < g_number_of_host_port_parsers; i++) {
//    if(!strcmp_all_of_the_host_port_parsers[i]->scheme, factory->scheme) {
//      gpr_log(GPR_INFO, "match found, using scheme %s", factory->scheme);
//      match = 1;
//      break;
//    }
//  }
//  if (match) {
//    factory->vtable->join_host_port(factory, joined_host_port, host, port);
//    return 0;
//  }
//
//  return -1;
}

int grpc_generic_split_host_port(const char *host_port, char **host, char **port) {
  gpr_log(GPR_INFO, "hello from grpc_generic_split_host_port");
  return 0;
//  gpr_log(GPR_INFO, "hello from grpc_generic_split_host_port");
//  return 0;
//  grpc_uri *parsed_uri = NULL;
//  grpc_resolver_factory *factory = NULL;
//  char *canonical_target = NULL;
//
//  factory = resolve_factory(uri, &parsed_uri, &canonical_target);
//  if (factory == NULL) {
//    gpr_log(GPR_ERROR, "coudn't find factory for uri: %s", uri);
//    *host = NULL;
//    *port = NULL;
//    return;
//  }
//  gpr_log(GPR_INFO, "canonical target is %s", canonical_target);
//  gpr_log(GPR_INFO, "split %s into host port", parsed_uri->authority);
//  factory->vtable->split_host_port(factory, parsed_uri->authority, host, port);
//  gpr_log(GPR_INFO, "destroy uri");
//  grpc_uri_destroy(parsed_uri);
}
