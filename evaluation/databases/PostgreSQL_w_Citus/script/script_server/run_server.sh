#!/bin/bash

# Change to this-file-exist-path.
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"/

# Default
BASE_DIR="$DIR""../../../PostgreSQL_w_Citus/"
DATA_DIR="/opt/nvme/zicio/PostgreSQL_w_Citus/data/"
LOGFILE=$BASE_DIR"logfile"

# Parse parameters.
for i in "$@"
do
case $i in
    -b=*|--base-dir=*)
    BASE_DIR="${i#*=}"
    shift
    ;;

    -d=*|--data-dir=*)
    DATA_DIR="${i#*=}"
    shift
    ;;

    -l=*|--logfile=*)
    LOGFILE="${i#*=}"
    shift
    ;;

    *)
          # unknown option
    ;;
esac
done

TARGET_DIR=$BASE_DIR"pgsql/"
BIN_DIR=$TARGET_DIR"bin/"
CONF_DIR=$BASE_DIR"config/"

export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$TARGET_DIR"lib"

rm -rf $LOGFILE

# server start
$BIN_DIR"pg_ctl" -D $DATA_DIR -l $LOGFILE start
