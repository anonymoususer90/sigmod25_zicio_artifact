#!/bin/bash
DATA_PATH="zicio"
DEVICE_PATH="/opt/nvme"

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

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" > /dev/null 2>&1 && pwd)"/
cd $DIR

for TEST_NUM in {0..10}
do
	./run_test_uring.sh --device-path=${DEVICE_PATH} --data-path=${DATA_PATH} --test-num=${TEST_NUM} --loop-cnt=$[100 * ${TEST_NUM}]
done

./run_test_uring.sh --device-path=${DEVICE_PATH} --data-path=${DATA_PATH} --test-num=11 --loop-cnt=5000
./run_test_uring.sh --device-path=${DEVICE_PATH} --data-path=${DATA_PATH} --test-num=12 --loop-cnt=10000
./run_test_uring.sh --device-path=${DEVICE_PATH} --data-path=${DATA_PATH} --test-num=13 --loop-cnt=50000