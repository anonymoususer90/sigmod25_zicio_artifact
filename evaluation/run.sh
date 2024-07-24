#!/bin/bash

PASSWORD=""

# Parse parameters
for i in "$@"
do
  case $i in
    --password=*)
      PASSWORD="${i#*=}"
      shift
      ;;

    *)
      # unknown option
      echo "Unknown option: ${i}"
      exit
      ;;
  esac
done



# Create database for MySQL
# python3 ./run.py --init-config --init --create-table --user-password=${PASSWORD} --db-password=zicio --build-thread-num=92 --scale-factor=100 --compile --database=mysql --use-existing-queries

# Create database for PostgreSQL
# python3 ./run.py --init-config --init --create-table --user-password=${PASSWORD} --db-password=zicio --build-thread-num=92 --scale-factor=100 --compile --database=postgres --use-existing-queries

# Run worker #1 MySQL
# python3 ./run.py --user-password=${PASSWORD} --db-password=zicio --worker-thread-num-eval1=1 --scale-factor=100 --eval1-small --eval1-large --vanilla --zicio --zicio-shared --database=mysql --use-existing-queries

# Run worker #2 MySQL
# python3 ./run.py --user-password=${PASSWORD} --db-password=zicio --worker-thread-num-eval1=2 --scale-factor=100 --eval1-small --eval1-large --vanilla --zicio --zicio-shared --database=mysql --use-existing-queries

# Run worker #4 MySQL
# python3 ./run.py --user-password=${PASSWORD} --db-password=zicio --worker-thread-num-eval1=4 --scale-factor=100 --eval1-small --eval1-large --vanilla --zicio --zicio-shared --database=mysql --use-existing-queries

# Run worker #8 MySQL
# python3 ./run.py --user-password=${PASSWORD} --db-password=zicio --worker-thread-num-eval1=8 --scale-factor=100 --eval1-small --eval1-large --vanilla --zicio --zicio-shared --database=mysql --use-existing-queries

# Run worker #16 MySQL
# python3 ./run.py --user-password=${PASSWORD} --db-password=zicio --worker-thread-num-eval1=16 --scale-factor=100 --eval1-small --eval1-large --vanilla --zicio --zicio-shared --database=mysql --use-existing-queries

# Run worker #1 Postgres
# python3 ./run.py --user-password=${PASSWORD} --db-password=zicio --worker-thread-num-eval1=1 --scale-factor=100 --eval1-small --eval1-large --vanilla --zicio --zicio-shared --database=postgres --use-existing-queries --test-hot

# Run worker #2 Postgres
# python3 ./run.py --user-password=${PASSWORD} --db-password=zicio --worker-thread-num-eval1=2 --scale-factor=100 --eval1-small --eval1-large --vanilla --zicio --zicio-shared --database=postgres --use-existing-queries --test-hot

# Run worker #4 Postgres
# python3 ./run.py --user-password=${PASSWORD} --db-password=zicio --worker-thread-num-eval1=4 --scale-factor=100 --eval1-small --eval1-large --vanilla --zicio --zicio-shared --database=postgres --use-existing-queries --test-hot

# Run worker #8 Postgres
# python3 ./run.py --user-password=${PASSWORD} --db-password=zicio --worker-thread-num-eval1=8 --scale-factor=100 --eval1-small --eval1-large --vanilla --zicio --zicio-shared --database=postgres --use-existing-queries --test-hot

# Run worker #16 Postgres
# python3 ./run.py --user-password=${PASSWORD} --db-password=zicio --worker-thread-num=16 --scale-factor=100 --eval1-small --eval1-large --vanilla --zicio --zicio-shared --database=postgre --use-existing-queries

# Evaluation 3
# python3 ./run.py --user-password=${PASSWORD} --db-password=zicio --worker-thread-num=1 --scale-factor=100 --eval3 --vanilla --zicio --zicio-shared --database=postgre --use-existing-queries
