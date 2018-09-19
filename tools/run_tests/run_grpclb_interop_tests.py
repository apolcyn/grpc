#!/usr/bin/env python
# Copyright 2015 gRPC authors.
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
"""Run interop (cross-language) tests in parallel."""

from __future__ import print_function

import argparse
import atexit
import itertools
import json
import multiprocessing
import os
import re
import subprocess
import sys
import tempfile
import time
import uuid
import six
import traceback

import python_utils.dockerjob as dockerjob
import python_utils.jobset as jobset
import python_utils.report_utils as report_utils

# Docker doesn't clean up after itself, so we do it on exit.
atexit.register(lambda: subprocess.call(['stty', 'echo']))

ROOT = os.path.abspath(os.path.join(os.path.dirname(sys.argv[0]), '../..'))
os.chdir(ROOT)

_DEFAULT_SERVER_PORT = 8080

_DNS_SCRIPTS = os.path.join(os.sep, 'var', 'local', 'dns_scripts')
_CLIENT_WITH_LOCAL_DNS_SERVER_RUNNER = os.path.join(_DNS_SCRIPTS, 'run_client_with_dns_server.py')

_GO_REPO_ROOT = os.path.join(os.sep, 'go', 'src', 'google.golang.org', 'grpc')

_TEST_TIMEOUT = 30

class CXXLanguage:

    def __init__(self):
        self.client_cwd = '/var/local/git/grpc'
        self.safename = 'cxx'

    def client_cmd(self, args):
        return ['bins/opt/interop_client'] + args

    def global_env(self):
        return {'GRPC_DNS_RESOLVER': 'ares',
                'GRPC_VERBOSITY': 'DEBUG',
                'GRPC_TRACE': 'client_channel,glb'}

    def __str__(self):
        return 'c++'


class JavaLanguage:

    def __init__(self):
        self.client_cwd = '/var/local/git/grpc-java'
        self.safename = str(self)

    def client_cmd(self, args):
        return ['./run-test-client.sh'] + args

    def global_env(self):
        return {'JAVA_OPTS': '-Dio.grpc.internal.DnsNameResolverProvider.enable_grpclb=true'}

    def __str__(self):
        return 'java'


class GoLanguage:

    def __init__(self):
        self.client_cwd = '/go/src/google.golang.org/grpc/interop/client'
        self.safename = str(self)

    def client_cmd(self, args):
        return ['go', 'run', 'client.go'] + args

    def global_env(self):
        return {'GRPC_GO_LOG_VERBOSITY_LEVEL': '3',
                'GRPC_GO_LOG_SEVERITY_LEVEL': 'INFO'}

    def __str__(self):
        return 'go'


_LANGUAGES = {
    'c++': CXXLanguage(),
    'go': GoLanguage(),
    'java': JavaLanguage(),
}

_TRANSPORT_SECURITY_OPTIONS = ['tls', 'alts', 'insecure']

DOCKER_WORKDIR_ROOT = '/var/local/git/grpc'


def docker_run_cmdline(cmdline, image, docker_args=[], cwd=None, environ=None):
    """Wraps given cmdline array to create 'docker run' cmdline from it."""
    docker_cmdline = ['docker', 'run', '-i', '--rm=true']

    # turn environ into -e docker args
    if environ:
        for k, v in environ.items():
            docker_cmdline += ['-e', '%s=%s' % (k, v)]

    # set working directory
    workdir = DOCKER_WORKDIR_ROOT
    if cwd:
        workdir = os.path.join(workdir, cwd)
    docker_cmdline += ['-w', workdir]

    docker_cmdline += docker_args + [image] + cmdline
    return docker_cmdline


def bash_cmdline(cmdline):
    """Creates bash -c cmdline from args list."""
    # Use login shell:
    # * makes error messages clearer if executables are missing
    return ['bash', '-c', ' '.join(cmdline)]


def _job_kill_handler(job):
    assert job._spec.container_name
    dockerjob.docker_kill(job._spec.container_name)
    # When the job times out and we decide to kill it,
    # we need to wait a before restarting the job
    # to prevent "container name already in use" error.
    # TODO(jtattermusch): figure out a cleaner way to to this.
    time.sleep(2)


