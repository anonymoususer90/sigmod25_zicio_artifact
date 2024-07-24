#!/bin/bash

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"/
BASE_DIR=$DIR"../../../MySQL/"
SRC_DIR="${BASE_DIR}/mysql-server"
BUILD_DIR="${SRC_DIR}/build"
CONFIG_FILE=${BASE_DIR}"config/my.cnf"
INST_DIR="${BASE_DIR}/mysql/"
DATA_DIR=/opt/nvme/zicio/data/
CUR_USER=$USER
PASSWORD="zicio"

# Parse parameters
for i in "$@"
do
  case $i in
    --base-dir=*)
      BASE_DIR="${i#*=}"
      CONFIG_FILE=${BASE_DIR}"/config/my.cnf"
	  INST_DIR="${BASE_DIR}/mysql/"
      shift
      ;;

    --data-dir=*)
      DATA_DIR="${i#*=}"
      shift
      ;;

    --inst-dir=*)
      INST_DIR="${i#*=}"
      shift
      ;;

    --pid-file=*)
      PID_FILE="${i#*=}"
      shift
      ;;

    --password=*)
      PASSWORD="${i#*=}"
      shift
      ;;

    -c=*|--config-file=*)
      CONFIG_FILE="${i#*=}"
      shift
      ;;

    *)
      # unknown option
      echo "Unknown option: ${i}"
      exit
      ;;
  esac
done

cd ${INST_DIR}

${INST_DIR}/bin/mysqladmin -uroot shutdown

rm -rf ${DATA_DIR}

${INST_DIR}/bin/mysqld --initialize-insecure --user=zicio --basedir=${INST_DIR} --datadir=${DATA_DIR} --port=23456 --lower_case_table_names=1

# server start
${INST_DIR}/bin/mysqld --defaults-file=${CONFIG_FILE} &

cd ${DIR}
