export GRPC_GO_LOG_VERBOSITY_LEVEL=3
export GRPC_GO_LOG_SEVERITY_LEVEL=INFO
docker run \
  -e GRPC_GO_LOG_VERBOSITY_LEVEL=3 \
  -e GRPC_GO_LOG_SEVERITY_LEVEL=INFO \
  --name="fake_backend" -p 8081 --rm go_client /go/bin/server --use_tls=false --port=8081 &
docker run \
  -e GRPC_GO_LOG_VERBOSITY_LEVEL=3 \
  -e GRPC_GO_LOG_SEVERITY_LEVEL=INFO \
  --name="fake_fallback" -p 8082 --rm go_client /go/bin/server --use_tls=false --port=8082 &
sleep 3
BACKEND="$(docker port fake_backend 8081 | cut -d":" -f 2)"
echo "backend port: |$BACKEND|"
docker run \
  -e GRPC_GO_LOG_VERBOSITY_LEVEL=3 \
  -e GRPC_GO_LOG_SEVERITY_LEVEL=INFO \
  --name="fake_grpclb" -p 8080 --rm fake_lb /go/src/google.golang.org/fake_grpclb/fake_grpclb --port=8080 --backend_port="$BACKEND" --debug_mode=true