def transport_security_to_args(transport_security):
    args = []
    if transport_security == 'tls':
        args += ['--use_tls=true']
    elif transport_security == 'alts':
        args += ['--use_tls=false', '--use_alts=true']
    elif transport_security == 'insecure':
        args += ['--use_tls=false']
    else:
        print('Invalid transport security option.')
        sys.exit(1)
    return args


def lb_client_interop_jobspec(language,
                      fake_dns_server_ip,
                      docker_image,
                      transport_security='tls'):
    """Creates jobspec for cloud-to-cloud interop test"""
    interop_only_options = [
        '--server_host=server.test.google.fr',
        '--server_port=443',
        '--use_test_ca=true',
    ] + transport_security_to_args(transport_security)
    client_args = language.client_cmd(interop_only_options)
    environ = language.global_env()
    container_name = dockerjob.random_name(
        'lb_interop_client_%s' % language.safename)
    docker_cmdline = docker_run_cmdline(
        client_args,
        image=docker_image,
        environ=environ,
        cwd=language.client_cwd,
        docker_args=['--dns=%s' % fake_dns_server_ip,
                     '--net=host',
                     '--name=%s' % container_name])
    jobset.message('IDLE', 'docker_cmdline:\b|%s|' % ' '.join(docker_cmdline), do_newline=True)
    test_job = jobset.JobSpec(
        cmdline=docker_cmdline,
        environ=environ,
        shortname=('lb_interop_client:%s' % language),
        timeout_seconds=_TEST_TIMEOUT,
        kill_handler=_job_kill_handler,
        verbose_success=True)
    test_job.container_name = container_name
    return test_job


def backend_or_fallback_server_jobspec(transport_security, shortname):
    """Create jobspec for running a fallback or backend server"""
    return grpc_server_in_docker_jobspec(
        server_run_script='tools/dockerfile/grpclb_interop_servers/run_fake_backend_or_fallback_server.sh',
        backend_port=None,
        transport_security=transport_security,
        shortname=shortname)


def fake_grpclb_jobspec(transport_security, fake_backend_port, shortname):
    """Create jobspec for running a server"""
    return grpc_server_in_docker_jobspec(
        server_run_script='tools/dockerfile/grpclb_interop_servers/run_fake_grpclb_server.sh',
        backend_port=fake_backend_port,
        transport_security=transport_security,
        shortname=shortname)


def grpc_server_in_docker_jobspec(server_run_script, backend_port, transport_security, shortname):
    container_name = dockerjob.random_name(shortname)
    build_and_run_docker_cmdline = (
        'bash -l tools/run_tests/dockerize/build_and_run_docker.sh').split()
    docker_extra_args = '-p=%s ' % str(_DEFAULT_SERVER_PORT)
    docker_extra_args += '-e GRPC_GO_LOG_VERBOSITY_LEVEL=3 '
    docker_extra_args += '-e GRPC_GO_LOG_SEVERITY_LEVEL=INFO '
    docker_extra_args += '-e PORT=%s ' % _DEFAULT_SERVER_PORT
    if backend_port is not None:
      docker_extra_args += '-e BACKEND_PORT=%s ' % backend_port
    if transport_security == 'alts':
      docker_extra_args += '-e USE_ALTS=true '
    elif transport_security == 'tls':
      docker_extra_args += '-e USE_TLS=true '
    else:
      assert transport_security == 'insecure'
    build_and_run_docker_environ = {
      'CONTAINER_NAME': container_name,
      'DOCKERFILE_DIR': 'tools/dockerfile/grpclb_interop_servers',
      'DOCKER_RUN_SCRIPT': server_run_script,
      'EXTRA_DOCKER_ARGS': docker_extra_args,
    }
    server_job = jobset.JobSpec(
        cmdline=build_and_run_docker_cmdline,
        environ=build_and_run_docker_environ,
        shortname=shortname,
        timeout_seconds=30 * 60,
        verbose_success=True)
    server_job.container_name = container_name
    return server_job


def dns_server_in_docker_jobspec(fake_grpclb_port, shortname):
    container_name = dockerjob.random_name(shortname)
    build_and_run_docker_cmdline = (
        'bash -l tools/run_tests/dockerize/build_and_run_docker.sh').split()
    build_and_run_docker_environ = {
      'CONTAINER_NAME': container_name,
      'DOCKERFILE_DIR': 'tools/dockerfile/grpclb_interop_servers',
      'DOCKER_RUN_SCRIPT': 'tools/dockerfile/grpclb_interop_servers/run_fake_dns_server.sh',
      'EXTRA_DOCKER_ARGS': '-e GRPCLB_PORT=%s ' % fake_grpclb_port,
    }
    server_job = jobset.JobSpec(
        cmdline=build_and_run_docker_cmdline,
        environ=build_and_run_docker_environ,
        shortname=shortname,
        timeout_seconds=30 * 60,
        verbose_success=True)
    server_job.container_name = container_name
    return server_job


