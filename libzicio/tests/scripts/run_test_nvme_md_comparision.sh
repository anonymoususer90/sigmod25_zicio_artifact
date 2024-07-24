#!/bin/bash
DATA_PATH=/opt/md_nvme/zicio # file path to read data

# Make data files
if [ ! -e ${DATA_PATH}/data.0 ]; then
	echo "Make data files"
	../gen 2
	mv data ${DATA_PATH}/data.0
fi

for VAR in {1..7};
do
	if [ ! -e ${DATA_PATH}/data.${VAR} ]; then
		cp ${DATA_PATH}/data.0 ${DATA_PATH}/data.${VAR} &
	fi
done
wait


sudo sync
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'

# Ingestion flow one process shared test
echo "0. ingestion flow one proc shared test"
start=`date +%s.%N`
../multi_process_ingestion_flow_test ${DATA_PATH} 1
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

# Ingestion flow two process shared test
echo "1. ingestion flow two proc shared test"
start=`date +%s.%N`
../multi_process_ingestion_flow_test ${DATA_PATH} 2
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

# Ingestion flow four process shared test
echo "2. ingestion flow four proc shared test"

start=`date +%s.%N`
../multi_process_ingestion_flow_test ${DATA_PATH} 4
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

# Ingestion flow 8 process shared test
echo "3. ingestion flow 8 proc shared test"

start=`date +%s.%N`
../multi_process_ingestion_flow_test ${DATA_PATH} 8
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

# Ingestion flow sixteen process shared test
echo "4. ingestion flow 12 proc shared test"

start=`date +%s.%N`
../multi_process_ingestion_flow_test ${DATA_PATH} 12
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

# Ingestion flow sixteen process shared test
echo "5. ingestion flow 16 proc shared test"

start=`date +%s.%N`
../multi_process_ingestion_flow_test ${DATA_PATH} 16
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

# Ingestion flow sixteen process shared test
echo "6. ingestion flow 32 proc shared test"

start=`date +%s.%N`
../multi_process_ingestion_flow_test ${DATA_PATH} 32
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

# Ingestion flow sixteen process shared test
echo "7. ingestion flow 48 proc shared test"

start=`date +%s.%N`
../multi_process_ingestion_flow_test ${DATA_PATH} 48
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

# Ingestion flow sixteen process shared test
echo "8. ingestion flow 64 proc shared test"

start=`date +%s.%N`
../multi_process_ingestion_flow_test ${DATA_PATH} 64
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

# Ingestion flow sixteen process shared test
echo "9. ingestion flow 96 proc shared test"

start=`date +%s.%N`
../multi_process_ingestion_flow_test ${DATA_PATH} 96
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
