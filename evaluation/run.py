import sys, subprocess
import time, threading
import os, random
import argparse
import pathlib
import json
import string
from datetime import datetime
from dataclasses import dataclass

import benchmarks.tpch.PostgreSQL.query as postgresql_query
import benchmarks.tpch.MySQL.procs as mysql_procs
# import benchmarks.tpch.MySQL.query as mysql_query

"""
Evaluation 1: Single query execution (Q1 ~ Q22)
Evaluation 2: Concurrent query execution
                - (Q1, Q6, Q12, Q14, Q15 for MySQL)
                - (Q1, Q6, Q7, Q12, Q14, Q15 for PostgreSQL)
Evaluation 3: Only for PostgreSQL (for figure9)
"""

# MySQL
import mysql.connector
from mysql.connector import errorcode
import pickle

# PostgreSQL
import psycopg2

BEGIN_TIME=0

# Run type
RUN_MODE=-1 # 0: vanilla(large memory), 1: vanilla(small memory), 2: zicio
RUN_DB=0 # 0: MySQL, 1: PostgreSQL, 2: Citus
RUN_EVALUATION=0 # 0: single process with local
                 # 1: multi process with local
                 # 2: multi process with sharing
                 
# Evaluation One
EVALUATION_ONE_TARGET_QUERY_MYSQL = [i for i in range(1, 23)]

# Evaluation Two
EVALUATION_TWO_START=False
EVALUATION_TWO_PROGRESS=[]
EVALUATION_TWO_LOCK = threading.Lock()
EVALUATION_TWO_FINISH_COUNTER=0
EVALUATION_TWO_TARGET_QUERY_MYSQL = [1, 6, 12, 14, 15] # [1, 6, 12, 14, 15]
EVALUATION_TWO_QUERY_NUM_MYSQL = len(EVALUATION_TWO_TARGET_QUERY_MYSQL) // 2
EVALUATION_TWO_TARGET_QUERY_POSTGRESQL = [1, 6, 7, 12, 14, 15]
EVALUATION_TWO_QUERY_NUM_POSTGRESQL = len(EVALUATION_TWO_TARGET_QUERY_POSTGRESQL) // 2

# Evaluation Three
EVALUATION_THREE_START=False
EVALUATION_THREE_PROGRESS={}
EVALUATION_THREE_LOCK = threading.Lock()
EVALUATION_THREE_FINISH_COUNTER=0

# Mode
VANILLA_SMALL=0
VANILLA_LARGE=1
ZICIO_SMALL=2
ZICIO_LARGE=3

# Evaluation number
EVALUATION_ONE=0 # single process with local
EVALUATION_TWO=1 # multi process with local
EVALUATION_THREE=2 # 5.4 TPC-H Workloads with Stream Sharing

# Cgroup mode
VANILLA_SMALL=0
VANILLA_LARGE=1
ZICIO_SMALL=2
ZICIO_LARGE=3

# Database
MYSQL=0
POSTGRESQL=1
CITUS=2
BOTH=3

RESULT_BASE=""
RESULT_DIR=""
QUERY_PARAMS = dict()
QUERIES = dict()
QUERIES_DICT={}
PROCS_ARGS=[["null"]]

# DB name
DB=["MySQL/", "PostgreSQL/", "PostgreSQL_w_Citus/"]
DB_NAME=["MySQL", "PostgreSQL", "Citus"]
DB_BASE=[f"{pathlib.Path('.').absolute()}/databases/" + db for db in DB]
DB_INSTALL_SCRIPT_FILE="install.sh"
DB_INSTALL_CITUS_SCRIPT_FILE="install_citus.sh"

# TODO: Get device path dynamically
# NVMe device base
DEVICE_PATH="/opt/nvme/zicio"
DEVICE_BASE=[DEVICE_PATH + db for db in DB]

# DB data directroy
DB_DATA=["" for base in DEVICE_BASE]
DB_DATA_BACKUP=["" for base in DEVICE_BASE]

# DB config directroy
DB_CONFIG=[base + "config/" for base in DB_BASE]

# DB script directory
DB_SCRIPT=[base + "script/" for base in DB_BASE]
DB_SERVER_SCRIPT=[base + "script_server/" for base in DB_SCRIPT]
DB_CLIENT_SCRIPT=[base + "script_client/" for base in DB_SCRIPT]
DB_INSTALL_SCRIPT=[base + "script_install/" for base in DB_SCRIPT]
DB_CONFIG_FILE=["conf-vanilla-small", "conf-vanilla-large", "conf-zicio-small", "conf-zicio-large"]
DB_CONFIG_FILE_MY=["my-small-vanilla.cnf", "my-large-vanilla.cnf", "my-small.cnf", "my-large.cnf"]

# TPC-H benchmark directory
BENCHMARK_TPCH=["./benchmarks/tpch/" + db for db in DB] 

# Cgroup memory setting diectory
CGROUP="/sys/fs/cgroup/memory/"

@dataclass
class DBExecTime:
    execution_time: float = 0
    seq_io_time: float = 0
    rand_io_time: float = 0
    zicio_wait_time: float = 0
    io_time: float = 0

class Client(threading.Thread):
    def __init__(self, client_no, args, query_no, zicio, zicio_shared):
        threading.Thread.__init__(self)
        self.client_no = client_no
        self.query_no = query_no
        self.args = args
        self.zicio = zicio
        self.zicio_shared = zicio_shared

        # Connect to database server
        self.make_connector()

    def run_postgres_eval1(self):    
        """
        Evaluation One
        """

        global QUERIES
        if self.zicio:
            self.cursor.execute('SELECT pg_enable_zicio()')
            if self.zicio_shared:
                self.cursor.execute('SELECT pg_enable_zicio_shared_pool()')


        # Start query
        query_no = self.query_no[0]
        if query_no != 15:
            self.cursor.execute(QUERIES[query_no])
            self.cursor.fetchall()
        else:
            self.cursor.execute(QUERIES["15_create"].replace("revenue","revenue" + str(self.client_no)))
            self.cursor.execute(QUERIES[query_no].replace("revenue","revenue" + str(self.client_no)))
            self.cursor.fetchall()
            self.cursor.execute(QUERIES["15_destroy"].replace("revenue","revenue" + str(self.client_no)))

        if self.zicio:
            self.cursor.execute('SELECT pg_disable_zicio()')
            if self.zicio_shared:
                self.cursor.execute('SELECT pg_disable_zicio_shared_pool()')

    def run_mysql_eval1(self):
        """
        Evaluation One
        """
        global PROCS_ARGS
        query_no = self.query_no[0]

        if self.zicio:
            self.cursor.execute('SET enable_zicio=ON;')
            if self.zicio_shared:
                self.cursor.execute(f'SET zicio_shared_pool_num={query_no};')

        # Start query
        try:
            self.cursor.execute('SET SESSION enable_zicio_stat = ON;')
            # self.cursor.fetchall()
            self.cursor.callproc(f"Q{query_no}", PROCS_ARGS[query_no])
            for result in self.cursor.stored_results():
                result.fetchall()
            self.cursor.execute('SET SESSION enable_zicio_stat = OFF;')
            # self.cursor.fetchall()
        except mysql.connector.Error as e:
            print("Error:", e)                      # errno, sqlstate, msg values

        if self.zicio:
            self.cursor.execute('SET enable_zicio=OFF;')
            if self.zicio_shared:
                self.cursor.execute(f'SET zicio_shared_pool_num=0;')
        
    def run_postgres_eval2(self):
        """
        Evaluation Two
        """
        global EVALUATION_TWO_START
        global EVALUATION_TWO_PROGRESS
        global EVALUATION_TWO_LOCK
        global EVALUATION_TWO_FINISH_COUNTER
        global QUERIES

        if self.zicio:
            self.cursor.execute('SELECT pg_enable_zicio()')
            if self.zicio_shared:
                self.cursor.execute('SELECT pg_enable_zicio_shared_pool()')
        
        # Set progress string
        progress = EVALUATION_TWO_PROGRESS[self.client_no]

        # Wait until start
        while EVALUATION_TWO_START is False:
            pass

        # Run queries
        for query_num in self.query_no:
            if query_num != 15:
                self.cursor.execute(QUERIES[query_num])
                self.cursor.fetchall()
            else:
                self.cursor.execute(QUERIES["15_create"].replace("revenue","revenue" + str(self.client_no)))
                self.cursor.execute(QUERIES[query_num].replace("revenue","revenue" + str(self.client_no)))
                self.cursor.fetchall()
                self.cursor.execute(QUERIES["15_destroy"].replace("revenue","revenue" + str(self.client_no)))
            progress += f"Q{query_num:<2}, "
            EVALUATION_TWO_PROGRESS[self.client_no] = progress
        progress += f"Complete"
        EVALUATION_TWO_PROGRESS[self.client_no] = progress
        
        if self.zicio:
            self.cursor.execute('SELECT pg_disable_zicio()')
            if self.zicio_shared:
                self.cursor.execute('SELECT pg_disable_zicio_shared_pool()')

        # Increment counter
        EVALUATION_TWO_LOCK.acquire()
        EVALUATION_TWO_FINISH_COUNTER += 1   
        EVALUATION_TWO_LOCK.release()   
 
    def run_mysql_eval2(self):
        """
        Evaluation Two
        """
        global EVALUATION_TWO_START
        global EVALUATION_TWO_PROGRESS
        global EVALUATION_TWO_LOCK
        global EVALUATION_TWO_FINISH_COUNTER
        global QUERIES

        if self.zicio or self.zicio_shared:
            self.cursor.execute('SET enable_zicio=ON;')
        
        # Set progress string
        progress = EVALUATION_TWO_PROGRESS[self.client_no]

        # Wait until start
        while EVALUATION_TWO_START is False:
            pass

        # Run queries
        len_query_no = len(self.query_no)
        start_idx = self.client_no % len_query_no
        for i in range(len_query_no):
            query_num = self.query_no[(start_idx + i) % len_query_no]
            if self.zicio_shared:
                self.cursor.execute(f'SET zicio_shared_pool_num={query_num};')
                
            try:
                self.cursor.execute('SET enable_zicio_stat=ON;')
                self.cursor.callproc(f"Q{query_num}", PROCS_ARGS[query_num])
                for result in self.cursor.stored_results():
                    result.fetchall()
                self.cursor.execute('SET enable_zicio_stat=OFF;')
            except mysql.connector.Error as e:
                print("Error:", e)                      # errno, sqlstate, msg values

            progress += f"Q{query_num:<2}, "
            EVALUATION_TWO_PROGRESS[self.client_no] = progress
        progress += f"Complete"
        EVALUATION_TWO_PROGRESS[self.client_no] = progress
        
        if self.zicio:
            self.cursor.execute('SET enable_zicio=OFF;')
            if self.zicio_shared:
                self.cursor.execute('SET zicio_shared_pool_num=0;')

        # Increment counter
        EVALUATION_TWO_LOCK.acquire()
        EVALUATION_TWO_FINISH_COUNTER += 1   
        EVALUATION_TWO_LOCK.release()   

    def run_postgres_eval3(self):
        """
        Evaluation Three
        """
        global EVALUATION_THREE_START
        global EVALUATION_THREE_PROGRESS
        global EVALUATION_THREE_LOCK
        global EVALUATION_THREE_FINISH_COUNTER
        global QUERIES

        # Set target query number
        query_num = self.query_no

        if self.zicio:
            self.cursor.execute('SELECT pg_enable_zicio()')
        if self.zicio_shared:
            self.cursor.execute(f'SELECT pg_enable_zicio_shared_pool({query_num})')

        progress = f"[Thread {self.client_no:>2}] "
        EVALUATION_THREE_PROGRESS[self.client_no] = progress

        # Wait until start
        while EVALUATION_THREE_START is False:
            pass

        # Run queries
        if query_num != 15:
            self.cursor.execute(QUERIES[query_num])
            self.cursor.fetchall()
        else:
            self.cursor.execute(QUERIES["15_create"].replace("revenue","revenue" + str(self.client_no)))
            self.cursor.execute(QUERIES[query_num].replace("revenue","revenue" + str(self.client_no)))
            self.cursor.fetchall()
            self.cursor.execute(QUERIES["15_destroy"].replace("revenue","revenue" + str(self.client_no)))

        progress += f"Q{query_num:<2}, "
        EVALUATION_THREE_PROGRESS[self.client_no] = progress

        progress += f"Complete"
        EVALUATION_THREE_PROGRESS[self.client_no] = progress
        
        if self.zicio:
            self.cursor.execute('SELECT pg_disable_zicio()')
        if self.zicio_shared:
            self.cursor.execute('SELECT pg_disable_zicio_shared_pool()')

        # Increment counter
        EVALUATION_THREE_LOCK.acquire()
        EVALUATION_THREE_FINISH_COUNTER += 1   
        EVALUATION_THREE_LOCK.release()   

    def run_mysql(self):
        global RUN_EVALUATION
        if RUN_EVALUATION == EVALUATION_ONE:
            self.run_mysql_eval1()
        elif RUN_EVALUATION == EVALUATION_TWO:
            self.run_mysql_eval2()

    def run_postgres(self):
        global RUN_EVALUATION
        if RUN_EVALUATION == EVALUATION_ONE:
            self.run_postgres_eval1()
        elif RUN_EVALUATION == EVALUATION_TWO:
            self.run_postgres_eval2()
        elif RUN_EVALUATION == EVALUATION_THREE:
            self.run_postgres_eval3()

    def run(self):
        global RUN_DB

        query_results=[]
        time.sleep(5)

        if RUN_DB == MYSQL:
            self.run_mysql()
        elif RUN_DB == POSTGRESQL or RUN_DB == CITUS:
            self.run_postgres()

        self.cursor.close()
        self.db.close()

    def make_connector(self):
        global RUN_DB

        time.sleep(1)
        if RUN_DB == MYSQL:
            self.db = mysql.connector.connect(
                    host=self.args.mysql_host,
                    user=self.args.mysql_user,
                    database="tpch",
                    password=self.args.db_password,
                    port=self.args.mysql_port,
                    autocommit = True)
            self.cursor = self.db.cursor()
        else:
            self.db = psycopg2.connect(
                    host=self.args.pgsql_host,
                    dbname=self.args.pgsql_db,
                    user=self.args.pgsql_user,
                    port=self.args.pgsql_port)
            self.cursor = self.db.cursor()

    def execute_query(self, target_query_number):
        if target_query_number != 15:
            self.cursor.execute(QUERIES[target_query_number])
            self.cursor.fetchall()
        else:
            self.cursor.execute(QUERIES["15_create"].replace("revenue","revenue" + str(self.client_no)))
            self.cursor.execute(QUERIES[target_query_number].replace("revenue","revenue" + str(self.client_no)))
            self.cursor.fetchall()
            self.cursor.execute(QUERIES["15_destroy"].replace("revenue","revenue" + str(self.client_no)))

