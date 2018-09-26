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
_CLIENT_WITH_LOCAL_DNS_SERVER_RUNNER = os.path.join(
    _DNS_SCRIPTS, 'run_client_with_dns_server.py')

_GO_REPO_ROOT = os.path.join(os.sep, 'go', 'src', 'google.golang.org', 'grpc')

_TEST_TIMEOUT = 60


class CXXLanguage:

    def __init__(self):
        self.client_cwd = '/var/local/git/grpc'
        self.safename = 'cxx'

    def client_cmd(self, args):
        return ['bins/opt/interop_client'] + args

    def global_env(self):
        return {
            'GRPC_DNS_RESOLVER': 'ares',
            'GRPC_VERBOSITY': 'DEBUG',
            'GRPC_TRACE': 'client_channel,glb'
        }

    def __str__(self):
        return 'c++'


class JavaLanguage:

    def __init__(self):
        self.client_cwd = '/var/local/git/grpc-java'
        self.safename = str(self)

    def client_cmd(self, args):
        return ['./run-test-client.sh'] + args

    def global_env(self):
        return {
            'JAVA_OPTS':
            ('-Dio.grpc.internal.DnsNameResolverProvider.enable_grpclb=true '
             '-Djava.util.logging.config.file=/var/local/grpc_java_logging/logconf.txt'
            )
        }

    def __str__(self):
        return 'java'


class GoLanguage:

    def __init__(self):
        self.client_cwd = '/go/src/google.golang.org/grpc/interop/client'
        self.safename = str(self)

    def client_cmd(self, args):
        return ['go', 'run', 'client.go'] + args

    def global_env(self):
        return {
            'GRPC_GO_LOG_VERBOSITY_LEVEL': '3',
            'GRPC_GO_LOG_SEVERITY_LEVEL': 'INFO'
        }

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
    elif transport_security == 'google_default_credentials':
        args += ['--custom_credentials_type=google_default_credentials']
    else:
        print('Invalid transport security option.')
        sys.exit(1)
    return args


def lb_client_interop_jobspec(language,
                              dns_server_ip,
                              docker_image,
                              transport_security='tls'):
    """Creates jobspec for cloud-to-cloud interop test"""
    interop_only_options = [
        '--server_host=server.test.google.fr',
    #    '--server_host_override=""',
        '--server_port=%d' % _DEFAULT_SERVER_PORT,
        '--use_test_ca=true',
    ] + transport_security_to_args(transport_security)
    client_args = language.client_cmd(interop_only_options)
    environ = language.global_env()
    environ['BUILD_AND_RUN_DOCKER_QUIET'] = 'true'
    container_name = dockerjob.random_name(
        'lb_interop_client_%s' % language.safename)
    docker_cmdline = docker_run_cmdline(
        client_args,
        image=docker_image,
        environ=environ,
        cwd=language.client_cwd,
        docker_args=[
            '--dns=%s' % dns_server_ip, '--net=host',
            '--name=%s' % container_name
        ])
    jobset.message(
        'IDLE',
        'docker_cmdline:\b|%s|' % ' '.join(docker_cmdline),
        do_newline=True)
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
        server_run_script=
        'tools/dockerfile/grpclb_interop_servers/run_backend_or_fallback_server.sh',
        backend_addrs=None,
        transport_security=transport_security,
        shortname=shortname)


def grpclb_jobspec(transport_security, backend_addrs, shortname):
    """Create jobspec for running a server"""
    return grpc_server_in_docker_jobspec(
        server_run_script=
        'tools/dockerfile/grpclb_interop_servers/run_grpclb_server.sh',
        backend_addrs=backend_addrs,
        transport_security=transport_security,
        shortname=shortname)


def grpc_server_in_docker_jobspec(server_run_script, backend_addrs,
                                  transport_security, shortname):
    container_name = dockerjob.random_name(shortname)
    build_and_run_docker_cmdline = (
        'bash -l tools/run_tests/dockerize/build_and_run_docker.sh').split()
    docker_extra_args = '-e GRPC_GO_LOG_VERBOSITY_LEVEL=3 '
    docker_extra_args += '-e GRPC_GO_LOG_SEVERITY_LEVEL=INFO '
    docker_extra_args += '-e PORT=%s ' % _DEFAULT_SERVER_PORT
    if backend_addrs is not None:
        docker_extra_args += '-e BACKEND_ADDRS=%s ' % ','.join(backend_addrs)
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
        timeout_seconds=30 * 60)
    server_job.container_name = container_name
    return server_job