def build_interop_image_jobspec(language):
    """Creates jobspec for building interop docker image for a language"""
    tag = 'grpc_interop_%s:%s' % (language.safename, uuid.uuid4())
    env = {
        'INTEROP_IMAGE': tag,
        'BASE_NAME': 'grpc_interop_%s' % language.safename,
    }
    build_job = jobset.JobSpec(
        cmdline=['tools/run_tests/dockerize/build_interop_image.sh'],
        environ=env,
        shortname='build_docker_%s' % language.safename,
        timeout_seconds=30 * 60,
        verbose_success=True)
    build_job.tag = tag
    return build_job


argp = argparse.ArgumentParser(description='Run interop tests.')
argp.add_argument(
    '-l',
    '--language',
    choices=['all'] + sorted(_LANGUAGES),
    nargs='+',
    default=['all'],
    help='Clients to run.')
argp.add_argument('-j', '--jobs', default=multiprocessing.cpu_count(), type=int)
argp.add_argument(
    '--transport_security',
    choices=_TRANSPORT_SECURITY_OPTIONS,
    default='insecure',
    type=str,
    nargs='?',
    const=True,
    help='Which transport security mechanism to use.')
argp.add_argument(
    '--image_tag',
    default=None,
    type=str,
    help='This is for iterative manual runs. Setting this skips the docker image build step and runs the client from the named image. Only supports running a one client language.')
argp.add_argument(
    '--save_images',
    default=False,
    type=bool,
    nargs='?',
    const=True,
    help='This is for iterative manual runs. Skip docker image removal.')
args = argp.parse_args()

docker_images = {}

build_jobs = []
if args.language == 'all':
  languages = _LANGUAGES.keys()
else:
  languages = args.language
for lang_name in languages:
    l = _LANGUAGES[lang_name]
    if args.image_tag is None:
        job = build_interop_image_jobspec(l)
        build_jobs.append(job)
        docker_images[str(l.safename)] = job.tag
    else:
        docker_images[str(l.safename)] = args.image_tag

if build_jobs:
    jobset.message(
        'START', 'Building interop docker images.', do_newline=True)
    print('Jobs to run: \n%s\n' % '\n'.join(str(j) for j in build_jobs))
    num_failures, _ = jobset.run(
        build_jobs, newline_on_success=True, maxjobs=args.jobs)
    if num_failures == 0:
        jobset.message(
            'SUCCESS',
            'All docker images built successfully.',
            do_newline=True)
    else:
        jobset.message(
            'FAILED',
            'Failed to build interop docker images.',
            do_newline=True)
        for image in six.itervalues(docker_images):
            dockerjob.remove_image(image, skip_nonexistent=True)
        sys.exit(1)

def wait_until_dns_server_is_up(fake_dns_server_ip):
  for i in range(0, 30):
    print('Health check: attempt to connect to DNS server over TCP.')
    tcp_connect_subprocess = subprocess.Popen([
        os.path.join(os.getcwd(), 'test/cpp/naming/utils/tcp_connect.py'),
        '--server_host', fake_dns_server_ip,
        '--server_port', str(53), 
        '--timeout', str(1)])
    tcp_connect_subprocess.communicate()
    if tcp_connect_subprocess.returncode == 0:
      print(('Health check: attempt to make an A-record '
             'query to DNS server.'))
      dns_resolver_subprocess = subprocess.Popen([
          os.path.join(os.getcwd(), 'test/cpp/naming/utils/dns_resolver.py'),
          '--qname', 'health-check-local-dns-server-is-alive.resolver-tests.grpctestingexp',
          '--server_host', fake_dns_server_ip,
          '--server_port', str(53)],
          stdout=subprocess.PIPE)
      dns_resolver_stdout, _ = dns_resolver_subprocess.communicate()
      if dns_resolver_subprocess.returncode == 0:
        if '123.123.123.123' in dns_resolver_stdout:
          print(('DNS server is up! '
                 'Successfully reached it over UDP and TCP.'))
          return
    time.sleep(0.1)
  raise Exception(('Failed to reach DNS server over TCP and/or UDP. '
                   'Exitting without running tests.'))

