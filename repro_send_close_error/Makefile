ldflags = -L. -fstack-protector -rdynamic -Wl,-export-dynamic /usr/local/google/home/apolcyn/grpc/tmp/x86_64-linux/grpc_c/2.3.0/libs/opt/libgrpc.a -Wl,-wrap,memcpy -ldl
cflags= -I/usr/local/google/home/apolcyn/grpc/include -std=c99 -Wall -Werror -Wextra -Wno-format -pedantic -O0 -pthread

all:
	gcc $(cflags) rb_repro_pure_c.c -o repro_error $(ldflags)
