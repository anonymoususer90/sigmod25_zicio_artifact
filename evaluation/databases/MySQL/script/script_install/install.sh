#!/bin/bash

# Change to this-file-exist-path.
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"/
BASE_DIR=$DIR"../../../MySQL/"
LIB_HEADER_DIR=$BASE_DIR"../../../libzicio/include/"
LIB_DIR=$BASE_DIR"../../../libzicio/"
SRC_DIR=$BASE_DIR"mysql-server/"
INST_DIR=$BASE_DIR"mysql/"
DATA_DIR=/opt/nvme/zicio/data/
MYSQL_PORT=23456
BUILD_MODE="Release"
COMPILE_OPTION=""

cd $DIR

# Parse parameters
for i in "$@"
do
  case $i in
    --src-dir=*)
      SRC_DIR="${i#*=}"
      shift
      ;;

    --base-dir=*)
      BASE_DIR="${i#*=}"
      INST_DIR=$BASE_DIR"/mysql/"
      shift
      ;;

    --data-dir=*)
      DATA_DIR="${i#*=}"
      shift
      ;;

    --compile-option=*)
      COMPILE_OPTION="${i#*=}"
      shift
      ;;

    --gdb)
	  BUILD_MODE="Debug"
      shift
      ;;

    --mysql-port=*)
      MYSQL_PORT="${i#*=}"
      shift
      ;;

    --inst-dir=*)
      INST_DIR="${i#*=}"
      shift
      ;;

    *)
      # unknown option
      echo "Unknown option: ${i}"
      exit
      ;;
  esac
done

if [ ! -d ${INST_DIR} ] ; then
	mkdir -p ${INST_DIR}
fi

if [ ! -d ${DATA_DIR} ] ; then
	mkdir -p ${DATA_DIR}
fi

cd ${SRC_DIR}
CMAKE_BUILD_DIR=$SRC_DIR"build/"

if [ ! -d ${CMAKE_BUILD_DIR} ] ; then
	mkdir -p ${CMAKE_BUILD_DIR}
# else
# 	cd ${CMAKE_BUILD_DIR}
# 	make clean -j
# 	cd ${SRC_DIR}
# 	rm -rf ${CMAKE_BUILD_DIR}
# 	mkdir -p ${CMAKE_BUILD_DIR}
fi

cp $LIB_HEADER_DIR"libzicio.h" $SRC_CIR"include/"

cd ${CMAKE_BUILD_DIR}

cmake .. \
  -DDOWNLOAD_BOOST=1 \
  -DWITH_BOOST=${BASE_DIR}"boost/" \
  -DCMAKE_BUILD_TYPE=${BUILD_MODE} \
  -DWITH_SSL=system \
  -DWITH_ZLIB=bundled \
  -DMYSQL_MAINTAINER_MODE=0 \
  -DENABLED_LOCAL_INFILE=1 \
  -DENABLE_DTRACE=0 \
  -DCMAKE_CXX_FLAGS="-Wno-implicit-fallthrough -Wno-int-in-bool-context \
  -Wno-shift-negative-value -Wno-misleading-indentation \
  -Wno-format-overflow -Wno-nonnull -Wno-unused-function \
  -Wno-aligned-new -march=native \
  -Wno-invalid-offsetof \
  ${COMPILE_OPTION}" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_INSTALL_PREFIX=${INST_DIR} \
  -DMYSQL_UNIX_ADDR=${DATA_DIR}/mysql.sock \
  -DSYSCONFDIR=${INST_DIR} \
  -DMYSQL_DATADIR=${DATA_DIR} \
  -DINSTALL_DOCDIR=${INST_DIR} \
  -DINSTALL_INFODIR=${INST_DIR} \
  -DINSTALL_DOCREADMEDIR=${INST_DIR} \
  -DENABLE_DOWNLOADS=1 \
  -DWITH_ZICIO=1 \
  -DMYSQL_TCP_PORT=${MYSQL_PORT}

make -j $(nproc)
make install -j $(nproc)

cd ${DIR}
