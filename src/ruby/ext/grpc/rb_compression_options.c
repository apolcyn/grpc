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
#include <string.h>

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

void set_default_compression_level(grpc_compression_options *compression_options, grpc_compression_level level) {
  compression_options->default_level.is_set = 1;
  compression_options->default_level.level = level;
}

VALUE grpc_rb_set_default_level(VALUE self, VALUE new_level) {
  char *level_name = NULL;
  grpc_rb_compression_options *wrapper = NULL;
  long name_len = 0;
  VALUE ruby_str = Qnil;

  TypedData_Get_Struct(self, grpc_rb_compression_options, &grpc_rb_compression_options_data_type, wrapper);
  ruby_str = rb_funcall(new_level, rb_intern("to_s"), 0);

  level_name = RSTRING_PTR(ruby_str);
  name_len = RSTRING_LEN(ruby_str);

  if(strncmp(level_name, "none", name_len) == 0) {
    set_default_compression_level(wrapper->wrapped, GRPC_COMPRESS_LEVEL_NONE);
  }
  else if(strncmp(level_name, "low", name_len) == 0) {
    set_default_compression_level(wrapper->wrapped, GRPC_COMPRESS_LEVEL_LOW);
  }
  else if(strncmp(level_name, "medium", name_len) == 0) {
    set_default_compression_level(wrapper->wrapped, GRPC_COMPRESS_LEVEL_MED);
  }
  else if(strncmp(level_name, "high", name_len) == 0) {
    set_default_compression_level(wrapper->wrapped, GRPC_COMPRESS_LEVEL_HIGH);
  }
  else {
    rb_raise(rb_eNameError, "Invalid compression level name");
  }

  return Qnil;
}

int get_internal_value_of_algorithm(VALUE algorithm_name, grpc_compression_algorithm *compression_algorithm) {
  VALUE ruby_str = Qnil;
  char *name_str= NULL;
  long name_len = 0;
  int internal_value = 0;

  ruby_str = rb_funcall(algorithm_name, rb_intern("to_s"), 0);
  name_str = RSTRING_PTR(ruby_str);
  name_len = RSTRING_LEN(ruby_str);

  if(!(internal_value = grpc_compression_algorithm_parse(name_str, name_len, compression_algorithm))) {
     rb_raise(rb_eNameError, "invalid algorithm name");//: %s", StringValueCStr(ruby_str));
  }

  return internal_value;
}

VALUE grpc_rb_set_default_algorithm(VALUE self, VALUE algorithm_name) {
  grpc_rb_compression_options *wrapper = NULL;

  TypedData_Get_Struct(self, grpc_rb_compression_options, &grpc_rb_compression_options_data_type, wrapper);

  if(get_internal_value_of_algorithm(algorithm_name, &wrapper->wrapped->default_algorithm.algorithm)) {
    wrapper->wrapped->default_algorithm.is_set = 1;
  }
  else {
    rb_raise(rb_eNameError, "invalid algorithm name");
  }

  return Qnil;
}

VALUE grpc_rb_get_default_algorithm_internal_value(VALUE self) {
  grpc_rb_compression_options *wrapper = NULL;

  TypedData_Get_Struct(self, grpc_rb_compression_options, &grpc_rb_compression_options_data_type, wrapper);

  return wrapper->wrapped->default_algorithm.is_set
    ? INT2NUM(wrapper->wrapped->default_algorithm.algorithm) : Qnil;
}

/* Gets the internal value of the default compression level that is to be passed
 * to the GRPC core as a channel argument. */