def compile_database_mysql(compile_option=""):
    global RUN_DB
    global DB_INSTALL_SCRIPT

    print("Compile database start")
    
    base_dir = f"--base-dir={DB_BASE[RUN_DB]}"
    data_dir = f"--data-dir={DB_DATA[RUN_DB][0]}"
    db_compile_option = f"--compile-option={compile_option}"

    subprocess.run(args=[DB_INSTALL_SCRIPT[RUN_DB] + DB_INSTALL_SCRIPT_FILE,
        base_dir, data_dir, db_compile_option], check=True)
    print("Compile database finish")

    print("Compile vanilla database start")

    data_dir = f"--data-dir={DB_DATA[RUN_DB][1]}"
    inst_dir = f"--inst-dir={DB_BASE[RUN_DB]}/mysql_vanilla/"
    db_compile_option = f"--compile-option=-D__ZICIO_STAT"

    subprocess.run(args=[DB_INSTALL_SCRIPT[RUN_DB] + DB_INSTALL_SCRIPT_FILE,
        base_dir, data_dir, inst_dir, db_compile_option], check=True)

    print("Compile vanilla database finish")

def compile_database_postgres(compile_option=""):
    global RUN_DB
    global DB_INSTALL_SCRIPT

    print("Compile database start")
    
    base_dir = f"--base-dir={DB_BASE[RUN_DB]}"
    db_compile_option = f"--compile-option={compile_option}"

    subprocess.run(args=[DB_INSTALL_SCRIPT[RUN_DB] + DB_INSTALL_SCRIPT_FILE,
        base_dir, db_compile_option], 
        check=True)

    if RUN_DB == CITUS:
        # Compile citus
        subprocess.run(args=[DB_INSTALL_SCRIPT[RUN_DB] + DB_INSTALL_CITUS_SCRIPT_FILE,
            base_dir, db_compile_option], 
            check=True)

    print("Compile database finish")

def compile_database(compile_option=""):
    if RUN_DB == MYSQL:
        compile_database_mysql(compile_option)
    elif RUN_DB == POSTGRESQL or RUN_DB == CITUS:
        compile_database_postgres(compile_option)

def drop_os_caches(args, print_log=True):
    if print_log is True:
        print("Drop OS caches")
    # Drop caches
    command = f"sh -c 'echo 3 > /proc/sys/vm/drop_caches'"
    subprocess.check_call('echo {} | sudo -S {}'.format(args.user_password, command), shell=True)

def run_server_postgres(print_log=True):
    global RUN_DB
    global DB_SERVER_SCRIPT

    if print_log is True:
        print("Run server start")

    base_dir = f"--base-dir={DB_BASE[RUN_DB]}"
    data_dir = f"--data-dir={DB_DATA[RUN_DB]}"
    logfile = f"--logfile={DB_BASE[RUN_DB] + 'logfile'}"

    subprocess.run(args=[DB_SERVER_SCRIPT[RUN_DB] + "run_server.sh",
        base_dir, data_dir, logfile], 
        #stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
        check=True)
    time.sleep(1)
    if print_log is True:
        print("Run server finish")

def run_server_mysql(print_log=True, vanilla=False):
    global RUN_DB
    global DB_SERVER_SCRIPT
    global RUN_MODE

    if print_log is True:
        print("Run server start")

    if vanilla:
        inst_dir = f"--inst-dir={DB_BASE[RUN_DB]}/mysql_vanilla/"
        data_dir = f"--data-dir={DB_DATA[RUN_DB][1]}"
        if RUN_MODE == -1:
            config_file = f"--config-file={DB_CONFIG[RUN_DB] + DB_CONFIG_FILE_MY[1]}"
        else:
            config_file = f"--config-file={DB_CONFIG[RUN_DB] + DB_CONFIG_FILE_MY[RUN_MODE]}"

        subprocess.run(args=[DB_SERVER_SCRIPT[RUN_DB] + "run_server.sh",
            data_dir, inst_dir, config_file],
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, check=True)
        time.sleep(10)
    else:
        base_dir = f"--base-dir={DB_BASE[RUN_DB]}"
        data_dir = f"--data-dir={DB_DATA[RUN_DB][0]}"
        if RUN_MODE == -1:
            config_file = f"--config-file={DB_CONFIG[RUN_DB] + DB_CONFIG_FILE_MY[3]}"
        else:
            config_file = f"--config-file={DB_CONFIG[RUN_DB] + DB_CONFIG_FILE_MY[RUN_MODE]}"

        subprocess.run(args=[DB_SERVER_SCRIPT[RUN_DB] + "run_server.sh",
            base_dir, data_dir, config_file],
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, check=True)
        time.sleep(10)

    if print_log is True:
        print("Run server finish")

def run_server(print_log=True, vanilla=False):
    global RUN_DB
    if RUN_DB == POSTGRESQL or RUN_DB == CITUS:
        run_server_postgres(print_log)
    elif RUN_DB == MYSQL:
        run_server_mysql(print_log, vanilla=vanilla)

def shutdown_server_postgres(print_log=True):
    global RUN_DB
    global DB_SERVER_SCRIPT

    time.sleep(5)
    if print_log is True:
        print("Shutdown server start")

    base_dir = f"--base-dir={DB_BASE[RUN_DB]}"
    data_dir = f"--data-dir={DB_DATA[RUN_DB]}"

    subprocess.run(args=[DB_SERVER_SCRIPT[RUN_DB] + "shutdown_server.sh",
        base_dir, data_dir], 
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
        check=True)
    time.sleep(10)
    if print_log is True:
        print("Shutdown server finish")

def shutdown_server_mysql(args, print_log=True, vanilla=False):
    global RUN_DB
    global DB_SERVER_SCRIPT

    time.sleep(5)
    if print_log is True:
        print("Shutdown server start")

    if not vanilla:
        base_dir = f"--base-dir={DB_BASE[RUN_DB]}"
        password=f"--password={args.db_password}"

        subprocess.run(args=[DB_SERVER_SCRIPT[RUN_DB] + "shutdown_server.sh", base_dir, password],
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
            check=True)
    else:
        base_dir = f"--base-dir={DB_BASE[RUN_DB]}"
        inst_dir = f"--inst-dir={DB_BASE[RUN_DB]}/mysql_vanilla/"
        password=f"--password={args.db_password}"

        subprocess.run(args=[DB_SERVER_SCRIPT[RUN_DB] + "shutdown_server.sh", base_dir, inst_dir, password],
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
            check=True)
    time.sleep(5)
    if print_log is True:
        print("Shutdown server finish")

def shutdown_server(args, print_log=True, vanilla=False):
    global RUN_DB
    if RUN_DB == POSTGRESQL or RUN_DB == CITUS:
        shutdown_server_postgres(print_log)
    elif RUN_DB == MYSQL:
        shutdown_server_mysql(args, print_log, vanilla)

def init_config_file_postgres(args):
    return

def init_config_file_mysql(args):
    global DB_BASE
    global DB_DATA
    global RUN_DB
    global DB_CONFIG

    new_cnf_lines = []
    new_vanilla_cnf_lines = []
    with open(DB_CONFIG[RUN_DB] + "my-small.cnf", 'r') as my_cnf:
        old_cnf_lines = my_cnf.readlines()
        for cnf_line in old_cnf_lines:
            if "basedir" in cnf_line:
                new_cnf_lines.append(f"basedir={DB_BASE[RUN_DB]}/mysql\n")
                new_vanilla_cnf_lines.append(f"basedir={DB_BASE[RUN_DB]}/mysql_vanilla\n")
            elif "datadir" in cnf_line:
                new_cnf_lines.append(f"datadir={DB_DATA[RUN_DB][0]}\n")
                new_vanilla_cnf_lines.append(f"datadir={DB_DATA[RUN_DB][1]}\n")
            elif "socket" in cnf_line:
                new_cnf_lines.append(f"socket={DB_DATA[RUN_DB][0]}/mysql.sock\n")
                new_vanilla_cnf_lines.append(f"socket={DB_DATA[RUN_DB][1]}/mysql.sock\n")
            elif "general_log_file" in cnf_line:
                new_cnf_lines.append(f"general_log_file={DB_DATA[RUN_DB][0]}/general.log\n")
                new_vanilla_cnf_lines.append(f"general_log_file={DB_DATA[RUN_DB][1]}/general.log\n")
            elif "log_error" in cnf_line:
                new_cnf_lines.append(f"log_error={DB_DATA[RUN_DB][0]}/error.log\n")
                new_vanilla_cnf_lines.append(f"log_error={DB_DATA[RUN_DB][1]}/error.log\n")
            else:
                new_cnf_lines.append(cnf_line)
                new_vanilla_cnf_lines.append(cnf_line)

    with open(DB_CONFIG[RUN_DB] + "my-small.cnf", 'w') as new_my_cnf:
        for cnf_line in new_cnf_lines:
            new_my_cnf.write(cnf_line)

    with open(DB_CONFIG[RUN_DB] + "my-small-vanilla.cnf", 'w') as new_my_cnf:
        for cnf_line in new_vanilla_cnf_lines:
            new_my_cnf.write(cnf_line)

    new_cnf_lines = []
    new_vanilla_cnf_lines = []
    with open(DB_CONFIG[RUN_DB] + "my-large.cnf", 'r') as my_cnf:
        old_cnf_lines = my_cnf.readlines()
        for cnf_line in old_cnf_lines:
            if "basedir" in cnf_line:
                new_cnf_lines.append(f"basedir={DB_BASE[RUN_DB]}/mysql\n")
                new_vanilla_cnf_lines.append(f"basedir={DB_BASE[RUN_DB]}/mysql_vanilla\n")
            elif "datadir" in cnf_line:
                new_cnf_lines.append(f"datadir={DB_DATA[RUN_DB][0]}\n")
                new_vanilla_cnf_lines.append(f"datadir={DB_DATA[RUN_DB][1]}\n")
            elif "socket" in cnf_line:
                new_cnf_lines.append(f"socket={DB_DATA[RUN_DB][0]}/mysql.sock\n")
                new_vanilla_cnf_lines.append(f"socket={DB_DATA[RUN_DB][1]}/mysql.sock\n")
            elif "general_log_file" in cnf_line:
                new_cnf_lines.append(f"general_log_file={DB_DATA[RUN_DB][0]}/general.log\n")
                new_vanilla_cnf_lines.append(f"general_log_file={DB_DATA[RUN_DB][1]}/general.log\n")
            elif "log_error" in cnf_line:
                new_cnf_lines.append(f"log_error={DB_DATA[RUN_DB][0]}/error.log\n")
                new_vanilla_cnf_lines.append(f"log_error={DB_DATA[RUN_DB][1]}/error.log\n")
            else:
                new_cnf_lines.append(cnf_line)
                new_vanilla_cnf_lines.append(cnf_line)

    with open(DB_CONFIG[RUN_DB] + "my-large.cnf", 'w') as new_my_cnf:
        for cnf_line in new_cnf_lines:
            new_my_cnf.write(cnf_line)

    with open(DB_CONFIG[RUN_DB] + "my-large-vanilla.cnf", 'w') as new_my_cnf:
        for cnf_line in new_vanilla_cnf_lines:
            new_my_cnf.write(cnf_line)



def init_config_file(args):
    global RUN_DB
    if RUN_DB == POSTGRESQL:
        init_config_file_postgres(args)
    elif RUN_DB == MYSQL:
        init_config_file_mysql(args)

def init_server(args, print_log=True):
    global RUN_DB
    global RUN_MODE
    global DB_SERVER_SCRIPT
    global DB_CONFIG
    global DB_CONFIG_FILE
    global DB_DATA
    global DB_NAME

    if print_log is True:
        print("Init server start")

    base_dir = f"--base-dir={DB_BASE[RUN_DB]}"

    # Get configuration files for tests
    if RUN_DB == POSTGRESQL or RUN_DB == CITUS:
        data_dir = f"--data-dir={DB_DATA[RUN_DB]}"

        # Initialize a data directory
        subprocess.run(args=[DB_SERVER_SCRIPT[RUN_DB] + "init_server.sh",
            base_dir, data_dir],
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
            check=True)
    elif RUN_DB == MYSQL:
        data_dir = f"--data-dir={DB_DATA[RUN_DB][0]}"
        config_file = f"--config-file={DB_CONFIG[RUN_DB]}" + "my-large.cnf"

        # Initialize a data directory
        subprocess.run(args=[DB_SERVER_SCRIPT[RUN_DB] + "init_server.sh",
            base_dir, data_dir, config_file],
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
            check=True)
        os.system("cp " + DB_CONFIG[RUN_DB] + "my-large.cnf" + " " + DB_DATA[RUN_DB][0])

        time.sleep(10)
        db = mysql.connector.connect(host=args.mysql_host, 
                user=args.mysql_user, port=args.mysql_port, autocommit=True)
        cursor = db.cursor()
        change_passwd=f"ALTER USER 'root'@'localhost' IDENTIFIED BY '{args.db_password}';"
        cursor.execute(change_passwd);
        cursor.fetchall()
        cursor.close()
        db.close()
        shutdown_server(args, print_log=False, vanilla=False)

        inst_dir = f"--inst-dir={DB_BASE[RUN_DB]}/mysql_vanilla/"
        data_dir = f"--data-dir={DB_DATA[RUN_DB][1]}"
        config_file = f"--config-file={DB_CONFIG[RUN_DB]}" + "my-large-vanilla.cnf"
        # Initialize a data directory
        subprocess.run(args=[DB_SERVER_SCRIPT[RUN_DB] + "init_server.sh",
            data_dir, inst_dir, config_file],
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
            check=True)
        os.system("cp " + DB_CONFIG[RUN_DB] + "my-large-vanilla.cnf" + " " +
            DB_DATA[RUN_DB][1] + "/my.cnf")

        time.sleep(10)
        db = mysql.connector.connect(host=args.mysql_host, 
                user=args.mysql_user, port=args.mysql_port, autocommit=True)
        cursor = db.cursor()
        change_passwd=f"ALTER USER 'root'@'localhost' IDENTIFIED BY '{args.db_password}';"
        cursor.execute(change_passwd)
        cursor.fetchall()
        cursor.close()
        db.close()
        shutdown_server(args, print_log=False, vanilla=True)

    if print_log is True:
        print("Init server finish")

def create_tpch_table_postgres(args):
    global BENCHMARK_TPCH
    global RUN_DB
    global DB_DATA
    global DB_DATA_BACKUP

    print("Table population start for postgresql")

    isExist = os.path.exists(DB_DATA_BACKUP[RUN_DB])
    if isExist is True:
        if os.path.exists(DB_DATA[RUN_DB]):
            os.system(f"rm -r {DB_DATA[RUN_DB]}")

        print("copy backup data")
        os.system("cp -r " + DB_DATA_BACKUP[RUN_DB] + " " + DB_DATA[RUN_DB])
    else:
        # copy config of zicio small
        os.system("cp " + DB_CONFIG[RUN_DB] + DB_NAME[RUN_DB] + "." + \
            "conf-create-tpch" + " " + DB_DATA[RUN_DB] + "/postgresql.conf")

        run_server()
        base_dir = f"--base-dir={DB_BASE[RUN_DB]}"
        port = f"--port={args.pgsql_port}"
    
        scale_factor = f"--scale-factor={args.scale_factor}"
        thread_num = f"--thread-num={args.build_thread_num}"

        # Create tpch table with HammerDB
        subprocess.run(args=[BENCHMARK_TPCH[RUN_DB] + "build.sh",
            base_dir, port, scale_factor, thread_num],
            check=False)
        shutdown_server(args)

    # Back up data if we need.
    if args.copy_data:
        os.system("cp -r " + DB_DATA[RUN_DB] + " " + DB_DATA_BACKUP[RUN_DB])
    print("Table population finish")

