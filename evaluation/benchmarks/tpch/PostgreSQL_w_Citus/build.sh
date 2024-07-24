#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"/

BASE_DIR=/opt/nvme/zicio/PostgreSQL_w_Citus/
PORT=12345
SCALE_FACTOR=1
THREAD_NUM=4

# Parse parameters.
for i in "$@"
do
case $i in
    -b=*|--base-dir=*)
    BASE_DIR="${i#*=}"
    shift
    ;;

    -p=*|--port=*)
    PORT="${i#*=}"
    shift
    ;;

    -s=*|--scale-factor=*)
    SCALE_FACTOR="${i#*=}"
    shift
    ;;

    -t=*|--thread-num=*)
    THREAD_NUM="${i#*=}"
    shift
    ;;

    *)
          # unknown option
    ;;
esac
done

BIN_DIR="$BASE_DIR"pgsql/bin
LIBRARY_DIR="$BASE_DIR"pgsql/lib

# fail if trying to reference a variable that is not set.
set -u
# exit immediately if a command fails
set -e
# echo commands
set -x

# set port and scale factor
echo "$PORT" > "$DIR"config.txt
echo "$SCALE_FACTOR" >> "$DIR"config.txt
echo "$THREAD_NUM" >> "$DIR"config.txt

# set shared library path
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:+LD_LIBRARY_PATH:}'$DIR'HammerDB-4.3/lib:$LIBRARY_DIR"

# create tpch database
$BIN_DIR/psql -h localhost -p $PORT -d postgres -f "$DIR"sql/create_user.sql

# create tpch database
$BIN_DIR/psql -U tpch -h localhost -p $PORT -d postgres -f "$DIR"sql/tpch_create.sql

# create citus extension
$BIN_DIR/psql -U tpch -h localhost -p $PORT -d tpch -f "$DIR"sql/extension_create.sql

# drop tables if they exist since we might be running hammerdb multiple times with different configs
$BIN_DIR/psql -U tpch -h localhost -p $PORT -d tpch -f "$DIR"sql/tpch_drop.sql

# build tpch tables
(cd "$DIR"HammerDB-4.3 && time ./hammerdbcli auto ../build.tcl | tee ../results/tpch_build.log)

cd -

# remote config file
rm -rf "$DIR"config.txt
