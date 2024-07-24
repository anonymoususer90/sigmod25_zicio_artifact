#!/bin/bash
DATA_PATH="zicio"
DEVICE_PATH="/opt/nvme"
TEST_PATH=""
LOOP_CNT=0

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

	--loop-cnt=*)
		LOOP_CNT=${i#*=}
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

echo "1. batched single file single user scan test"
../drop_cache
start=`date +%s.%N`
sudo perf stat -e cycles,instructions,cs ../batched_single_file_single_user_scan_test ${TEST_PATH} ${LOOP_CNT}
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

sleep 1s

echo "2. uring single file single user scan test, idle: 1 ms"
../drop_cache
start=`date +%s.%N`
sudo perf stat -e cycles,instructions,cs ../uring_single_file_single_user_scan_test ${TEST_PATH} on on ${LOOP_CNT} 1
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] uring single file single user scan test success"
	echo 'uring single file single user scan test start:' ${start}
	echo 'uring single file single user scan test finish:' ${finish}
	echo 'uring single file single user scan test diff:' ${diff}
else
	echo "[FAILED] uring single file single user scan test fail"
	exit 1
fi
echo ""

sleep 1s

echo "3. uring single file single user scan test, idle: 2 ms"
../drop_cache
start=`date +%s.%N`
sudo perf stat -e cycles,instructions,cs ../uring_single_file_single_user_scan_test ${TEST_PATH} on on ${LOOP_CNT} 2
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] uring single file single user scan test success"
	echo 'uring single file single user scan test start:' ${start}
	echo 'uring single file single user scan test finish:' ${finish}
	echo 'uring single file single user scan test diff:' ${diff}
else
	echo "[FAILED] uring single file single user scan test fail"
	exit 1
fi
echo ""

sleep 1s

echo "4. uring single file single user scan test, idle: 5 ms"
../drop_cache
start=`date +%s.%N`
sudo perf stat -e cycles,instructions,cs ../uring_single_file_single_user_scan_test ${TEST_PATH} on on ${LOOP_CNT} 5
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] uring single file single user scan test success"
	echo 'uring single file single user scan test start:' ${start}
	echo 'uring single file single user scan test finish:' ${finish}
	echo 'uring single file single user scan test diff:' ${diff}
else
	echo "[FAILED] uring single file single user scan test fail"
	exit 1
fi
echo ""

sleep 1s

echo "5. uring single file single user scan test, idle: 10 ms"
../drop_cache
start=`date +%s.%N`
sudo perf stat -e cycles,instructions,cs ../uring_single_file_single_user_scan_test ${TEST_PATH} on on ${LOOP_CNT} 10
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] uring single file single user scan test success"
	echo 'uring single file single user scan test start:' ${start}
	echo 'uring single file single user scan test finish:' ${finish}
	echo 'uring single file single user scan test diff:' ${diff}
else
	echo "[FAILED] uring single file single user scan test fail"
	exit 1
fi
echo ""
