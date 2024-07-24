#!/bin/bash
NUM_DATA_PATH=2
DATA_PATH=/opt/nvme0/zicio # file path to read data
DATA_PATH2=/opt/nvme1/zicio # file path to read data

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

# Open test
echo "1. open test"
start=`date +%s.%N`
../open_test
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] open test success"
	echo 'open test start:' ${start}
	echo 'open test finish:' ${finish}
	echo 'open test diff:' ${diff}
else
	echo "[FAILED] open test fail"
	exit 1
fi
echo ""


# Open stress test
echo "2. open stress test"
start=`date +%s.%N`
../open_stress_test
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] open stress test success"
	echo 'open stress test start:' ${start}
	echo 'open stress test finish:' ${finish}
	echo 'open stress test diff:' ${diff}
else
	echo "[FAILED] open stress test fail"
	exit 1
fi
echo ""

# Simple ingestion test
echo "3. simple ingestion test"
start=`date +%s.%N`
../simple_ingestion_test ${DATA_PATH}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] simple ingestion test success"
	echo 'simple ingestion start:' ${start}
	echo 'simple ingestion finish:' ${finish}
	echo 'simple ingestion diff:' ${diff}
else
	echo "[FAILED] simple ingestion test fail"
	exit 1
fi
echo ""

# Simple ingestion test
echo "3. simple ingestion test"
start=`date +%s.%N`
../simple_ingestion_test ${DATA_PATH}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] simple ingestion test success"
	echo 'simple ingestion start:' ${start}
	echo 'simple ingestion finish:' ${finish}
	echo 'simple ingestion diff:' ${diff}
else
	echo "[FAILED] simple ingestion test fail"
	exit 1
fi
echo ""

# Ingestion flow test
echo "4. ingestion flow test"
start=`date +%s.%N`
../ingestion_flow_test ${DATA_PATH}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] ingestion flow test success"
	echo 'ingestion flow start:' ${start}
	echo 'ingestion flow finish:' ${finish}
	echo 'ingestion flow diff:' ${diff}
else
	echo "[FAILED] ingestion flow test fail"
	exit 1
fi
echo ""


# Multiple ingestion test
echo "5. ingestion flow test with multi thread"
start=`date +%s.%N`
../multi_ingestion_test ${DATA_PATH}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] ingestion flow test with multi thread success"
	echo 'multi thread ingestion flow test start:' ${start}
	echo 'multi thread ingestion flow test finish:' ${finish}
	echo 'multi thread ingestion flow test diff:' ${diff}
else
	echo "[FAILED] ingestion flow test with multi thread fail"
	exit 1
fi
echo ""


# Multiple ingestion test
echo "6. ingestion flow test with multi device"
start=`date +%s.%N`
../multi_ingestion_one_device_per_channel_test $NUM_DATA_PATH ${DATA_PATH} ${DATA_PATH2}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] multi ingestion flow test with multi thread success"
	echo 'multi thread ingestion flow test start:' ${start}
	echo 'multi thread ingestion flow test finish:' ${finish}
	echo 'multi thread ingestion flow test diff:' ${diff}
else
	echo "[FAILED] multi ingestion flow test with multi thread fail"
	exit 1
fi
echo ""


# Customable chunk size test
echo "7. customable chunk size test"
start=`date +%s.%N`
../custom_ingestion_test ${DATA_PATH}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] customable chunk size test success"
	echo 'customable chunk size test start:' ${start}
	echo 'customable chunk size finish:' ${finish}
	echo 'customable chunk size diff:' ${diff}
else
	echo "[FAILED] customable chunk size test success fail"
	exit 1
fi
echo ""


# Make not aligned data
if [ ! -e ${DATA_PATH}/not_aligned_data.0 ]; then
	echo "Make not aligned data file"
	../gen_kb 16800
	mv data ${DATA_PATH}/not_aligned_data.0
fi

sudo sync
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'

# Not aligned data test
echo "8. not aligned data test"
start=`date +%s.%N`
../not_aligned_data_test ${DATA_PATH}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Not aligned data test test success"
	echo 'not aligned data start:' ${start}
	echo 'not aligned data finish:' ${finish}
	echo 'not aligned data diff:' ${diff}
else
	echo "[FAILED] Not aligned data test fail"
	exit 1
fi
echo ""


echo "9. regression test"
start=`date +%s.%N`
../regression_test ${DATA_PATH}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] regression test with multi thread success"
	echo 'regression test start:' ${start}
	echo 'regression test finish:' ${finish}
	echo 'regression test diff:' ${diff}
else
	echo "[FAILED] regression test with multi thread fail"
	exit 1
fi
echo ""

echo "10. multi process ingestion flow test"
start=`date +%s.%N`
../multi_process_ingestion_flow_test ${DATA_PATH} 40
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

# Simple ingestion single process shared test
echo "11. simple ingestion single process shared test"
start=`date +%s.%N`
../simple_ingestion_shared_single_proc_test ${DATA_PATH}
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] simple ingestion single process shared test success"
	echo 'simple ingestion single process shared test start:' ${start}
	echo 'simple ingestion single process shared test finish:' ${finish}
	echo 'simple ingestion single process shared test diff:' ${diff}
else
	echo "[FAILED] simple ingestion single process shared test fail"
	exit 1
fi
echo ""

# Ingestion flow single process shared test
echo "12. ingestion flow single process shared test"
start=`date +%s.%N`
../ingestion_flow_shared_single_proc_test ${DATA_PATH} 
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] ingestion flow single process shared test success"
	echo 'ingestion flow single process proc test start:' ${start}
	echo 'ingestion flow single process proc test finish:' ${finish}
	echo 'ingestion flow single process proc test diff:' ${diff}
else
	echo "[FAILED] ingestion flow single process shared test fail"
	exit 1
fi
echo ""

# Customable chunk size single process shared test
echo "13. customable chunk size single process shared test"
start=`date +%s.%N`
../custom_ingestion_shared_single_proc_test ${DATA_PATH} 
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] customable chunk size single process shared test success"
	echo 'customable chunk size single process shared test start:' ${start}
	echo 'customable chunk size single process shared test finish:' ${finish}
	echo 'customable chunk size single process shared test diff:' ${diff}
else
	echo "[FAILED] customable chunk size single process shared test success fail"
	exit 1
fi
echo ""

# Ingestion flow shared single process shared test
echo "14. ingestion flow shared single proc repeat test"
start=`date +%s.%N`
../ingestion_flow_shared_single_proc_repeat_test ${DATA_PATH} 
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared single proc repeat test"
	echo 'ingestion flow shared single proc repeat test start:' ${start}
	echo 'ingestion flow shared single proc repeat test finish:' ${finish}
	echo 'ingestion flow shared single proc repeat test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared single proc repeat fail"
	exit 1
fi
echo ""

# Simple ingestion two process shared test
echo "15. simple ingestion two proc shared test"
start=`date +%s.%N`
../simple_ingestion_shared_test ${DATA_PATH} 2
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared two proc repeat test"
	echo 'simple ingestion shared test start:' ${start}
	echo 'simple ingestion shared test finish:' ${finish}
	echo 'simple ingestion shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared two proc repeat fail"
	exit 1
fi
echo ""

# Ingestion flow two process shared test
echo "16. ingestion flow two proc shared test"
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
echo "17. ingestion flow four proc shared test"
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
echo "18. ingestion flow 8 proc shared test"
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
echo "19. ingestion flow 12 proc shared test"
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
echo "20. ingestion flow 16 proc shared test"
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
echo "21. ingestion flow 32 proc shared test"
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
echo "22. ingestion flow 64 proc shared test"
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
