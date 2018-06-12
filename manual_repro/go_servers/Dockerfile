FROM golang:1.8

RUN apt-get update && apt-get install vim -y && apt-get clean

RUN ln -s /usr/local/go/bin/go /usr/local/bin
RUN mkdir -p /go/src/google.golang.org/grpc
RUN git clone https://github.com/grpc/grpc-go.git /go/src/google.golang.org/grpc
# The grpc-go commit here is againt arbitrary but stable. To be updated as needed.
# Pre-build relevant go test servers.
RUN mkdir /go/src/google.golang.org/fake_grpclb
COPY fake_grpclb.go /go/src/google.golang.org/fake_grpclb/
RUN export GOPATH=/go && cd /go/src/google.golang.org/grpc && \
    git checkout 5d8897144f741c0358f5f194503db84142d0a985 && \
    make deps && \
    make testdeps && \
    cd interop/server && go build server.go && \
    cd /go/src/google.golang.org/fake_grpclb && \
    go build fake_grpclb.go

# Make sure that changes to fake_grpclb.go force this docker image to rebuild.
RUN FAKE_GRPCLB_SHA1="$(sha1sum /go/src/google.golang.org/fake_grpclb/fake_grpclb.go | awk '{ print $1 }')" && \
    EXPECTED_SHA1=766ef8987f2d9093bf58e783b2fc19beb66a1e62 && \
    echo "$FAKE_GRPCLB_SHA1" | grep "$EXPECTED_SHA1"

RUN apt-get install python2.7 -y

# Define the default command.
CMD ["bash"]