server_jobs = {}
server_addresses = {}
suppress_server_logs = True
try:
    # Start fake backend, fallback, and grpclb server
    fake_backend_shortname = 'fake_backend_server'
    fake_backend_spec = backend_or_fallback_server_jobspec(
        args.transport_security, fake_backend_shortname)
    fake_backend_job = dockerjob.DockerJob(fake_backend_spec)
    server_jobs[fake_backend_shortname] = fake_backend_job
    print('sleep 3 seconds - HACK')
    time.sleep(3)
    fake_backend_port = fake_backend_job.mapped_port(_DEFAULT_SERVER_PORT)
    # Start fake fallback server
    fake_fallback_shortname = 'fake_fallback_server'
    fake_fallback_spec = backend_or_fallback_server_jobspec(
        args.transport_security, fake_fallback_shortname)
    fake_fallback_job = dockerjob.DockerJob(fake_fallback_spec)
    server_jobs[fake_fallback_shortname] = fake_fallback_job
    print('sleep 3 seconds - HACK')
    time.sleep(3)
    fake_fallback_port = fake_fallback_job.mapped_port(_DEFAULT_SERVER_PORT)
    # Start fake grpclb server
    fake_grpclb_shortname = 'fake_grpclb_server'
    fake_grpclb_spec = fake_grpclb_jobspec(
        args.transport_security, fake_backend_port, fake_grpclb_shortname)
    fake_grpclb_job = dockerjob.DockerJob(fake_grpclb_spec)
    server_jobs[fake_grpclb_shortname] = fake_grpclb_job
    print('sleep 3 seconds - HACK')
    time.sleep(3)
    fake_grpclb_port = fake_grpclb_job.mapped_port(_DEFAULT_SERVER_PORT)
    # Start fake DNS server
    fake_dns_server_shortname = 'fake_dns_server'
    fake_dns_server_spec = dns_server_in_docker_jobspec(fake_grpclb_port, fake_dns_server_shortname)
    fake_dns_server_job = dockerjob.DockerJob(fake_dns_server_spec)
    server_jobs[fake_dns_server_shortname] = fake_dns_server_job
    # Get the IP address of the docker container running the DNS server
    # (and not the port). The DNS server is running on port 53 of
    # that IP address. We need to use the DNS server's IP address
    # rather than port because we will
    # point the DNS resolvers of grpc clients under test to our
    # controlled DNS server by modifying the /etc/resolve.conf "nameserver"
    # lists of their docker containers, and /etc/resolv.conf
    # "nameserver" lists don't support port number specification
    # (port 53 is always assumed).
    print('sleep 3 seconds - HACK')
    time.sleep(3)
    fake_dns_server_ip = fake_dns_server_job.ip_address()
    wait_until_dns_server_is_up(fake_dns_server_ip)
    # Run clients
    jobs = []
    for lang_name in _LANGUAGES.keys():
        test_job = lb_client_interop_jobspec(
            _LANGUAGES[lang_name],
            fake_dns_server_ip,
            docker_image=docker_images.get(l.safename),
            transport_security=args.transport_security)
        jobs.append(test_job)
    print('Jobs to run: \n%s\n' % '\n'.join(str(job) for job in jobs))
    num_failures, resultset = jobset.run(
        jobs,
        newline_on_success=True,
        maxjobs=args.jobs)
    if num_failures:
        suppress_server_logs = False
        jobset.message('FAILED', 'Some tests failed', do_newline=True)
    else:
        jobset.message('SUCCESS', 'All tests passed', do_newline=True)

    if num_failures:
        sys.exit(1)
    else:
        sys.exit(0)
except Exception as e:
    print('exception occurred:')
    traceback.print_exc(file=sys.stdout)
    suppress_server_logs = False
    raise e
finally:
    # Check if servers are still running.
    for server, job in server_jobs.items():
        if not job.is_running():
            print('Server "%s" has exited prematurely.' % server)
    dockerjob.finish_jobs([j for j in six.itervalues(server_jobs)], suppress_failure=suppress_server_logs)
    if not args.save_images and args.image_tag is None:
        for image in six.itervalues(docker_images):
            print('Removing docker image %s' % image)
            dockerjob.remove_image(image)
