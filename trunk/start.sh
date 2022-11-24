#!/bin/bash

pkill mysqld

sleep 1s

/usr/local/libexec/mysqld --defaults-file=./my.cnf --user=mysql --datadir=/usr/local/share/mysql/data &

