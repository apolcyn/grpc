mkdir -p $OUTPUT_DIR

# Generate text output from profiles
echo "creating perf text report on remote host $USER_AT_HOST"

# "perf report -i" to not wait for terminal input
ssh $USER_AT_HOST "cd ~/performance_workspace/grpc && perf report -i perf.data -v --header > $OUTPUT_FILENAME"

ssh $USER_AT_HOST "cd ~/performance_workspace/grpc && gzip $OUTPUT_FILENAME"

echo "copying perf text report from $USER_AT_HOST to here"
scp $USER_AT_HOST:~/performance_workspace/grpc/${OUTPUT_FILENAME}.gz "${OUTPUT_DIR}/${OUTPUT_FILENAME}.gz"

gzip -d -f $OUTPUT_DIR/${OUTPUT_FILENAME}.gz
mv $OUTPUT_DIR/${OUTPUT_FILENAME} $OUTPUT_DIR/${OUTPUT_FILENAME}.txt

# Generate Flame graphs
echo "running perf script on $USER_AT_HOST with perf.data"
ssh $USER_AT_HOST "cd ~/performance_workspace/grpc && perf script -i perf.data > out.perf"

ssh $USER_AT_HOST "cd ~/performance_workspace/grpc && gzip out.perf"

scp $USER_AT_HOST:~/performance_workspace/grpc/out.perf.gz .

gzip -d -f out.perf.gz

~/FlameGraph/stackcollapse-perf.pl --kernel out.perf | ~/FlameGraph/flamegraph.pl --color=java --hash > ${OUTPUT_DIR}/${OUTPUT_FILENAME}.svg
