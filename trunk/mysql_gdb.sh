#!/bin/bash

PID=`ps -ef | grep mysqld | grep -v grep | awk '{print $2}'`

if [ "" == "$PID" ]; then
    echo "ERROR: PID is NULL"
fi

gdb -p $PID
