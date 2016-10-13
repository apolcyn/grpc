#!/usr/bin/env bash
export QPS_WORKERS=localhost:13000,localhost:13010
export GRPC_VERBOSITY=INFO
tools/run_tests/performance/run_qps_driver.sh '--scenarios_json={"scenarios": [{"name": "go_protobuf_sync_streaming_qps_unconstrained_secure", "warmup_seconds": 5, "benchmark_seconds": 30, "num_servers": 1, "server_config": {"async_server_threads": 0, "core_limit": 0, "security_params": {"use_test_ca": true, "server_host_override": "foo.test.google.fr"}, "server_type": "SYNC_SERVER"}, "client_config": {"client_type": "SYNC_CLIENT", "security_params": {"use_test_ca": true, "server_host_override": "foo.test.google.fr"}, "payload_config": {"simple_params": {"resp_size": 0, "req_size": 0}}, "client_channels": 64, "async_client_threads": 0, "outstanding_rpcs_per_channel": 100, "rpc_type": "STREAMING", "load_params": {"closed_loop": {}}, "histogram_params": {"max_possible": 60000000000.0, "resolution": 0.01}}, "num_clients": 0}]}'
