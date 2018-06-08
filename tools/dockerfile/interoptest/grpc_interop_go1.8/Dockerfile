# Copyright 2017 gRPC authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

FROM golang:1.8

# Using login shell removes Go from path, so we add it.
RUN ln -s /usr/local/go/bin/go /usr/local/bin

#====================
# Python dependencies

# Install dependencies

RUN apt-get update && apt-get install -y \
    python-all-dev \
    python3-all-dev \
    python-pip

# Install Python packages from PyPI
RUN pip install --upgrade pip==10.0.1
RUN pip install virtualenv
RUN pip install futures==2.2.0 enum34==1.0.4 protobuf==3.5.2.post1 six==1.10.0 twisted==17.5.0

RUN mkdir /var/local/local_dns_server
RUN curl -sL https://raw.githubusercontent.com/grpc/grpc/66e67809aeff7cc7be811447e87313d415b75f9e/test/cpp/naming/utils/dns_server.py > /var/local/local_dns_server/dns_server.py
RUN curl -sL https://raw.githubusercontent.com/grpc/grpc/66e67809aeff7cc7be811447e87313d415b75f9e/test/cpp/naming/utils/tcp_connect.py > /var/local/local_dns_server/tcp_connect.py
RUN curl -sL https://raw.githubusercontent.com/grpc/grpc/66e67809aeff7cc7be811447e87313d415b75f9e/test/cpp/naming/utils/dns_resolver.py > /var/local/local_dns_server/dns_resolver.py
RUN chmod a+x /var/local/local_dns_server/dns_server.py
RUN chmod a+x /var/local/local_dns_server/tcp_connect.py
RUN chmod a+x /var/local/local_dns_server/dns_resolver.py

COPY build_interop.sh .
RUN echo "22inv" && bash build_interop.sh

RUN pip install pyyaml

# Define the default command.
CMD ["bash"]