def dns_server_in_docker_jobspec(grpclb_ips, fallback_ips, shortname):
    container_name = dockerjob.random_name(shortname)
    build_and_run_docker_cmdline = (
        'bash -l tools/run_tests/dockerize/build_and_run_docker.sh').split()
    extra_docker_args = '-e GRPCLB_IPS=%s ' % ','.join(grpclb_ips)
    extra_docker_args += '-e FALLBACK_IPS=%s ' % ','.join(fallback_ips)
    build_and_run_docker_environ = {
        'CONTAINER_NAME':
        container_name,
        'DOCKERFILE_DIR':
        'tools/dockerfile/grpclb_interop_servers',
        'DOCKER_RUN_SCRIPT':
        'tools/dockerfile/grpclb_interop_servers/run_dns_server.sh',
        'EXTRA_DOCKER_ARGS':
        extra_docker_args,
    }
    server_job = jobset.JobSpec(
        cmdline=build_and_run_docker_cmdline,
        environ=build_and_run_docker_environ,
        shortname=shortname,
        timeout_seconds=30 * 60)
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
    '-s',
    '--scenarios_file',
    default=None,
    type=str,
    help='File containing test scenarios as JSON configs.')
argp.add_argument(
    '-n',
    '--scenario_name',
    default=None,
    type=str,
    help=(
        'Useful for manual runs: specify the name of '
        'the scenario to run from scenarios_file. Run all scenarios if unset.'))
argp.add_argument(
    '--image_tag',
    default=None,
    type=str,
    help=
    'Setting this skips the clients docker image build step and runs the client from the named image. Only supports running a one client language.'
)
argp.add_argument(
    '--save_images',
    default=False,
    type=bool,
    nargs='?',
    const=True,
    help='Skip docker image removal.')
args = argp.parse_args()

docker_images = {}

build_jobs = []
if len(args.language) and args.language[0] == 'all':
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
    jobset.message('START', 'Building interop docker images.', do_newline=True)
    print('Jobs to run: \n%s\n' % '\n'.join(str(j) for j in build_jobs))
    num_failures, _ = jobset.run(
        build_jobs, newline_on_success=True, maxjobs=args.jobs)
    if num_failures == 0:
        jobset.message(
            'SUCCESS', 'All docker images built successfully.', do_newline=True)
    else:
        jobset.message(
            'FAILED', 'Failed to build interop docker images.', do_newline=True)
        for image in six.itervalues(docker_images):
            dockerjob.remove_image(image, skip_nonexistent=True)
        sys.exit(1)


