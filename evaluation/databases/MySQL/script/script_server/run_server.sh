#!/bin/bash

# Change to this-file-exist-path.
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"/
BASE_DIR=$DIR"../../../MySQL/"
INST_DIR=${BASE_DIR}"mysql/"
DATA_DIR=/opt/nvme/zicio/MySQL/data/
CUR_USER=$USER
CONFIG_FILE=${BASE_DIR}"config/my.cnf"

# Parse parameters.
for i in "$@"
do
case $i in
    -b=*|--base-dir=*)
    BASE_DIR="${i#*=}"
    INST_DIR="${BASE_DIR}mysql/"
    shift
    ;;

    -d=*|--data-dir=*)
    DATA_DIR="${i#*=}"
    shift
    ;;

    -i=*|--inst-dir=*)
    INST_DIR="${i#*=}"
    shift
    ;;

	-c=*|--config-file=*)
    CONFIG_FILE="${i#*=}"
    shift
    ;;


    *)
          # unknown option
    ;;
esac
done

BIN_DIR=$INST_DIR"bin/"

export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$INST_DIR"lib"

# server start
$BIN_DIR"mysqld" --defaults-file=${CONFIG_FILE} --gdb &
