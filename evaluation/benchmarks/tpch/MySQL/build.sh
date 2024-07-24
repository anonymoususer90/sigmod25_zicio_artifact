#!/bin/bash
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"/

BASE_DIR=${DIR}"../../../databases/MySQL/"
DATA_DIR=/opt/nvme/zicio/MySQL/data/
BIN_DIR="$INST_DIR"/bin
LIBRARY_DIR="$INST_DIR"/lib
PORT=23456
SCALE_FACTOR=1
THREAD_NUM=32
SOCKET=${DATA_DIR}"/mysql.sock"
PASSWORD="zicio"

# Parse parameters.
for i in "$@"
do
case $i in
    -b=*|--base-dir=*)
    BASE_DIR="${i#*=}"
	BIN_DIR="$BASE_DIR"mysql/bin
	LIBRARY_DIR="$BASE_DIR"mysql/lib
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

    -p=*|--password=*)
    PASSWORD="${i#*=}"
    shift
    ;;

    -i=*|--inst-dir=*)
    INST_DIR="${i#*=}"
	BIN_DIR="$INST_DIR"/bin
	LIBRARY_DIR="$INST_DIR"/lib
    shift
    ;;

	--data-dir=*)
	DATA_DIR="${i#*=}"
	shift
	;;

	--socket=*)
	SOCKET="${i#*=}"
	shift
	;;

    *)
          # unknown option
    ;;
esac
done

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
echo "$SOCKET" >> "$DIR"config.txt

# set shared library path
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:+LD_LIBRARY_PATH:}'$DIR'HammerDB-4.3/lib:$LIBRARY_DIR"

# create tpch database
$BIN_DIR/mysql --bind-address=localhost --port=$PORT --user=root --password=${PASSWORD} < "$DIR"sql/create_user.sql

# create tpch database
$BIN_DIR/mysql --user=root --password=${PASSWORD} --bind-address=localhost --port=$PORT < "$DIR"sql/tpch_create.sql

# drop tables if they exist since we might be running hammerdb multiple times with different configs
$BIN_DIR/mysql --user=tpch --password=tpch --bind-address=localhost --port=$PORT --database=tpch < "$DIR"sql/tpch_drop.sql

# build tpch tables
(cd "$DIR"HammerDB-4.3 && time ./hammerdbcli auto ../build.tcl | tee ../results/tpch_build.log)

cd -

# remote config file
rm -rf "$DIR"config.txt
