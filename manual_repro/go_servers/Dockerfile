FROM golang:1.10

RUN apt-get update && apt-get install -y \
  dnsutils \
  git \
  vim \
  curl \
  python-pip \
  python-yaml \
  make && apt-get clean

RUN ln -s /usr/local/go/bin/go /usr/local/bin
RUN mkdir -p /go/src/google.golang.org/grpc
RUN git clone -b grpclb_interop_client https://github.com/apolcyn/grpc-go.git /go/src/google.golang.org/grpc
# The grpc-go commit here is againt arbitrary but stable. To be updated as needed.
# Pre-build relevant go test servers.
RUN mkdir /go/src/google.golang.org/fake_grpclb
RUN export GOPATH=/go && cd /go/src/google.golang.org/grpc && \
    make deps
RUN cd /go/src/google.golang.org/grpc/interop/grpclb && \
    go build fake_grpclb.go

# Define the default command.
CMD ["bash"]
