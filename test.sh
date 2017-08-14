gcloud dns record-sets transaction start -z=exp-grpc-testing-2
#str="grpc_config=[{\\\"percentage\\\":0,\\\"serviceConfig\\\":{\\\"loadBalancingPolicy\\\":\\\"round_robin\\\",\\\"methodConfig\\\":[{\\\"name\\\":[{\\\"method\\\":\\\"Foo\\\",\\\"service\\\":\\\"NeverPickedService\\\",\\\"waitForReady\\\":true}]}]}},,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,]"
#str='"grpc_config=[{\"percentage\":0,\"serviceConfig\":{\"loadBalancingPolicy\":\"round_robin\",\"methodConfig\":[{\"name\":[{\"method\":\"Foo\",\"service\":\"CppService\",\"waitForReady\":true}]}]}}]"'
str='{"name":"val"}'
echo "other is ${#other}"
echo "fails at ${#str}"
gcloud dns record-sets transaction add -z=exp-grpc-testing-2 --name=quotes-4-txt-trial.test-2.grpctestingexp. --type=TXT --ttl=2100 $str
#gcloud dns record-sets transaction remove $str -z=exp-grpc-testing-2 --name=quotes-4-txt-trial.test-2.grpctestingexp. --type=TXT --ttl=2100
gcloud dns record-sets transaction execute -z=exp-grpc-testing-2
