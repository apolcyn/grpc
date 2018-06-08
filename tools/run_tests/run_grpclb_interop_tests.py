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


class CXXLanguage:

    def __init__(self):
        self.client_cwd = None
        self.server_cwd = None
        self.http2_cwd = None
        self.safename = 'cxx'

    def client_cmd(self, args):
        return ['bins/opt/interop_client'] + args

    def client_cmd_http2interop(self, args):
        return ['bins/opt/http2_client'] + args

    def cloud_to_prod_env(self):
        return {}

    def server_cmd(self, args):
        return ['bins/opt/interop_server'] + args

    def global_env(self):
        return {}

    def unimplemented_test_cases(self):
        return _SKIP_DATA_FRAME_PADDING

    def unimplemented_test_cases_server(self):
        return []

    def __str__(self):
        return 'c++'


class JavaLanguage:

    def __init__(self):
        self.client_cwd = '../grpc-java'
        self.server_cwd = '../grpc-java'
        self.http2_cwd = '../grpc-java'
        self.safename = str(self)

    def client_cmd(self, args):
        return ['./run-test-client.sh'] + args

    def client_cmd_http2interop(self, args):
        return [
            './interop-testing/build/install/grpc-interop-testing/bin/http2-client'
        ] + args

    def cloud_to_prod_env(self):
        return {}

    def server_cmd(self, args):
        return ['./run-test-server.sh'] + args

    def global_env(self):
        return {}

    def unimplemented_test_cases(self):
        return []

    def unimplemented_test_cases_server(self):
        return _SKIP_COMPRESSION

    def __str__(self):
        return 'java'


class GoLanguage:

    def __init__(self):
        # TODO: this relies on running inside docker
        self.client_cwd = '/go/src/google.golang.org/grpc/interop/client'
        self.server_cwd = '/go/src/google.golang.org/grpc/interop/server'
        self.http2_cwd = '/go/src/google.golang.org/grpc/interop/http2'
        self.safename = str(self)

    def client_cmd(self, args):
        return ['go', 'run', 'client.go'] + args

    def client_cmd_http2interop(self, args):
        return ['go', 'run', 'negative_http2_client.go'] + args

    def cloud_to_prod_env(self):
        return {}

    def server_cmd(self, args):
        return ['go', 'run', 'server.go'] + args

    def global_env(self):
        return {}

    def unimplemented_test_cases(self):
        return _SKIP_COMPRESSION

    def unimplemented_test_cases_server(self):
        return _SKIP_COMPRESSION

    def __str__(self):
        return 'go'


_LANGUAGES = {
    'c++': CXXLanguage(),
    'go': GoLanguage(),
    'java': JavaLanguage(),
}

# languages supported as grpclb backend servers
_SERVERS = [
    'c++', 'java', 'go',
]

_TEST_CASES = [
    'empty_unary',
]

#TODO: Add c++ when c++ ALTS interop client is ready.
_LANGUAGES_FOR_ALTS_TEST_CASES = ['c++', 'java', 'go']

#TODO: Add c++ when c++ ALTS interop server is ready.
_SERVERS_FOR_ALTS_TEST_CASES = ['c++', 'java', 'go']

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


def lb_client_server_jobspec(language,
                           test_case,
                           fake_backend_port,
                           fake_fallback_port,
                           fake_grpclb_port,
                           docker_image,
                           transport_security='tls'):
    """Creates jobspec for cloud-to-cloud interop test"""
    interop_only_options = [
        '--server_host_override=foo.test.google.fr',
        '--use_test_ca=true',
    ]
    if transport_security == 'tls':
        interop_only_options += ['--use_tls=true']
    elif transport_security == 'alts':
        interop_only_options += ['--use_tls=false', '--use_alts=true']
    elif transport_security == 'insecure':
        interop_only_options += ['--use_tls=false']
    else:
        print('Invalid transport security option.')
        sys.exit(1)
    client_test_case = test_case
    cmdline = bash_cmdline(
        language.client_cmd([
            '--test_case=%s' % client_test_case,
            '--server_host=%s' % server_host,
            '--server_port=%s' % server_port
        ])
    )
    cwd = language.client_cwd
    environ = language.global_env()
    container_name = dockerjob.random_name(
        'lb_interop_client_%s' % language.safename)
    cmdline = docker_run_cmdline(
        cmdline,
        image=docker_image,
        environ=environ,
        cwd=cwd,
        docker_args=['--net=host',
                     '--name=%s' % container_name])
    cwd = None
    test_job = jobset.JobSpec(
        cmdline=cmdline,
        cwd=cwd,
        environ=environ,
        shortname='cloud_to_cloud:%s:%s_server:%s' % (language, server_name,
                                                      test_case),
        timeout_seconds=_TEST_TIMEOUT,
        kill_handler=_job_kill_handler)
    test_job.container_name = container_name
    return test_job


