import json
import os

all_scenarios = []


def generate_client_referred_to_backend():
    all_configs = []
    for transport_sec in ['insecure', 'alts', 'tls']:
        config = {
            'name': 'client_referred_to_backend_%s' % transport_sec,
            'transport_sec': transport_sec,
            'balancer_configs': [{
                'transport_sec': transport_sec,
            }],
            'backend_configs': [{
                'transport_sec': transport_sec,
            }],
            'fallback_configs': [{
                'transport_sec': transport_sec,
            }],
        }
        all_configs.append(config)
    return all_configs


all_scenarios += generate_client_referred_to_backend()


def generate_client_referred_to_backend_multiple_backends():
    all_configs = []
    for transport_sec in ['insecure', 'alts', 'tls']:
        config = {
            'name':
            'client_referred_to_backend_multiple_backends_%s' % transport_sec,
            'transport_sec':
            transport_sec,
            'balancer_configs': [{
                'transport_sec': transport_sec,
            }],
            'backend_configs': [{
                'transport_sec': transport_sec,
            }, {
                'transport_sec': transport_sec,
            }, {
                'transport_sec': transport_sec,
            }, {
                'transport_sec': transport_sec,
            }, {
                'transport_sec': transport_sec,
            }],
            'fallback_configs': [{
                'transport_sec': transport_sec,
            }],
        }
        all_configs.append(config)
    return all_configs


all_scenarios += generate_client_referred_to_backend_multiple_backends()


def generate_client_falls_back_because_no_backends():
    all_configs = []
    for transport_sec in ['insecure', 'alts', 'tls']:
        config = {
            'name': 'client_falls_back_because_no_backends_%s' % transport_sec,
            'transport_sec': transport_sec,
            'balancer_configs': [{
                'transport_sec': transport_sec,
            }],
            'backend_configs': [],
            'fallback_configs': [{
                'transport_sec': transport_sec,
            }],
        }
        all_configs.append(config)
    return all_configs


all_scenarios += generate_client_falls_back_because_no_backends()


def generate_client_falls_back_because_balancer_connection_broken():
    all_configs = []
    for transport_sec in ['alts', 'tls']:
        config = {
            'name':
            'client_falls_back_because_balancer_connection_broken_%s' %
            transport_sec,
            'transport_sec':
            transport_sec,
            'balancer_configs': [{
                'transport_sec': 'insecure',
            }],
            'backend_configs': [],
            'fallback_configs': [{
                'transport_sec': transport_sec,
            }],
        }
        all_configs.append(config)
    return all_configs


all_scenarios += generate_client_falls_back_because_balancer_connection_broken()


def generate_client_referred_to_backend_multiple_balancers():
    all_configs = []
    for transport_sec in ['insecure', 'alts', 'tls']:
        config = {
            'name':
            'client_referred_to_backend_multiple_balancers_%s' % transport_sec,
            'transport_sec':
            transport_sec,
            'balancer_configs': [
                {
                    'transport_sec': transport_sec,
                },
                {
                    'transport_sec': transport_sec,
                },
                {
                    'transport_sec': transport_sec,
                },
                {
                    'transport_sec': transport_sec,
                },
                {
                    'transport_sec': transport_sec,
                },
            ],
            'backend_configs': [],
            'fallback_configs': [
                {
                    'transport_sec': transport_sec,
                },
            ],
        }
        all_configs.append(config)
    return all_configs


all_scenarios += generate_client_referred_to_backend_multiple_balancers()


def generate_client_referred_to_backend_multiple_balancers_one_works():
    all_configs = []
    for transport_sec in ['alts', 'tls']:
        config = {
            'name':
            'client_referred_to_backend_multiple_balancers_one_works_%s' %
            transport_sec,
            'transport_sec':
            transport_sec,
            'balancer_configs': [{
                'transport_sec': transport_sec,
            }, {
                'transport_sec': 'insecure',
            }, {
                'transport_sec': 'insecure',
            }, {
                'transport_sec': 'insecure',
            }, {
                'transport_sec': 'insecure',
            }],
            'backend_configs': [],
            'fallback_configs': [{
                'transport_sec': transport_sec,
            }],
        }
        all_configs.append(config)
    return all_configs


all_scenarios += generate_client_referred_to_backend_multiple_balancers_one_works(
)

with open(
        os.path.join(
            os.path.dirname(__file__), 'grpclb_interop_scenarios.json'),
        'w') as output:
    output.write(json.dumps(all_scenarios, indent=4, sort_keys=True))
