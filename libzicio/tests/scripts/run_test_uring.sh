#!/bin/bash
DATA_PATH="zicio"
DEVICE_PATH="/opt/nvme"
TEST_PATH=""
TEST_NUM=0
LOOP_CNT=0
BUFFER_PAGE_SHIFT=21

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

	--test-num=*)
		TEST_NUM=${i#*=}
		shift
		;;

	--loop-cnt=*)
		LOOP_CNT=${i#*=}
		shift
		;;

	--buffer-page-shift=*)
		BUFFER_PAGE_SHIFT=${i#*=}
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

echo $[5*${TEST_NUM}+1]". zicio sequential scan test, loop cnt: "${LOOP_CNT}
../drop_cache
start=`date +%s.%N`
sudo perf stat -e cycles,instructions,cs.cache-misses ../zicio_seq_scan ${TEST_PATH} ${LOOP_CNT}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] zicio sequential scan test success"
else
	echo "[FAILED] zicio sequential scan test fail"
	exit 1
fi
echo ""

sleep 1s

echo $[5*${TEST_NUM}+2]". uring sequential scan test, loop_cnt: "${LOOP_CNT}", buffer: "$[2**${BUFFER_PAGE_SHIFT}]", sq polling: on, os bypass: on"
../drop_cache
start=`date +%s.%N`
sudo perf stat -e cycles,instructions,cs.cache-misses ../uring_seq_scan ${TEST_PATH} on on ${LOOP_CNT} ${BUFFER_PAGE_SHIFT}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] uring sequential scan test success"
else
	echo "[FAILED] uring sequential scan test fail"
	exit 1
fi
echo ""

sleep 1s

echo $[5*${TEST_NUM}+3]". uring sequential scan test, loop_cnt: "${LOOP_CNT}", buffer: "$[2**${BUFFER_PAGE_SHIFT}]", sq polling: off, os bypass: on"
../drop_cache
start=`date +%s.%N`
sudo perf stat -e cycles,instructions,cs.cache-misses ../uring_seq_scan ${TEST_PATH} off on ${LOOP_CNT} ${BUFFER_PAGE_SHIFT}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] uring sequential scan test success"
else
	echo "[FAILED] uring sequential scan test fail"
	exit 1
fi
echo ""

echo $[5*${TEST_NUM}+4]". uring sequential scan test, loop_cnt: "${LOOP_CNT}", buffer: "$[2**${BUFFER_PAGE_SHIFT}]", sq polling: on, os bypass: off"
../drop_cache
start=`date +%s.%N`
sudo perf stat -e cycles,instructions,cs.cache-misses ../uring_seq_scan ${TEST_PATH} on off ${LOOP_CNT} ${BUFFER_PAGE_SHIFT}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] uring sequential scan test success"
else
	echo "[FAILED] uring sequential scan test fail"
	exit 1
fi
echo ""

sleep 1s

echo $[5*${TEST_NUM}+5]". uring sequential scan test, loop_cnt: "${LOOP_CNT}", buffer: "$[2**${BUFFER_PAGE_SHIFT}]", sq polling: off, os bypass: off"
../drop_cache
start=`date +%s.%N`
sudo perf stat -e cycles,instructions,cs.cache-misses ../uring_seq_scan ${TEST_PATH} off off ${LOOP_CNT}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] uring sequential scan test success"
else
	echo "[FAILED] uring sequential scan test fail"
	exit 1
fi
echo ""

sleep 1s
