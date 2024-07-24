#!/bin/bash
DATA_PATH="zicio"
DEVICE_PATH="/opt/nvme"
TEST_PATH=""
RESULT_PATH="."
LOOP_CNT=0
MEMORY_SIZE="no_limit"
COMMAND=""

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

	--memory=*)
		MEMORY_SIZE=${i#*=}
		shift
		;;

	*)
		# unknown option
		;;
esac
done

MEMS=("no_limit" "small_16" "large_256")
if ! [[ ${MEMS[@]} =~ ${MEMORY_SIZE} ]]; then
	exit 1
elif [ ${MEMORY_SIZE} != "no_limit" ]; then
	COMMAND="cgexec -g memory:"${MEMORY_SIZE}".slice"
fi

TEST_PATH="${DEVICE_PATH}/${DATA_PATH}"
RESULT_PATH="${RESULT_PATH}/multi_clients_${LOOP_CNT}_${MEMORY_SIZE}"

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" > /dev/null 2>&1 && pwd)"/
cd $DIR

if [ ! -d ${RESULT_PATH} ]; then
	mkdir ${RESULT_PATH}
fi

echo -e "memory size: ${MEMORY_SIZE}"

TEST() {
	../drop_cache
	(/usr/bin/time -f "\n%e,real,%U,user,%S,sys" ${COMMAND} ../$@) &> ${FILE_NAME}
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
for NUM_OF_PROCESS in 1 4 8 16
do
	TEST_CASE="pread seq scan multi process"

	BUFFER_PAGE_SHIFT=12
	BUFFER_PAGE_SIZE=$[2 ** ${BUFFER_PAGE_SHIFT}]

	FILE_NAME="${RESULT_PATH}/pread_X_${LOOP_CNT}_${BUFFER_PAGE_SIZE}_${NUM_OF_PROCESS}.dat"
	echo -e "${YELLOW}${BOLD}[${TEST_NUM}-1]${DEFAULT}. ${TEST_CASE} test, loop count: ${LOOP_CNT}, request size: ${BUFFER_PAGE_SIZE}, number of process: ${NUM_OF_PROCESS}, os bypass: off"
	TEST "pread_seq_scan_multi_process" ${TEST_PATH} "off" ${LOOP_CNT} ${NUM_OF_PROCESS} ${BUFFER_PAGE_SHIFT}

	echo -e "Sleep 60 seconds to rest device\n"
	sleep 60s

	FILE_NAME="${RESULT_PATH}/pread_O_${LOOP_CNT}_${BUFFER_PAGE_SIZE}_${NUM_OF_PROCESS}.dat"
	echo -e "${YELLOW}${BOLD}[${TEST_NUM}-2]${DEFAULT}. ${TEST_CASE} test, loop count: ${LOOP_CNT}, request size: ${BUFFER_PAGE_SIZE}, number of process: ${NUM_OF_PROCESS}, os bypass: on"
	TEST "pread_seq_scan_multi_process" ${TEST_PATH} "on" ${LOOP_CNT} ${NUM_OF_PROCESS} ${BUFFER_PAGE_SHIFT}

	echo -e "Sleep 120 seconds to rest device\n"
	sleep 120s

	BUFFER_PAGE_SHIFT=13
	BUFFER_PAGE_SIZE=$[2 ** ${BUFFER_PAGE_SHIFT}]

	FILE_NAME="${RESULT_PATH}/pread_X_${LOOP_CNT}_${BUFFER_PAGE_SIZE}_${NUM_OF_PROCESS}.dat"
	echo -e "${YELLOW}${BOLD}[${TEST_NUM}-3]${DEFAULT}. ${TEST_CASE} test, loop count: ${LOOP_CNT}, request size: ${BUFFER_PAGE_SIZE}, number of process: ${NUM_OF_PROCESS}, os bypass: off"
	TEST "pread_seq_scan_multi_process" ${TEST_PATH} "off" ${LOOP_CNT} ${NUM_OF_PROCESS} ${BUFFER_PAGE_SHIFT}

	echo -e "Sleep 60 seconds to rest device\n"
	sleep 60s

	FILE_NAME="${RESULT_PATH}/pread_O_${LOOP_CNT}_${BUFFER_PAGE_SIZE}_${NUM_OF_PROCESS}.dat"
	echo -e "${YELLOW}${BOLD}[${TEST_NUM}-4]${DEFAULT}. ${TEST_CASE} test, loop count: ${LOOP_CNT}, request size: ${BUFFER_PAGE_SIZE}, number of process: ${NUM_OF_PROCESS}, os bypass: on"
	TEST "pread_seq_scan_multi_process" ${TEST_PATH} "on" ${LOOP_CNT} ${NUM_OF_PROCESS} ${BUFFER_PAGE_SHIFT}

	echo -e "Sleep 120 seconds to rest device\n"
	sleep 120s

	BUFFER_PAGE_SHIFT=14
	BUFFER_PAGE_SIZE=$[2 ** ${BUFFER_PAGE_SHIFT}]

	FILE_NAME="${RESULT_PATH}/pread_X_${LOOP_CNT}_${BUFFER_PAGE_SIZE}_${NUM_OF_PROCESS}.dat"
	echo -e "${YELLOW}${BOLD}[${TEST_NUM}-5]${DEFAULT}. ${TEST_CASE} test, loop count: ${LOOP_CNT}, request size: ${BUFFER_PAGE_SIZE}, number of process: ${NUM_OF_PROCESS}, os bypass: off"
	TEST "pread_seq_scan_multi_process" ${TEST_PATH} "off" ${LOOP_CNT} ${NUM_OF_PROCESS} ${BUFFER_PAGE_SHIFT}

	echo -e "Sleep 60 seconds to rest device\n"
	sleep 60s

	FILE_NAME="${RESULT_PATH}/pread_O_${LOOP_CNT}_${BUFFER_PAGE_SIZE}_${NUM_OF_PROCESS}.dat"
	echo -e "${YELLOW}${BOLD}[${TEST_NUM}-6]${DEFAULT}. ${TEST_CASE} test, loop count: ${LOOP_CNT}, request size: ${BUFFER_PAGE_SIZE}, number of process: ${NUM_OF_PROCESS}, os bypass: on"
	TEST "pread_seq_scan_multi_process" ${TEST_PATH} "on" ${LOOP_CNT} ${NUM_OF_PROCESS} ${BUFFER_PAGE_SHIFT}

	echo -e "Sleep 120 seconds to rest device\n"
	sleep 120s

	TEST_CASE="zicio seq scan multi process"

	FILE_NAME="${RESULT_PATH}/zicio_X_${LOOP_CNT}_${NUM_OF_PROCESS}.dat"
	echo -e "${YELLOW}${BOLD}[${TEST_NUM}-7]${DEFAULT}. ${TEST_CASE} test, loop count: ${LOOP_CNT}, number of process: ${NUM_OF_PROCESS}, sharing: off"
	TEST "zicio_seq_scan_multi_process" ${TEST_PATH} ${LOOP_CNT} ${NUM_OF_PROCESS} "off"

	echo -e "Sleep 60 seconds to rest device\n"
	sleep 60s

	FILE_NAME="${RESULT_PATH}/zicio_O_${LOOP_CNT}_${NUM_OF_PROCESS}.dat"
	echo -e "${YELLOW}${BOLD}[${TEST_NUM}-8]${DEFAULT}. ${TEST_CASE} test, loop count: ${LOOP_CNT}, number of process: ${NUM_OF_PROCESS}, sharing: on"
	TEST "zicio_seq_scan_multi_process" ${TEST_PATH} ${LOOP_CNT} ${NUM_OF_PROCESS} "on"

	echo -e "Sleep 60 seconds to rest device\n"
	sleep 60s

	((TEST_NUM++))
done
