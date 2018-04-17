docker run --rm \
  -v "$(pwd):/external_mount:ro" \
  --sysctl net.ipv6.conf.all.disable_ipv6=0 \
  cxx_creds_test \
  bash -l /external_mount/run_grpclb_creds_test_within_docker.sh
