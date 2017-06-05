#!/bin/sh
# Commands for populating DNS records in GCE that are used in testing.
#
# This file has been automatically generated from a template file.
# Please look at the templates directory instead.
# This file can be regenerated from the template by running
# tools/buildgen/generate_projects.sh

# Copyright 2015, Google Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#     * Neither the name of Google Inc. nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


# Populates a DNS managed zone with records, for use in testing

set -ex

cd $(dirname $0)

gcloud dns record-sets transaction start -z="apolcyn-zone"

gcloud dns record-sets transaction add \
   -z="apolcyn-zone" \
   --name="ipv4-single-target.test.apolcyntest." \
   --type="A" \
   --ttl="2100" \
   "1.2.3.4"

gcloud dns record-sets transaction add \
   -z="apolcyn-zone" \
   --name="ipv4-multi-target.test.apolcyntest." \
   --type="A" \
   --ttl="2100" \
   "100.1.1.1,100.2.2.2,100.3.3.3"

gcloud dns record-sets transaction add \
   -z="apolcyn-zone" \
   --name="ipv6-single-target.test.apolcyntest." \
   --type="AAAA" \
   --ttl="2100" \
   "2607:f8b0:400a:801::1005"

gcloud dns record-sets transaction add \
   -z="apolcyn-zone" \
   --name="ipv6-multi-target.test.apolcyntest." \
   --type="AAAA" \
   --ttl="2100" \
   "2607:f8b0:400a:801::1001,2607:f8b0:400a:801::1002,2607:f8b0:400a:801::1003"

gcloud dns record-sets transaction add \
   -z="apolcyn-zone" \
   --name="srv-ipv4-single-target.test.apolcyntest." \
   --type="SRV" \
   --ttl="2100" \
   "ipv4-single-target.test.apolcyntest."

gcloud dns record-sets transaction add \
   -z="apolcyn-zone" \
   --name="srv-ipv4-multi-target.test.apolcyntest." \
   --type="SRV" \
   --ttl="2100" \
   "ipv4-multi-target.test.apolcyntest."

gcloud dns record-sets transaction add \
   -z="apolcyn-zone" \
   --name="srv-ipv6-single-target.test.apolcyntest." \
   --type="SRV" \
   --ttl="2100" \
   "ipv6-single-target.test.apolcyntest."

gcloud dns record-sets transaction add \
   -z="apolcyn-zone" \
   --name="srv-ipv6-multi-target.test.apolcyntest." \
   --type="SRV" \
   --ttl="2100" \
   "ipv6-multi-target.test.apolcyntest."

gcloud dns record-sets transaction describe -z="apolcyn-zone"

gcloud dns record-sets transaction execute -z="apolcyn-zone"

gcloud dns record-sets list -z="apolcyn-zone"