def create_tpch_table_mysql(args, vanilla=False):
    global BENCHMARK_TPCH
    global RUN_DB
    global DB_DATA
    global DB_DATA_BACKUP
    
    run_server(print_log=False, vanilla=vanilla)

    print("Table population start for mysql")

    if vanilla:
        isExist = os.path.exists(DB_DATA_BACKUP[RUN_DB][1])
        base_dir = f"--base-dir={DB_BASE[RUN_DB]}"
        inst_dir = f"--inst-dir={DB_BASE[RUN_DB]}/mysql_vanilla/"
        data_dir = f"--data-dir={DB_DATA[RUN_DB][1]}"
        socket = f"--socket={DB_DATA[RUN_DB][1]}/mysql.sock"
        port = f"--port={args.mysql_port}"
        password = f"--password={args.db_password}"
    
        scale_factor = f"--scale-factor={args.scale_factor}"
        thread_num = f"--thread-num={args.build_thread_num}"

        subprocess.run(args=[BENCHMARK_TPCH[RUN_DB] + "build.sh",
            data_dir, inst_dir, socket, port, scale_factor, thread_num, password],
            check=False)
    else:
        isExist = os.path.exists(DB_DATA_BACKUP[RUN_DB][0])
        base_dir = f"--base-dir={DB_BASE[RUN_DB]}"
        data_dir = f"--data-dir={DB_DATA[RUN_DB][0]}"
        socket = f"--socket={DB_DATA[RUN_DB][0] + 'mysql.sock'}"
        port = f"--port={args.mysql_port}"
        password = f"--password={args.db_password}"
    
        scale_factor = f"--scale-factor={args.scale_factor}"
        thread_num = f"--thread-num={args.build_thread_num}"

        subprocess.run(args=[BENCHMARK_TPCH[RUN_DB] + "build.sh",
            base_dir, data_dir, socket, port, scale_factor, thread_num, password],
            check=False)

    for i in range(1, 23):
        db, cursor = create_connector(args)
        cursor.execute(f"drop procedure if exists Q{i};")
        cursor.fetchall()
        cursor.execute(mysql_procs.get_procs(i))
        cursor.fetchall()
        close_connector(db, cursor)
        print(f"Procedure Q{i} is created")
    
    shutdown_server(args, vanilla=vanilla)

    if args.copy_data:
        os.system("cp -r " + DB_DATA[RUN_DB] + " " + DB_DATA_BACKUP[RUN_DB])

    print("Table population finish")

def create_tpch_table(args, vanilla=False):
    global RUN_DB

    if RUN_DB == POSTGRESQL or RUN_DB == CITUS:
        create_tpch_table_postgres(args)
    elif RUN_DB == MYSQL:
        create_tpch_table_mysql(args, vanilla=vanilla)

def create_procedures_mysql(args, vanilla=False):
    global BENCHMARK_TPCH
    global RUN_DB
    global DB_DATA
    global DB_DATA_BACKUP
    
    try:
        run_server(print_log=False, vanilla=vanilla)

        print("Create procedures started")

        for i in range(1, 23):
            db, cursor = create_connector(args)
            cursor.execute(f"drop procedure if exists Q{i};")
            cursor.fetchall()
            cursor.execute(mysql_procs.get_procs(i))
            cursor.fetchall()
            close_connector(db, cursor)
            print(f"Procedure Q{i} is created")
    
    finally:
        shutdown_server(args, vanilla=vanilla)

    if args.copy_data:
        os.system("cp -r " + DB_DATA[RUN_DB] + " " + DB_DATA_BACKUP[RUN_DB])

    print("Create procedures finished")

def mount_cgroup_postgres(args):
    print("Mount cgroup")
    command = "mount -t cgroup -o memory postgres /sys/fs/cgroup/memory"
    subprocess.call('echo {} | sudo -S {}'.format(args.user_password, command), shell=True)

def mount_cgroup_mysql(args):
    print("Mount cgroup")
    command = "mount -t cgroup -o memory mysql /sys/fs/cgroup/memory"
    subprocess.call('echo {} | sudo -S {}'.format(args.user_password, command), shell=True)

def mount_cgroup(args):
    global RUN_DB

    if RUN_DB == POSTGRESQL or RUN_DB == CITUS:
        mount_cgroup_postgres(args)
    elif RUN_DB == MYSQL:
        mount_cgroup_mysql(args)

def unmount_cgroup(args):
    print("Unmount cgroup")

    command = "umount /sys/fs/cgroup/memory"
    subprocess.call('echo {} | sudo -S {}'.format(args.user_password, command), shell=True)

