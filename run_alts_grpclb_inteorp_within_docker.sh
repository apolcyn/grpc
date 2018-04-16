set -eux
readonly BACKEND_PORT=15353
readonly FALLBACK_PORT=2000
readonly GRPCLB_PORT=15354
readonly DNS_SERVER_PORT=15355
readonly GRPCLB_LOG="$(mktemp)"
readonly FALLBACK_LOG="$(mktemp)"
readonly BACKEND_LOG="$(mktemp)"
readonly DNS_SERVER_LOG="$(mktemp)"
readonly RECORDS_CONFIG="$(mktemp)"
readonly CXX_TEST_CONFIG="${CXX_TEST_CONFIG:-opt}"

cd /var/local/git/grpc

# Create DNS records that allows the local DNS server
# to resolve the grpclb balancer and "fallback" server.
echo "resolver_tests_common_zone_name: test.com.
resolver_component_tests:
- records:
    _grpclb._tcp.server:
    - {TTL: '2100', data: 0 0 15354 grpclb-balancer, type: SRV}
    grpclb-balancer:
    - {TTL: '2100', data: 127.0.0.1, type: A}
    server:
    - {TTL: '2100', data: 127.0.0.1, type: A}" > "$RECORDS_CONFIG"
# Start up all of the "fake" local servers.
export GRPC_GO_VERBOSITY_LEVEL=3
export GRPC_GO_SEVERITY_LEVEL=INFO
export GOPATH=/go
/go/src/google.golang.org/grpc/interop/server/server \
  --port="$FALLBACK_PORT" \
  --use_tls=true > "$FALLBACK_LOG" 2>&1 &
FALLBACK_PID="$!"
/go/src/google.golang.org/grpc/interop/server/server \
  --port="$BACKEND_PORT" \
  --use_alts=true > "$BACKEND_LOG" 2>&1 &
BACKEND_PID="$!"
/go/src/google.golang.org/fake_grpclb/fake_grpclb \
  --port="$GRPCLB_PORT" \
  --backend_port="$BACKEND_PORT" > "$GRPCLB_LOG" 2>&1 &
GRPCLB_PID="$!"
python test/cpp/naming/utils/dns_server.py \
  -p "$DNS_SERVER_PORT" \
  -r "$RECORDS_CONFIG" > "$DNS_SERVER_LOG" 2>&1 &
DNS_SERVER_PID="$!"

function display_log() {
  local LOG="$1"
  local NAME="$2"
  echo "======= $2 merged stderr and stdout ======="
  cat "$LOG"
  echo "======= END $2 log ======="
}

function kill_all_servers() {
  kill "$DNS_SERVER_PID" || true
  kill "$FALLBACK_PID" || true
  kill "$GRPCLB_PID" || true
  kill "$BACKEND_PID" || true
  wait
}

function display_all_logs_and_exit() {
  set +x
  display_log "$BACKEND_LOG" "backend"
  display_log "$FALLBACK_LOG" "fallback"
  display_log "$GRPCLB_LOG" "grpclb"
  display_log "$DNS_SERVER_LOG" "DNS server"
  kill_all_servers
  exit 1
}

function tcp_health_check() {
  local PORT="$1"
  local MAX_RETRIES=4
  local ATTEMPT=0
  while [[ "$ATTEMPT" -lt "$MAX_RETRIES" ]]; do
    python test/cpp/naming/utils/tcp_connect.py \
      --server_host=localhost \
      --server_port="$PORT" \
      --timeout=1 && return
    ATTEMPT="$((ATTEMPT+1))"
    sleep 1
  done
  echo "Could not connect to server on port "$PORT"."
  display_all_logs_and_exit
}

tcp_health_check "$DNS_SERVER_PORT" # Check the DNS server's TCP port
tcp_health_check "$BACKEND_PORT"
tcp_health_check "$FALLBACK_PORT"
tcp_health_check "$GRPCLB_PORT"

function dns_health_check() {
  local MAX_RETRIES=20
  local ATTEMPT=0
  while [[ "$ATTEMPT" -lt "$MAX_RETRIES" ]]; do
    python test/cpp/naming/utils/dns_resolver.py \
      -s 127.0.0.1 \
      -p "$DNS_SERVER_PORT" \
      -n health-check-local-dns-server-is-alive.resolver-tests.grpctestingexp. \
      -t 1 && return
    ATTEMPT="$((ATTEMPT+1))"
    sleep 1
  done
  echo "Could not resolve records from DNS server. It's likely not runnning."
  display_all_logs_and_exit
}

dns_health_check

export GRPC_VERBOSITY=DEBUG
export GRPC_DNS_RESOLVER=ares # TODO: unset when c-ares is default DNS resolver.
ONE_FAILED=0
# A successful grpclb interaction should be fast, so give a short timeout.
timeout 2 "bins/${CXX_TEST_CONFIG}/interop_client" \
  --server_host="dns://127.0.0.1:${DNS_SERVER_PORT}/server.test.com" \
  --server_port="$FALLBACK_PORT" --use_alts=true || ONE_FAILED=1
[[ "$?" != 0 ]] && \
  echo "FAILED: ALTS client -> successful grpclb interaction -> backend server."
# When the client is not able to get backends from the balancer
# (in this case because of attempt to use SSL to connect to an ALTS balancer),
# the client if forced to wait for the "fallback timeout" to go off before
# attempting to connect to the "fallback servers", so give this
# test a long timeout (at this time of writing, the default
# "fallback timeout" is 10 seconds, so 30 seconds should be plenty of time).
timeout 30 "bins/${CXX_TEST_CONFIG}/interop_client" \
  --server_host="dns://127.0.0.1:${DNS_SERVER_PORT}/server.test.com" \
  --server_port="$FALLBACK_PORT" \
  --use_tls=true \
  --use_test_ca=true || ONE_FAILED=1
[[ "$?" != 0 ]] && \
  echo "FAILED: SSL client -> failed grpclb interaction -> fallback server."
if [[ "$ONE_FAILED" != 0 ]]; then
  echo "At least one test failed."
  display_all_logs_and_exit
fi
kill_all_servers
