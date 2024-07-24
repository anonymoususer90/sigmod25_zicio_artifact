#!/bin/bash
DATA_PATH=/opt/nvme/zicio # file path to read data
NUM_CHILD_PROC=5 # number of child proccess for shared mode

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
# Ingestion flow two process shared test
echo "1. ingestion flow two proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test ${DATA_PATH} 2
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared two proc repeat test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared two proc repeat fail"
	exit 1
fi
echo ""

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
../ingestion_flow_shared_test ${DATA_PATH} 4
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared four proc repeat test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared four proc repeat fail"
	exit 1
fi
echo ""

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
../ingestion_flow_shared_test ${DATA_PATH} 8
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared 8 proc repeat test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared 8 proc repeat fail"
	exit 1
fi
echo ""

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
../ingestion_flow_shared_test ${DATA_PATH} 12
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared 12 proc repeat test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared 12 proc repeat fail"
	exit 1
fi
echo ""

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
../ingestion_flow_shared_test ${DATA_PATH} 16
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared 16 proc repeat test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared 16 proc repeat fail"
	exit 1
fi
echo ""

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
../ingestion_flow_shared_test ${DATA_PATH} 32
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared 32 proc repeat test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared 32 proc repeat fail"
	exit 1
fi
echo ""

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
echo "7. ingestion flow 64 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test ${DATA_PATH} 64
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared 64 proc repeat test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared 64 proc repeat fail"
	exit 1
fi
echo ""

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
