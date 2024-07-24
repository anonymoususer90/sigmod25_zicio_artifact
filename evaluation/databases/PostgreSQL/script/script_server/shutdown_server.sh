#!/bin/bash

# Change to this-file-exist-path.
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"/

# Default
BASE_DIR="$DIR""../../../PostgreSQL/"
BIN_DIR="$BASE_DIR"pgsql/bin/
DATA_DIR="/opt/nvme/zicio/PostgreSQL/data/"

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

    *)
          # unknown option
    ;;
esac
done

# server start
$BIN_DIR"pg_ctl" -D $DATA_DIR stop
