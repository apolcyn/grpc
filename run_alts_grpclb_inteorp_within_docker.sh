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

# Create a config that allows the local DNS server
# to resolve:
# 1) _grpclb._tcp.server.test.com
#    > 'SRV' record of '0 0 $GRPCLB_PORT lb-target.test.com'
# 2) server.test.com
#    > 'A' record of '127.0.0.1' (for the fallback server)
# 3) lb-target.test.com (the grpclb sever)
#    > 'A' record of 127.0.0.1 (for the grpclb server)
echo "resolver_tests_common_zone_name: test.com.
resolver_component_tests:
- records:
    _grpclb._tcp.server:
    - {TTL: '2100', data: 0 0 15354 lb-target, type: SRV}
    lb-target:
    - {TTL: '2100', data: 127.0.0.1, type: A}
    server:
    - {TTL: '2100', data: 127.0.0.1, type: A}" > "$RECORDS_CONFIG"
# Start up all of the "fake" local servers.
export GRPC_GO_VERBOSITY_LEVEL=3
export GRPC_GO_SEVERITY_LEVEL=INFO
export GOPATH=/
/src/google.golang.org/grpc/interop/server/server --port="$FALLBACK_PORT" --use_tls=true > "$FALLBACK_LOG" 2>&1 &
FALLBACK_PID="$!"
/src/google.golang.org/grpc/interop/server/server --port="$BACKEND_PORT" --use_alts=true > "$BACKEND_LOG" 2>&1 &
BACKEND_PID="$!"
/src/google.golang.org/fake_grpclb/fake_grpclb --port="$GRPCLB_PORT" --backend_port="$BACKEND_PORT" > "$GRPCLB_LOG" 2>&1 &
GRPCLB_PID="$!"
python test/cpp/naming/utils/dns_server.py -p "$DNS_SERVER_PORT" -r "$RECORDS_CONFIG" > "$DNS_SERVER_LOG" 2>&1 &
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

function tcp_health_check_server() {
  local PORT="$1"
  local MAX_RETRIES=4
  local ATTEMPT=0
  while [[ "$ATTEMPT" -lt "$MAX_RETRIES" ]]; do
    python test/cpp/naming/utils/tcp_connect.py --server_host=localhost --server_port="$PORT" --timeout=1 && return
    ATTEMPT="$((ATTEMPT+1))"
    sleep 1
  done
  echo "Could not connect to server on port "$PORT"."
  display_all_logs_and_exit
}

tcp_health_check_server "$DNS_SERVER_PORT" # Check the DNS server's TCP port
tcp_health_check_server "$BACKEND_PORT"
tcp_health_check_server "$FALLBACK_PORT"
tcp_health_check_server "$GRPCLB_PORT"

function dns_health_check() {
  local MAX_RETRIES=30
  local ATTEMPT=0
  while [[ "$ATTEMPT" -lt "$MAX_RETRIES" ]]; do
    python test/cpp/naming/utils/dns_resolver.py -s 127.0.0.1 -p "$DNS_SERVER_PORT" -n health-check-local-dns-server-is-alive.resolver-tests.grpctestingexp. -t 1 && return
    ATTEMPT="$((ATTEMPT+1))"
    sleep 1
  done
  echo "Could not resolve records from DNS server. It's likely not runnning."
  exit 1
}

dns_health_check

export GRPC_VERBOSITY=DEBUG
export GRPC_DNS_RESOLVER=ares # TODO: unset this when c-ares is default DNS resolver.
ONE_FAILED=0
timeout 2 "bins/${CXX_TEST_CONFIG}/interop_client" --server_host="dns://127.0.0.1:${DNS_SERVER_PORT}/server.test.com" --server_port="$FALLBACK_PORT" --use_alts=true || ONE_FAILED=1
if [[ "$?" != 0 ]]; then
  echo "FAILED: ALTS client -> successul grpclb server interaction -> backend."
fi
#   # TODO: perhaps don't share the fake servers across each of these tests.
#   timeout 30 "bins/${CXX_TEST_CONFIG}/interop_client" --server_host="dns://127.0.0.1:${DNS_SERVER_PORT}/server.test.com" --server_port="$FALLBACK_PORT" --use_tls=true --use_test_ca=true || ONE_FAILED=1
#   if [[ "$?" != 0 ]]; then
#     echo "FAILED: SSL client -> failed grpclb server interaction -> fallback."
#   fi

# Display fake server logs in case of error.
if [[ "$ONE_FAILED" != 0 ]]; then
  echo "At least one test failed."
  display_all_logs_and_exit
fi
kill_all_servers
