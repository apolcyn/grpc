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
RUN echo hello
RUN echo hello
RUN git clone -b grpclb_interop_client https://github.com/apolcyn/grpc-go.git /go/src/google.golang.org/grpc
# The grpc-go commit here is againt arbitrary but stable. To be updated as needed.
# Pre-build relevant go test servers.
RUN export GOPATH=/go && cd /go/src/google.golang.org/grpc && \
    make deps
RUN cd /go/src/google.golang.org/grpc/interop/fake_grpclb && \
    go install
RUN cd /go/src/google.golang.org/grpc/interop/server && \
    go install
RUN cd /go/src/google.golang.org/grpc/interop/client && \
    go install

# Install Python packages from PyPI
RUN pip install --upgrade pip==10.0.1
RUN pip install virtualenv
RUN pip install futures==2.2.0 enum34==1.0.4 protobuf==3.5.2.post1 six==1.10.0 twisted==17.5.0

# We need the grpc/grpc repo because it has the fake local DNS server scripts
RUN mkdir -p /var/local/git/grpc
RUN echo hello
RUN echo hello
RUN git clone -b grpclb_interop https://github.com/apolcyn/grpc.git /var/local/git/grpc

# Define the default command.
CMD ["bash"]
