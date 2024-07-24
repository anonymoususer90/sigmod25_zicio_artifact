#!/bin/bash

DEVICE_PATH="/opt/nvme"
DATA_PATH="zicio"
TEST_PATH=""
NUM_DEVICE=1
MULTI_DEVICE=NO

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" > /dev/null 2>&1 && pwd)"/
cd $DIR

for i in "$@"
do
case $i in
	--data-path=*)
		DATA_PATH="${i#*=}"
		shift
		;;

	--device-path=*)
		DEVICE_PATH="${i#*=}"
		shift
		;;

	--num-device=*)
		NUM_DEVICE="${i#*=}"
		shift
		;;

	--multi-device=*)
		MULTI_DEVICE="${i#*=}"
		shift
		;;

	*)
		# unknown option	
		;;
esac
done

if [[ "${MULTI_DEVICE}" == "NO" ]]
then
	TEST_PATH="${DEVICE_PATH}/${DATA_PATH}"
	# Make data files
	if [ ! -e ${TEST_PATH}/data_seq.0 ]; then
		echo "Make data files"
		../gen_seq 2
		mv data ${TEST_PATH}/data_seq.0
	fi

	for VAR in {1..7};
	do
		if [ ! -e ${TEST_PATH}/data_seq.${VAR} ]; then
			cp ${TEST_PATH}/data_seq.0 ${TEST_PATH}/data_seq.${VAR} &
		fi
	done
else
	echo "Make data files"
	for ((DEV_VAR=0; DEV_VAR<NUM_DEVICE; DEV_VAR++))
	do
		TEST_PATH="${DEVICE_PATH}/${DEV_VAR}/${DATA_PATH}"
		if [ ! -e ${TEST_PATH}/data_seq.0 ]; then
			../gen_seq 2
			mv data ${TEST_PATH}/data_seq.0
		fi
		for VAR in {1..7};
		do
			if [ ! -e ${TEST_PATH}/data_seq.${VAR} ]; then
				cp ${TEST_PATH}/data_seq.0 ${TEST_PATH}/data_seq.${VAR} &
			fi
		done
	done
fi

wait

sudo sync
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
