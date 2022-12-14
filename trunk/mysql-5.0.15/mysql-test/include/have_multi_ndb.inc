# Setup connections to both MySQL Servers connected to the cluster
connect (server1,127.0.0.1,root,,test,$MASTER_MYPORT,);
connect (server2,127.0.0.1,root,,test,$MASTER_MYPORT1,);

# Check that server1 has NDB  support
connection server1;
disable_query_log;
--disable_warnings
drop table if exists t1, t2;
--enable_warnings
flush tables;
@r/have_ndb.require show variables like "have_ndbcluster";
# @r/server_id.require show variables like "server_id";
enable_query_log;

# Check that server2 has NDB support
connection server2;
disable_query_log;
--disable_warnings
drop table if exists t1, t2;
--enable_warnings
flush tables;
@r/have_ndb.require show variables like "have_ndbcluster";
# @r/server_id1.require show variables like "server_id";
enable_query_log;

# Set the default connection to 'server1'
connection server1;
