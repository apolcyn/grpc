mkdir -p $OUTPUT_DIR

PERF_DATA_FILE=${PERF_BASE_NAME}-perf.data
PERF_SCRIPT_OUTPUT=${PERF_BASE_NAME}-out.perf

# Generate text output from profiles
perf report -i $PERF_DATA_FILE  -v > ${OUTPUT_DIR}/${OUTPUT_FILENAME}.txt

# Generate Flame graphs
echo "running perf script on $PERF_DATA_FILE"
perf script -i $PERF_DATA_FILE > $PERF_SCRIPT_OUTPUT

~/FlameGraph/stackcollapse-perf.pl --kernel $PERF_SCRIPT_OUTPUT | ~/FlameGraph/flamegraph.pl --color=java --hash > ${OUTPUT_DIR}/${OUTPUT_FILENAME}.svg
