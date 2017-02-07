#!/bin/bash
params="`echo "$@" | wc -w`"
if [[ $params != "2" ]]
then
    echo "bad params"
      exit 1
fi

cd ../grpc-go && git fetch origin $1 && git checkout origin/$1 && cd ../grpc && tools/run_tests/performance/run_worker_go.sh --driver_port $2
