/* Copyright (c) 2004, 2019, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file ha_vtml.cc

  @brief
  This class sends the field name and row info to the remote server
  by VTMLCommunicationHandler. Differently from any other storage engines,
  it stores the data in the server. In need of reading the data this class sends
  a json string to the server and request the data.
*/

#include "storage/vtml/ha_vtml.h"

#include "my_dbug.h"
#include "mysql/plugin.h"
#include "sql/sql_class.h"
#include "sql/sql_plugin.h"
#include "typelib.h"
#include <stdio.h>
#include <stdlib.h>
#include <storage/vtml/VTMLCommunicationHandler.h>
#include "sql_string.h"
#include "sql/field.h"
#include <string>
#include <vector>
void replaceString(std::string& str, const std::string& occurance,
                          const std::string& replace) {
    size_t pos = 0;
    while ((pos = str.find(occurance, pos)) != std::string::npos) {
        str.replace(pos, occurance.length(), replace);
         pos += replace.length();
    }
}

static handler *vtml_create_handler(handlerton *hton, TABLE_SHARE *table,
                                       bool partitioned, MEM_ROOT *mem_root);

handlerton *vtml_hton;

/* Interface to mysqld, to check system tables supported by SE */
static bool vtml_is_supported_system_table(const char *db,
                                              const char *table_name,
                                              bool is_sql_layer_system_table);

Vtml_share::Vtml_share() { thr_lock_init(&lock); }

static int vtml_init_func(void *p) {
  DBUG_TRACE;

  vtml_hton = (handlerton *)p;
  vtml_hton->state = SHOW_OPTION_YES;
  vtml_hton->create = vtml_create_handler;
  vtml_hton->flags = HTON_CAN_RECREATE;
  vtml_hton->is_supported_system_table = vtml_is_supported_system_table;

  return 0;
}

/**
  @brief
  Example of simple lock controls. The "share" it creates is a
  structure we will pass to each vtml handler. Do you have to have
  one of these? Well, you have pieces that are used for locking, and
  they are needed to function.
*/
Vtml_share *ha_vtml::get_share() {
    Vtml_share *tmp_share;

  DBUG_TRACE;

  lock_shared_ha_data();
  if (!(tmp_share = static_cast<Vtml_share *>(get_ha_share_ptr()))) {
    tmp_share = new Vtml_share;
    if (!tmp_share) goto err;

    set_ha_share_ptr(static_cast<Handler_share *>(tmp_share));
  }
err:
  unlock_shared_ha_data();
  return tmp_share;
}

static handler *vtml_create_handler(handlerton *hton, TABLE_SHARE *table,
                                       bool, MEM_ROOT *mem_root) {
  return new (mem_root) ha_vtml(hton, table);
}

ha_vtml::ha_vtml(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg) {}

/*
  List of all system tables specific to the SE.
  Array element would look like below,
     { "<database_name>", "<system table name>" },
  The last element MUST be,
     { (const char*)NULL, (const char*)NULL }

  This array is optional, so every SE need not implement it.
*/
static st_handler_tablename ha_vtml_system_tables[] = {
    {(const char *)NULL, (const char *)NULL}};

/**
  @brief Check if the given db.tablename is a system table for this SE.

  @param db                         Database name to check.
  @param table_name                 table name to check.
  @param is_sql_layer_system_table  if the supplied db.table_name is a SQL
                                    layer system table.

  @return
    @retval true   Given db.table_name is supported system table.
    @retval false  Given db.table_name is not a supported system table.
*/
static bool vtml_is_supported_system_table(const char *db,
                                              const char *table_name,
                                              bool is_sql_layer_system_table) {
  st_handler_tablename *systab;

  // Does this SE support "ALL" SQL layer system tables ?
  if (is_sql_layer_system_table) return false;

  // Check if this is SE layer system tables
  systab = ha_vtml_system_tables;
  while (systab && systab->db) {
    if (systab->db == db && strcmp(systab->tablename, table_name) == 0)
      return true;
    systab++;
  }

  return false;
}

