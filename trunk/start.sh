#!/bin/bash

# pkill mysqld

# sleep 3s

/usr/local/libexec/mysqld --defaults-file=./my.cnf --user=mysql --datadir=/usr/local/share/mysql/data &

