set -ex
export CONFIG=dbg
export GRPC_VERBOSITY=DEBUG
make interop_client interop_server -j2
bins/dbg/interop_server --port=10443 --use_tls=true &
INTEROP_SERVER_PID=$!
export GRPC_DNS_RESOLVER=ares
python test/cpp/naming/test_dns_server.py --port=15353 --records_config_path=test/cpp/naming/resolver_test_record_groups.yaml &

FLAGS_test_dns_server_port=15353
# Health check local DNS server TCP and UDP ports
for ((i=0;i<30;i++));
do
  echo "Retry health-check DNS query to local DNS server over tcp and udp"
  RETRY=0
  dig A health-check-local-dns-server-is-alive.resolver-tests.grpctestingexp. @localhost -p "$FLAGS_test_dns_server_port" +tries=1 +timeout=1 | grep '123.123.123.123' || RETRY=1
  dig A health-check-local-dns-server-is-alive.resolver-tests.grpctestingexp. @localhost -p "$FLAGS_test_dns_server_port" +tries=1 +timeout=1 +tcp | grep '123.123.123.123' || RETRY=1
  if [[ "$RETRY" == 0 ]]; then
    break
  fi;
  sleep 0.1
done
if [[ "$RETRY" == 1 ]]; then
  echo "Failed to start DNS server" && exit 1
fi

echo "Just started DNS server"
DNS_SERVER_PID=$!
DNS_SERVER=127.0.0.1:15353
SERVER_NAME=srv-ipv4-target-has-backend-and-balancer.resolver-tests-version-4.grpctestingexp
CLIENT_TARGET="dns://$DNS_SERVER/$SERVER_NAME"
FAILED=0
bins/dbg/interop_client --server_host=$CLIENT_TARGET --server_port=10443  --use_tls=true --use_test_ca=true --test_case=empty_unary || FAILED=1
echo "Failed? $FAILED"
kill -SIGINT $DNS_SERVER_PID
kill -SIGINT $INTEROP_SERVER_PID
wait
