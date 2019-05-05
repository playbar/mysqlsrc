/* Copyright (c) 2008 MySQL AB, 2009 Sun Microsystems, Inc.
   Use is subject to license terms.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */


#include "semisync_slave.h"

ReplSemiSyncSlave repl_semisync_slave;
char rpl_semi_sync_slave_enabled;
char rpl_semi_sync_slave_delay_master;
char rpl_semi_sync_slave_status= 0;
unsigned long rpl_semi_sync_slave_trace_level;
bool semi_sync_need_reply= false;
unsigned int rpl_semi_sync_slave_kill_conn_timeout;
unsigned long long rpl_semi_sync_slave_send_ack = 0;

int ReplSemiSyncSlave::initObject()
{
  int result= 0;

  init_done_ = true;

  /* References to the parameter works after set_options(). */
  setSlaveEnabled(rpl_semi_sync_slave_enabled);
  setTraceLevel(rpl_semi_sync_slave_trace_level);
  setDelayMaster(rpl_semi_sync_slave_delay_master);
  setKillConnTimeout(rpl_semi_sync_slave_kill_conn_timeout);

  return result;
}

int ReplSemiSyncSlave::slaveReadSyncHeader(const char *header,
                                      unsigned long total_len,
                                      int  *semi_flags,
                                      const char **payload,
                                      unsigned long *payload_len)
{
  const char *kWho = "ReplSemiSyncSlave::slaveReadSyncHeader";
  int read_res = 0;
  function_enter(kWho);

  if (rpl_semi_sync_slave_status)
  {
    if (DBUG_EVALUATE_IF("semislave_corrupt_log", 0, 1)
        && (unsigned char)(header[0]) == kPacketMagicNum)
    {
      semi_sync_need_reply  = (header[1] & kPacketFlagSync);
      *payload_len = total_len - 2;
      *payload     = header + 2;

      if (trace_level_ & kTraceDetail)
        sql_print_information("%s: reply - %d", kWho, semi_sync_need_reply);

      if (semi_sync_need_reply)
        *semi_flags |= SEMI_SYNC_NEED_ACK;
      if (isDelayMaster())
        *semi_flags |= SEMI_SYNC_SLAVE_DELAY_SYNC;
    }
    else
    {
      sql_print_error("Missing magic number for semi-sync packet, packet "
                      "len: %lu", total_len);
      read_res = -1;
    }
  } else {
    *payload= header;
    *payload_len= total_len;
  }

  return function_exit(kWho, read_res);
}

int ReplSemiSyncSlave::slaveStart(Master_info *mi)
{
  bool semi_sync= getSlaveEnabled();

  sql_print_information("Slave I/O thread: Start %s replication to\
 master '%s@%s:%d' in log '%s' at position %lu",
			semi_sync ? "semi-sync" : "asynchronous",
			const_cast<char *>(mi->get_user()), mi->host, mi->port,
			const_cast<char *>(mi->get_master_log_name()),
                        (unsigned long)(mi->get_master_log_pos()));

  if (semi_sync && !rpl_semi_sync_slave_status)
    rpl_semi_sync_slave_status= 1;

  /*clear the counter*/
  rpl_semi_sync_slave_send_ack= 0;
  return 0;
}

int ReplSemiSyncSlave::slaveStop(Master_info *mi)
{
  if (rpl_semi_sync_slave_status)
    rpl_semi_sync_slave_status= 0;
  if (getSlaveEnabled())
    killConnection(mi->mysql);
  return 0;
}

int ReplSemiSyncSlave::resetSlave(Master_info *mi)
{
  /*TODO: reset all slave semisync status*/
  return 0;
}

