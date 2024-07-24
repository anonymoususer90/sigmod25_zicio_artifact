#!/bin/bash
DATA_PATH="zicio"
DEVICE_PATH="/opt/nvme"
MULTI_DEVICE=NO
TEST_PATH=""

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

	--multi-device)
		MULTI_DEVICE=YES
		shift
		;;

	*)
		# unknown option
		;;
esac
done

if [[ ${MULTI_DEVICE} == "YES" ]]
then
	TEST_PATH="${DEVICE_PATH}1/${DATA_PATH}"
else
	TEST_PATH="${DEVICE_PATH}/${DATA_PATH}"
fi

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" > /dev/null 2>&1 && pwd)"/
cd $DIR

# Open test
echo "1. open test"
start=`date +%s.%N`
../open_test
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] open test success"
	echo 'open test start:' ${start}
	echo 'open test finish:' ${finish}
	echo 'open test diff:' ${diff}
else
	echo "[FAILED] open test fail"
	exit 1
fi
echo ""

# Open stress test
echo "2. open stress test"
start=`date +%s.%N`
../open_stress_test
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] open stress test success"
	echo 'open stress test start:' ${start}
	echo 'open stress test finish:' ${finish}
	echo 'open stress test diff:' ${diff}
else
	echo "[FAILED] open stress test fail"
	exit 1
fi
echo ""

# Simple ingestion test
echo "3. simple ingestion test"
start=`date +%s.%N`
../simple_ingestion_test $TEST_PATH
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

# Ingestion flow test
echo "4. ingestion flow test"
start=`date +%s.%N`
../ingestion_flow_test $TEST_PATH
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] ingestion flow test success"
	echo 'ingestion flow start:' ${start}
	echo 'ingestion flow finish:' ${finish}
	echo 'ingestion flow diff:' ${diff}
else
	echo "[FAILED] ingestion flow test fail"
	exit 1
fi
echo ""

# Multiple ingestion test
echo "5. ingestion flow test with multi thread"
start=`date +%s.%N`
../multi_ingestion_test $TEST_PATH
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] ingestion flow test with multi thread success"
	echo 'multi thread ingestion flow test start:' ${start}
	echo 'multi thread ingestion flow test finish:' ${finish}
	echo 'multi thread ingestion flow test diff:' ${diff}
else
	echo "[FAILED] ingestion flow test with multi thread fail"
	exit 1
fi
echo ""

# Customable chunk size test
echo "6. customable chunk size test"
start=`date +%s.%N`
../custom_ingestion_test $TEST_PATH
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] customable chunk size test success"
	echo 'customable chunk size test start:' ${start}
	echo 'customable chunk size finish:' ${finish}
	echo 'customable chunk size diff:' ${diff}
else
	echo "[FAILED] customable chunk size test success fail"
	exit 1
fi
echo ""

# Not aligned data test
echo "7. not aligned data test"
start=`date +%s.%N`
../not_aligned_data_test $TEST_PATH
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Not aligned data test test success"
	echo 'not aligned data start:' ${start}
	echo 'not aligned data finish:' ${finish}
	echo 'not aligned data diff:' ${diff}
else
	echo "[FAILED] Not aligned data test fail"
	exit 1
fi
echo ""

echo "8. regression test"
start=`date +%s.%N`
../regression_test $TEST_PATH
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] regression test with multi thread success"
	echo 'regression test start:' ${start}
	echo 'regression test finish:' ${finish}
	echo 'regression test diff:' ${diff}
else
	echo "[FAILED] regression test with multi thread fail"
	exit 1
fi
echo ""

echo "9. multi process ingestion flow test"
start=`date +%s.%N`
../multi_process_ingestion_flow_test $TEST_PATH 40
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] multi process ingestion flow test success"
	echo 'multi process ingestion start:' ${start}
	echo 'multi process ingestion finish:' ${finish}
	echo 'multi process ingestion diff:' ${diff}
else
	echo "[FAILED] multi process ingestion flow test fail"
	exit 1
fi
echo ""
