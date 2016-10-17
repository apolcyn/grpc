ldflags= -L. -fstack-protector -rdynamic -Wl,-export-dynamic /usr/local/google/home/apolcyn/grpc/tmp/x86_64-linux/grpc_c/2.3.0/libs/dbg/libgrpc.a -Wl,-wrap,memcpy

lib:
	cd ~/grpc && make -j -C ~/grpc /usr/local/google/home/apolcyn/grpc/tmp/x86_64-linux/grpc_c/2.3.0/libs/dbg/libgrpc.a CONFIG=dbg

all:
	gcc -I/usr/local/google/home/apolcyn/grpc/include -Wno-format -std=c99 -Wall -Werror -Wextra -pedantic -O0 $(ldflags) bm_break.c
