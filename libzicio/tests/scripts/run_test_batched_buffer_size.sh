#!/bin/bash
DATA_PATH="zicio"
DEVICE_PATH="/opt/nvme"
TEST_PATH=""
RESULT_PATH="."
LOOP_CNT=0

RED='\033[31m'
GREEN='\033[32m'
YELLOW='\033[33m'
BOLD='\033[1m'
DEFAULT='\033[0m'

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

	--result-path=*)
		RESULT_PATH="${i#*=}"
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
RESULT_PATH="${RESULT_PATH}/batched_raw_results_${LOOP_CNT}"

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" > /dev/null 2>&1 && pwd)"/
cd ${DIR}

if [ ! -d ${RESULT_PATH} ]; then
	mkdir ${RESULT_PATH}
fi

TEST() {
	../drop_cache
	sudo /usr/bin/time -f "\n%e,real,%U,user,%S,sys" perf stat -x ',' -e cycles,instructions,duration_time ../$@ &> ${FILE_NAME}
	RESULT=$?
	awk -F ',' '$2 != "real" { print }' ${FILE_NAME}
	tail -n 1 ${FILE_NAME} | awk -F ',' '{ print "real: '"${BOLD}"'"$1"'"${DEFAULT}"', user: '"${GREEN}"'"$3"'"${DEFAULT}"', system: '"${RED}"'"$5"'"${DEFAULT}"'" }'

	if [ ${RESULT} == 0 ]; then
		echo -e "${GREEN}[SUCCESS]${DEFAULT} ${TEST_CASE} test success\n"
	else
		echo -e "${RED}[FAILED]${DEFAULT} ${TEST_CASE} test fail\n"
		exit 1
	fi
}

TEST_NUM=1
for BUFFER_PAGE_SHIFT in 18 16 14 13 12
do
	BUFFER_PAGE_SIZE=$[2 ** ${BUFFER_PAGE_SHIFT}]

	TEST_CASE="zicio batched scan"

	FILE_NAME="${RESULT_PATH}/zicio_${BUFFER_PAGE_SIZE}_${LOOP_CNT}.dat"
	echo -e "${YELLOW}${BOLD}[${TEST_NUM}-1]${DEFAULT}. ${TEST_CASE} test, loop count: ${LOOP_CNT}, batch size: ${BUFFER_PAGE_SIZE}"
	TEST "zicio_batched_scan" ${TEST_PATH} ${LOOP_CNT} ${BUFFER_PAGE_SHIFT}

	echo -e "Sleep 60 seconds to rest device\n"
	sleep 60s

	TEST_CASE="uring batched scan"

	FILE_NAME="${RESULT_PATH}/uring_${BUFFER_PAGE_SIZE}_O_O_${LOOP_CNT}.dat"
	echo -e "${YELLOW}${BOLD}[${TEST_NUM}-2]${DEFAULT}. ${TEST_CASE} test, loop count: ${LOOP_CNT}, batch size: ${BUFFER_PAGE_SIZE}, sq polling: on, os bypass: on"
	TEST "uring_batched_scan" ${TEST_PATH} "on" "on" ${LOOP_CNT} ${BUFFER_PAGE_SHIFT}

	echo -e "Sleep 60 seconds to rest device\n"
	sleep 60s

	FILE_NAME="${RESULT_PATH}/uring_${BUFFER_PAGE_SIZE}_X_O_${LOOP_CNT}.dat"
	echo -e "${YELLOW}${BOLD}[${TEST_NUM}-3]${DEFAULT}. ${TEST_CASE} test, loop count: ${LOOP_CNT}, batch size: ${BUFFER_PAGE_SIZE}, sq polling: off, os bypass: on"
	TEST "uring_batched_scan" ${TEST_PATH} "off" "on" ${LOOP_CNT} ${BUFFER_PAGE_SHIFT}

	echo -e "Sleep 60 seconds to rest device\n"
	sleep 60s

	FILE_NAME="${RESULT_PATH}/uring_${BUFFER_PAGE_SIZE}_O_X_${LOOP_CNT}.dat"
	echo -e "${YELLOW}${BOLD}[${TEST_NUM}-4]${DEFAULT}. ${TEST_CASE} test, loop count: ${LOOP_CNT}, batch size: ${BUFFER_PAGE_SIZE}, sq polling: on, os bypass: off"
	TEST "uring_batched_scan" ${TEST_PATH} "on" "off" ${LOOP_CNT} ${BUFFER_PAGE_SHIFT}

	echo -e "Sleep 60 seconds to rest device\n"
	sleep 60s

	FILE_NAME="${RESULT_PATH}/uring_${BUFFER_PAGE_SIZE}_X_X_${LOOP_CNT}.dat"
	echo -e "${YELLOW}${BOLD}[${TEST_NUM}-5]${DEFAULT}. ${TEST_CASE} test, loop count: ${LOOP_CNT}, batch size: ${BUFFER_PAGE_SIZE}, sq polling: off, os bypass: off"
	TEST "uring_batched_scan" ${TEST_PATH} "off" "off" ${LOOP_CNT} ${BUFFER_PAGE_SHIFT}

	echo -e "Sleep 60 seconds to rest device\n"
	sleep 60s

	TEST_CASE="pread batched scan"

	FILE_NAME="${RESULT_PATH}/pread_${BUFFER_PAGE_SIZE}_O_${LOOP_CNT}.dat"
	echo -e "${YELLOW}${BOLD}[${TEST_NUM}-6]${DEFAULT}. ${TEST_CASE} test, loop count: ${LOOP_CNT}, batch size: ${BUFFER_PAGE_SIZE}, os bypass: on"
	TEST "pread_batched_scan" ${TEST_PATH} "on" ${LOOP_CNT} ${BUFFER_PAGE_SHIFT}

	echo -e "Sleep 60 seconds to rest device\n"
	sleep 60s

	FILE_NAME="${RESULT_PATH}/pread_${BUFFER_PAGE_SIZE}_X_${LOOP_CNT}.dat"
	echo -e "${YELLOW}${BOLD}[${TEST_NUM}-7]${DEFAULT}. ${TEST_CASE} test, loop count: ${LOOP_CNT}, batch size: ${BUFFER_PAGE_SIZE}, os bypass: off"
	TEST "pread_batched_scan" ${TEST_PATH} "off" ${LOOP_CNT} ${BUFFER_PAGE_SHIFT}

	echo -e "Sleep 60 seconds to rest device\n"
	sleep 60s

	((TEST_NUM++))
done
