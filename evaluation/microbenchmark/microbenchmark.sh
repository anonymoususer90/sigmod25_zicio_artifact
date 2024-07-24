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

if [ ! -d ${DEVICE_PATH}/${DATA_PATH} ]; then
	echo -e "No such directory \"${DEVICE_PATH}/${DATA_PATH}\"(use options: --device-path, --data-path)"
	exit 1
fi

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" > /dev/null 2>&1 && pwd)"/
cd ${DIR}

LIB_DIR=../../libzicio
SCRIPTS_DIR=${LIB_DIR}/tests/scripts

if [ ! -e ${LIB_DIR}/libzicio.so ]; then
	cd ${LIB_DIR}
	make test
	cd ${DIR}
fi

if [ ! -e ${DEVICE_PATH}/${DATA_PATH}/data_seq.0 ]; then
	cd ${SCRIPTS_DIR}/../
	./gen_seq 10 8 ${DEVICE_PATH}/${DATA_PATH}
	cd ${DIR}
fi

./${SCRIPTS_DIR}/cgroup.sh

./${SCRIPTS_DIR}/run_test_seq_buffer_size.sh --device-path=${DEVICE_PATH} --data-path=${DATA_PATH} --result-path=${DIR}
./${SCRIPTS_DIR}/run_test_batched_buffer_size.sh --device-path=${DEVICE_PATH} --data-path=${DATA_PATH} --result-path=${DIR}
./${SCRIPTS_DIR}/run_test_multi_clients.sh --device-path=${DEVICE_PATH} --data-path=${DATA_PATH} --result-path=${DIR} --memory=small_16
./${SCRIPTS_DIR}/run_test_multi_clients.sh --device-path=${DEVICE_PATH} --data-path=${DATA_PATH} --result-path=${DIR} --memory=large_256

python3 data_reformatting_fig_6.py
python3 data_reformatting_fig_7.py
python3 data_reformatting_fig_10.py

gnuplot figure_6.gp
gnuplot figure_7.gp
gnuplot figure_10.gp