VALUE grpc_rb_get_default_level_internal_value(VALUE self) {
  grpc_rb_compression_options *wrapper = NULL;

  TypedData_Get_Struct(self, grpc_rb_compression_options, &grpc_rb_compression_options_data_type, wrapper);

  if(wrapper->wrapped->default_level.is_set) {
    return INT2NUM((int)wrapper->wrapped->default_level.level);
   }
     else {
     return Qnil;
}

/* Disables compression algorithms by their names. Raises an error if an unkown name was passed. */
VALUE grpc_rb_disable_algorithms(int argc, VALUE *argv, VALUE self) {
  VALUE algorithm_names = Qnil;
  VALUE ruby_str = Qnil;
  grpc_compression_algorithm internal_algorithm_value;

  /* read variadic argument list of names into the algorithm_name ruby array. */
  rb_scan_args(argc, argv, "0*", &algorithm_names);

  for(int i = 0; i < RARRAY_LEN(algorithm_names); i++) {
    ruby_str = rb_funcall(rb_ary_entry(algorithm_names, i), rb_intern("to_s"), 0);
    get_internal_value_of_algorithm(ruby_str, &internal_algorithm_value);
    rb_funcall(self, rb_intern(disable_algorithm_internal), 1, LONG2NUM((long)internal_algorithm_value));
  }

  return Qnil;
}

/* Provides a ruby hash of GRPC core channel argument key-values that
 * correspond to the compression settings on this instance. */
VALUE grpc_rb_get_channel_arguments_hash(VALUE self) {
  grpc_rb_compression_options *wrapper = NULL;
  grpc_compression_options *compression_options = NULL;
  VALUE channel_arg_hash = rb_funcall(rb_cHash, rb_intern("new"), 0);

  TypedData_Get_Struct(self, grpc_rb_compression_options, &grpc_rb_compression_options_data_type, wrapper);
  compression_options = wrapper->wrapped;

  if(compression_options->default_level.is_set) {
    rb_funcall(channel_arg_hash, rb_intern("[]="), 2, rb_str_new2(GRPC_COMPRESSION_CHANNEL_DEFAULT_LEVEL)
      , INT2NUM((int)compression_options->default_level.level));
  }

  if(compression_options->default_algorithm.is_set) {
    rb_funcall(channel_arg_hash, rb_intern("[]="), 2, rb_str_new2(GRPC_COMPRESSION_CHANNEL_DEFAULT_ALGORITHM)
      , INT2NUM((int)compression_options->default_algorithm.algorithm));
  }

  rb_funcall(channel_arg_hash, rb_intern("[]="), 2, rb_str_new2(GRPC_COMPRESSION_CHANNEL_ENABLED_ALGORITHMS_BITSET)
    , INT2NUM((int)compression_options->enabled_algorithms_bitset));

  return channel_arg_hash;
}

/* Provides a ruby string representation of the current channel arg hash. */
VALUE grpc_rb_display_channel_arguments_as_string(VALUE self) {
  VALUE channel_arg_hash = rb_funcall(self, rb_intern("to_hash"), 0);
  return rb_funcall(channel_arg_hash, rb_intern("to_s"), 0);
}

void Init_grpc_compression_options() {
  grpc_rb_cCompressionOptions =
      rb_define_class_under(grpc_rb_mGrpcCore, "CompressionOptions", rb_cObject);

  /* Allocates an object managed by the ruby runtime */
  rb_define_alloc_func(grpc_rb_cCompressionOptions,
                       grpc_rb_compression_options_alloc);

  /* Provides a ruby constructor and support for dup/clone. */
  rb_define_method(grpc_rb_cCompressionOptions, "initialize", grpc_rb_compression_options_init, 0);

  rb_define_method(grpc_rb_cCompressionOptions, "disable_algorithm_internal", grpc_rb_disable_compression_algorithm, 1);
  rb_define_method(grpc_rb_cCompressionOptions, "disable_algorithms", grpc_rb_disable_algorithms, -1);

  rb_define_method(grpc_rb_cCompressionOptions, "is_algorithm_enabled_internal", grpc_rb_is_algorithm_enabled, 1);
  rb_define_method(grpc_rb_cCompressionOptions, "enabled_algorithms_bitset", grpc_rb_get_enabled_algorithms_bitset, 0);

  rb_define_method(grpc_rb_cCompressionOptions, "default_algorithm=", grpc_rb_set_default_algorithm, 1);
  rb_define_method(grpc_rb_cCompressionOptions, "default_algorithm", grpc_rb_get_default_algorithm_name, 1);
  rb_define_method(grpc_rb_cCompressionOptions, "default_algorithm_internal_value", grpc_rb_get_default_algorithm_internal_value, 0);

  rb_define_method(grpc_rb_cCompressionOptions, "default_level=", grpc_rb_set_default_level, 1);
  rb_define_method(grpc_rb_cCompressionOptions, "default_level", grpc_rb_get_default_level_name, 1);
  rb_define_method(grpc_rb_cCompressionOptions, "default_level_internal_value", grpc_rb_get_default_level_internal_value, 0);

  rb_define_method(grpc_rb_cCompressionOptions, "to_hash", grpc_rb_get_channel_arguments_hash, 0);
  rb_define_method(grpc_rb_cCompressionOptions, "to_s", grpc_rb_display_channel_arguments_as_string, 0);
}
