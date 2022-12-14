/* Copyright (C) 2000 MySQL AB & MySQL Finland AB & TCX DataKonsult AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/*
  This files defines some MySQL C API functions that are server specific
*/

#include "mysql_priv.h"

/*
  Function called by my_net_init() to set some check variables
*/

extern "C"
{
  void my_net_local_init(NET *net)
  {
#ifndef EMBEDDED_LIBRARY
    net->max_packet = (uint)global_system_variables.net_buffer_length;
    net->read_timeout = (uint)global_system_variables.net_read_timeout;
    net->write_timeout = (uint)global_system_variables.net_write_timeout;
    net->retry_count = (uint)global_system_variables.net_retry_count;
    net->max_packet_size = max(global_system_variables.net_buffer_length, global_system_variables.max_allowed_packet);
#endif
  }
}
