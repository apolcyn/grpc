ldflags = -L. -fstack-protector -fPIE -pie -rdynamic -Wl,-export-dynamic ../tmp/x86_64-linux/grpc_c/2.3.0/libs/dbg/libgrpc.a -Wl,-wrap,memcpy -ldl
cflags= -I../include -fsanitize=thread -g -std=c99 -Wall -Werror -Wextra -Wno-format -pedantic -O0 -pthread

all:
	clang $(cflags) rb_repro_pure_c.c -o repro_error $(ldflags)
