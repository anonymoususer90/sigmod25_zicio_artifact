#!/bin/bash
DATA_PATH="zicio"
DEVICE_PATH="/opt/nvme"
MULTI_DEVICE=NO
TEST_PATH=""
COMPARE=NO

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

	--multi-device)
		MULTI_DEVICE=YES
		shift
		;;

	*)
		# unknown option
		;;
esac
done

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" > /dev/null 2>&1 && pwd)"/
cd $DIR

if [[ ${MULTI_DEVICE} == "YES" ]]
then
	TEST_PATH="${DEVICE_PATH}1/${DATA_PATH}"
else
	TEST_PATH="${DEVICE_PATH}/${DATA_PATH}"
fi

# Simple ingestion single process shared test
echo "1. simple ingestion single process shared test"
start=`date +%s.%N`
../simple_ingestion_shared_single_proc_test ${TEST_PATH}
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
echo "2. ingestion flow single process shared test"
start=`date +%s.%N`
../ingestion_flow_shared_single_proc_test ${TEST_PATH} 
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
echo "3. customable chunk size single process shared test"
start=`date +%s.%N`
../custom_ingestion_shared_single_proc_test ${TEST_PATH} 
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
echo "4. ingestion flow shared single proc repeat test"
start=`date +%s.%N`
../ingestion_flow_shared_single_proc_repeat_test ${TEST_PATH} 
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
echo "5. simple ingestion two proc shared test"
start=`date +%s.%N`
../simple_ingestion_shared_test ${TEST_PATH} 2
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
echo "6. ingestion flow two proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test ${TEST_PATH} 2
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared two proc test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared two proc fail"
	exit 1
fi
echo ""

if [[ "${COMPARE}" == "YES" ]]
then
	start=`date +%s.%N`
	../multi_process_ingestion_flow_test ${TEST_PATH} 2
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
fi

# Ingestion flow four process shared test
echo "7. ingestion flow four proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test ${TEST_PATH} 4
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared four proc test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared four proc fail"
	exit 1
fi
echo ""

if [[ "${COMPARE}" == "YES" ]]
then
	start=`date +%s.%N`
	../multi_process_ingestion_flow_test ${TEST_PATH} 4
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
fi

# Ingestion flow 8 process shared test
echo "8. ingestion flow 8 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test ${TEST_PATH} 8
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared 8 proc test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared 8 proc fail"
	exit 1
fi
echo ""

if [[ "${COMPARE}" == "YES" ]]
then
	start=`date +%s.%N`
	../multi_process_ingestion_flow_test ${TEST_PATH} 8
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
fi

# Ingestion flow sixteen process shared test
echo "9. ingestion flow 12 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test ${TEST_PATH} 12
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared 12 proc test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared 12 proc fail"
	exit 1
fi
echo ""

if [[ "${COMPARE}" == "YES" ]]
then
	start=`date +%s.%N`
	../multi_process_ingestion_flow_test ${TEST_PATH} 12
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
fi

# Ingestion flow sixteen process shared test
echo "10. ingestion flow 16 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test ${TEST_PATH} 16
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared 16 proc test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared 16 proc fail"
	exit 1
fi
echo ""

if [[ "${COMPARE}" == "YES" ]]
then
	start=`date +%s.%N`
	../multi_process_ingestion_flow_test ${TEST_PATH} 16
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
fi

# Ingestion flow sixteen process shared test
echo "11. ingestion flow 32 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test ${TEST_PATH} 32
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared 32 proc test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared 32 proc fail"
	exit 1
fi
echo ""

if [[ "${COMPARE}" == "YES" ]]
then
	start=`date +%s.%N`
	../multi_process_ingestion_flow_test ${TEST_PATH} 32
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
fi

# Ingestion flow sixteen process shared test
echo "12. ingestion flow 48 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test ${TEST_PATH} 48
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared 32 proc test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared 32 proc fail"
	exit 1
fi
echo ""

