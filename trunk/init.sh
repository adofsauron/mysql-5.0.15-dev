#!/bin/bash

groupadd mysql
useradd -g mysql mysql

rm /usr/local/share/mysql/data -rf

mkdir -p /usr/local/share/mysql/data 

chown -R mysql:mysql /usr/local/share/mysql/data 

mkdir -p /var/log/mysql/

chown -R mysql:mysql /var/log/mysql/

/usr/local/bin/mysql_install_db --user=mysql --datadir=/usr/local/share/mysql/data


chown -R mysql:mysql /usr/local/share/mysql/data 

