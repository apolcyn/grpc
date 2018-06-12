docker run  -p 8080 -p 8081 -p 8082 --rm -v "$(pwd)/../server_logs":/var/local/server_logs:rw -v `pwd`:/var/local/server_startup:ro go_servers python /var/local/server_startup/start_fake_servers.py --grpclb_args="--port=8080 --backend_port=8081 --debug_mode=true" --backend_args="--use_tls=false --port=8081" --fallback_args="--use_tls=false --port 8082" --output_dir="/var/local/server_logs" --timeout=120