/**
  @brief
  Used for opening tables. The name will be the name of the file.

  @details
  A table is opened when it needs to be opened; e.g. when a request comes in
  for a SELECT on the table (tables are not open and closed for each request,
  they are cached).

  Called from handler.cc by handler::ha_open(). The server opens all tables by
  calling ha_open() which then calls the handler specific open().

  @see
  handler::ha_open() in handler.cc
*/

int ha_vtml::open(const char *, int, uint, const dd::Table *) {
  DBUG_TRACE;

  if (!(share = get_share())) return 1;
  thr_lock_data_init(&share->lock, &lock, NULL);

  return 0;
}

/**
  @brief
  Closes a table.

  @details
  Called from sql_base.cc, sql_select.cc, and table.cc. In sql_select.cc it is
  only used to close up temporary tables or during the process where a
  temporary table is converted over to being a myisam table.

  For sql_base.cc look at close_data_tables().

  @see
  sql_base.cc, sql_select.cc and table.cc
*/

int ha_vtml::close(void) {
  DBUG_TRACE;
  return 0;
}

/**
  @brief
  write_row() inserts a row. No extra() hint is given currently if a bulk load
  is happening. buf() is a byte array of data. You can use the field
  information to extract the data from the native byte array type.

  @details
  Example of this would be:
  @code
  for (Field **field=table->field ; *field ; field++)
  {
    ...
  }
  @endcode

  See ha_tina.cc for an example of extracting all of the data as strings.
  ha_berekly.cc has an example of how to store it intact by "packing" it
  for ha_berkeley's own native storage type.

  See the note for update_row() on auto_increments. This case also applies to
  write_row().

  Called from item_sum.cc, item_sum.cc, sql_acl.cc, sql_insert.cc,
  sql_insert.cc, sql_select.cc, sql_table.cc, sql_udf.cc, and sql_update.cc.

  @see
  item_sum.cc, item_sum.cc, sql_acl.cc, sql_insert.cc,
  sql_insert.cc, sql_select.cc, sql_table.cc, sql_udf.cc and sql_update.cc
*/

int ha_vtml::encode_quote() {

  char attribute_buffer[1024];
  String attribute(attribute_buffer, sizeof(attribute_buffer), &my_charset_bin);

  std::string table_str(table->alias);
  auto const pos=table_str.find_last_of('/');
  const auto extracted_table_name=table_str.substr(pos+1);

  for (Field **field = table->field; *field; field++) {
    const char *ptr;
    const char *end_ptr;
    const bool was_null = (*field)->is_null();

    String JsonString;
    JsonString.length(0);
    JsonString.append("{\"token\":\"");
    JsonString.append(extracted_table_name.c_str());
    JsonString.append("\",\"learnmode\":true,\"activationThreshold\":0.49,\"outputLMT\":0,\"inputFeature\":['");
    JsonString.append((*field)->field_name);
    JsonString.append("']}");
    sendLearningData(JsonString.c_ptr());

    buffer.length(0);

   //  assistance for backwards compatibility in production builds.
   //   note: this will not work for ENUM columns.
    if (was_null) {
      (*field)->set_default();
      (*field)->set_notnull();
    }

    (*field)->val_str(&attribute, &attribute);

    if (was_null) (*field)->set_null();

    if ((*field)->str_needs_quotes()) {
      ptr = attribute.ptr();
      end_ptr = attribute.length() + ptr;

      buffer.append('"');

      for (; ptr < end_ptr; ptr++) {
        if (*ptr == '"') {
          buffer.append('\\');
          buffer.append('"');
        } else if (*ptr == '\r') {
          buffer.append('\\');
          buffer.append('r');
        } else if (*ptr == '\\') {
          buffer.append('\\');
          buffer.append('\\');
        } else if (*ptr == '\n') {
          buffer.append('\\');
          buffer.append('n');
        } else
          buffer.append(*ptr);
      }
      buffer.append('"');
    } else {
      buffer.append(attribute);
    }

    std::string value_str(buffer.c_ptr());
    replaceString(value_str, "\"","");
    JsonString.length(0);
    JsonString.append("{\"token\":\"");
    JsonString.append(extracted_table_name.c_str());
    JsonString.append("\",\"learnmode\":true,\"activationThreshold\":0.49,\"outputLMT\":20,\"inputFeature\":['");
    JsonString.append(value_str.c_str());
    JsonString.append("']}");
    sendLearningData(JsonString.c_ptr());

  }

  return (buffer.length());
}