def server_jobspec(server_binary,
                   docker_image,
                   transport_security='tls'):
    """Create jobspec for running a server"""
    server_cmd_args = ['--port=%s' % _DEFAULT_SERVER_PORT]
    if transport_security == 'tls':
        server_cmd_args += ['--use_tls=true']
    elif transport_security == 'alts':
        server_cmd_args += ['--use_tls=false', '--use_alts=true']
    elif transport_security == 'insecure':
        server_cmd_args += ['--use_tls=false']
    else:
        print('Invalid transport security option.')
        sys.exit(1)
    cmdline = bash_cmdline(server_binary + server_cmd_args)
    container_name = dockerjob.random_name(
        'lb_interop_server_%s' % server_binary)
    environ = language.global_env()
    docker_args = ['--name=%s' % container_name]
    docker_args += ['-p', str(_DEFAULT_SERVER_PORT)]
    docker_cmdline = docker_run_cmdline(
        cmdline,
        image=docker_image,
        cwd=language.server_cwd,
        environ=environ,
        docker_args=docker_args)
    server_job = jobset.JobSpec(
        cmdline=docker_cmdline,
        environ=environ,
        shortname='lb_backend_interop_server_%s' % language,
        timeout_seconds=30 * 60)
    server_job.container_name = container_name
    return server_job


def build_interop_image_jobspec(language):
    """Creates jobspec for building interop docker image for a language"""
    tag = 'grpc_interop_%s:%s' % (language.safename, uuid.uuid4())
    env = {
        'INTEROP_IMAGE': tag,
        'BASE_NAME': 'grpc_lb_interop_%s' % language.safename
    }
    build_job = jobset.JobSpec(
        cmdline=['tools/run_tests/dockerize/build_interop_image.sh'],
        environ=env,
        shortname='build_docker_%s' % (language),
        timeout_seconds=30 * 60)
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
    default='tls',
    type=str,
    nargs='?',
    const=True,
    help='Which transport security mechanism to use.')
args = argp.parse_args()

languages = _LANGUAGES.keys()

docker_images = {}

build_jobs = []
for l in languages:
    job = build_interop_image_jobspec(l)
    docker_images[str(l)] = job.tag
    build_jobs.append(job)

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

server_jobs = {}
server_addresses = {}
try:
    # Start fake backend 
    fake_backend_spec = server_jobspec(
        os.path.join(_GO_REPO_ROOT, 'interop', 'server', 'server')
        fake_backend_docker_image,
        args.transport_security)
    fake_backend_job = dockerjob.DockerJob(fake_backend_spec)
    fake_backend_port = fake_backend_job.mapped_port(_DEFAULT_SERVER_PORT))
    # Start fake fallback server
    fake_fallback_spec = server_jobspec(
        os.path.join(_GO_REPO_ROOT, 'interop', 'server', 'server')
        fake_fallback_docker_image,
        args.transport_security)
    fake_fallback_job = dockerjob.DockerJob(fake_fallback_spec)
    fake_fallback_port = fake_fallback_job.mapped_port(_DEFAULT_SERVER_PORT))
    # Start fake grpclb server
    fake_grpclb_spec = server_jobspec(
        os.path.join(_FAKE_GRPCLB_REPO_ROOT, 'fake_grpclb')
        fake_grpclb_docker_image,
        args.transport_security)
    fake_grpclb_job = dockerjob.DockerJob(fake_grpclb_spec)
    fake_grpclb_port = fake_grpclb_job.mapped_port(_DEFAULT_SERVER_PORT))
    # Run clients, with local DNS servers running in their docker containers
    jobs = []
    for language in languages:
        test_job = lb_client_interop_jobspec(
            language,
            fake_backend_port,
            fake_fallback_port,
            fake_grpclb_port,
            docker_image=docker_images.get(str(language)),
            transport_security=args.transport_security)
        jobs.append(test_job)
    print('Jobs to run: \n%s\n' % '\n'.join(str(job) for job in jobs))
    num_failures, resultset = jobset.run(
        jobs,
        newline_on_success=True,
        maxjobs=args.jobs)
    if num_failures:
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
finally:
    # Check if servers are still running.
    for server, job in server_jobs.items():
        if not job.is_running():
            print('Server "%s" has exited prematurely.' % server)
    dockerjob.finish_jobs([j for j in six.itervalues(server_jobs)])
    for image in six.itervalues(docker_images):
        print('Removing docker image %s' % image)
        dockerjob.remove_image(image)