def wait_until_dns_server_is_up(dns_server_ip):
    for i in range(0, 30):
        print('Health check: attempt to connect to DNS server over TCP.')
        tcp_connect_subprocess = subprocess.Popen([
            os.path.join(os.getcwd(), 'test/cpp/naming/utils/tcp_connect.py'),
            '--server_host', dns_server_ip, '--server_port',
            str(53), '--timeout',
            str(1)
        ])
        tcp_connect_subprocess.communicate()
        if tcp_connect_subprocess.returncode == 0:
            print(('Health check: attempt to make an A-record '
                   'query to DNS server.'))
            dns_resolver_subprocess = subprocess.Popen(
                [
                    os.path.join(
                        os.getcwd(),
                        'test/cpp/naming/utils/dns_resolver.py'), '--qname',
                    'health-check-local-dns-server-is-alive.resolver-tests.grpctestingexp',
                    '--server_host', dns_server_ip, '--server_port',
                    str(53)
                ],
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


def shortname(shortname_prefix, shortname, index):
    return '%s_%s_%d' % (shortname_prefix, shortname, index)


def run_one_scenario(scenario_config):
    jobset.message('START', 'Run scenario: %s' % scenario_config['name'])
    server_jobs = {}
    server_addresses = {}
    suppress_server_logs = True
    # TODO: change logic to go by the scenario config
    try:
        backend_addrs = []
        fallback_ips = []
        grpclb_ips = []
        shortname_prefix = scenario_config['name']
        # Start backends
        for i in xrange(len(scenario_config['backend_configs'])):
            backend_config = scenario_config['backend_configs'][i]
            backend_shortname = shortname(shortname_prefix, 'backend_server', i)
            backend_spec = backend_or_fallback_server_jobspec(
                backend_config['transport_sec'], backend_shortname)
            backend_job = dockerjob.DockerJob(backend_spec)
            server_jobs[backend_shortname] = backend_job
            backend_addrs.append('%s:%d' % (backend_job.ip_address(),
                                            _DEFAULT_SERVER_PORT))
        # Start fallbacks
        for i in xrange(len(scenario_config['fallback_configs'])):
            fallback_config = scenario_config['fallback_configs'][i]
            fallback_shortname = shortname(shortname_prefix, 'fallback_server',
                                           i)
            fallback_spec = backend_or_fallback_server_jobspec(
                fallback_config['transport_sec'], fallback_shortname)
            fallback_job = dockerjob.DockerJob(fallback_spec)
            server_jobs[fallback_shortname] = fallback_job
            fallback_ips.append(fallback_job.ip_address())
        # Start balancers
        for i in xrange(len(scenario_config['balancer_configs'])):
            balancer_config = scenario_config['balancer_configs'][i]
            grpclb_shortname = shortname(shortname_prefix, 'grpclb_server', i)
            grpclb_spec = grpclb_jobspec(balancer_config['transport_sec'],
                                         backend_addrs, grpclb_shortname)
            grpclb_job = dockerjob.DockerJob(grpclb_spec)
            server_jobs[grpclb_shortname] = grpclb_job
            grpclb_ips.append(grpclb_job.ip_address())
        # Start DNS server
        dns_server_shortname = shortname(shortname_prefix, 'dns_server', 0)
        dns_server_spec = dns_server_in_docker_jobspec(grpclb_ips, fallback_ips,
                                                       dns_server_shortname)
        dns_server_job = dockerjob.DockerJob(dns_server_spec)
        server_jobs[dns_server_shortname] = dns_server_job
        # Get the IP address of the docker container running the DNS server.
        # The DNS server is running on port 53 of that IP address. Note we will
        # point the DNS resolvers of grpc clients under test to our controlled
        # DNS server by effectively modifying the /etc/resolve.conf "nameserver"
        # lists of their docker containers.
        dns_server_ip = dns_server_job.ip_address()
        wait_until_dns_server_is_up(dns_server_ip)
        # Run clients
        jobs = []
        for lang_name in languages:
            lang = _LANGUAGES[lang_name]
            test_job = lb_client_interop_jobspec(
                lang,
                dns_server_ip,
                docker_image=docker_images.get(lang.safename),
                transport_security=scenario_config['transport_sec'])
            jobs.append(test_job)
        jobset.message('IDLE', 'Jobs to run: \n%s\n' % '\n'.join(
            str(job) for job in jobs))
        num_failures, resultset = jobset.run(
            jobs, newline_on_success=True, maxjobs=args.jobs)
        report_utils.render_junit_xml_report(resultset, 'sponge_log.xml')
        if num_failures:
            suppress_server_logs = False
            jobset.message(
                'FAILED',
                'Scenario: %s. Some tests failed' % scenario_config['name'],
                do_newline=True)
        else:
            jobset.message(
                'SUCCESS',
                'Scenario: %s. All tests passed' % scenario_config['name'],
                do_newline=True)
        return num_failures
    finally:
        # Check if servers are still running.
        for server, job in server_jobs.items():
            if not job.is_running():
                print('Server "%s" has exited prematurely.' % server)
        dockerjob.finish_jobs(
            [j for j in six.itervalues(server_jobs)],
            suppress_failure=suppress_server_logs)


try:
    num_failures = 0
    with open(args.scenarios_file, 'r') as scenarios_input:
        all_scenarios = json.loads(scenarios_input.read())
        for scenario in all_scenarios:
            if args.scenario_name:
                if args.scenario_name != scenario['name']:
                    jobset.message('IDLE',
                                   'Skipping scenario: %s' % scenario['name'])
                    continue
            num_failures += run_one_scenario(scenario)
    if num_failures == 0:
        sys.exit(0)
    else:
        sys.exit(1)
finally:
    if not args.save_images and args.image_tag is None:
        for image in six.itervalues(docker_images):
            print('Removing docker image %s' % image)
            dockerjob.remove_image(image)
