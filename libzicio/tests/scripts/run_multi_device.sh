#!/bin/bash
DATA_PATH="zicio"
DEVICE_PATH="/opt/nvme"
TEST_PATH=()
NUM_DEVICE=2

for i in "$@"
do
case $i in
	--device-path=*)
		DEVICE_PATH="${i#*=}"
		shift
		;;

	--data-path=*)
		DATA_PATH="${i#*=}"
		shift
		;;

	--num-device=*)
		NUM_DEVICE="${i#*=}"
		shift
		;;

	*)
		# unknown option
		;;
esac
done

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" > /dev/null 2>&1 && pwd)"/
cd $DIR

for ((VAR=0; VAR<NUM_DEVICE; VAR++))
do
	TEST_PATH+=("${DEVICE_PATH}/${VAR}/${DATA_PATH}")
done

# Simple ingestion test
echo "1. simple ingestion test with multi device"
start=`date +%s.%N`
../simple_ingestion_multi_device_test ${NUM_DEVICE} ${TEST_PATH[@]}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] simple ingestion test success"
	echo 'simple ingestion start:' ${start}
	echo 'simple ingestion finish:' ${finish}
	echo 'simple ingestion diff:' ${diff}
else
	echo "[FAILED] simple ingestion test fail"
	exit 1
fi
echo ""

# Multiple ingestion test
echo "2. ingestion flow test with multi device"
start=`date +%s.%N`
../ingestion_flow_multi_device_test ${NUM_DEVICE} ${TEST_PATH[@]}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] multi ingestion flow test with multi thread success"
	echo 'multi thread ingestion flow test start:' ${start}
	echo 'multi thread ingestion flow test finish:' ${finish}
	echo 'multi thread ingestion flow test diff:' ${diff}
else
	echo "[FAILED] multi ingestion flow test with multi thread fail"
	exit 1
fi
echo ""

# Multiple ingestion test
echo "3. multi ingestion test with one device per channel"
start=`date +%s.%N`
../multi_ingestion_one_device_per_channel_test ${NUM_DEVICE} ${TEST_PATH[@]}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] multi ingestion flow test with multi thread success"
	echo 'multi thread ingestion flow test start:' ${start}
	echo 'multi thread ingestion flow test finish:' ${finish}
	echo 'multi thread ingestion flow test diff:' ${diff}
else
	echo "[FAILED] multi ingestion flow test with multi thread fail"
	exit 1
fi
echo ""

# Multiple ingestion test
echo "4. multi ingestion test with multi device per channel"
start=`date +%s.%N`
../multi_ingestion_multi_device_per_channel_test ${NUM_DEVICE} ${TEST_PATH[@]}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] multi ingestion flow test with multi thread success"
	echo 'multi thread ingestion flow test start:' ${start}
	echo 'multi thread ingestion flow test finish:' ${finish}
	echo 'multi thread ingestion flow test diff:' ${diff}
else
	echo "[FAILED] multi ingestion flow test with multi thread fail"
	exit 1
fi
echo ""

# Multiple ingestion test
echo "5. shared ingestion flow test with multi device and single process"
start=`date +%s.%N`
../ingestion_flow_shared_single_proc_with_multi_device_test ${NUM_DEVICE} ${TEST_PATH[@]}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] shared ingestion flow test with multi thread success and single process"
	echo 'shared ingestion flow test multi device and single process start:' ${start}
	echo 'shared ingestion flow test multi device and single process finish:' ${finish}
	echo 'shared ingestion flow test multi device and single process diff:' ${diff}
else
	echo "[FAILED] shared ingestion flow test with multi device and single process fail"
	exit 1
fi
echo ""

# Multiple ingestion test
echo "6. shared ingestion flow repeat test with multi device and single process"
start=`date +%s.%N`
../ingestion_flow_shared_single_proc_repeat_with_multi_device_test ${NUM_DEVICE} ${TEST_PATH[@]}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] shared ingestion flow repeat test with multi thread success and single process"
	echo 'shared ingestion flow repeat test multi device and single process start:' ${start}
	echo 'shared ingestion flow repeat test multi device and single process finish:' ${finish}
	echo 'shared ingestion flow repeat test multi device and single process diff:' ${diff}
else
	echo "[FAILED] shared ingestion flow repeat test with multi device and single process fail"
	exit 1
fi
echo ""

# Ingestion flow to process shared test
echo "7. ingestion flow two proc shared test with multi device"
start=`date +%s.%N`
../ingestion_flow_shared_with_multi_device_test 2 ${NUM_DEVICE} ${TEST_PATH[@]}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] ingestion flow shared 2 proc test with multi device"
	echo 'ingestion flow shared 2 proc test with multi device start:' ${start}
	echo 'ingestion flow shared 2 proc test with multi device finish:' ${finish}
	echo 'ingestion flow shared 2 proc test with multi device diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared 2 proc test with multi device fail"
	exit 1
fi
echo ""

# Ingestion flow to process shared test
echo "8. ingestion flow 4 proc shared test with multi device"
start=`date +%s.%N`
../ingestion_flow_shared_with_multi_device_test 4 ${NUM_DEVICE} ${TEST_PATH[@]}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] ingestion flow shared 4 proc test with multi device"
	echo 'ingestion flow shared 4 proc test with multi device start:' ${start}
	echo 'ingestion flow shared 4 proc test with multi device finish:' ${finish}
	echo 'ingestion flow shared 4 proc test with multi device diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared 4 proc test with multi device fail"
	exit 1
fi
echo ""

# Ingestion flow to process shared test
echo "9. ingestion flow 32 proc shared test with multi device"
start=`date +%s.%N`
../ingestion_flow_shared_with_multi_device_test 32 ${NUM_DEVICE} ${TEST_PATH[@]}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] ingestion flow shared 32 proc test with multi device"
	echo 'ingestion flow shared 32 proc test with multi device start:' ${start}
	echo 'ingestion flow shared 32 proc test with multi device finish:' ${finish}
	echo 'ingestion flow shared 32 proc test with multi device diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared 32 proc test with multi device fail"
	exit 1
fi
echo ""

# Ingestion flow to process shared test
echo "9. ingestion flow 64 proc shared test with multi device"
start=`date +%s.%N`
../ingestion_flow_shared_with_multi_device_test 64 ${NUM_DEVICE} ${TEST_PATH[@]}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] ingestion flow shared 64 proc test with multi device"
	echo 'ingestion flow shared 64 proc test with multi device start:' ${start}
	echo 'ingestion flow shared 64 proc test with multi device finish:' ${finish}
	echo 'ingestion flow shared 64 proc test with multi device diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared 64 proc test with multi device fail"
	exit 1
fi
echo ""
