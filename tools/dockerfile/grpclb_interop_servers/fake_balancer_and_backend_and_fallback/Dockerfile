FROM golang:1.10

RUN apt-get update && apt-get install -y \
  git \
  make && apt-get clean

RUN ln -s /usr/local/go/bin/go /usr/local/bin
RUN mkdir -p /go/src/google.golang.org/grpc
RUN echo hello2
# TODO: don't use personal github branch after merging fake grpclb.go into go master
RUN git clone -b grpclb_interop_client https://github.com/apolcyn/grpc-go.git /go/src/google.golang.org/grpc
RUN mkdir /go/src/google.golang.org/fake_grpclb
RUN export GOPATH=/go && cd /go/src/google.golang.org/grpc && \
    make deps && make testdeps

# Pre-compile the fake grpclb balancer
RUN cd /go/src/google.golang.org/grpc/interop/fake_grpclb && go install
# Pre-compile the go interop server. The grpclb inserop test will overload the server
# and use it as both "backend" and "fallback" server(s).
RUN cd /go/src/google.golang.org/grpc/interop/server && go install

# Define the default command.
CMD ["bash"]