void ReplSemiSyncSlave::killConnection(MYSQL *mysql)
{
  if (!mysql)
    return;

  char kill_buffer[30];
  MYSQL *kill_mysql = NULL;
  kill_mysql = mysql_init(kill_mysql);
  mysql_options(kill_mysql, MYSQL_OPT_CONNECT_TIMEOUT, &kill_conn_timeout_);
  mysql_options(kill_mysql, MYSQL_OPT_READ_TIMEOUT, &kill_conn_timeout_);
  mysql_options(kill_mysql, MYSQL_OPT_WRITE_TIMEOUT, &kill_conn_timeout_);

  bool ret= (!mysql_real_connect(kill_mysql, mysql->host,
            mysql->user, mysql->passwd,0, mysql->port, mysql->unix_socket, 0));
  if (DBUG_EVALUATE_IF("semisync_slave_failed_kill", 1, 0) || ret)
  {
    sql_print_information("cannot connect to master to kill slave io_thread's "
                          "connection");
    if (!ret)
      mysql_close(kill_mysql);
    return;
  }
  uint kill_buffer_length = my_snprintf(kill_buffer, 30, "KILL %lu",
                                        mysql->thread_id);
  mysql_real_query(kill_mysql, kill_buffer, kill_buffer_length);
  mysql_close(kill_mysql);
}

int ReplSemiSyncSlave::requestTransmit(Master_info *mi)
{
  MYSQL *mysql= mi->mysql;
  MYSQL_RES *res= 0;
  MYSQL_ROW row;
  const char *query;

  if (!getSlaveEnabled())
    return 0;

  query= "SHOW VARIABLES LIKE 'rpl_semi_sync_master_enabled'";
  if (mysql_real_query(mysql, query, strlen(query)) ||
      !(res= mysql_store_result(mysql)))
  {
    sql_print_error("Execution failed on master: %s, error :%s", query, mysql_error(mysql));
    return 1;
  }

  row= mysql_fetch_row(res);
  if (DBUG_EVALUATE_IF("master_not_support_semisync", 1, 0)
      || !row)
  {
    /* Master does not support semi-sync */
    sql_print_warning("Master server does not support semi-sync, "
                      "fallback to asynchronous replication");
    rpl_semi_sync_slave_status= 0;
    mysql_free_result(res);
    return 0;
  }
  mysql_free_result(res);

  /*
   Tell master dump thread that we want to do semi-sync
   replication
  */
  query= "SET @rpl_semi_sync_slave= 1";
  if (mysql_real_query(mysql, query, strlen(query)))
  {
    sql_print_error("Set 'rpl_semi_sync_slave=1' on master failed");
    return 1;
  }
  mysql_free_result(mysql_store_result(mysql));
  rpl_semi_sync_slave_status= 1;

  return 0;
}

int ReplSemiSyncSlave::slaveReply(Master_info *mi)
{
  const char *kWho = "ReplSemiSyncSlave::slaveReply";
  MYSQL* mysql= mi->mysql;
  const char *binlog_filename= const_cast<char *>(mi->get_master_log_name());
  my_off_t binlog_filepos= mi->get_master_log_pos();

  NET *net= &mysql->net;
  uchar reply_buffer[REPLY_MAGIC_NUM_LEN
                     + REPLY_BINLOG_POS_LEN
                     + REPLY_BINLOG_NAME_LEN];
  int  reply_res = 0;
  int name_len = strlen(binlog_filename);

  function_enter(kWho);

  if (rpl_semi_sync_slave_status && semi_sync_need_reply)
  {
    /* Prepare the buffer of the reply. */
    reply_buffer[REPLY_MAGIC_NUM_OFFSET] = kPacketMagicNum;
    int8store(reply_buffer + REPLY_BINLOG_POS_OFFSET, binlog_filepos);
    memcpy(reply_buffer + REPLY_BINLOG_NAME_OFFSET,
           binlog_filename,
           name_len + 1 /* including trailing '\0' */);

    if (trace_level_ & kTraceDetail)
      sql_print_information("%s: reply (%s, %lu)", kWho,
                            binlog_filename, (ulong)binlog_filepos);

    net_clear(net, 0);
    /* Send the reply. */
    reply_res = my_net_write(net, reply_buffer,
                             name_len + REPLY_BINLOG_NAME_OFFSET);
    if (!reply_res)
    {
      reply_res = DBUG_EVALUATE_IF("semislave_failed_net_flush", 1, net_flush(net));
      if (reply_res)
        sql_print_error("Semi-sync slave net_flush() reply failed");
      rpl_semi_sync_slave_send_ack++;
    }
    else
    {
      sql_print_error("Semi-sync slave send reply failed: %s (%d)",
                      net->last_error, net->last_errno);
    }
  }

  return function_exit(kWho, reply_res);
}
