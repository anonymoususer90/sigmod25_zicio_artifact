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

# Ingestion flow sixteen process shared test
echo "0. ingestion flow 16 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test_with_page_num ${TEST_PATH} 16 0
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

# Ingestion flow sixteen process shared test
echo "1. ingestion flow 16 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test_with_page_num ${TEST_PATH} 16 1
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

# Ingestion flow sixteen process shared test
echo "2. ingestion flow 16 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test_with_page_num ${TEST_PATH} 16 2
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

# Ingestion flow sixteen process shared test
echo "3. ingestion flow 16 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test_with_page_num ${TEST_PATH} 16 4
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

# Ingestion flow sixteen process shared test
echo "4. ingestion flow 16 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test_with_page_num ${TEST_PATH} 16 8
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

# Ingestion flow sixteen process shared test
echo "4. ingestion flow 16 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test_with_page_num ${TEST_PATH} 16 16
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

# Ingestion flow sixteen process shared test
echo "4. ingestion flow 16 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test_with_page_num ${TEST_PATH} 16 32
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

# Ingestion flow sixteen process shared test
echo "5. ingestion flow 16 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test_with_page_num ${TEST_PATH} 16 64
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
echo "4. ingestion flow 16 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test_with_page_num ${TEST_PATH} 16 32
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

# Ingestion flow sixteen process shared test
echo "5. ingestion flow 16 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test_with_page_num ${TEST_PATH} 16 64
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
echo "5. ingestion flow 2 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test_with_page_num ${TEST_PATH} 16 128
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
echo "6. ingestion flow 2 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test_with_page_num ${TEST_PATH} 16 256
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
echo "7. ingestion flow 2 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test_with_page_num ${TEST_PATH} 2 0
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
echo "8. ingestion flow 2 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test_with_page_num ${TEST_PATH} 2 1
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
echo "9. ingestion flow 2 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test_with_page_num ${TEST_PATH} 2 2
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
echo "5. ingestion flow 2 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test_with_page_num ${TEST_PATH} 2 4
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
echo "5. ingestion flow 2 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test_with_page_num ${TEST_PATH} 2 8
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
echo "5. ingestion flow 2 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test_with_page_num ${TEST_PATH} 2 16
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
echo "9. ingestion flow 2 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test_with_page_num ${TEST_PATH} 2 32
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
echo "5. ingestion flow 2 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test_with_page_num ${TEST_PATH} 2 64
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
echo "5. ingestion flow 2 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test_with_page_num ${TEST_PATH} 2 128
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
echo "5. ingestion flow 2 proc shared test"
start=`date +%s.%N`
../ingestion_flow_shared_test_with_page_num ${TEST_PATH} 2 256
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