int ha_vtml::write_row(uchar *) {
 
  DBUG_TRACE;
  encode_quote();
  return 0;
}

int ha_vtml::encode_update_quote(uchar *buf) {
  char attribute_buffer[1024];
  String attribute(attribute_buffer, sizeof(attribute_buffer), &my_charset_bin);

  std::string table_str(table->alias);
  auto const pos=table_str.find_last_of('/');
  const auto extracted_table_name=table_str.substr(pos+1);

  m_update_row_items.clear();
  m_fields.clear();
  for (Field **field = table->field; *field; field++) {
    const char *ptr;
    const char *end_ptr;
    const bool was_null = (*field)->is_null();

    buffer.length(0);

    if (was_null) {
      (*field)->set_default();
      (*field)->set_notnull();
    }

    (*field)->val_str(&attribute, &attribute);

    if (was_null) (*field)->set_null();

    if ((*field)->str_needs_quotes()) {
      ptr = attribute.ptr();
      end_ptr = attribute.length() + ptr;

      buffer.append('"');

      for (; ptr < end_ptr; ptr++) {
        if (*ptr == '"') {
          buffer.append('\\');
          buffer.append('"');
        } else if (*ptr == '\r') {
          buffer.append('\\');
          buffer.append('r');
        } else if (*ptr == '\\') {
          buffer.append('\\');
          buffer.append('\\');
        } else if (*ptr == '\n') {
          buffer.append('\\');
          buffer.append('n');
        } else
          buffer.append(*ptr);
      }
      buffer.append('"');
    } else {
      buffer.append(attribute);
    }
    m_update_row_items.push_back(buffer.c_ptr());
    m_fields.push_back((*field)->field_name);
  }

  String JsonFeature;
  JsonFeature.length(0);
  String JsonInput;
  JsonInput.length(0);

  JsonInput.append("{\"token\":\"");
  JsonInput.append(extracted_table_name.c_str());
  JsonInput.append("\",\"learnmode\":true,\"activationThreshold\":0.49,\"outputLMT\":20,\"inputFeature\":['");
  JsonFeature.append("{\"token\":\"");
  JsonFeature.append(extracted_table_name.c_str());
  JsonFeature.append("\",\"learnmode\":true,\"activationThreshold\":0.49,\"outputLMT\":0,\"inputFeature\":['");

  for (int i = 0; i < m_update_row_items.size(); i++){

     if (m_update_row_items[i].find(m_row_items[i]) == std::string::npos )
     {
        std::string value_str(m_update_row_items[i].c_str());
        replaceString(value_str, "\"","");
        JsonInput.append(value_str.c_str());
        JsonFeature.append(m_fields[i].c_str());
        break;
     }
  }
  JsonFeature.append("'],\"featureMatrix\":[");
  for (int i = 0; i < m_update_row_items.size(); i++){
     if (m_update_row_items[i].find(m_row_items[i]) != std::string::npos)
     {
        JsonFeature.append("'");
        std::string value_str(m_update_row_items[i].c_str());
        replaceString(value_str, "\"","");
        JsonFeature.append(value_str.c_str());
        JsonFeature.append("',");
     }
  }
  JsonInput.append("']}");
  JsonFeature.length(JsonFeature.length() - 1);
  JsonFeature.append("]}");

  sendLearningData(JsonFeature.c_ptr());
  sendLearningData(JsonInput.c_ptr());
  return (buffer.length());
}

/**
  @brief
  Yes, update_row() does what you expect, it updates a row. old_data will have
  the previous row record in it, while new_data will have the newest data in it.
  Keep in mind that the server can do updates based on ordering if an ORDER BY
  clause was used. Consecutive ordering is not guaranteed.

  @details
  Currently new_data will not have an updated auto_increament record. You can
  do this for example by doing:

  @code

  if (table->next_number_field && record == table->record[0])
    update_auto_increment();

  @endcode

  Called from sql_select.cc, sql_acl.cc, sql_update.cc, and sql_insert.cc.

  @see
  sql_select.cc, sql_acl.cc, sql_update.cc and sql_insert.cc
*/
int ha_vtml::update_row(const uchar *, uchar *new_data) {
  DBUG_TRACE;

  encode_update_quote(new_data);

  return 0;
}

