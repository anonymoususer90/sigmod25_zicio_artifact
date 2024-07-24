#!/bin/bash
if [[ "$1" == "-h" ]]; then
	echo "Usage: `basename $0` [options]"
	echo "Options:"
	echo "  --gen							create file"
	echo "	--data_path {DATA_PATH}			data path"
	echo "  --device_path {DEVICE_PATH}		device path. DEVICE_PATH/DATA_PATH will be used"
	echo "  --num_device {NUM_DEVICE}		number of device"
	echo "  --single_channel				test single channel"
	echo "  --shared_pool					test shared pool"
	echo "  --multi_device					test multiple device"
	exit 0
fi

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" > /dev/null 2>&1 && pwd)"/
cd $DIR

SCRIPT_DIR="${DIR}/scripts"
DEVICE_PATH="/opt/nvme"
DATA_PATH="/zicio"

GEN=NO
SINGLE_CHANNEL=NO
SHARED_POOL=NO
MULTI_DEVICE=NO
NUM_DEVICE=1

for i in "$@"
do
case $i in
	--data_path=*)
		DATA_PATH="${i#*=}"
		shift
		;;

	--device_path=*)
		DEVICE_PATH="${i#*=}"
		shift
		;;

	--num_device=*)
		NUM_DEVICE="${i#*=}"
		shift
		;;

	--g|--gen)
		GEN=YES
		shift
		;;

	--single_channel)
		SINGLE_CHANNEL=YES
		shift
		;;

	--multi_device)
		MULTI_DEVICE=YES
		shift
		;;

	--shared_pool)
		SHARED_POOL=YES
		shift
		;;

	*)
		# unknown option	
		;;
esac
done

echo "GEN			= ${GEN}"
echo "DEVICE PATH   = ${DEVICE_PATH}"
echo "DATA PATH		= ${DATA_PATH}"
echo "NUM DEVICE	= ${NUM_DEVICE}"

if [[ "${GEN}" == "YES" ]]
then
	${SCRIPT_DIR}/gen.sh	\
		--device_path=${DEVICE_PATH} \
		--data_path=${DATA_PATH} \
		--multi_device=${MULTI_DEVICE} \
		--num_device=${NUM_DEVICE}
	cd $DIR
fi

if [[ "${SINGLE_CHANNEL}" == "YES" ]]
then
	${SCRIPT_DIR}/run_single_channel.sh		\
		--device_path=${DEVICE_PATH}		\
		--data_path=${DATA_PATH}			\
		--multi_device=${MULTI_DEVICE}
	cd $DIR
fi

if [[ "${SHARED_POOL}" == "YES" ]]
then
	${SCRIPT_DIR}/run_shared_pool.sh		\
		--device_path=${DEVICE_PATH}		\
		--data_path=${DATA_PATH}			\
		--multi_device=${MULTI_DEVICE}
	cd $DIR
fi

if [[ "${MULTI_DEVICE}" == "YES" ]]
then
	${SCRIPT_DIR}/run_multi_device.sh		\
		--device_path=${DEVICE_PATH}		\
		--data_path=${DATA_PATH}			\
		--num_device=${NUM_DEVICE}
	cd $DIR
fi
