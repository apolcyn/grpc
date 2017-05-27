#!/usr/bin/env python
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

# Populates a DNS managed zone on DNS with records for testing

import argparse
import subprocess

import dns_records_config


argp = argparse.ArgumentParser(description='')
argp.add_argument('--dry_run', default=False, action='store_const', const=True,
                  help='Print the commands that would be ran, without running them')
argp.add_argument('--list_records', default=False, action='store_const', const=True,
                  help='Don\'t modify records and use gcloud API to print existing records')
args = argp.parse_args()


def main():
  cmds = []
  cmds.append(('gcloud dns record-sets transaction start -z=%s' % dns_records_config.ZONE_NAME).split(' '))

  for r in dns_records_config.DNS_RECORDS:
    add_cmd = []
    prefix = ('gcloud dns record-sets transaction add '
              '-z=%s --name=%s --type=%s --ttl=%s') % (dns_records_config.ZONE_NAME,
                                                       r.record_name,
                                                       r.record_type,
                                                       dns_records_config.TTL)

    add_cmd.extend(prefix.split(' '))
    add_cmd.extend(r.uploadable_data())
    cmds.append(add_cmd)

  cmds.append(('gcloud dns record-sets transaction describe -z=%s' % dns_records_config.ZONE_NAME).split(' '))
  cmds.append(('gcloud dns record-sets transaction execute -z=%s' % dns_records_config.ZONE_NAME).split(' '))
  cmds.append(('gcloud dns record-sets list -z=%s' % dns_records_config.ZONE_NAME).split(' '))

  if args.list_records:
    subprocess.call(('gcloud dns record-sets list -z=%s' % dns_records_config.ZONE_NAME).split(' '))
    return

  if args.dry_run:
    print('printing commands that would be run: (tokenizing may differ)')

  for c in cmds:
    if args.dry_run:
      print(' '.join(c))
    else:
      subprocess.call(c)

main()
