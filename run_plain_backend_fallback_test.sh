set -ex
export CONFIG=dbg
make interop_client interop_server -j2
bins/dbg/interop_server --port=10443 --use_tls=true &
export GRPC_DNS_RESOLVER=ares
test/cpp/naming/test_dns_server.py --port=15353 --records_config_path=test/cpp/naming/resolver_test_record_groups.yaml || echo "DID NOT START DNS SERVER"
DNS_SERVER=127.0.0.1:15353
SERVER_NAME=srv-ipv4-target-has-backend-and-balancer.resolver-tests-version-4.grpctestingexp
CLIENT_TARGET="dns://$DNS_SERVER/$SERVER_NAME"
bins/bdg/interop_client --server_host=$SERVER_NAME --server_port=10443  --use_tls=true --use_test_ca=true --test_case=empty_unary