def is_process_running(pid):
    try:
        os.kill(int(pid), 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    else:
        return True

def activate_cgroup_postgres(args, cgroup_mode, print_log=True):
    """
    Setting up for cgroup
    """
    global CGROUP
    if print_log is True:
        print("Activate cgroup")
    background_pids = []
    limit_bytes = 0

    # Make cgroup folder
    command = f"mkdir {CGROUP + 'postgres'}"
    subprocess.call('echo {} | sudo -S {}'.format(args.user_password, command), shell=True)

    # Set cgroup's swappiness
    command = f"sh -c 'echo 0 > {CGROUP + 'memory.swap.max'}'"
    subprocess.call('echo {} | sudo -S {}'.format(args.user_password, command), shell=True)

    # Get pids of background processes
    result = subprocess.run( \
        args=["ps -ef | grep postgres | grep -v python | awk '{print $2;}'"], \
        capture_output=True, text=True, check=True, shell=True)
    background_pids = result.stdout.splitlines()

    for pid in background_pids:
        if not is_process_running(pid):
            continue
        # Add processes to cgroup
        command = f"sh -c 'echo '{pid}' >> {CGROUP + 'postgres/cgroup.procs'}'"
        subprocess.call('echo {} | sudo -S {}'.format(args.user_password, command), shell=True,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    if cgroup_mode == VANILLA_LARGE:
        # 256 GiB
        limit_bytes = 256 * 1024 * 1024 * 1024
    elif cgroup_mode == VANILLA_SMALL:
        # 16 GiB
        limit_bytes = 16 * 1024 * 1024 * 1024
    elif cgroup_mode == ZICIO_SMALL:
        # 16 GiB
        limit_bytes = 16 * 1024 * 1024 * 1024
    elif cgroup_mode == ZICIO_LARGE:
        # 256 GiB
        limit_bytes = 256 * 1024 * 1024 * 1024

    # Set memory limit
    command = f"sh -c 'echo '{limit_bytes}' > {CGROUP + 'memory.max'}'"
    subprocess.call('echo {} | sudo -S {}'.format(args.user_password, command), shell=True)

def activate_cgroup_mysql(args, cgroup_mode, print_log=True):
    """
    Setting up for cgroup
    """
    global CGROUP
    if print_log is True:
        print("Activate cgroup")
    background_pids = []
    limit_bytes = 0

    # Make cgroup folder
    command = f"mkdir {CGROUP + 'mysql'}"
    subprocess.call('echo {} | sudo -S {}'.format(args.user_password, command), shell=True)

    # Set cgroup's swappiness
    '''
    command = f"sh -c 'echo 0 > {CGROUP + 'mysql/memory.swappiness'}'"
    subprocess.call('echo {} | sudo -S {}'.format(args.user_password, command), shell=True)
    '''

    # Get pids of background processes
    result = subprocess.run( \
        args=["ps -ef | grep mysqld | awk '{print $2;}'"], \
        capture_output=True, text=True, check=True, shell=True)
    background_pids = result.stdout.splitlines()

    for pid in background_pids:
        if not is_process_running(pid):
            continue
        # Add processes to cgroup
        command = f"sh -c 'echo '{pid}' >> {CGROUP + 'mysql/cgroup.procs'}'"
        subprocess.call('echo {} | sudo -S {}'.format(args.user_password, command), shell=True,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    if cgroup_mode == VANILLA_LARGE:
        # 256 GiB
        limit_bytes = 256 * 1024 * 1024 * 1024
    elif cgroup_mode == VANILLA_SMALL:
        # 8 GiB
        limit_bytes = 8 * 1024 * 1024 * 1024
    elif cgroup_mode == ZICIO_SMALL:
        # 8 GiB
        limit_bytes = 8 * 1024 * 1024 * 1024
    elif cgroup_mode == ZICIO_LARGE:
        # 256 GiB
        limit_bytes = 256 * 1024 * 1024 * 1024

    # Set memory limit
    command = f"sh -c 'echo '{limit_bytes}' > {CGROUP + 'memory.max'}'"
    subprocess.call('echo {} | sudo -S {}'.format(args.user_password, command), shell=True)

def activate_cgroup(args, cgroup_mode, print_log=True):
    global RUN_DB
    if RUN_DB == POSTGRESQL or RUN_DB == CITUS:
        activate_cgroup_postgres(args, cgroup_mode, print_log)
    elif RUN_DB == MYSQL:
        activate_cgroup_mysql(args, cgroup_mode, print_log)

def adjust_vm_swappiness(args):
    print("Adjust vm swwappiness")
    # Set swappiness
    command = f"sysctl vm.swappiness=1"
    subprocess.call('echo {} | sudo -S {}'.format(args.user_password, command), shell=True)

def deactivate_cgroup_postgres(args):
    """
    Remove cgroup firectory and files
    """
    time.sleep(5)
    # Get pids of background processes
    result = subprocess.run( \
        args=["ps -ef | grep postgres | grep -v python | awk '{print $2;}'"], \
        capture_output=True, text=True, check=True, shell=True)
    background_pids = result.stdout.splitlines()

    for pid in background_pids:
        # Kill process
        subprocess.run(args=[f"kill -9 {pid}"], \
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, \
            check=False, shell=True)

    # Remove cgroup folder
    command = f"rmdir {CGROUP + 'postgres'}"
    subprocess.call('echo {} | sudo -S {}'.format(args.user_password, command), shell=True)

def deactivate_cgroup_mysql(args):
    """
    Remove cgroup firectory and files
    """
    time.sleep(5)
    # Get pids of background processes
    result = subprocess.run( \
        args=["ps -ef | grep mysqld | awk '{print $2;}'"], \
        capture_output=True, text=True, check=True, shell=True)
    background_pids = result.stdout.splitlines()

    for pid in background_pids:
        # Kill process
        subprocess.run(args=[f"kill -9 {pid}"], \
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, \
            check=False, shell=True)

    # Remove cgroup folder
    command = f"rmdir {CGROUP + 'mysql'}"
    subprocess.call('echo {} | sudo -S {}'.format(args.user_password, command), shell=True)

def deactivate_cgroup(args):
    global RUN_DB

    if RUN_DB == POSTGRESQL or RUN_DB == CITUS:
        deactivate_cgroup_postgres(args)
    elif RUN_DB == MYSQL:
        deactivate_cgroup_mysql(args)
    print("cgroup is deactivated")

def move_raw_result_postgres(name=""):
    global RUN_DB
    global RUN_EVALUATION

    if RUN_EVALUATION == EVALUATION_ONE:
        raw_results_dir = RESULT_DIR + "raw_results/evaluation_1/" + name
    elif RUN_EVALUATION == EVALUATION_TWO:
        raw_results_dir = RESULT_DIR + "raw_results/evaluation_2/" + name
    elif RUN_EVALUATION == EVALUATION_THREE:
        raw_results_dir = RESULT_DIR + "raw_results/evaluation_3/" + name
    
    # Make directory for raw results
    pathlib.Path(raw_results_dir).mkdir(parents=True, exist_ok=True)

    # Copy data
    subprocess.run( \
        args=["mv " + DB_DATA[RUN_DB] + "/results/* " + raw_results_dir], \
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, \
        check=False, shell=True)

def move_raw_result_mysql(args, name="", query_no=-1, vanilla=False):
    global RUN_DB
    global RUN_EVALUATION

    result_dir = RESULT_DIR + name
    pathlib.Path(result_dir).mkdir(parents=True, exist_ok=True)
    
    data_path = ""
    if vanilla:
        data_path = DB_DATA[RUN_DB][1]
    else:
        data_path = DB_DATA[RUN_DB][0]

    if query_no == -1:
        subprocess.run( \
            args=[f"mv {data_path}/error.log {result_dir}/error.log"], \
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, \
            check=False, shell=True)
        subprocess.run( \
            args=[f"awk '/stat/' {result_dir}/error.log > {result_dir}/stat.log"], \
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, \
            check=False, shell=True)
    else:
        subprocess.run( \
            args=[f"mv {data_path}/error.log {result_dir}/error_{query_no}.log"], \
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, \
            check=False, shell=True)
        subprocess.run( \
            args=[f"awk '/stat/' {result_dir}/error_{query_no}.log > {result_dir}/stat_{query_no}.log"], \
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, \
            check=False, shell=True)
    return

    if RUN_EVALUATION == EVALUATION_ONE:
        raw_results_dir = RESULT_DIR + "raw_results/evaluation_1/" + name
    elif RUN_EVALUATION == EVALUATION_TWO:
        raw_results_dir = RESULT_DIR + "raw_results/evaluation_2/" + name
    
    # Make directory for raw results
    pathlib.Path(raw_results_dir).mkdir(parents=True, exist_ok=True)

    data_path = ''

    if vanilla:
        data_path = DB_DATA[RUN_DB][1]
    else:
        data_path = DB_DATA[RUN_DB][0]

    if query_no == -1:
        # Copy data
        subprocess.run( \
            args=["mv " + data_path + args.mysql_db + "/" + "results/*" + " " + raw_results_dir], \
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, \
            check=False, shell=True)
    else:
        # Copy data
        for filename in os.listdir(data_path + args.mysql_db + "/" + "results/"):
            q_filename = f"Q{query_no}_" + filename
            subprocess.run( \
                args=["mv " + data_path + args.mysql_db + "/" + f"results/{filename}" + " " + raw_results_dir + "/" + q_filename], \
                stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, \
                check=False, shell=True)

def move_raw_result(args, name="", query_no=-1, vanilla=False):
    global RUN_DB

    if RUN_DB == POSTGRESQL or RUN_DB == CITUS:
        move_raw_result_postgres(name)
    elif RUN_DB == MYSQL:
        move_raw_result_mysql(args, name, query_no, vanilla=vanilla)

def create_connector_postgres(args):
    db = psycopg2.connect(host=args.pgsql_host, dbname=args.pgsql_db,
        user=args.pgsql_user, port=args.pgsql_port)
    cursor = db.cursor()

    return (db, cursor)

def create_connector_mysql(args):
    db = mysql.connector.connect(host=args.mysql_host, user=args.mysql_user,
        database=args.mysql_db, password=args.db_password, port=args.mysql_port,
        autocommit = True)
    cursor = db.cursor()

    return (db, cursor)

def create_connector(args):
    global RUN_DB

    if RUN_DB == POSTGRESQL:
        return create_connector_postgres(args)
    elif RUN_DB == MYSQL:
        return create_connector_mysql(args)

def close_connector(db, cursor):
    cursor.fetchall()
    cursor.close()
    db.close()

def create_shared_pool_postgres(cursor, pool_num=0, print_log=True):
    if print_log:
        print("Create shared pool")
    cursor.execute(f"SELECT pg_create_zicio_shared_pool('lineitem'::regclass, {pool_num})")
    cursor.execute(f"SELECT pg_create_zicio_shared_pool('orders'::regclass, {pool_num})")
    cursor.execute(f"SELECT pg_create_zicio_shared_pool('partsupp'::regclass, {pool_num})")

def create_shared_pool_mysql(args, cursor, pool_num=0, print_log=True):
    if print_log:
        print("Create shared pool")
    cursor.execute(f"SELECT zicio_create_shared_pool('{args.mysql_db}', 'lineitem', '{pool_num}');")
    cursor.fetchall()
    print(f"lineitem shared pool is created, query_no: {pool_num}")
    cursor.execute(f"SELECT zicio_create_shared_pool('{args.mysql_db}', 'orders', '{pool_num}');")
    cursor.fetchall()
    print(f"orders shared pool is created, query_no: {pool_num}")
    cursor.execute(f"SELECT zicio_create_shared_pool('{args.mysql_db}', 'partsupp', '{pool_num}');")
    cursor.fetchall()
    print(f"partsupp shared pool is created, query_no: {pool_num}")

def create_shared_pool(args, cursor, pool_num=0, print_log=True):
    global RUN_DB
    if RUN_DB == POSTGRESQL:
        create_shared_pool_postgres(cursor, pool_num, print_log)
    elif RUN_DB == MYSQL:
        create_shared_pool_mysql(args, cursor, pool_num, print_log)

def destroy_shared_pool_postgres(cursor, pool_num=0, print_log=True):
    if print_log:
        print("Destroy shared pool")
    cursor.execute(f"SELECT pg_destroy_zicio_shared_pool('lineitem'::regclass, {pool_num})")
    cursor.execute(f"SELECT pg_destroy_zicio_shared_pool('orders'::regclass, {pool_num})")
    cursor.execute(f"SELECT pg_destroy_zicio_shared_pool('partsupp'::regclass, {pool_num})")

def destroy_shared_pool_mysql(args, cursor, pool_num=0, print_log=True):
    if print_log:
        print("Destroy shared pool")
    cursor.fetchall()
    cursor.execute(f"SELECT zicio_destroy_shared_pool('{args.mysql_db}', 'lineitem', '{pool_num}');")
    cursor.fetchall()
    cursor.execute(f"SELECT zicio_destroy_shared_pool('{args.mysql_db}', 'orders', '{pool_num}');")
    cursor.fetchall()
    cursor.execute(f"SELECT zicio_destroy_shared_pool('{args.mysql_db}', 'partsupp', '{pool_num}');")

def destroy_shared_pool(args, cursor, pool_num=0, print_log=True):
    global RUN_DB
    if RUN_DB == POSTGRESQL:
        destroy_shared_pool_postgres(cursor, pool_num, print_log)
    elif RUN_DB == MYSQL:
        destroy_shared_pool_mysql(args, cursor, pool_num, print_log)

def create_shuffled_queries_eval2_mysql():
    global EVALUATION_TWO_TARGET_QUERY_MYSQL
    global EVALUATION_TWO_QUERY_NUM_MYSQL
    split_idx = random.randint(0, EVALUATION_TWO_QUERY_NUM_MYSQL)
    shuffled_queries = \
        EVALUATION_TWO_TARGET_QUERY_MYSQL[split_idx:] + EVALUATION_TWO_TARGET_QUERY_MYSQL[:split_idx]
    return shuffled_queries

def create_shuffled_queries_eval2_postgres():
    global EVALUATION_TWO_TARGET_QUERY_POSTGRESQL
    global EVALUATION_TWO_QUERY_NUM_POSTGRESQL
    split_idx = random.randint(0, EVALUATION_TWO_QUERY_NUM_POSTGRESQL)
    shuffled_queries = \
        EVALUATION_TWO_TARGET_QUERY_POSTGRESQL[split_idx:] + EVALUATION_TWO_TARGET_QUERY_POSTGRESQL[:split_idx]
    return shuffled_queries

def create_shuffled_queries_eval2():
    global RUN_DB
    if RUN_DB == MYSQL:
        return create_shuffled_queries_eval2_mysql()
    elif RUN_DB == POSTGRESQL:
        return create_shuffled_queries_eval2_postgres()

def strip_non_printable(s):
    return ''.join(ch for ch in s if ch in string.printable)

def set_data_dir_path_postgres(args, run_db):
    global RUN_DB
    data_path = args.device_path + "/" + args.device_user_path + "/" + DB[RUN_DB] + args.datadir_path + "/"
    backup_data_path =  args.device_path + "/" + args.device_user_path + "/" + DB[RUN_DB] + args.backup_datadir_path + "/"
    DB_DATA[RUN_DB] = data_path
    DB_DATA_BACKUP[RUN_DB] = backup_data_path

def set_data_dir_path_mysql(args, run_db):
    global RUN_DB
    DB_DATA[RUN_DB] = []
    DB_DATA_BACKUP[RUN_DB] = []

    data_path = args.device_path + "/" + args.device_user_path + "/" + DB[RUN_DB] + args.datadir_path + "/"
    backup_data_path =  args.device_path + "/" + args.device_user_path + "/" + DB[RUN_DB] + args.backup_datadir_path + "/"
    DB_DATA[RUN_DB].append(data_path)
    DB_DATA_BACKUP[RUN_DB].append(backup_data_path)

    data_path = args.device_path + "/" + args.device_user_path + "/" + DB[RUN_DB] + args.datadir_path + "_vanilla/"
    backup_data_path =  args.device_path + "/" + args.device_user_path + "/" + DB[RUN_DB] + args.backup_datadir_path + "_vanilla/"
    DB_DATA[RUN_DB].append(data_path)
    DB_DATA_BACKUP[RUN_DB].append(backup_data_path)

def set_data_dir_path(args, run_db):
    global RUN_DB
    if RUN_DB == POSTGRESQL or RUN_DB == CITUS:
        set_data_dir_path_postgres(args, run_db)
    elif RUN_DB == MYSQL:
        set_data_dir_path_mysql(args, run_db)

def do_data_processing_eval1_postgres(args, names):
    global RESULT_DIR

    """
    Evaluation 1: single process with local
    """
    raw_result_dir = RESULT_DIR + "raw_results/evaluation_1/"
    processed_results_dir = RESULT_DIR + "processed_results/evaluation_1/"

    # Make directory for raw results
    pathlib.Path(processed_results_dir).mkdir(parents=True, exist_ok=True)

    avg_exec_time = dict()
    min_exec_time = dict()
    max_exec_time = dict()
 
    for processed_data_file_name in names:
        processed_result_file = open(processed_results_dir + f"{processed_data_file_name}.dat", 'w+')
        processed_result_file.write( \
            f"{'#query':>6} " + \
            f"{'elapsed':>14} " + \
            f"{'seq_buf_time':>14} " + \
            f"{'buf_time':>16} " + \
            f"{'io_time':>14} " + \
            f"{'min_elapsed':>14} " + \
            f"{'max_elapsed':>14} " + \
            f"{'mode_switch':>14} " + \
            f"{'data_copy':>14} " + \
            f"{'storage_stack':>14} " + \
            f"{'physical_io':>14} " + \
            "\n")
        # Process data
        for filename in os.listdir(raw_result_dir + f'{processed_data_file_name}'):
            with open(raw_result_dir + f'{processed_data_file_name}/' + filename, 'r') as data_file:
                data_string_lines = data_file.readlines()
                line_length = len(data_string_lines)
                # Process line by line
                for i in range(0, line_length):
                    line = data_string_lines[i].strip('\t')
                    if '[QUERY RESULT]' in line:
                        query_result = line[15:].split(sep=',')

                        query_data = {}
                        for item in query_result:
                            key_value = item.split()
                            if len(key_value) == 2:
                                key = key_value[0]
                                value = float(key_value[1])
                                query_data[key] = value

                        query_num = query_result[0].strip()[1:]
                        execution_time = query_data.get('execution_time(s)')
                        buf_time = query_data.get('buffer_wait_time(s)')
                        seq_buf_time = query_data.get('seq_buffer_wait_time(s)')
                        zicio_wait_time = query_data.get('zicio_wait_time(s)')
                        io_time = \
                            query_data.get('physical_io_time_with_buf(s)')


                        if avg_exec_time.get(query_num) == None:
                            avg_exec_time[query_num] = DBExecTime()

                        avg_exec_time[query_num].execution_time += execution_time
                        avg_exec_time[query_num].seq_io_time += seq_buf_time + zicio_wait_time
                        avg_exec_time[query_num].rand_io_time += buf_time
                        avg_exec_time[query_num].zicio_wait_time += zicio_wait_time
                        avg_exec_time[query_num].io_time += io_time

                        if min_exec_time.get(query_num) == None:
                            min_exec_time[query_num] = DBExecTime()
                            min_exec_time[query_num].execution_time = execution_time
                            min_exec_time[query_num].seq_io_time = (seq_buf_time + zicio_wait_time)
                            min_exec_time[query_num].rand_io_time = buf_time
                            min_exec_time[query_num].zicio_wait_time = zicio_wait_time
                            min_exec_time[query_num].io_time = io_time
                        else:
                            if min_exec_time[query_num].execution_time > execution_time:
                                min_exec_time[query_num].execution_time = execution_time
                            if min_exec_time[query_num].seq_io_time > (seq_buf_time + zicio_wait_time):
                                min_exec_time[query_num].seq_io_time = (seq_buf_time + zicio_wait_time)
                            if min_exec_time[query_num].rand_io_time > buf_time:
                                min_exec_time[query_num].rand_io_time = buf_time
                            if min_exec_time[query_num].zicio_wait_time > zicio_wait_time:
                                min_exec_time[query_num].zicio_wait_time = zicio_wait_time
                            if min_exec_time[query_num].io_time > io_time:
                                min_exec_time[query_num].io_time = io_time

                        if max_exec_time.get(query_num) == None:
                            max_exec_time[query_num] = DBExecTime()
                            max_exec_time[query_num].execution_time = execution_time
                            max_exec_time[query_num].seq_io_time = (seq_buf_time + zicio_wait_time) 
                            max_exec_time[query_num].rand_io_time = buf_time
                            max_exec_time[query_num].zicio_wait_time = zicio_wait_time
                            max_exec_time[query_num].io_time = io_time
                        else:
                            if max_exec_time[query_num].execution_time < execution_time:
                                max_exec_time[query_num].execution_time = execution_time
                            if max_exec_time[query_num].seq_io_time < (seq_buf_time + zicio_wait_time):
                                max_exec_time[query_num].seq_io_time = (seq_buf_time + zicio_wait_time)
                            if max_exec_time[query_num].rand_io_time < buf_time:
                                max_exec_time[query_num].rand_io_time = buf_time
                            if max_exec_time[query_num].zicio_wait_time < zicio_wait_time:
                                max_exec_time[query_num].zicio_wait_time = zicio_wait_time
                            if max_exec_time[query_num].io_time < io_time:
                                max_exec_time[query_num].io_time = io_time


        for k, v in avg_exec_time.items():
            v.execution_time /= float(args.worker_thread_num_eval1)
            v.seq_io_time /= float(args.worker_thread_num_eval1)
            v.rand_io_time /= float(args.worker_thread_num_eval1)
            v.zicio_wait_time /= float(args.worker_thread_num_eval1)
            v.io_time /= float(args.worker_thread_num_eval1)

            processed_result_file.write( \
                f"{k:>6}" + \
                f"{v.execution_time:>14.6f} " + \
                f"{v.seq_io_time:>14.6f} " + \
                f"{v.rand_io_time:>14.6f} " + \
                f"{v.io_time:>14.6f} " + \
                f"{min_exec_time[k].execution_time:>14.6f} " + \
                f"{max_exec_time[k].execution_time:>14.6f} " + \
                "\n")

        # Close file
        processed_result_file.close()
        # Sort by query num
        os.system(f"sort -nk1 -o {processed_results_dir + f'{processed_data_file_name}.dat'} {processed_results_dir + f'{processed_data_file_name}.dat'}")

def do_data_processing_mysql(args, eval, names, vanilla):
    global RUN_DB
    global RESULT_DIR
    global QUERIES_DICT

    result_dir = RESULT_DIR + names[0]
    
    queries=[]
    worker = 0
    if eval == 1:
        queries = EVALUATION_ONE_TARGET_QUERY_MYSQL
        worker = int(args.worker_thread_num_eval1)
    elif eval == 2:
        queries = EVALUATION_TWO_TARGET_QUERY_MYSQL
        worker = int(args.worker_thread_num_eval2)
    
    len_queries = len(queries)

    sum_elapsed_time = [0.0 for _ in range(len_queries)]
    sum_io_time = [0.0 for _ in range(len_queries)]
    sum_scan_time = [0.0 for _ in range(len_queries)]

    min_elapsed_time = [1000000.0 for _ in range(len_queries)]
    min_io_time = [1000000.0 for _ in range(len_queries)]
    min_scan_time = [1000000.0 for _ in range(len_queries)]

    max_elapsed_time = [0.0 for _ in range(len_queries)]
    max_io_time = [0.0 for _ in range(len_queries)]
    max_scan_time = [0.0 for _ in range(len_queries)]

    sum_zicio_wait_time = [0.0 for _ in range(len_queries)]
    min_zicio_wait_time = [1000000.0 for _ in range(len_queries)]
    max_zicio_wait_time = [0.0 for _ in range(len_queries)]
    
    if eval == 1:
        for query_no in queries:
            with open(f"{result_dir}/stat_{query_no}.log", 'r') as stat:
                lines = stat.readlines()
                for line in lines:
                    words = line.split(',')
                    header = words[0].split()
                    _query_no = int(header[3][1:])
                    if query_no != _query_no:
                        print(f"stat is wrong, query_no: {query_no}, in stat: {_query_no}")
                    ind = QUERIES_DICT[query_no]

                    if header[1] == "[IO]":
                        elapsed_time = float(words[1].split()[1]) / 1000000000
                        sum_elapsed_time[ind] += elapsed_time
                        if min_elapsed_time[ind] > elapsed_time:
                            min_elapsed_time[ind] = elapsed_time
                        if max_elapsed_time[ind] < elapsed_time:
                            max_elapsed_time[ind] = elapsed_time

                        io_time = float(words[2].split()[1]) / 1000000000
                        sum_io_time[ind] += io_time
                        if min_io_time[ind] > io_time:
                            min_io_time[ind] = io_time
                        if max_io_time[ind] < io_time:
                            max_io_time[ind] = io_time

                        scan_time = float(words[3].split()[1]) / 1000000000
                        sum_scan_time[ind] += scan_time
                        if min_scan_time[ind] > scan_time:
                            min_scan_time[ind] = scan_time
                        if max_scan_time[ind] < scan_time:
                            max_scan_time[ind] = scan_time
                    elif header[1] == "[ZICIO]":
                        zicio_wait_time = float(words[2].split()[1]) * 0.001
                        sum_zicio_wait_time[ind] += zicio_wait_time
                        if min_zicio_wait_time[ind] > zicio_wait_time:
                            min_zicio_wait_time[ind] = zicio_wait_time
                        if max_zicio_wait_time[ind] < zicio_wait_time:
                            max_zicio_wait_time[ind] = zicio_wait_time
                    else:
                        print("stat error")
    elif eval == 2:
        with open(f"{result_dir}/stat.log", 'r') as stat:
            lines = stat.readlines()
            for line in lines:
                words = line.split(',')
                header = words[0].split()
                query_no = int(header[3][1:])
                ind = QUERIES_DICT[query_no]

                if header[1] == "[IO]":
                    elapsed_time = float(words[1].split()[1]) / 1000000000
                    sum_elapsed_time[ind] += elapsed_time
                    if min_elapsed_time[ind] > elapsed_time:
                        min_elapsed_time[ind] = elapsed_time
                    if max_elapsed_time[ind] < elapsed_time:
                        max_elapsed_time[ind] = elapsed_time

                    io_time = float(words[2].split()[1]) / 1000000000
                    sum_io_time[ind] += io_time
                    if min_io_time[ind] > io_time:
                        min_io_time[ind] = io_time
                    if max_io_time[ind] < io_time:
                        max_io_time[ind] = io_time

                    scan_time = float(words[3].split()[1]) / 1000000000
                    sum_scan_time[ind] += scan_time
                    if min_scan_time[ind] > scan_time:
                        min_scan_time[ind] = scan_time
                    if max_scan_time[ind] < scan_time:
                        max_scan_time[ind] = scan_time
                elif header[1] == "[zicio]":
                    zicio_wait_time = float(words[2].split()[1]) * 0.001
                    sum_zicio_wait_time[ind] += zicio_wait_time
                    if min_zicio_wait_time[ind] > zicio_wait_time:
                        min_zicio_wait_time[ind] = zicio_wait_time
                    if max_zicio_wait_time[ind] < zicio_wait_time:
                        max_zicio_wait_time[ind] = zicio_wait_time
                else:
                    print("stat error")

    stat_dat = open(f"{result_dir}/stat.dat", 'w')
    for i in range(len_queries):
        stat_dat.write("%d %.06f %.06f %.06f %.06f %.06f %.06f %.06f %.06f %.06f %.06f %.06f %.06f\n"%(queries[i], \
            sum_elapsed_time[i] / worker, min_elapsed_time[i], max_elapsed_time[i], \
            sum_io_time[i] / worker, min_io_time[i], max_io_time[i], \
            sum_scan_time[i] / worker, min_scan_time[i], max_scan_time[i], \
            sum_zicio_wait_time[i] / worker, min_zicio_wait_time[i], max_zicio_wait_time[i]))
    stat_dat.close()
            

    return

    """
    Evaluation 1: single process with local
    """
    raw_result_dir = RESULT_DIR + "raw_results/evaluation_1/"
    processed_results_dir = RESULT_DIR + "processed_results/evaluation_1/"

    # Make directory for raw results
    pathlib.Path(processed_results_dir).mkdir(parents=True, exist_ok=True)

    avg_exec_time = dict()
    min_exec_time = dict()
    max_exec_time = dict()

    for processed_data_file_name in names:
        processed_result_file = open(processed_results_dir + f"{processed_data_file_name}.dat", 'w')
        processed_result_file.write( \
            f"{'#query':>6} " + \
            f"{'elapsed':>14} " + \
            f"{'zicio_wait_time':>16} " + \
            f"{'seq_io_time':>14} " + \
            f"{'rnd_io_time':>14} " + \
            "\n")
        # Process data
        for filename in os.listdir(raw_result_dir + f'{processed_data_file_name}'):
            with open(raw_result_dir + f'{processed_data_file_name}/' + filename, 'r', errors='ignore') as data_file:
                data_string_lines = data_file.readlines()
                line_length = len(data_string_lines)
                query_num = filename.split(sep='_')[0][1:]
                # Process line by line
                for i in range(0, line_length):
                    data_string_lines[i] = strip_non_printable(data_string_lines[i])
                    line = data_string_lines[i].strip('\t')
                    if '[QUERY RESULT]' in line:
                        query_result = line[15:].split(sep=',')

                        execution_time = float(query_result[1].split()[1])
                        zicio_wait_time = float(query_result[2].split()[1])
                        io_time = float(query_result[3].split()[1])
                        tot_io_time = float(query_result[4].split()[1])
                        rnd_io_time = tot_io_time - io_time

                        if avg_exec_time.get(query_num) == None:
                            avg_exec_time[query_num] = DBExecTime()

                        avg_exec_time[query_num].execution_time += execution_time
                        avg_exec_time[query_num].seq_io_time += io_time
                        avg_exec_time[query_num].rand_io_time += rnd_io_time
                        avg_exec_time[query_num].zicio_wait_time += zicio_wait_time

                        if min_exec_time.get(query_num) == None:
                            min_exec_time[query_num] = DBExecTime()
                            min_exec_time[query_num].execution_time = execution_time
                            min_exec_time[query_num].seq_io_time = io_time
                            min_exec_time[query_num].rand_io_time = rnd_io_time
                            min_exec_time[query_num].zicio_wait_time = zicio_wait_time
                        else:
                            if min_exec_time[query_num].execution_time > execution_time:
                                min_exec_time[query_num].execution_time = execution_time
                            if min_exec_time[query_num].seq_io_time > io_time:
                                min_exec_time[query_num].seq_io_time = io_time
                            if min_exec_time[query_num].rand_io_time > rnd_io_time:
                                min_exec_time[query_num].rand_io_time = rnd_io_time
                            if min_exec_time[query_num].zicio_wait_time > zicio_wait_time:
                                min_exec_time[query_num].zicio_wait_time = zicio_wait_time

                        if max_exec_time.get(query_num) == None:
                            max_exec_time[query_num] = DBExecTime()
                            max_exec_time[query_num].execution_time = execution_time
                            max_exec_time[query_num].seq_io_time = io_time
                            max_exec_time[query_num].rand_io_time = rnd_io_time
                            max_exec_time[query_num].zicio_wait_time = zicio_wait_time
                        else:
                            if max_exec_time[query_num].execution_time < execution_time:
                                max_exec_time[query_num].execution_time = execution_time
                            if max_exec_time[query_num].seq_io_time < io_time:
                                max_exec_time[query_num].seq_io_time = io_time
                            if max_exec_time[query_num].rand_io_time < rnd_io_time:
                                max_exec_time[query_num].rand_io_time = rnd_io_time
                            if max_exec_time[query_num].zicio_wait_time < zicio_wait_time:
                                max_exec_time[query_num].zicio_wait_time = zicio_wait_time

        for k, v in avg_exec_time.items():
            v.execution_time /= float(args.worker_thread_num_eval1)
            v.seq_io_time /= float(args.worker_thread_num_eval1)
            v.rand_io_time /= float(args.worker_thread_num_eval1)
            v.zicio_wait_time /= float(args.worker_thread_num_eval1)

            processed_result_file.write( \
                f"{k:>6}" + \
                f"{v.execution_time:>14.6f} " + \
                f"{v.zicio_wait_time:>16.6f} " + \
                f"{v.seq_io_time:>14.6f} " + \
                f"{v.rand_io_time:>14.6f} " + \
                f"{min_exec_time[k].execution_time:>14.6f} " + \
                f"{max_exec_time[k].execution_time:>14.6f} " + \
                "\n")

        # Close file
        processed_result_file.close()
        # Sort by query num
        os.system(f"sort -nk1 -o {processed_results_dir + f'{processed_data_file_name}.dat'} {processed_results_dir + f'{processed_data_file_name}.dat'}")


def do_data_processing_eval1(args, names, vanilla):
    global RUN_DB
    if RUN_DB == POSTGRESQL:
        do_data_processing_eval1_postgres(args, names)
    elif RUN_DB == MYSQL:
        do_data_processing_mysql(args, 1, names, vanilla)

def do_data_processing_eval2_mysql(args, names, vanilla):
    global RESULT_DIR
    return

    """
    Evaluation 1: single process with local
    """
    raw_result_dir = RESULT_DIR + "raw_results/evaluation_2/"
    processed_results_dir = RESULT_DIR + "processed_results/evaluation_2/"

    # Make directory for raw results
    pathlib.Path(processed_results_dir).mkdir(parents=True, exist_ok=True)

    avg_exec_time = dict()
    min_exec_time = dict()
    max_exec_time = dict()

    for processed_data_file_name in names:
        processed_result_file = open(processed_results_dir + f"{processed_data_file_name}.dat", 'w')
        processed_result_file.write( \
            f"{'#query':>6} " + \
            f"{'elapsed':>14} " + \
            f"{'zicio_wait_time':>16} " + \
            f"{'seq_io_time':>14} " + \
            f"{'rnd_io_time':>14} " + \
            "\n")
        # Process data
        for filename in os.listdir(raw_result_dir + f'{processed_data_file_name}'):
            with open(raw_result_dir + f'{processed_data_file_name}/' + filename, 'r', errors='ignore') as data_file:
                data_string_lines = data_file.readlines()
                line_length = len(data_string_lines)
                query_num = filename.split(sep='_')[0][1:]
                # Process line by line
                for i in range(0, line_length):
                    data_string_lines[i] = strip_non_printable(data_string_lines[i])
                    line = data_string_lines[i].strip('\t')
                    if '[QUERY RESULT]' in line:
                        query_result = line[15:].split(sep=',')

                        execution_time = float(query_result[1].split()[1])
                        zicio_wait_time = float(query_result[2].split()[1])
                        io_time = float(query_result[3].split()[1])
                        tot_io_time = float(query_result[4].split()[1])
                        rnd_io_time = tot_io_time - io_time

                        if avg_exec_time.get(query_num) == None:
                            avg_exec_time[query_num] = DBExecTime()

                        avg_exec_time[query_num].execution_time += execution_time
                        avg_exec_time[query_num].seq_io_time += io_time
                        avg_exec_time[query_num].rand_io_time += rnd_io_time
                        avg_exec_time[query_num].zicio_wait_time += zicio_wait_time

                        if min_exec_time.get(query_num) == None:
                            min_exec_time[query_num] = DBExecTime()
                            min_exec_time[query_num].execution_time = execution_time
                            min_exec_time[query_num].seq_io_time = io_time
                            min_exec_time[query_num].rand_io_time = rnd_io_time
                            min_exec_time[query_num].zicio_wait_time = zicio_wait_time
                        else:
                            if min_exec_time[query_num].execution_time > execution_time:
                                min_exec_time[query_num].execution_time = execution_time
                            if min_exec_time[query_num].seq_io_time > io_time:
                                min_exec_time[query_num].seq_io_time = io_time
                            if min_exec_time[query_num].rand_io_time > rnd_io_time:
                                min_exec_time[query_num].rand_io_time = rnd_io_time
                            if min_exec_time[query_num].zicio_wait_time > zicio_wait_time:
                                min_exec_time[query_num].zicio_wait_time = zicio_wait_time

                        if max_exec_time.get(query_num) == None:
                            max_exec_time[query_num] = DBExecTime()
                            max_exec_time[query_num].execution_time = execution_time
                            max_exec_time[query_num].seq_io_time = io_time
                            max_exec_time[query_num].rand_io_time = rnd_io_time
                            max_exec_time[query_num].zicio_wait_time = zicio_wait_time
                        else:
                            if max_exec_time[query_num].execution_time < execution_time:
                                max_exec_time[query_num].execution_time = execution_time
                            if max_exec_time[query_num].seq_io_time < io_time:
                                max_exec_time[query_num].seq_io_time = io_time
                            if max_exec_time[query_num].rand_io_time < rnd_io_time:
                                max_exec_time[query_num].rand_io_time = rnd_io_time
                            if max_exec_time[query_num].zicio_wait_time < zicio_wait_time:
                                max_exec_time[query_num].zicio_wait_time = zicio_wait_time

        for k, v in avg_exec_time.items():
            v.execution_time /= float(args.worker_thread_num_eval2)
            v.seq_io_time /= float(args.worker_thread_num_eval2)
            v.rand_io_time /= float(args.worker_thread_num_eval2)
            v.zicio_wait_time /= float(args.worker_thread_num_eval2)

            processed_result_file.write( \
                f"{k:>6}" + \
                f"{v.execution_time:>14.6f} " + \
                f"{v.zicio_wait_time:>16.6f} " + \
                f"{v.seq_io_time:>14.6f} " + \
                f"{v.rand_io_time:>14.6f} " + \
                f"{min_exec_time[k].execution_time:>14.6f} " + \
                f"{max_exec_time[k].execution_time:>14.6f} " + \
                "\n")

        # Close file
        processed_result_file.close()
        # Sort by query num
        os.system(f"sort -nk1 -o {processed_results_dir + f'{processed_data_file_name}.dat'} {processed_results_dir + f'{processed_data_file_name}.dat'}")

def do_data_processing_eval2_postgres(args, names):
    global RESULT_DIR
    global RUN_MODE

    raw_result_dir = RESULT_DIR + "raw_results/evaluation_2/"
    processed_results_dir = RESULT_DIR + "processed_results/evaluation_2/"

    # Make directory for raw results
    pathlib.Path(processed_results_dir).mkdir(parents=True, exist_ok=True)

    for processed_data_file_name in names:
        # Open file for result file
        processed_result_file = open(processed_results_dir + f"{processed_data_file_name}.dat", 'w')
        processed_result_file.write( \
            f"{'#query':>6} " + \
            f"{'elapsed(avg)':>13} " + \
            f"{'buf_time(avg)':>13} " + \
            f"{'seq_buf_time(avg)':>17} " + \
            f"{'io_time(avg)':>12} " + \
            f"{'io_time_w_buf(avg)':>19} " + \
            f"{'io_time_wo_buf(avg)':>19} " + \
            \
            f"{'elapsed(min)':>13} " + \
            f"{'buf_time(min)':>13} " + \
            f"{'seq_buf_time(min)':>17} " + \
            f"{'io_time(min)':>12} " + \
            f"{'io_time_w_buf(min)':>19} " + \
            f"{'io_time_wo_buf(min)':>19} " + \
            \
            f"{'elapsed(max)':>13} " + \
            f"{'buf_time(max)':>13} " + \
            f"{'seq_buf_time(max)':>17} " + \
            f"{'io_time(max)':>12} " + \
            f"{'io_time_w_buf(max)':>19} " + \
            f"{'io_time_wo_buf(max)':>19} " + \
            \
            "\n")
        # Set variable for aggregation {query_num: value}
        query_results = {}

        # Start processing data file
        for filename in os.listdir(raw_result_dir + processed_data_file_name):
            with open(raw_result_dir + f"{processed_data_file_name}/" + filename) as data_file:
                data_string_lines = data_file.readlines()
    
                # Process line by line
                for line in data_string_lines:
                    if '[QUERY RESULT]' in line:
                        query_result = line[15:].split(sep=',')
    
                        query_num = int(query_result[0].strip()[1:])
                        execution_time = float(query_result[2].split()[1])
                        buffer_wait_time = float(query_result[3].split()[1])
                        seq_buffer_wait_time = float(query_result[4].split()[1])
                        zicio_wait_time = float(query_result[5].split()[1])
                        io_time_with_buf = float(query_result[6].split()[1])
                        io_time_without_buf = float(query_result[7].split()[1])
    
                        if query_num not in query_results:
                            query_results[query_num] = {\
                                'total_execution_time': 0,
                                'total_buffer_wait_time': 0,
                                'total_seq_buffer_wait_time': 0,
                                'total_zicio_wait_time': 0,
                                'total_io_time': 0,
                                'total_io_time_w_buf': 0,
                                'total_io_time_wo_buf': 0,
                                'min_execution_time': 999999999999,
                                'min_buffer_wait_time': 999999999999,
                                'min_seq_buffer_wait_time': 999999999999,
                                'min_zicio_wait_time': 999999999999,
                                'min_io_time': 999999999999,
                                'min_io_time_w_buf': 999999999999,
                                'min_io_time_wo_buf': 999999999999,
                                'max_execution_time': 0,
                                'max_buffer_wait_time': 0,
                                'max_seq_buffer_wait_time': 0,
                                'max_zicio_wait_time': 0,
                                'max_io_time': 0,
                                'max_io_time_w_buf': 0,
                                'max_io_time_wo_buf': 0}
    
                        # Aggregate a result
                        query_results[query_num]['total_execution_time'] += execution_time
                        query_results[query_num]['total_buffer_wait_time'] += buffer_wait_time
                        query_results[query_num]['total_seq_buffer_wait_time'] += seq_buffer_wait_time
                        query_results[query_num]['total_zicio_wait_time'] += zicio_wait_time
                        query_results[query_num]['total_io_time'] += \
                            io_time_with_buf + io_time_without_buf
                        query_results[query_num]['total_io_time_w_buf'] += \
                            io_time_with_buf
                        query_results[query_num]['total_io_time_wo_buf'] += \
                            io_time_without_buf
    
                        query_results[query_num]['min_execution_time'] = \
                            min(query_results[query_num]['min_execution_time'], execution_time)
                        query_results[query_num]['min_buffer_wait_time'] = \
                            min(query_results[query_num]['min_buffer_wait_time'], buffer_wait_time)
                        query_results[query_num]['min_seq_buffer_wait_time'] = \
                            min(query_results[query_num]['min_seq_buffer_wait_time'], seq_buffer_wait_time)
                        query_results[query_num]['min_zicio_wait_time'] = \
                            min(query_results[query_num]['min_zicio_wait_time'], zicio_wait_time)
                        query_results[query_num]['min_io_time'] = \
                            min(query_results[query_num]['min_io_time'], \
                            io_time_with_buf + io_time_without_buf)
                        query_results[query_num]['min_io_time_w_buf'] = \
                            min(query_results[query_num]['min_io_time_w_buf'], io_time_with_buf)
                        query_results[query_num]['min_io_time_wo_buf'] = \
                            min(query_results[query_num]['min_io_time_wo_buf'], io_time_without_buf)
    
                        query_results[query_num]['max_execution_time'] = \
                            max(query_results[query_num]['max_execution_time'], execution_time)
                        query_results[query_num]['max_buffer_wait_time'] = \
                            max(query_results[query_num]['max_buffer_wait_time'], buffer_wait_time)
                        query_results[query_num]['max_seq_buffer_wait_time'] = \
                            max(query_results[query_num]['max_seq_buffer_wait_time'], seq_buffer_wait_time)
                        query_results[query_num]['max_zicio_wait_time'] = \
                            max(query_results[query_num]['max_zicio_wait_time'], zicio_wait_time)
                        query_results[query_num]['max_io_time'] = \
                            max(query_results[query_num]['max_io_time'], \
                            io_time_with_buf + io_time_without_buf)
                        query_results[query_num]['max_io_time_w_buf'] = \
                            max(query_results[query_num]['max_io_time_w_buf'], io_time_with_buf)
                        query_results[query_num]['max_io_time_wo_buf'] = \
                            max(query_results[query_num]['max_io_time_wo_buf'], io_time_without_buf)
        # Write result query by query  
        for query_num, query_result in query_results.items():
            # Calculate average
            avg_execution_time = \
                query_result['total_execution_time'] / args.thread_num_evaluation_2
            avg_buffer_wait_time = \
                query_result['total_buffer_wait_time'] / args.thread_num_evaluation_2
            avg_seq_buffer_wait_time = \
                query_result['total_seq_buffer_wait_time'] / args.thread_num_evaluation_2
            avg_zicio_wait_time = \
                query_result['total_zicio_wait_time'] / args.thread_num_evaluation_2
            avg_io_time = \
                query_result['total_io_time'] / args.thread_num_evaluation_2
            avg_io_time_w_buf = \
                query_result['total_io_time_w_buf'] / args.thread_num_evaluation_2
            avg_io_time_wo_buf = \
                query_result['total_io_time_wo_buf'] / args.thread_num_evaluation_2
    
            processed_result_file.write( \
                f"{query_num:>6} " + \
                f"{avg_execution_time:>13.6f} " + \
                f"{avg_buffer_wait_time:>13.6f} " + \
                f"{avg_seq_buffer_wait_time + avg_zicio_wait_time:>17.6f} " + \
                f"{avg_io_time:>12.6f} " + \
                f"{avg_io_time_w_buf:>19.6f} " + \
                f"{avg_io_time_wo_buf:>19.6f} " + \
                \
                f"{query_result['min_execution_time']:>13.6f} " + \
                f"{query_result['min_buffer_wait_time']:>13.6f} " + \
                f"{query_result['min_seq_buffer_wait_time'] + query_result['min_zicio_wait_time']:>17.6f} " + \
                f"{query_result['min_io_time']:>12.6f} " + \
                f"{query_result['min_io_time_w_buf']:>19.6f} " + \
                f"{query_result['min_io_time_wo_buf']:>19.6f} " + \
                \
                f"{query_result['max_execution_time']:>13.6f} " + \
                f"{query_result['max_buffer_wait_time']:>13.6f} " + \
                f"{query_result['max_seq_buffer_wait_time'] + query_result['min_zicio_wait_time']:>17.6f} " + \
                f"{query_result['max_io_time']:>12.6f} " + \
                f"{query_result['max_io_time_w_buf']:>19.6f} " + \
                f"{query_result['max_io_time_wo_buf']:>19.6f} " + \
                \
                "\n")
        # Close file
        processed_result_file.close()
        # Sort by query num
        os.system(f"sort -nk1 -o {processed_results_dir + f'{processed_data_file_name}.dat'} {processed_results_dir + f'{processed_data_file_name}.dat'}")

def do_data_processing_eval2(args, names, vanilla):
    global RUN_DB
    if RUN_DB == POSTGRESQL:
        do_data_processing_eval2_postgres(args, names)
    elif RUN_DB == MYSQL:
        do_data_processing_mysql(args, 2, names, vanilla)

def do_data_processing_eval3_postgres(args, names, client_numbers):
    global RESULT_DIR
    global RUN_MODE

    raw_result_dir = RESULT_DIR + "raw_results/evaluation_3/"
    processed_results_dir = RESULT_DIR + "processed_results/evaluation_3/"

    # Make directory for raw results
    pathlib.Path(processed_results_dir).mkdir(parents=True, exist_ok=True)

    for processed_data_file_name in names:
        print(processed_data_file_name)

        # Open file for result file
        processed_result_file = open(processed_results_dir + f"{processed_data_file_name}.dat", 'w')
        processed_result_file.write( \
            f"{'#query':>6} " + \
            f"{'elapsed(avg)':>13} " + \
            f"{'buf_time(avg)':>13} " + \
            f"{'seq_buf_time(avg)':>17} " + \
            f"{'io_time(avg)':>12} " + \
            f"{'io_time_w_buf(avg)':>19} " + \
            f"{'io_time_wo_buf(avg)':>19} " + \
            \
            f"{'elapsed(min)':>13} " + \
            f"{'buf_time(min)':>13} " + \
            f"{'seq_buf_time(min)':>17} " + \
            f"{'io_time(min)':>12} " + \
            f"{'io_time_w_buf(min)':>19} " + \
            f"{'io_time_wo_buf(min)':>19} " + \
            \
            f"{'elapsed(max)':>13} " + \
            f"{'buf_time(max)':>13} " + \
            f"{'seq_buf_time(max)':>17} " + \
            f"{'io_time(max)':>12} " + \
            f"{'io_time_w_buf(max)':>19} " + \
            f"{'io_time_wo_buf(max)':>19} " + \
            \
            "\n")
        # Set variable for aggregation {query_num: value}
        query_results = {}

        # Start processing data file
        for filename in os.listdir(raw_result_dir + processed_data_file_name):
            with open(raw_result_dir + f"{processed_data_file_name}/" + filename) as data_file:
                data_string_lines = data_file.readlines()
    
                # Process line by line
                for line in data_string_lines:
                    if '[QUERY RESULT]' in line:
                        query_result = line[15:].split(sep=',')
    
                        query_num = int(query_result[0].strip()[1:])
                        execution_time = float(query_result[2].split()[1])
                        buffer_wait_time = float(query_result[3].split()[1])
                        seq_buffer_wait_time = float(query_result[4].split()[1])
                        zicio_wait_time = float(query_result[5].split()[1])
                        io_time_with_buf = float(query_result[6].split()[1])
                        io_time_without_buf = float(query_result[7].split()[1])
    
                        if query_num not in query_results:
                            query_results[query_num] = {\
                                'total_execution_time': 0,
                                'total_buffer_wait_time': 0,
                                'total_seq_buffer_wait_time': 0,
                                'total_zicio_wait_time': 0,
                                'total_io_time': 0,
                                'total_io_time_w_buf': 0,
                                'total_io_time_wo_buf': 0,
                                'min_execution_time': 999999999999,
                                'min_buffer_wait_time': 999999999999,
                                'min_seq_buffer_wait_time': 999999999999,
                                'min_zicio_wait_time': 999999999999,
                                'min_io_time': 999999999999,
                                'min_io_time_w_buf': 999999999999,
                                'min_io_time_wo_buf': 999999999999,
                                'max_execution_time': 0,
                                'max_buffer_wait_time': 0,
                                'max_seq_buffer_wait_time': 0,
                                'max_zicio_wait_time': 0,
                                'max_io_time': 0,
                                'max_io_time_w_buf': 0,
                                'max_io_time_wo_buf': 0}
    
                        # Aggregate a result
                        query_results[query_num]['total_execution_time'] += execution_time
                        query_results[query_num]['total_buffer_wait_time'] += buffer_wait_time
                        query_results[query_num]['total_seq_buffer_wait_time'] += seq_buffer_wait_time
                        query_results[query_num]['total_zicio_wait_time'] += zicio_wait_time
                        query_results[query_num]['total_io_time'] += \
                            io_time_with_buf + io_time_without_buf
                        query_results[query_num]['total_io_time_w_buf'] += \
                            io_time_with_buf
                        query_results[query_num]['total_io_time_wo_buf'] += \
                            io_time_without_buf
    
                        query_results[query_num]['min_execution_time'] = \
                            min(query_results[query_num]['min_execution_time'], execution_time)
                        query_results[query_num]['min_buffer_wait_time'] = \
                            min(query_results[query_num]['min_buffer_wait_time'], buffer_wait_time)
                        query_results[query_num]['min_seq_buffer_wait_time'] = \
                            min(query_results[query_num]['min_seq_buffer_wait_time'], seq_buffer_wait_time)
                        query_results[query_num]['min_zicio_wait_time'] = \
                            min(query_results[query_num]['min_zicio_wait_time'], zicio_wait_time)
                        query_results[query_num]['min_io_time'] = \
                            min(query_results[query_num]['min_io_time'], \
                            io_time_with_buf + io_time_without_buf)
                        query_results[query_num]['min_io_time_w_buf'] = \
                            min(query_results[query_num]['min_io_time_w_buf'], io_time_with_buf)
                        query_results[query_num]['min_io_time_wo_buf'] = \
                            min(query_results[query_num]['min_io_time_wo_buf'], io_time_without_buf)
    
                        query_results[query_num]['max_execution_time'] = \
                            max(query_results[query_num]['max_execution_time'], execution_time)
                        query_results[query_num]['max_buffer_wait_time'] = \
                            max(query_results[query_num]['max_buffer_wait_time'], buffer_wait_time)
                        query_results[query_num]['max_seq_buffer_wait_time'] = \
                            max(query_results[query_num]['max_seq_buffer_wait_time'], seq_buffer_wait_time)
                        query_results[query_num]['max_zicio_wait_time'] = \
                            max(query_results[query_num]['max_zicio_wait_time'], zicio_wait_time)
                        query_results[query_num]['max_io_time'] = \
                            max(query_results[query_num]['max_io_time'], \
                            io_time_with_buf + io_time_without_buf)
                        query_results[query_num]['max_io_time_w_buf'] = \
                            max(query_results[query_num]['max_io_time_w_buf'], io_time_with_buf)
                        query_results[query_num]['max_io_time_wo_buf'] = \
                            max(query_results[query_num]['max_io_time_wo_buf'], io_time_without_buf)
        # Write result query by query  
        for query_num, query_result in query_results.items():
            # Calculate average
            avg_execution_time = \
                query_result['total_execution_time'] / client_numbers
            avg_buffer_wait_time = \
                query_result['total_buffer_wait_time'] / client_numbers
            avg_seq_buffer_wait_time = \
                query_result['total_seq_buffer_wait_time'] / client_numbers
            avg_zicio_wait_time = \
                query_result['total_zicio_wait_time'] / client_numbers
            avg_io_time = \
                query_result['total_io_time'] / client_numbers
            avg_io_time_w_buf = \
                query_result['total_io_time_w_buf'] / client_numbers
            avg_io_time_wo_buf = \
                query_result['total_io_time_wo_buf'] / client_numbers
    
            processed_result_file.write( \
                f"{query_num:>6} " + \
                f"{avg_execution_time:>13.6f} " + \
                f"{avg_buffer_wait_time:>13.6f} " + \
                f"{avg_seq_buffer_wait_time + avg_zicio_wait_time:>17.6f} " + \
                f"{avg_io_time:>12.6f} " + \
                f"{avg_io_time_w_buf:>19.6f} " + \
                f"{avg_io_time_wo_buf:>19.6f} " + \
                \
                f"{query_result['min_execution_time']:>13.6f} " + \
                f"{query_result['min_buffer_wait_time']:>13.6f} " + \
                f"{query_result['min_seq_buffer_wait_time'] + query_result['min_zicio_wait_time']:>17.6f} " + \
                f"{query_result['min_io_time']:>12.6f} " + \
                f"{query_result['min_io_time_w_buf']:>19.6f} " + \
                f"{query_result['min_io_time_wo_buf']:>19.6f} " + \
                \
                f"{query_result['max_execution_time']:>13.6f} " + \
                f"{query_result['max_buffer_wait_time']:>13.6f} " + \
                f"{query_result['max_seq_buffer_wait_time'] + query_result['min_zicio_wait_time']:>17.6f} " + \
                f"{query_result['max_io_time']:>12.6f} " + \
                f"{query_result['max_io_time_w_buf']:>19.6f} " + \
                f"{query_result['max_io_time_wo_buf']:>19.6f} " + \
                \
                "\n")
        # Close file
        processed_result_file.close()
        # Sort by query num
        os.system(f"sort -nk1 -o {processed_results_dir + f'{processed_data_file_name}.dat'} {processed_results_dir + f'{processed_data_file_name}.dat'}")



def do_data_processing(args, names, client_numbers=0, vanilla=False):
    """
    Data processing
    """
    global RUN_EVALUATION

    print("data processing start")

    if RUN_EVALUATION == EVALUATION_ONE:
        do_data_processing_eval1(args, names, vanilla)
    elif RUN_EVALUATION == EVALUATION_TWO:
        do_data_processing_eval2(args, names, vanilla)
    elif RUN_EVALUATION == EVALUATION_THREE:
        do_data_processing_eval3_postgres(args, names, client_numbers);

def preprocess_all_evaluation(args, run_db):
    global RUN_DB
    global QUERIES

    # Create cgroup memory directory if not exists
    if os.path.exists(CGROUP) == False:
        command = f"mkdir {CGROUP}"
        subprocess.call('echo {} | sudo -S {}'.format(args.user_password, command), shell=True)

    # Set data directory path
    set_data_dir_path(args, run_db)

    # Make data directory
    if RUN_DB == POSTGRESQL or RUN_DB == CITUS:
        if not os.path.exists(DB_DATA[RUN_DB]):
            pathlib.Path(DB_DATA[RUN_DB]).mkdir(parents=True, exist_ok=True)
    elif RUN_DB == MYSQL:
        if not os.path.exists(DB_DATA[RUN_DB][0]):
            pathlib.Path(DB_DATA[RUN_DB][0]).mkdir(parents=True, exist_ok=True)
        if not os.path.exists(DB_DATA[RUN_DB][1]):
            pathlib.Path(DB_DATA[RUN_DB][1]).mkdir(parents=True, exist_ok=True)

    # Compile database
    if args.compile:
        if RUN_DB == POSTGRESQL:
            compile_database(compile_option=args.postgres_compile_option)
        elif RUN_DB == MYSQL:
            compile_database(compile_option=args.mysql_compile_option)
        elif RUN_DB == CITUS:
            if args.zicio is True:
                args.citus_compile_option = " -DZICIO -DZICIO_STAT"

            compile_database(compile_option=args.citus_compile_option)
        else:
            exit(1)

    # Initialize config files
    if args.init_config:
        init_config_file(args)
        
    # Initialize database server
    if args.init:
        init_server(args)

    # Create table
    if args.create_table:
        try:
            if RUN_DB == POSTGRESQL or RUN_DB == CITUS:
                create_tpch_table(args)
            elif RUN_DB == MYSQL:
                create_tpch_table(args, vanilla=False)
                create_tpch_table(args, vanilla=True)
        finally:
            drop_os_caches(args)
    elif args.create_procedures and RUN_DB == MYSQL:
        create_procedures_mysql(args, vanilla=False)
        create_procedures_mysql(args, vanilla=True)

    existing_queries = ""
    if args.use_existing_queries:
        existing_queries = f"{pathlib.Path('.').absolute()}/results/queries/{DB_NAME[RUN_DB]}_zicio{args.scale_factor}/"
        if not os.path.exists(existing_queries):
            pathlib.Path(existing_queries).mkdir(parents=True, exist_ok=True)
        else:
            if RUN_DB == POSTGRESQL:
                for i in range(1, 23):
                    target_query = f"{existing_queries}{i}.sql"  
                    if not os.path.exists(target_query):
                        exit(1)
                    with open(target_query, 'r') as f_query:
                        QUERIES[i] = f_query.read()
                    if i == 15:
                        view_create = "15_create"
                        view_destroy = "15_destroy"
                        target_query = f"{existing_queries}{view_create}.sql"  
                        with open(target_query, 'r') as f_query:
                            QUERIES[view_create] = f_query.read()
                        target_query = f"{existing_queries}{view_destroy}.sql"  
                        with open(target_query, 'r') as f_query:
                            QUERIES[view_destroy] = f_query.read()
            elif RUN_DB == MYSQL:
                for i in range(1, 23):
                    target_args = f"{existing_queries}{i}.dat"  
                    if not os.path.exists(target_args):
                        exit(1)
                    with open(target_args, 'rb') as f_args:
                        PROCS_ARGS.append(pickle.load(f_args))
                        # print(PROCS_ARGS[i])
            else:
                print("wrong DB")

            return

    # Set query set for test
    if len(QUERY_PARAMS) == 0:
        for i in range(1, 23):
            if RUN_DB == POSTGRESQL or RUN_DB == CITUS:
                QUERY_PARAMS[i] = postgresql_query.get_query_parameters(i, args.scale_factor)
            elif RUN_DB == MYSQL:
                # QUERY_PARAMS[i] = mysql_procs.get_query_parameters(i, args.scale_factor)
                PROCS_ARGS.append(mysql_procs.get_query_parameters(i, args.scale_factor))

    for i in range(1, 23):
        if RUN_DB == POSTGRESQL or RUN_DB == CITUS:
            QUERIES[i] = postgresql_query.get_query_template(i)
            QUERIES[i] = QUERIES[i] % tuple(QUERY_PARAMS[i])
            QUERIES[i] = QUERIES[i].replace('\n', ' ')

            if i == 15:
                chunks_q15 = QUERIES[i].split(';')
                QUERIES[i] = "/* Q15 */ " + chunks_q15[1] + ';'
                QUERIES["15_create"] = chunks_q15[0].replace('/* Q15 */', '') + ';'
                QUERIES["15_destroy"] = chunks_q15[2] + ';'
                if args.use_existing_queries:
                    target_query = f"{existing_queries}{i}.sql"
                    with open(target_query, 'w') as f_query:
                        f_query.write(QUERIES[i])
                    view_create = "15_create"
                    view_destroy = "15_destroy"
                    target_query = f"{existing_queries}{view_create}.sql"  
                    with open(target_query, 'w') as f_query:
                        f_query.write(QUERIES[view_create])
                    target_query = f"{existing_queries}{view_destroy}.sql"  
                    with open(target_query, 'w') as f_query:
                        f_query.write(QUERIES[view_destroy])
            else:
                if args.use_existing_queries:
                    target_query = f"{existing_queries}{i}.sql"
                    with open(target_query, 'w') as f_query:
                        f_query.write(QUERIES[i])
        elif RUN_DB == MYSQL:
            if args.use_existing_queries:
                target_args = f"{existing_queries}{i}.dat"
                with open(target_args, 'wb') as f_args:
                    # print(PROCS_ARGS[i])
                    pickle.dump(PROCS_ARGS[i], f_args)
            

def preprocess_evaluation_postgres(args):
    global RESULT_DIR
    global RUN_DB
    global RUN_MODE

    # Set result directory
    RESULT_DIR = str(pathlib.Path("./results/" + DB[RUN_DB] + datetime.now().strftime( \
                "%y-%m-%d_%H:%M:%S")).absolute()) + "/"

    pathlib.Path(RESULT_DIR).mkdir(parents=True, exist_ok=True)

    # Save evaluation settings
    with open(RESULT_DIR + "args", 'w') as f:
        json.dump(args.__dict__, f, indent=2)
    os.system("cp " + DB_CONFIG[RUN_DB] + DB_NAME[RUN_DB] + "." + \
        DB_CONFIG_FILE[RUN_MODE] + " " + RESULT_DIR)
    os.system("cp " + DB_CONFIG[RUN_DB] + DB_NAME[RUN_DB] + "." + \
        DB_CONFIG_FILE[RUN_MODE] + " " + DB_DATA[RUN_DB] + "/postgresql.conf")

def preprocess_evaluation_mysql(args):
    global RESULT_DIR
    global RUN_DB
    global RUN_MODE

    # Set result directory
    RESULT_DIR = str(pathlib.Path("./results/" + DB[RUN_DB] + datetime.utcnow().strftime( \
                "%y-%m-%d_%H:%M:%S")).absolute()) + "/"

    pathlib.Path(RESULT_DIR).mkdir(parents=True, exist_ok=True)

    # Save evaluation settings
    with open(RESULT_DIR + "args", 'w') as f:
        json.dump(args.__dict__, f, indent=2)
    os.system("cp " + DB_CONFIG[RUN_DB] + DB_CONFIG_FILE_MY[RUN_MODE] + " " + RESULT_DIR)

def preprocess_evaluation_citus(args):
    global RESULT_DIR
    global RUN_DB
    global RUN_MODE

    # Set result directory
    RESULT_DIR = str(pathlib.Path("./results/" + DB[RUN_DB] + \
                        datetime.now().strftime("%y-%m-%d_%H:%M:%S")).absolute()) + "/"

    pathlib.Path(RESULT_DIR).mkdir(parents=True, exist_ok=True)

    # Save evaluation settings
    with open(RESULT_DIR + "args", 'w') as f:
        json.dump(args.__dict__, f, indent=2)
    os.system("cp " + DB_CONFIG[RUN_DB] + DB_NAME[RUN_DB] + "." + \
        DB_CONFIG_FILE[RUN_MODE] + " " + RESULT_DIR)
    os.system("cp " + DB_CONFIG[RUN_DB] + DB_NAME[RUN_DB] + "." + \
        DB_CONFIG_FILE[RUN_MODE] + " " + DB_DATA[RUN_DB] + "/postgresql.conf")


def preprocess_evaluation(args):
    global RUN_DB
    if RUN_DB == POSTGRESQL or RUN_DB == CITUS:
        preprocess_evaluation_postgres(args)
    elif RUN_DB == MYSQL:
        preprocess_evaluation_mysql(args)

def run_evaluation3(args, enable_zicio_shared_pool, client_numbers):
    global RUN_MODE
    global EVALUATION_THREE_START
    global EVALUATION_THREE_FINISH_COUNTER
    global EVALUATION_THREE_PROGRESS

    # Set evaluation name for post-processing data.
    if RUN_MODE == VANILLA_LARGE:
        evaluation_name = f"vanilla_large_cold_workers_{client_numbers}"
    elif RUN_MODE == VANILLA_SMALL:
        evaluation_name = f"vanilla_small_cold_workers_{client_numbers}"
    elif RUN_MODE == ZICIO_LARGE:
        evaluation_name = f"zicio_large_workers_{client_numbers}"
    elif RUN_MODE == ZICIO_SMALL:
        evaluation_name = f"zicio_small_workers_{client_numbers}"

    if RUN_MODE == ZICIO_LARGE or RUN_MODE == ZICIO_SMALL:
        enable_zicio = True
    else:
        enable_zicio = False

    target_query_numbers = [6]

    client_id = 0

    try:
        # Run database server
        run_server()
        # Turn on cgroup
        activate_cgroup(args, RUN_MODE)
        # Set swappiness
        adjust_vm_swappiness(args)
        # Drop caches
        drop_os_caches(args)
        # Create connector for postgres
        if enable_zicio_shared_pool:
            db, cursor = create_connector(args)

            for target_query_number in target_query_numbers:
                # Create shared pool for postgres
                create_shared_pool(args, cursor, target_query_number, False)

        # In evaluation 3 (= 5.4 TPC-H Workloads with Steam Sharing), we have to make warm buffer.
        if RUN_MODE == VANILLA_LARGE:
           print("making warm buffer...")
           tmp_client = Client(client_no=0, query_no=target_query_number, \
                             args=args, zicio=False, zicio_shared=False)
           tmp_client.execute_query(target_query_number)

        # Set these values before starting evaluation 3
        EVALUATION_THREE_FINISH_COUNTER = 0
        EVALUATION_THREE_START = False
        EVALUATION_THREE_PROGRESS = {}

        multi_query_clients = []

        for i in range(0, len(target_query_numbers)):
            target_query_number = target_query_numbers[i]
            client_id += 1

            # Run clients
            tmp_query_clients = \
                [Client(client_no = (client_id - 1) * 4 + i, query_no=target_query_number, \
                    args=args, zicio=enable_zicio, \
                    zicio_shared=enable_zicio_shared_pool) \
                        for i in range(0, client_numbers)]

            multi_query_clients.extend(tmp_query_clients)

        # Start clients
        for client in multi_query_clients:
            client.start()

        time.sleep(5)

        # Set start value to send signal threads
        EVALUATION_THREE_START = True

        # Print process of evaluation two
        while True:
            time.sleep(5)

            # Print process
            print("---- EVALUATION THREE PROGRESS " + evaluation_name + " ----")
            for query_num, progress in EVALUATION_THREE_PROGRESS.items():
                print(progress)
            print("")

            # If all threads are complete, break loop.
            if EVALUATION_THREE_FINISH_COUNTER == client_numbers:
                break

        # Wait to join
        for client in multi_query_clients:
            client.join()

        if enable_zicio_shared_pool:
            for target_query_number in target_query_numbers:
                # Destroy shared pool for postgres
                destroy_shared_pool(args, cursor, target_query_number, False)

            # Close connector for postgres
            close_connector(db, cursor)

        # Move vanilla cold data
        move_raw_result(args, name=evaluation_name)
    finally:
        # Shutdown database server
        shutdown_server(args)
        # Turn off cgroup
        deactivate_cgroup(args)

    do_data_processing(args, [evaluation_name], client_numbers=client_numbers)

def run_evaluation1(args, zicio_shared_pool):
    global RUN_DB
    global RUN_MODE
    global QUERIES_DICT
    zicio = False
    name_cold = ""
    name_hot = ""

    if RUN_MODE == VANILLA_LARGE:
        zicio_shared_pool = False
        name_cold = f"{DB_NAME[RUN_DB]}_eval1_vanilla_large_cold_workers_{args.worker_thread_num_eval1}"
        name_hot = f"{DB_NAME[RUN_DB]}_eval1_vanilla_large_hot_workers_{args.worker_thread_num_eval1}"
    elif RUN_MODE == VANILLA_SMALL:
        zicio_shared_pool = False
        name_cold = f"{DB_NAME[RUN_DB]}_eval1_vanilla_small_cold_workers_{args.worker_thread_num_eval1}"
        name_hot = f"{DB_NAME[RUN_DB]}_eval1_vanilla_small_hot_workers_{args.worker_thread_num_eval1}"
    elif RUN_MODE == ZICIO_LARGE:
        zicio = True
        if zicio_shared_pool:
            name_cold = f"{DB_NAME[RUN_DB]}_eval1_zicio_shared_large_cold_workers_{args.worker_thread_num_eval1}"
        else:
            name_cold = f"{DB_NAME[RUN_DB]}_eval1_zicio_large_cold_workers_{args.worker_thread_num_eval1}"
    else:
        zicio = True
        if zicio_shared_pool:
            name_cold = f"{DB_NAME[RUN_DB]}_eval1_zicio_shared_small_cold_workers_{args.worker_thread_num_eval1}"
        else:
            name_cold = f"{DB_NAME[RUN_DB]}_eval1_zicio_small_cold_workers_{args.worker_thread_num_eval1}"
    
    test_hot = args.test_hot and (RUN_DB == POSTGRESQL or RUN_DB == CITUS) and not zicio

    QUERIES_DICT={}
    for i in range(len(EVALUATION_ONE_TARGET_QUERY_MYSQL)):
        QUERIES_DICT[EVALUATION_ONE_TARGET_QUERY_MYSQL[i]] = i

    for query_no in EVALUATION_ONE_TARGET_QUERY_MYSQL:
        print ("Query " + str(query_no) + " start", flush=True)
        try:
            # Run database server
            run_server(print_log=True, vanilla=not zicio)
            # Turn on cgroup
            activate_cgroup(args, RUN_MODE)
            # Set swappiness
            adjust_vm_swappiness(args)
            # Drop caches
            drop_os_caches(args, print_log=True)
            
            # Create connector for database
            if zicio_shared_pool:
                db, cursor = create_connector(args)

                # Create shared pool for database
                create_shared_pool(args, cursor, query_no, False)

            # Run clients
            multi_query_clients = \
                [Client(client_no = i, query_no=[query_no], args=args, zicio=zicio, zicio_shared=zicio_shared_pool) \
                    for i in range(0, int(args.worker_thread_num_eval1))]

            # Start clients
            for client in multi_query_clients:
                client.start()

            for client in multi_query_clients:
                client.join()

            if RUN_DB == POSTGRESQL or RUN_DB == CITUS:
                move_raw_result(args, name=name_cold, query_no = query_no, vanilla=not zicio)
            if test_hot:
                print("cold finished")

                time.sleep(3)

                multi_query_clients = \
                    [Client(client_no = i, query_no=[query_no], args=args, zicio=zicio, zicio_shared=zicio_shared_pool) \
                        for i in range(0, int(args.worker_thread_num_eval1))]

                # Start clients
                for client in multi_query_clients:
                    client.start()

                for client in multi_query_clients:
                    client.join()

                if RUN_DB == POSTGRESQL or RUN_DB == CITUS:
                    move_raw_result(args, name=name_hot, query_no = query_no, vanilla=not zicio)
                print("hot finished")

            if zicio_shared_pool:
                # Destroy shared pool for postgres
                destroy_shared_pool(args, cursor, query_no, False)

                # Close connector for postgres
                close_connector(db, cursor)
        finally:
            # Shutdown database server
            shutdown_server(args, vanilla=not zicio)
            # Turn off cgroup
            deactivate_cgroup(args)

            if RUN_DB == MYSQL:
                move_raw_result(args, name=name_cold, query_no = query_no, vanilla=not zicio)

    if test_hot:
        do_data_processing(args, [name_cold, name_hot], vanilla=not zicio)
    else:
        do_data_processing(args, [name_cold], vanilla=not zicio)

def run_evaluation2(args, zicio_shared_pool):
    global EVALUATION_TWO_FINISH_COUNTER
    global EVALUATION_TWO_PROGRESS
    global EVALUATION_TWO_START
    global RUN_MODE
    global QUERIES_DICT
    
    EVALUATION_TWO_START = False
    EVALUATION_TWO_FINISH_COUNTER = 0
    EVALUATION_TWO_PROGRESS = [f"[Thread {i:>2}] " for i in range(int(args.worker_thread_num_eval2))]

    zicio=False
    name_eval = ''
    print_eval = ''

    if RUN_MODE == VANILLA_LARGE:
        zicio_shared_pool = False
        name_eval = f"{DB_NAME[RUN_DB]}_eval2_vanilla_large_workers_{args.worker_thread_num_eval2}"
        print_eval = '(Vanilla Large)'
    elif RUN_MODE == VANILLA_SMALL:
        zicio_shared_pool = False
        name_eval = f"{DB_NAME[RUN_DB]}_eval2_vanilla_small_workers_{args.worker_thread_num_eval2}"
        print_eval = '(Vanilla Small)'
    elif RUN_MODE == ZICIO_LARGE:
        zicio = True
        if zicio_shared_pool:
            name_eval = f"{DB_NAME[RUN_DB]}_eval2_zicio_shared_large_workers_{args.worker_thread_num_eval2}"
            print_eval = '(zicIO Shared Large)'
        else:
            name_eval = f"{DB_NAME[RUN_DB]}_eval2_zicio_large_workers_{args.worker_thread_num_eval2}"
            print_eval = '(zicIO Large)'
    else:
        zicio = True
        if zicio_shared_pool:
            name_eval = f"{DB_NAME[RUN_DB]}_eval2_zicio_shared_small_workers_{args.worker_thread_num_eval2}"
            print_eval = '(zicIO Shared Small)'
        else:
            name_eval = f"{DB_NAME[RUN_DB]}_eval2_zicio_small_workers_{args.worker_thread_num_eval2}"
            print_eval = '(zicIO Small)'

    QUERIES_DICT={}
    for i in range(len(EVALUATION_TWO_TARGET_QUERY_MYSQL)):
        QUERIES_DICT[EVALUATION_TWO_TARGET_QUERY_MYSQL[i]] = i

    try:
        # Run database server
        run_server(print_log=True, vanilla=not zicio)
        
        # Turn on cgroup
        activate_cgroup(args, RUN_MODE)
        
        # Set swappiness
        adjust_vm_swappiness(args)
        
        # Drop caches
        drop_os_caches(args, print_log=True)

        shuffled_queries = create_shuffled_queries_eval2()

        # Create connector for database
        if zicio_shared_pool:
            db, cursor = create_connector(args)

            # Create shared pool for database
            for query_no in shuffled_queries:
                create_shared_pool(args, cursor, query_no, False)

        # Set progress string
        # Run clients
        multi_query_clients = \
            [Client(client_no = i, query_no=shuffled_queries, \
                args=args, zicio=zicio, zicio_shared=zicio_shared_pool) \
                for i in range(0, int(args.worker_thread_num_eval2))]

        # Start clients
        for client in multi_query_clients:
            client.start()
            
        time.sleep(1)

        # Set start value to send signal threads
        EVALUATION_TWO_START = True
        print("Clients start")

        # Print process of evaluation two
        while True:
            time.sleep(5)

            # Print process
            print("---- EVALUATION TWO PROGRESS " + print_eval + " ----")
            for progress in EVALUATION_TWO_PROGRESS:
                print(progress)
            print("")

            # If all threads are complete, break loop.
            if EVALUATION_TWO_FINISH_COUNTER == int(args.worker_thread_num_eval2):
                break

        # Wait to join
        for client in multi_query_clients:
            client.join()

        # Move vanilla cold data
        if RUN_DB == POSTGRESQL or RUN_DB == CITUS:
            move_raw_result(args, name=name_eval, vanilla=not zicio)

        # Destroy shared pool for postgres
        if zicio_shared_pool:
            for query_no in shuffled_queries:
                destroy_shared_pool(args, cursor, query_no, False)

            # Close connector for postgres
            close_connector(db, cursor)

    finally:
        # Shutdown database server
        shutdown_server(args, vanilla=not zicio)
        # Turn off cgroup
        deactivate_cgroup(args)

        if RUN_DB == MYSQL:
            move_raw_result(args, name=name_eval, vanilla=not zicio)

    do_data_processing(args, [name_eval], vanilla=not zicio)

def run_evaluation(args, run_mode, run_evaluation_type, zicio_shared_pool, eval3_client_numbers=0):
    global RUN_MODE
    global RUN_DB
    global RUN_EVALUATION
    RUN_MODE = run_mode
    RUN_EVALUATION = run_evaluation_type

    preprocess_evaluation(args)

    #try:
        # Mount cgroup
        #mount_cgroup(args)
    if RUN_EVALUATION == EVALUATION_ONE:
        run_evaluation1(args, zicio_shared_pool)
    if RUN_EVALUATION == EVALUATION_TWO:
        run_evaluation2(args, zicio_shared_pool)
    if RUN_EVALUATION == EVALUATION_THREE:
        run_evaluation3(args, zicio_shared_pool, eval3_client_numbers)

    # except ProcessLookupError:
    #finally:
        # Unmount cgroup
        #unmount_cgroup(args)

def do_evaluation(args, RUN_DB):
    global BEGIN_TIME
    global RESULT_DIR
    global RUN_EVALUATION
    global EVALUATION_TWO_START
    global EVALUATION_TWO_PROGRESS
    global EVALUATION_TWO_FINISH_COUNTER

    if RUN_DB == POSTGRESQL:
        print("PostgreSQL evaluation start")
    elif RUN_DB == MYSQL:
        print("MySQL evaluation start")
    elif RUN_DB == CITUS:
        print("Citus evaluation start")
    else:
        exit(1)

    preprocess_all_evaluation(args, RUN_DB)

    # Evaluation 1
    print("Evaluation One Phase start")
    if args.vanilla:
        if args.eval1_small:
            print("Vanilla small start")
            run_evaluation(args, VANILLA_SMALL, EVALUATION_ONE, False)
            print("Vanilla small finish")
        if args.eval1_large:
            print("Vanilla large start")
            run_evaluation(args, VANILLA_LARGE, EVALUATION_ONE, False)
            print("Vanilla large finish")
    if args.zicio:
        if args.eval1_small:
            print("zicIO small start")
            run_evaluation(args, ZICIO_SMALL, EVALUATION_ONE, False)
            print("zicIO small finish")
        if args.eval1_large:
            print("zicIO large start")
            run_evaluation(args, ZICIO_LARGE, EVALUATION_ONE, False)
            print("zicIO large finish")

    if args.zicio_shared and args.eval1_small:
        print("zicIO shared small start")
        run_evaluation(args, ZICIO_SMALL, EVALUATION_ONE, True)
        print("zicIO shared small finish")

    # Evaluation 2 setting
    print("Evaluation Two Phase start")
    if args.vanilla:
        if args.eval2_small:
            print("Vanilla small start")
            run_evaluation(args, VANILLA_SMALL, EVALUATION_TWO, False)
            print("Vanilla small finish")
        if args.eval2_large:
            print("Vanilla large start")
            run_evaluation(args, VANILLA_LARGE, EVALUATION_TWO, False)
            print("Vanilla large finish")
    if args.zicio and args.eval2_small:
        print("zicIO small start")
        run_evaluation(args, ZICIO_SMALL, EVALUATION_TWO, False)
        print("zicIO small finish")
    if args.zicio_shared and args.eval2_small:
        print("zicIO shared small start")
        run_evaluation(args, ZICIO_SMALL, EVALUATION_TWO, True)
        print("zicIO shared small finish")

    # Evaluation Three Setting
    if args.eval3 and RUN_DB == POSTGRESQL:
        #target_client_numbers_list = [1, 4, 8, 16, 32]
        target_client_numbers_list = [1, 4, 8, 16, 32]

        print("Evaluation Three Phase Start")

        if args.vanilla:
            print("Vanilla small start")
            for client_number in target_client_numbers_list:
                run_evaluation(args, VANILLA_SMALL, EVALUATION_THREE, False, eval3_client_numbers=client_number)
            print("Vanilla small finish")

            print("Vanilla large start")
            for client_number in target_client_numbers_list:
                run_evaluation(args, VANILLA_LARGE, EVALUATION_THREE, False, eval3_client_numbers=client_number)
            print("Vanilla large finish")

        if args.zicio:
            print("zicIO small start")
            for client_number in target_client_numbers_list:
                run_evaluation(args, ZICIO_SMALL, EVALUATION_THREE, False, eval3_client_numbers=client_number)
            print("zicIO small finish")

        if args.zicio_shared:
            print("zicIO shared small start")
            for client_number in target_client_numbers_list:
                run_evaluation(args, ZICIO_SMALL, EVALUATION_THREE, True, eval3_client_numbers=client_number)
            print("zicIO shared small finish")

if __name__ == "__main__":
    # Make argument parser
    parser = argparse.ArgumentParser(description="Automator of zicio evaluation")

    # MySQL parser
    mysql_parser = parser.add_argument_group('mysql', 'mysql options')
    # PostgrSQL parser
    pgsql_parser = parser.add_argument_group('pgsql', 'postgresql options')
    # Evaluation parser
    options_parser = parser.add_argument_group('options', 'other options')

    options_parser.add_argument("--database", default="all", help="mysql, postgres, citus, all")
    options_parser.add_argument("--postgres-compile-option", default="-DZICIO -DZICIO_STAT", help="compile options")
    options_parser.add_argument("--mysql-compile-option", default="-D__ZICIO_ACTIVATE -D__ZICIO_PAGINATION -D__ZICIO_STAT", help="compile options")
    options_parser.add_argument("--citus-compile-option", default="-DZICIO -DZICIO_STAT", help="compile options")
    options_parser.add_argument("--compile", action='store_true', help="compile database")
    options_parser.add_argument("--init", action='store_true', help="initialize database")
    options_parser.add_argument("--init-config", action='store_true', help="initialize config")
    options_parser.add_argument("--create-table", action='store_true', help="unmount cgroup")
    options_parser.add_argument("--copy-data", action='store_true', help="unmount cgroup")
    options_parser.add_argument("--user-password", default="", help="user password")
    options_parser.add_argument("--db-password", default="", help="db password")
    options_parser.add_argument("--use-existing-queries", action='store_true', help="whether to use existing queries")

    # Global experimantal setting
    options_parser.add_argument("--build-thread-num", default=os.cpu_count() - 1, help="build thread number")
    options_parser.add_argument("--worker-thread-num-eval1", default=1, help="worker thread number for evaluation 1")
    options_parser.add_argument("--worker-thread-num-eval2", default=20, help="worker thread number for evaluation 2")
    options_parser.add_argument("--scale-factor", default=100, help="TPC-H scale factor")
    options_parser.add_argument("--device-path", default="/opt/nvme", help="target device path")
    options_parser.add_argument("--device-user-path", default="/zicio/", help="user space path in target device")
    options_parser.add_argument("--eval1-small", action='store_true', help="Run test for evaluation 1 with small buffer")
    options_parser.add_argument("--eval1-large", action='store_true', help="Run test for evaluation 1 large buffer")
    options_parser.add_argument("--eval2-small", action='store_true', help="Run test for evaluation 2 small buffer")
    options_parser.add_argument("--eval2-large", action='store_true', help="Run test for evaluation 2 large buffer")
    options_parser.add_argument("--eval3", action='store_true', help="Run test for evaluation 3 (= figure 9)")
    options_parser.add_argument("--test-hot", action='store_true', help="Run test for hot buffer")
    options_parser.add_argument("--vanilla", action='store_true', help="Run test for vanilla small buffer")
    options_parser.add_argument("--zicio", action='store_true', help="Run test for zicio local channel")
    options_parser.add_argument("--zicio-shared", action='store_true', help="Run test for zicio shared channel")
    options_parser.add_argument("--create-procedures", action='store_true', help="Create procedures")

    # MySQL setting
    mysql_parser.add_argument("--mysql-host", default="localhost", help="mysql host")
    mysql_parser.add_argument("--mysql-db", default="tpch", help="mysql database")
    mysql_parser.add_argument("--mysql-user", default="root", help="mysql user")
    mysql_parser.add_argument("--mysql-port", default="23456", help="mysql port number")
    mysql_parser.add_argument("--mysql-socket", default=os.getcwd() + "/" + \
            "databases"+ DB[MYSQL] +"inst/mysql.sock", help="mysql.sock file path")

    # PostgreSQL setting
    pgsql_parser.add_argument("--pgsql-host", default="localhost", help="pgsql host")
    pgsql_parser.add_argument("--pgsql-db", default="tpch", help="pgsql database")
    pgsql_parser.add_argument("--pgsql-user", default="tpch", help="pgsql user")
    pgsql_parser.add_argument("--pgsql-port", default="12345", help="pgsql port number")
    pgsql_parser.add_argument("--datadir-path", default="/data", help="base directory path")
    pgsql_parser.add_argument("--backup-datadir-path", default="/data_backup", help="base directory path")

    args=parser.parse_args()

    if args.database == "mysql" or args.database == "all":
        # Set target DB 
        RUN_DB = MYSQL
        do_evaluation(args, MYSQL)
    if args.database == "postgres" or args.database == "all":
        # Set target DB 
        RUN_DB = POSTGRESQL
        do_evaluation(args, POSTGRESQL)
    if args.database == "citus" or args.database == "all":
        # Set target DB
        RUN_DB = CITUS
        do_evaluation(args, CITUS)
