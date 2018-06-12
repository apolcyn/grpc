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
  python-pip \
  python-yaml \
  make && apt-get clean

RUN mkdir -p /var/local/git/grpc && \
    git clone https://github.com/grpc/grpc.git /var/local/git/grpc
# Fully initialize the github repo at an arbitrary but stable commit.
# We can update this commit to sync with later gRPC revisions.
RUN cd /var/local/git/grpc && \
    git remote add apolcyn https://github.com/apolcyn/grpc.git && \
    git fetch apolcyn my_alts_kokoro_tests && \
    git checkout 246071a4e22d3d2ea19dad8714b19ef2dae5464a && \
    git submodule update --init
# Pre-compile to speed up test runs.
RUN cd /var/local/git/grpc && make interop_client -j4

# Install go.
RUN GOPKG=go1.10.1.linux-amd64.tar.gz && \
    curl -sL "https://dl.google.com/go/${GOPKG}" > "$GOPKG" && \
    tar -C /usr/local -xzf "$GOPKG"
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

# Install JDK and grpc-java
RUN echo oracle-java8-installer shared/accepted-oracle-license-v1-1 select true | /usr/bin/debconf-set-selections && \
  echo "deb http://ppa.launchpad.net/webupd8team/java/ubuntu trusty main" | tee /etc/apt/sources.list.d/webupd8team-java.list && \
  echo "deb-src http://ppa.launchpad.net/webupd8team/java/ubuntu trusty main" | tee -a /etc/apt/sources.list.d/webupd8team-java.list && \
  apt-key adv --keyserver hkp://keyserver.ubuntu.com:80 --recv-keys EEA14886
RUN apt-get update && apt-get -y install \
      git \
      libapr1 \
      oracle-java8-installer \
      && \
    apt-get clean && rm -r /var/cache/oracle-jdk8-installer/
# Trigger download of as many Gradle artifacts as possible.
RUN git clone --recursive --depth 1 https://github.com/grpc/grpc-java.git && \
  cd grpc-java && \
  ./gradlew :grpc-interop-testing:installDist -PskipCodegen=true && \
  rm -r "$(pwd)"
ENV JAVA_HOME /usr/lib/jvm/java-8-oracle
ENV PATH $PATH:$JAVA_HOME/bin
# Compiler grpc-java client
RUN mkdir /var/local/git/grpc-java
RUN git clone --recursive --depth 1 https://github.com/grpc/grpc-java.git /var/local/git/grpc-java
RUN cd /var/local/git/grpc-java && ./gradlew :grpc-interop-testing:installDist -PskipCodegen=true

# Install Python test utility deps.
RUN pip install --upgrade pip==9.0.2
RUN pip install twisted==17.5.0

# Make sure that changes to fake_grpclb.go force this docker image to rebuild.
RUN FAKE_GRPCLB_SHA1="$(sha1sum /go/src/google.golang.org/fake_grpclb/fake_grpclb.go | awk '{ print $1 }')" && \
    EXPECTED_SHA1=766ef8987f2d9093bf58e783b2fc19beb66a1e62 && \
    echo "$FAKE_GRPCLB_SHA1" | grep "$EXPECTED_SHA1"

RUN apt-get install procps -y

# Define the default command.
CMD ["bash"]