/**
  @brief
  This will delete a row. buf will contain a copy of the row to be deleted.
  The server will call this right after the current row has been called (from
  either a previous rnd_nexT() or index call).

  @details
  If you keep a pointer to the last row or can access a primary key it will
  make doing the deletion quite a bit easier. Keep in mind that the server does
  not guarantee consecutive deletions. ORDER BY clauses can be used.

  Called in sql_acl.cc and sql_udf.cc to manage internal table
  information.  Called in sql_delete.cc, sql_insert.cc, and
  sql_select.cc. In sql_select it is used for removing duplicates
  while in insert it is used for REPLACE calls.

  @see
  sql_acl.cc, sql_udf.cc, sql_delete.cc, sql_insert.cc and sql_select.cc
*/

int ha_vtml::delete_row(const uchar *) {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

/**
  @brief
  Positions an index cursor to the index specified in the handle. Fetches the
  row if available. If the key value is null, begin at the first key of the
  index.
*/

int ha_vtml::index_read_map(uchar *, const uchar *, key_part_map,
                               enum ha_rkey_function) {
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  @brief
  Used to read forward through the index.
*/

int ha_vtml::index_next(uchar *) {
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  @brief
  Used to read backwards through the index.
*/

int ha_vtml::index_prev(uchar *) {
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  @brief
  index_first() asks for the first key in the index.

  @details
  Called from opt_range.cc, opt_sum.cc, sql_handler.cc, and sql_select.cc.

  @see
  opt_range.cc, opt_sum.cc, sql_handler.cc and sql_select.cc
*/
int ha_vtml::index_first(uchar *) {
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  @brief
  index_last() asks for the last key in the index.

  @details
  Called from opt_range.cc, opt_sum.cc, sql_handler.cc, and sql_select.cc.

  @see
  opt_range.cc, opt_sum.cc, sql_handler.cc and sql_select.cc
*/
int ha_vtml::index_last(uchar *) {
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  @brief
  rnd_init() is called when the system wants the storage engine to do a table
  scan.

  @details
  Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc,
  sql_table.cc, and sql_update.cc.

  @see
  filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and
  sql_update.cc
*/
int ha_vtml::rnd_init(bool) {
  DBUG_TRACE;
  return 0;
}

int ha_vtml::rnd_end() {
  DBUG_TRACE;
  return 0;
}

/**
  @brief
  This is called for each row of the table scan. When you run out of records
  you should return HA_ERR_END_OF_FILE. Fill buff up with the row information.
  The Field structure for the table is the key to getting data into buf
  in a manner that will allow the server to understand it.

  @details
  Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc,
  sql_table.cc, and sql_update.cc.

  @see
  filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and
  sql_update.cc
*/
bool first = true;
int ha_vtml::rnd_next(uchar *buf) {

  DBUG_TRACE;
  int rc = 0;

  if (first){
      rc = 0;
      first = false;
  }
  else{
      rc = HA_ERR_END_OF_FILE;
      first = true;
      return rc;
  }
  memset(buf, 0, table->s->null_bytes);

  std::string table_str(table->alias);
  auto const pos=table_str.find_last_of('/');
  const auto extracted_table_name=table_str.substr(pos+1);
  m_row_items.clear();
  for (Field **field = table->field; *field; field++) {

      String JsonString;
      JsonString.length(0);
      JsonString.append("{\"token\":\"");
      JsonString.append(extracted_table_name.c_str());
      JsonString.append("\",\"learnmode\":false,\"activationThreshold\":0.49,\"outputLMT\":20,\"inputFeature\":['");
      JsonString.append((*field)->field_name);
      JsonString.append("']}");

      char response[100] = {0};
      sendQueryData(JsonString.c_ptr(), response);
      m_row_items.push_back(response);
      (*field)->store(response, strlen(response), &my_charset_bin, CHECK_FIELD_WARN);
  }

  return rc;
}

/**
  @brief
  position() is called after each call to rnd_next() if the data needs
  to be ordered. You can do something like the following to store
  the position:
  @code
  my_store_ptr(ref, ref_length, current_position);
  @endcode

  @details
  The server uses ref to store data. ref_length in the above case is
  the size needed to store current_position. ref is just a byte array
  that the server will maintain. If you are using offsets to mark rows, then
  current_position should be the offset. If it is a primary key like in
  BDB, then it needs to be a primary key.

  Called from filesort.cc, sql_select.cc, sql_delete.cc, and sql_update.cc.

  @see
  filesort.cc, sql_select.cc, sql_delete.cc and sql_update.cc
*/
void ha_vtml::position(const uchar *) { DBUG_TRACE; }

/**
  @brief
  This is like rnd_next, but you are given a position to use
  to determine the row. The position will be of the type that you stored in
  ref. You can use ha_get_ptr(pos,ref_length) to retrieve whatever key
  or position you saved when position() was called.

  @details
  Called from filesort.cc, records.cc, sql_insert.cc, sql_select.cc, and
  sql_update.cc.

  @see
  filesort.cc, records.cc, sql_insert.cc, sql_select.cc and sql_update.cc
*/
int ha_vtml::rnd_pos(uchar *, uchar *) {
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  @brief
  ::info() is used to return information to the optimizer. See my_base.h for
  the complete description.

  @details
  Currently this table handler doesn't implement most of the fields really
  needed. SHOW also makes use of this data.

  You will probably want to have the following in your code:
  @code
  if (records < 2)
    records = 2;
  @endcode
  The reason is that the server will optimize for cases of only a single
  record. If, in a table scan, you don't know the number of records, it
  will probably be better to set records to two so you can return as many
  records as you need. Along with records, a few more variables you may wish
  to set are:
    records
    deleted
    data_file_length
    index_file_length
    delete_length
    check_time
  Take a look at the public variables in handler.h for more information.

  Called in filesort.cc, ha_heap.cc, item_sum.cc, opt_sum.cc, sql_delete.cc,
  sql_delete.cc, sql_derived.cc, sql_select.cc, sql_select.cc, sql_select.cc,
  sql_select.cc, sql_select.cc, sql_show.cc, sql_show.cc, sql_show.cc,
  sql_show.cc, sql_table.cc, sql_union.cc, and sql_update.cc.

  @see
  filesort.cc, ha_heap.cc, item_sum.cc, opt_sum.cc, sql_delete.cc,
  sql_delete.cc, sql_derived.cc, sql_select.cc, sql_select.cc, sql_select.cc,
  sql_select.cc, sql_select.cc, sql_show.cc, sql_show.cc, sql_show.cc,
  sql_show.cc, sql_table.cc, sql_union.cc and sql_update.cc
*/
int ha_vtml::info(uint) {

  DBUG_TRACE;
  stats.records = 2;
  return 0;
}

/**
  @brief
  extra() is called whenever the server wishes to send a hint to
  the storage engine. The myisam engine implements the most hints.
  ha_innodb.cc has the most exhaustive list of these hints.

    @see
  ha_innodb.cc
*/
int ha_vtml::extra(enum ha_extra_function) {
  DBUG_TRACE;
  return 0;
}

/**
  @brief
  Used to delete all rows in a table, including cases of truncate and cases
  where the optimizer realizes that all rows will be removed as a result of an
  SQL statement.

  @details
  Called from item_sum.cc by Item_func_group_concat::clear(),
  Item_sum_count_distinct::clear(), and Item_func_group_concat::clear().
  Called from sql_delete.cc by mysql_delete().
  Called from sql_select.cc by JOIN::reinit().
  Called from sql_union.cc by st_select_lex_unit::exec().

  @see
  Item_func_group_concat::clear(), Item_sum_count_distinct::clear() and
  Item_func_group_concat::clear() in item_sum.cc;
  mysql_delete() in sql_delete.cc;
  JOIN::reinit() in sql_select.cc and
  st_select_lex_unit::exec() in sql_union.cc.
*/
int ha_vtml::delete_all_rows() {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

/**
  @brief
  This create a lock on the table. If you are implementing a storage engine
  that can handle transacations look at ha_berkely.cc to see how you will
  want to go about doing this. Otherwise you should consider calling flock()
  here. Hint: Read the section "locking functions for mysql" in lock.cc to
  understand this.

  @details
  Called from lock.cc by lock_external() and unlock_external(). Also called
  from sql_table.cc by copy_data_between_tables().

  @see
  lock.cc by lock_external() and unlock_external() in lock.cc;
  the section "locking functions for mysql" in lock.cc;
  copy_data_between_tables() in sql_table.cc.
*/
int ha_vtml::external_lock(THD *, int) {
  DBUG_TRACE;
  return 0;
}

/**
  @brief
  The idea with handler::store_lock() is: The statement decides which locks
  should be needed for the table. For updates/deletes/inserts we get WRITE
  locks, for SELECT... we get read locks.

  @details
  Before adding the lock into the table lock handler (see thr_lock.c),
  mysqld calls store lock with the requested locks. Store lock can now
  modify a write lock to a read lock (or some other lock), ignore the
  lock (if we don't want to use MySQL table locks at all), or add locks
  for many tables (like we do when we are using a MERGE handler).

  Berkeley DB, for example, changes all WRITE locks to TL_WRITE_ALLOW_WRITE
  (which signals that we are doing WRITES, but are still allowing other
  readers and writers).

  When releasing locks, store_lock() is also called. In this case one
  usually doesn't have to do anything.

  In some exceptional cases MySQL may send a request for a TL_IGNORE;
  This means that we are requesting the same lock as last time and this
  should also be ignored. (This may happen when someone does a flush
  table when we have opened a part of the tables, in which case mysqld
  closes and reopens the tables and tries to get the same locks at last
  time). In the future we will probably try to remove this.

  Called from lock.cc by get_lock_data().

  @note
  In this method one should NEVER rely on table->in_use, it may, in fact,
  refer to a different thread! (this happens if get_lock_data() is called
  from mysql_lock_abort_for_thread() function)

  @see
  get_lock_data() in lock.cc
*/
THR_LOCK_DATA **ha_vtml::store_lock(THD *, THR_LOCK_DATA **to,
                                       enum thr_lock_type lock_type) {
  if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK) lock.type = lock_type;
  *to++ = &lock;
  return to;
}

/**
  @brief
  Used to delete a table. By the time delete_table() has been called all
  opened references to this table will have been closed (and your globally
  shared references released). The variable name will just be the name of
  the table. You will need to remove any files you have created at this point.

  @details
  If you do not implement this, the default delete_table() is called from
  handler.cc and it will delete all files with the file extensions from
  handlerton::file_extensions.

  Called from handler.cc by delete_table and ha_create_table(). Only used
  during create if the table_flag HA_DROP_BEFORE_CREATE was specified for
  the storage engine.

  @see
  delete_table and ha_create_table() in handler.cc
*/
int ha_vtml::delete_table(const char *, const dd::Table *) {
  DBUG_TRACE;
  /* This is not implemented but we want someone to be able that it works. */
  return 0;
}

/**
  @brief
  Renames a table from one name to another via an alter table call.

  @details
  If you do not implement this, the default rename_table() is called from
  handler.cc and it will delete all files with the file extensions from
  handlerton::file_extensions.

  Called from sql_table.cc by mysql_rename_table().

  @see
  mysql_rename_table() in sql_table.cc
*/
int ha_vtml::rename_table(const char *, const char *, const dd::Table *,
                             dd::Table *) {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

/**
  @brief
  Given a starting key and an ending key, estimate the number of rows that
  will exist between the two keys.

  @details
  end_key may be empty, in which case determine if start_key matches any rows.

  Called from opt_range.cc by check_quick_keys().

  @see
  check_quick_keys() in opt_range.cc
*/
ha_rows ha_vtml::records_in_range(uint, key_range *, key_range *) {
  DBUG_TRACE;
  return 10;  // low number to force index usage
}

static MYSQL_THDVAR_STR(last_create_thdvar, PLUGIN_VAR_MEMALLOC, NULL, NULL,
                        NULL, NULL);

static MYSQL_THDVAR_UINT(create_count_thdvar, 0, NULL, NULL, NULL, 0, 0, 1000,
                         0);

/**
  @brief
  create() is called to create a database. The variable name will have the name
  of the table.

  @details
  When create() is called you do not need to worry about
  opening the table. Also, the .frm file will have already been
  created so adjusting create_info is not necessary. You can overwrite
  the .frm file at this point if you wish to change the table
  definition, but there are no methods currently provided for doing
  so.

  Called from handle.cc by ha_create_table().

  @see
  ha_create_table() in handle.cc
*/

int ha_vtml::create(const char *name, TABLE *, HA_CREATE_INFO *,
                       dd::Table *) {
  DBUG_TRACE;
  /*
    This is not implemented but we want someone to be able to see that it
    works.
  */

  /*
    It's just an example of THDVAR_SET() usage below.
  */

  THD *thd = ha_thd();
  char *buf = (char *)my_malloc(PSI_NOT_INSTRUMENTED, SHOW_VAR_FUNC_BUFF_SIZE,
                                MYF(MY_FAE));
  snprintf(buf, SHOW_VAR_FUNC_BUFF_SIZE, "Last creation '%s'", name);
  THDVAR_SET(thd, last_create_thdvar, buf);


  my_free(buf);

  uint count = THDVAR(thd, create_count_thdvar) + 1;
  THDVAR_SET(thd, create_count_thdvar, &count);

  return 0;
}

struct st_mysql_storage_engine vtml_storage_engine = {
    MYSQL_HANDLERTON_INTERFACE_VERSION};

static ulong srv_enum_var = 0;
static ulong srv_ulong_var = 0;
static double srv_double_var = 0;
static int srv_signed_int_var = 0;
static long srv_signed_long_var = 0;
static longlong srv_signed_longlong_var = 0;

const char *enum_var_names_vtml[] = {"e1", "e2", NullS};

TYPELIB enum_var_typelib_vtml = {array_elements(enum_var_names_vtml) - 1,
                            "enum_var_typelib_vtml", enum_var_names_vtml, NULL};

static MYSQL_SYSVAR_ENUM(enum_var,                        // name
                         srv_enum_var,                    // varname
                         PLUGIN_VAR_RQCMDARG,             // opt
                         "Sample ENUM system variable.",  // comment
                         NULL,                            // check
                         NULL,                            // update
                         0,                               // def
                         &enum_var_typelib_vtml);         // typelib

static MYSQL_SYSVAR_ULONG(ulong_var, srv_ulong_var, PLUGIN_VAR_RQCMDARG,
                          "0..1000", NULL, NULL, 8, 0, 1000, 0);

static MYSQL_SYSVAR_DOUBLE(double_var, srv_double_var, PLUGIN_VAR_RQCMDARG,
                           "0.500000..1000.500000", NULL, NULL, 8.5, 0.5,
                           1000.5,
                           0);  // reserved always 0

static MYSQL_THDVAR_DOUBLE(double_thdvar, PLUGIN_VAR_RQCMDARG,
                           "0.500000..1000.500000", NULL, NULL, 8.5, 0.5,
                           1000.5, 0);

static MYSQL_SYSVAR_INT(signed_int_var, srv_signed_int_var, PLUGIN_VAR_RQCMDARG,
                        "INT_MIN..INT_MAX", NULL, NULL, -10, INT_MIN, INT_MAX,
                        0);

static MYSQL_THDVAR_INT(signed_int_thdvar, PLUGIN_VAR_RQCMDARG,
                        "INT_MIN..INT_MAX", NULL, NULL, -10, INT_MIN, INT_MAX,
                        0);

static MYSQL_SYSVAR_LONG(signed_long_var, srv_signed_long_var,
                         PLUGIN_VAR_RQCMDARG, "LONG_MIN..LONG_MAX", NULL, NULL,
                         -10, LONG_MIN, LONG_MAX, 0);

static MYSQL_THDVAR_LONG(signed_long_thdvar, PLUGIN_VAR_RQCMDARG,
                         "LONG_MIN..LONG_MAX", NULL, NULL, -10, LONG_MIN,
                         LONG_MAX, 0);

static MYSQL_SYSVAR_LONGLONG(signed_longlong_var, srv_signed_longlong_var,
                             PLUGIN_VAR_RQCMDARG, "LLONG_MIN..LLONG_MAX", NULL,
                             NULL, -10, LLONG_MIN, LLONG_MAX, 0);

static MYSQL_THDVAR_LONGLONG(signed_longlong_thdvar, PLUGIN_VAR_RQCMDARG,
                             "LLONG_MIN..LLONG_MAX", NULL, NULL, -10, LLONG_MIN,
                             LLONG_MAX, 0);

static SYS_VAR *vtml_system_variables[] = {
    MYSQL_SYSVAR(enum_var),
    MYSQL_SYSVAR(ulong_var),
    MYSQL_SYSVAR(double_var),
    MYSQL_SYSVAR(double_thdvar),
    MYSQL_SYSVAR(last_create_thdvar),
    MYSQL_SYSVAR(create_count_thdvar),
    MYSQL_SYSVAR(signed_int_var),
    MYSQL_SYSVAR(signed_int_thdvar),
    MYSQL_SYSVAR(signed_long_var),
    MYSQL_SYSVAR(signed_long_thdvar),
    MYSQL_SYSVAR(signed_longlong_var),
    MYSQL_SYSVAR(signed_longlong_thdvar),
    NULL};

// this is an vtml of SHOW_FUNC
static int show_func_vtml(MYSQL_THD, SHOW_VAR *var, char *buf) {
  var->type = SHOW_CHAR;
  var->value = buf;  // it's of SHOW_VAR_FUNC_BUFF_SIZE bytes
  snprintf(buf, SHOW_VAR_FUNC_BUFF_SIZE,
           "enum_var is %lu, ulong_var is %lu, "
           "double_var is %f, signed_int_var is %d, "
           "signed_long_var is %ld, signed_longlong_var is %lld",
           srv_enum_var, srv_ulong_var, srv_double_var, srv_signed_int_var,
           srv_signed_long_var, srv_signed_longlong_var);
  return 0;
}

struct vtml_vars_t {
  ulong var1;
  double var2;
  char var3[64];
  bool var4;
  bool var5;
  ulong var6;
};

vtml_vars_t vtml_vars = {100, 20.01, "three hundred", true, 0, 8250};

static SHOW_VAR show_status_vtml[] = {
    {"var1", (char *)&vtml_vars.var1, SHOW_LONG, SHOW_SCOPE_GLOBAL},
    {"var2", (char *)&vtml_vars.var2, SHOW_DOUBLE, SHOW_SCOPE_GLOBAL},
    {0, 0, SHOW_UNDEF, SHOW_SCOPE_UNDEF}  // null terminator required
};

static SHOW_VAR show_array_vtml[] = {
    {"array", (char *)show_status_vtml, SHOW_ARRAY, SHOW_SCOPE_GLOBAL},
    {"var3", (char *)&vtml_vars.var3, SHOW_CHAR, SHOW_SCOPE_GLOBAL},
    {"var4", (char *)&vtml_vars.var4, SHOW_BOOL, SHOW_SCOPE_GLOBAL},
    {0, 0, SHOW_UNDEF, SHOW_SCOPE_UNDEF}};

static SHOW_VAR func_status[] = {
    {"vtml_func_vtml", (char *)show_func_vtml, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"vtml_status_var5", (char *)&vtml_vars.var5, SHOW_BOOL,
     SHOW_SCOPE_GLOBAL},
    {"vtml_status_var6", (char *)&vtml_vars.var6, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {"vtml_status", (char *)show_array_vtml, SHOW_ARRAY,
     SHOW_SCOPE_GLOBAL},
    {0, 0, SHOW_UNDEF, SHOW_SCOPE_UNDEF}};

mysql_declare_plugin(vtml){
    MYSQL_STORAGE_ENGINE_PLUGIN,
    &vtml_storage_engine,
    "VTML",
    "Hadi Aydogdu, hadiaydogdu@gmail.com",
    "VTML Storage Engine",
    PLUGIN_LICENSE_GPL,
    vtml_init_func, /* Plugin Init */
    NULL,              /* Plugin check uninstall */
    NULL,              /* Plugin Deinit */
    0x0001 /* 0.1 */,
    func_status,              /* status variables */
    vtml_system_variables, /* system variables */
    NULL,                     /* config options */
    0,                        /* flags */
} mysql_declare_plugin_end;
