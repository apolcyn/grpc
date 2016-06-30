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

#include <ruby/ruby.h>

#include "rb_grpc_imports.generated.h"
#include "rb_compression_options.h"

#include <grpc/grpc.h>
#include <grpc/impl/codegen/compression_types.h>
#include <grpc/impl/codegen/alloc.h>
#include <grpc/impl/codegen/grpc_types.h>
#include <grpc/compression.h>

#include "rb_grpc.h"

static VALUE grpc_rb_cCompressionOptions = Qnil;

/* grpc_rb_channel_credentials wraps a grpc_channel_credentials.  It provides a
 * mark object that is used to hold references to any objects used to create
 * the credentials. */
typedef struct grpc_rb_compression_options {
  /* Holder of ruby objects involved in constructing the credentials */
  VALUE mark;

  /* The actual credentials */
  grpc_compression_options *wrapped;
} grpc_rb_compression_options;

/* Destroys the credentials instances. */
static void grpc_rb_compression_options_free(void *p) {
  grpc_rb_compression_options *wrapper = NULL;
  if (p == NULL) {
    return;
  };
  wrapper = (grpc_rb_compression_options *)p;
  wrapper->wrapped = NULL;

  xfree(p);
}

/* Protects the mark object from GC */
static void grpc_rb_compression_options_mark(void *p) {
  grpc_rb_compression_options *wrapper = NULL;
  if (p == NULL) {
    return;
  }
  wrapper = (grpc_rb_compression_options *)p;

  if (wrapper->mark != Qnil) {
    rb_gc_mark(wrapper->mark);
  }
}

static rb_data_type_t grpc_rb_compression_options_data_type = {
    "grpc_compression_options",
    {grpc_rb_compression_options_mark, grpc_rb_compression_options_free,
     GRPC_RB_MEMSIZE_UNAVAILABLE, {NULL, NULL}},
    NULL,
    NULL,
#ifdef RUBY_TYPED_FREE_IMMEDIATELY
    RUBY_TYPED_FREE_IMMEDIATELY
#endif
};

/* Allocates ChannelCredential instances.
   Provides safe initial defaults for the instance fields. */
static VALUE grpc_rb_compression_options_alloc(VALUE cls) {
  grpc_rb_compression_options *wrapper = ALLOC(grpc_rb_compression_options);
  wrapper->wrapped = NULL;
  wrapper->mark = Qnil;
  return TypedData_Wrap_Struct(cls, &grpc_rb_compression_options_data_type, wrapper);
}

/*
  call-seq:
    creds1 = Credentials.new()
    ...
    creds2 = Credentials.new(pem_root_certs)
    ...
    creds3 = Credentials.new(pem_root_certs, pem_private_key,
                             pem_cert_chain)
    pem_root_certs: (optional) PEM encoding of the server root certificate
    pem_private_key: (optional) PEM encoding of the client's private key
    pem_cert_chain: (optional) PEM encoding of the client's cert chain
    Initializes Credential instances. */
static VALUE grpc_rb_compression_options_init(VALUE self) {
  grpc_rb_compression_options *wrapper = NULL;
  grpc_compression_options *compression_options = NULL;
  /* "03" == no mandatory arg, 3 optional */
  /*rb_scan_args(argc, argv, "03", &pem_root_certs, &pem_private_key,
               &pem_cert_chain);*/

  TypedData_Get_Struct(self, grpc_rb_compression_options,
                       &grpc_rb_compression_options_data_type, wrapper);

  compression_options = gpr_malloc(sizeof(grpc_compression_options));
  grpc_compression_options_init(compression_options);
  wrapper->wrapped = compression_options;

  return self;
}

VALUE grpc_rb_enable_compression_algorithm(VALUE self, VALUE algorithm_to_enable) {
  grpc_compression_algorithm compression_algorithm = 0;
  grpc_rb_compression_options *wrapper = NULL;

  TypedData_Get_Struct(self, grpc_rb_compression_options, &grpc_rb_compression_options_data_type, wrapper);
  compression_algorithm = (grpc_compression_algorithm)NUM2INT(algorithm_to_enable);

  grpc_compression_options_enable_algorithm(wrapper->wrapped, compression_algorithm);

  return Qnil;
}

VALUE grpc_rb_disable_compression_algorithm(VALUE self, VALUE algorithm_to_disable) {
  grpc_compression_algorithm compression_algorithm = 0;
  grpc_rb_compression_options *wrapper = NULL;

  TypedData_Get_Struct(self, grpc_rb_compression_options, &grpc_rb_compression_options_data_type, wrapper);
  compression_algorithm = (grpc_compression_algorithm)NUM2INT(algorithm_to_disable);

  grpc_compression_options_disable_algorithm(wrapper->wrapped, compression_algorithm);

  return Qnil;
}

VALUE grpc_rb_is_algorithm_enabled(VALUE self, VALUE algorithm_to_enable) {
  grpc_compression_algorithm compression_algorithm = 0;
  grpc_rb_compression_options *wrapper = NULL;

  TypedData_Get_Struct(self, grpc_rb_compression_options, &grpc_rb_compression_options_data_type, wrapper);
  compression_algorithm = (grpc_compression_algorithm)NUM2INT(algorithm_to_enable);

  return grpc_compression_options_is_algorithm_enabled(wrapper->wrapped, compression_algorithm) ? Qtrue : Qfalse;
}

