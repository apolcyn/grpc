FROM debian:stretch

RUN apt-get update && apt-get install -y \
  autoconf \
  autotools-dev \
  build-essential \
  dnsutils \
  gcc \
  gcc-multilib \
  g++ \
  g++-multilib \
  git \
  libc6 \
  libgflags-dev \
  libtool \
  libgtest-dev \
  libc++-dev \
  dnsutils \
  vim \
  curl \
  make && apt-get clean

RUN mkdir -p /var/local/git/grpc && git clone https://github.com/grpc/grpc.git /var/local/git/grpc
# Fully initialize the github repo at an arbitrary but stable commit.
# We can update this commit to sync with later gRPC revisions.
# TODO: don't use personal branch here
RUN echo invalidate cache
RUN cd /var/local/git/grpc && \
    git remote add apolcyn https://github.com/apolcyn/grpc.git && \
    git fetch apolcyn my_alts_kokoro_tests && \
    git checkout apolcyn/my_alts_kokoro_tests && \
    git submodule update --init
# Pre-compile to speed up test runs
RUN cd /var/local/git/grpc && make interop_client -j4

# Install go and the go github repo
RUN GOPKG=go1.10.1.linux-amd64.tar.gz && \
    curl -sL "https://dl.google.com/go/${GOPKG}" > "$GOPKG" && \
    tar -C /usr/local -xzf "$GOPKG"
RUN ln -s /usr/local/go/bin/go /usr/local/bin
RUN mkdir -p src/google.golang.org/grpc
RUN git clone https://github.com/grpc/grpc-go.git src/google.golang.org/grpc
RUN cd src/google.golang.org/grpc && git checkout 5d8897144f741c0358f5f194503db84142d0a985 && make deps && make testdeps
RUN cd /src/google.golang.org/grpc/interop/server && go build server.go
RUN mkdir /src/google.golang.org/fake_grpclb
COPY fake_grpclb.go /src/google.golang.org/fake_grpclb/
RUN cd /src/google.golang.org/fake_grpclb && go build fake_grpclb.go
#TODO: copy the fake grpclb server.go file in to here now, and built it.

RUN apt-get update && apt-get install -y \
    python-pip \
    python-yaml && \
    apt-get clean

# Install Python packages from PyPI
RUN pip install --upgrade pip==9.0.2
RUN pip install twisted==17.5.0

#TODO: install python and those python deps

# Define the default command.
CMD ["bash"]
