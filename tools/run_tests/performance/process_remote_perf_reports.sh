mkdir -p $OUTPUT_DIR

PERF_DATA_FILE=${PERF_BASE_NAME}-perf.data
PERF_SCRIPT_OUTPUT=${PERF_BASE_NAME}-out.perf

# Generate text output from profiles
echo "creating perf text report on remote host $USER_AT_HOST"

# "perf report -i" to not wait for terminal input
ssh $USER_AT_HOST "cd ~/performance_workspace/grpc && perf report -i $PERF_DATA_FILE -v --header > $OUTPUT_FILENAME"

ssh $USER_AT_HOST "cd ~/performance_workspace/grpc && gzip $OUTPUT_FILENAME"

echo "copying perf text report from $USER_AT_HOST to here"
scp $USER_AT_HOST:~/performance_workspace/grpc/${OUTPUT_FILENAME}.gz "${OUTPUT_DIR}/${OUTPUT_FILENAME}.gz"

gzip -d -f $OUTPUT_DIR/${OUTPUT_FILENAME}.gz
mv $OUTPUT_DIR/${OUTPUT_FILENAME} $OUTPUT_DIR/${OUTPUT_FILENAME}.txt

# Generate Flame graphs
echo "running perf script on $USER_AT_HOST with perf.data"
ssh $USER_AT_HOST "cd ~/performance_workspace/grpc && perf script -i $PERF_DATA_FILE > $PERF_SCRIPT_OUTPUT"

ssh $USER_AT_HOST "cd ~/performance_workspace/grpc && gzip $PERF_SCRIPT_OUTPUT"

scp $USER_AT_HOST:~/performance_workspace/grpc/$PERF_SCRIPT_OUTPUT.gz .

gzip -d -f $PERF_SCRIPT_OUTPUT.gz

~/FlameGraph/stackcollapse-perf.pl --kernel $PERF_SCRIPT_OUTPUT | ~/FlameGraph/flamegraph.pl --color=java --hash > ${OUTPUT_DIR}/${OUTPUT_FILENAME}.svg
