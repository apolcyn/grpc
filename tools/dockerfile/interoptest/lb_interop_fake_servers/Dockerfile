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

# Install Python packages from PyPI
RUN pip install --upgrade pip==10.0.1
RUN pip install virtualenv
RUN pip install futures==2.2.0 enum34==1.0.4 protobuf==3.5.2.post1 six==1.10.0 twisted==17.5.0

# Define the default command.
CMD ["bash"]
