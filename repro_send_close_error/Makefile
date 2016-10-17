ldflags = -L. -fstack-protector -rdynamic -Wl,-export-dynamic ../tmp/x86_64-linux/grpc_c/2.3.0/libs/opt/libgrpc.a -Wl,-wrap,memcpy -ldl
cflags= -I../include -std=c99 -Wall -Werror -Wextra -Wno-format -pedantic -O0 -pthread

all:
	gcc $(cflags) rb_repro_pure_c.c -o repro_error $(ldflags)
