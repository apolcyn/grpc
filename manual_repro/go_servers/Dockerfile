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
RUN git clone https://github.com/grpc/grpc-go.git /go/src/google.golang.org/grpc
# The grpc-go commit here is againt arbitrary but stable. To be updated as needed.
# Pre-build relevant go test servers.
RUN mkdir /go/src/google.golang.org/fake_grpclb
RUN export GOPATH=/go && cd /go/src/google.golang.org/grpc && \
    git checkout 5d8897144f741c0358f5f194503db84142d0a985 && \
    make deps
COPY fake_grpclb.go /go/src/google.golang.org/fake_grpclb/
RUN cd /go/src/google.golang.org/fake_grpclb && \
    go build fake_grpclb.go

# Define the default command.
CMD ["bash"]