VALUE grpc_rb_get_enabled_algorithms_bitset(VALUE self) {
  grpc_rb_compression_options *wrapper = NULL;

  TypedData_Get_Struct(self, grpc_rb_compression_options, &grpc_rb_compression_options_data_type, wrapper);
  return INT2NUM((int)wrapper->wrapped->enabled_algorithms_bitset);
}

VALUE grpc_rb_get_default_algorithm(VALUE self) {
  grpc_rb_compression_options *wrapper = NULL;

  TypedData_Get_Struct(self, grpc_rb_compression_options, &grpc_rb_compression_options_data_type, wrapper);

  return RTEST(wrapper->wrapped->default_level.level) ? INT2NUM((int)wrapper->wrapped->default_level.level) : Qnil;
}

VALUE grpc_rb_get_default_level(VALUE self) {
  grpc_rb_compression_options *wrapper = NULL;

  TypedData_Get_Struct(self, grpc_rb_compression_options, &grpc_rb_compression_options_data_type, wrapper);

  return RTEST(wrapper->wrapped->default_algorithm.algorithm) ? INT2NUM((int)wrapper->wrapped->default_level.level) : Qnil;
}

VALUE grpc_rb_get_default_algorithm_internal_value(VALUE self) {
  grpc_rb_compression_options *wrapper = NULL;

  TypedData_Get_Struct(self, grpc_rb_compression_options, &grpc_rb_compression_options_data_type, wrapper);

  return wrapper->wrapped->default_algorithm.is_set ? wrapper->wrapped->default_algorithm.algorithm : GRPC_COMPRESS_NONE;
}

VALUE grpc_rb_get_default_level_internal_value(VALUE self) {
  grpc_rb_compression_options *wrapper = NULL;

  TypedData_Get_Struct(self, grpc_rb_compression_options, &grpc_rb_compression_options_data_type, wrapper);

  return wrapper->wrapped->default_level.is_set ? wrapper->wrapped->default_level.level: GRPC_COMPRESS_NONE;
}

VALUE grpc_rb_enable_algorithms(int argc, VALUE *argv, VALUE self) {
  VALUE algorithm_names = NULL;
  char *name = NULL;
  compression_algorithm internal_algorithm_value;
  int name_len;

  rb_scan_args(argc, argv, "0*", &algorithm_names);
  for(int i = 0; i < RARRAY_LEN(algorithm_names); i++) {
    name = RSTRING(rb_ary_entry(algorithm_names, i))->ptr;
    name_len = RSTRING(rb_ary_entry(algorithm_names, i))->len;

    grpc_compression_algorithm_parse(name, name_len, &internal_algorithm_value);

  }


  return Qnil;
}

VALUE grpc_rb_disable_algorithms(int argc, VALUE *argv, VALUE self) {
  return Qnil;
}

void Init_grpc_compression_options() {
  grpc_rb_cCompressionOptions =
      rb_define_class_under(grpc_rb_mGrpcCore, "CompressionOptions", rb_cObject);

  /* Allocates an object managed by the ruby runtime */
  rb_define_alloc_func(grpc_rb_cCompressionOptions,
                       grpc_rb_compression_options_alloc);

  /* Provides a ruby constructor and support for dup/clone. */
  rb_define_method(grpc_rb_cCompressionOptions, "initialize",
                   grpc_rb_compression_options_init, 0);

  rb_define_method(grpc_rb_cCompressionOptions, "internal_enable_algorithm", grpc_rb_enable_compression_algorithm, 1);
  rb_define_method(grpc_rb_cCompressionOptions, "internal_disable_algorithm", grpc_rb_disable_compression_algorithm, 1);
  rb_define_method(grpc_rb_cCompressionOptions, "internal_is_algorithm_enabled", grpc_rb_is_algorithm_enabled, 1);

  rb_define_method(grpc_rb_cCompressionOptions, "enabled_algorithms_bitset", grpc_rb_get_enabled_algorithms_bitset, 0);
  rb_define_method(grpc_rb_cCompressionOptions, "default_level=", grpc_rb_get_default_algorithm, 1);
  rb_define_method(grpc_rb_cCompressionOptions, "default_algorithm=", grpc_rb_get_default_level, 1);

  rb_define_method(grpc_rb_cCompressionOptions, "default_level_internal_value", grpc_rb_get_default_level_internal_value, 0);
  rb_define_method(grpc_rb_cCompressionOptions, "default_algorithm_internal_value", grpc_rb_get_default_algorithm_internal_value, 0);

  rb_define_method(grpc_rb_cCompressionOptions, "enable_algorithms", grpc_rb_enable_algorithms, -1);
  rb_define_method(grpc_rb_cCompressionOptions, "disable_algorithms", grpc_rb_disable_algorithms, -1);
 /* rb_define_method(grpc_rb_cCompressionOptions, "initialize_copy",

                   grpc_rb_channel_credentials_init_copy, 1);*/
}
