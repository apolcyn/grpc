cfg=tsan
lp=-fPIE -pie
ldflags = -L. -fstack-protector $(lp) -rdynamic -Wl,-export-dynamic ../tmp/x86_64-linux/grpc_c/2.3.0/libs/$(cfg)/libgrpc.a -Wl,-wrap,memcpy -ldl
op=-fsanitize=thread
cflags= -I../include $(op) -fno-omit-frame-pointer -g -std=c99 -Wall -Werror -Wextra -Wno-format -pedantic -O0 -pthread

all:
	clang $(cflags) rb_repro_pure_c.c -o repro_error $(ldflags)
