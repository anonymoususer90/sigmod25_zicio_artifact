#!/bin/bash
DATA_PATH="zicio"
DEVICE_PATH="/opt/nvme"
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

	*)
		# unknown option
		;;
esac
done

TEST_PATH="${DEVICE_PATH}/${DATA_PATH}"

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" > /dev/null 2>&1 && pwd)"/
cd $DIR

# single file, single user
echo "1. batched single file single user test"
start=`date +%s.%N`
../batched_single_file_single_user_test ${TEST_PATH}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] batched single file single user test success"
	echo 'batched single file single user test start:' ${start}
	echo 'batched single file single user test finish:' ${finish}
	echo 'batched single file single user test diff:' ${diff}
else
	echo "[FAILED] batched single file single user test fail"
	exit 1
fi
echo ""

# multi file, single user
echo "2. batched multi file single user test"
start=`date +%s.%N`
../batched_multi_file_single_user_test ${TEST_PATH}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] batched multi file single user test success"
	echo 'batched multi file single user test start:' ${start}
	echo 'batched multi file single user test finish:' ${finish}
	echo 'batched multi file single user test diff:' ${diff}
else
	echo "[FAILED] batched multi file single user test fail"
	exit 1
fi
echo ""

# multi file, multi thread
echo "3. batched multi file multi thread test"
start=`date +%s.%N`
../batched_multi_file_multi_thread_test ${TEST_PATH} 16
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] batched multi file multi thread test success"
	echo 'batched multi file multi thread test start:' ${start}
	echo 'batched multi file multi thread test finish:' ${finish}
	echo 'batched multi file multi thread test diff:' ${diff}
else
	echo "[FAILED] batched multi file multi thread test fail"
	exit 1
fi
echo ""

# multi file, multi process
echo "4. batched multi file multi process test"
start=`date +%s.%N`
../batched_multi_file_multi_process_test ${TEST_PATH} 16
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] batched multi file multi process test success"
	echo 'batched multi file multi process test start:' ${start}
	echo 'batched multi file multi process test finish:' ${finish}
	echo 'batched multi file multi process test diff:' ${diff}
else
	echo "[FAILED] batched multi file multi process test fail"
	exit 1
fi
echo ""

# single file, single user scan
echo "5. batched single file single user scan test"
start=`date +%s.%N`
../batched_single_file_single_user_scan_test ${TEST_PATH} 0
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] batched single file single user scan test success"
	echo 'batched single file single user scan test start:' ${start}
	echo 'batched single file single user scan test finish:' ${finish}
	echo 'batched single file single user scan test diff:' ${diff}
else
	echo "[FAILED] batched single file single user scan test fail"
	exit 1
fi
echo ""

# multi file, single user scan
echo "6. batched multi file single user scan test"
start=`date +%s.%N`
../batched_multi_file_single_user_scan_test ${TEST_PATH}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] batched multi file single user scan test success"
	echo 'batched multi file single user scan test start:' ${start}
	echo 'batched multi file single user scan test finish:' ${finish}
	echo 'batched multi file single user scan test diff:' ${diff}
else
	echo "[FAILED] batched multi file single user scan test fail"
	exit 1
fi
echo ""

# multi file, multi thread scan
echo "7. batched multi file multi thread scan test"
start=`date +%s.%N`
../batched_multi_file_multi_thread_scan_test ${TEST_PATH} 16
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] batched multi file multi thread scan test success"
	echo 'batched multi file multi thread scan test start:' ${start}
	echo 'batched multi file multi thread scan test finish:' ${finish}
	echo 'batched multi file multi thread scan test diff:' ${diff}
else
	echo "[FAILED] batched multi file multi thread scan test fail"
	exit 1
fi
echo ""

# multi file, multi process scan
echo "8. batched multi file multi process scan test"
start=`date +%s.%N`
../batched_multi_file_multi_process_scan_test ${TEST_PATH} 16
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] batched multi file multi process scan test success"
	echo 'batched multi file multi process scan test start:' ${start}
	echo 'batched multi file multi process scan test finish:' ${finish}
	echo 'batched multi file multi process scan test diff:' ${diff}
else
	echo "[FAILED] batched multi file multi process scan test fail"
	exit 1
fi
echo ""
