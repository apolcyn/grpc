#first="\'grpc_=[{\"clientLanguage\":[\"go\"],\"serviceConfig\":{\"loadBalancingPolicy\":\"round_robin\",\"methodConfig\":[{\"name\":[{\"method\":\"Foo\",\"service\":\"GoService\",\"waitForReady\":true}]}]}},{\"clientLanguage\":[\"c++\"],\"serviceConfig\":{\"loadBalancingPolicy\":\"round_rob\'"
#second="\'in\",\"methodConfig\":[{\"name\":[{\"method\":\"Foo\",\"service\":\"CppService\",\"waitForReady\":true}]}]}}]\'"
#echo "first: ${#first}"
#echo "secodn: ${#second}"
gcloud dns record-sets transaction start -z=exp-grpc-testing-2
#gcloud dns record-sets transaction add -z=exp-grpc-testing-2 --name=test-thing.test-2.grpctestingexp. --type=TXT --ttl=2100 'grpc_config=[{"clientLanguage":["go"],"serviceConfig":{"loadBalancingPolicy":"round_robin","methodConfig":[{"name":[{"method":"Foo","service":"GoService","waitForReady":true}]}]}},{"clientLanguage":["c++"],"serviceConfig":{"loadBalancingPolicy":"round_rob' 'in","methodConfig":[{"name":[{"method":"Foo","service":"CppService","waitForReady":true}]}]}}]'
#str="grpc_config=[{\\\"percentage\\\":0,\\\"serviceConfig\\\":{\\\"loadBalancingPolicy\\\":\\\"round_robin\\\",\\\"methodConfig\\\":[{\\\"name\\\":[{\\\"method\\\":\\\"Foo\\\",\\\"service\\\":\\\"NeverPickedService\\\",\\\"waitForReady\\\":true}]}]}},,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,]"
#str='"grpc_config=[{\"percentage\":0,\"serviceConfig\":{\"loadBalancingPolicy\":\"round_robin\",\"methodConfig\":[{\"name\":[{\"method\":\"Foo\",\"service\":\"CppService\",\"waitForReady\":true}]}]}}]"'
str="{\"name\":\"val\"}"
#echo "other is ${#other}"
echo "str is ${#str}"
gcloud dns record-sets transaction add -z=exp-grpc-testing-2 --name=quotes-5-txt-trial.test-2.grpctestingexp. --type=TXT --ttl=2100 $str
#gcloud dns record-sets transaction remove $str -z=exp-grpc-testing-2 --name=quotes-5-txt-trial.test-2.grpctestingexp. --type=TXT --ttl=2100
gcloud dns record-sets transaction execute -z=exp-grpc-testing-2