if [[ "${COMPARE}" == "YES" ]]
then
	start=`date +%s.%N`
	../multi_process_ingestion_flow_test ${TEST_PATH} 48
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
fi


# Ingestion flow sixteen process shared test
echo "15. ingestion flow 64 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test ${TEST_PATH} 64
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared 32 proc test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared 32 proc fail"
	exit 1
fi
echo ""

if [[ "${COMPARE}" == "YES" ]]
then
	start=`date +%s.%N`
	../multi_process_ingestion_flow_test ${TEST_PATH} 64
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
fi

# Ingestion flow sixteen process shared test
echo "14. ingestion flow 2 proc shared multi pool test"
start=`date +%s.%N`
../ingestion_flow_shared_repeat_test_multiproc_multipool ${TEST_PATH} 2 1
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared 2 proc test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared 2 proc fail"
	exit 1
fi
echo ""

# Ingestion flow sixteen process shared test
echo "14. ingestion flow 16 proc shared multi pool test"
start=`date +%s.%N`
../ingestion_flow_shared_repeat_test_multiproc_multipool ${TEST_PATH} 16 1
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared repeat 16 proc test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared repeat 16 proc fail"
	exit 1
fi
echo ""

# Ingestion flow sixteen process shared test
echo "16. ingestion flow 32 proc shared multi pool test"
start=`date +%s.%N`
../ingestion_flow_shared_repeat_test_multiproc_multipool ${TEST_PATH} 32 1
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared repeat 32 proc test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared repeat 32 proc fail"
	exit 1
fi
echo ""

# Ingestion flow sixteen process shared test
echo "17. ingestion flow 2 proc shared repeat test"
start=`date +%s.%N`
../ingestion_flow_shared_repeat_test_multiproc ${TEST_PATH} 2
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared 4 proc test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared 4 proc fail"
	exit 1
fi
echo ""

# Ingestion flow sixteen process shared test
echo "18. ingestion flow 16 proc shared repeat test"
start=`date +%s.%N`
../ingestion_flow_shared_repeat_test_multiproc ${TEST_PATH} 16
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared repeat 16 proc test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared repeat 16 proc fail"
	exit 1
fi
echo ""

# Ingestion flow sixteen process shared test
echo "19. ingestion flow 32 proc shared repeat test"
start=`date +%s.%N`
../ingestion_flow_shared_repeat_test_multiproc ${TEST_PATH} 32
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared repeat 32 proc test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared repeat 32 proc fail"
	exit 1
fi
echo ""

# Ingestion flow sixteen process shared test
echo "20. ingestion flow 2 proc shared multi pool test"
start=`date +%s.%N`
../ingestion_flow_shared_repeat_test_multiproc_multipool ${TEST_PATH} 2 8
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared 2 proc test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared 2 proc fail"
	exit 1
fi
echo ""

# Ingestion flow sixteen process shared test
echo "21. ingestion flow 16 proc shared multi pool test"
start=`date +%s.%N`
../ingestion_flow_shared_repeat_test_multiproc_multipool ${TEST_PATH} 16 8
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared repeat 16 proc test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared repeat 16 proc fail"
	exit 1
fi
echo ""

# Ingestion flow sixteen process shared test
echo "22. ingestion flow 32 proc shared multi pool test"
start=`date +%s.%N`
../ingestion_flow_shared_repeat_test_multiproc_multipool ${TEST_PATH} 32 8
finish=`date +%s.%N`
diff=$( echo "${finish} - ${start}" | bc -l )

if [ $? == 0 ]; then
	echo "[SUCCESS] Ingestion flow shared repeat 32 proc test"
	echo 'ingestion flow shared test start:' ${start}
	echo 'ingestion flow shared test finish:' ${finish}
	echo 'ingestion flow shared test diff:' ${diff}
else
	echo "[FAILED] Ingestion flow shared repeat 32 proc fail"
	exit 1
fi
echo ""
