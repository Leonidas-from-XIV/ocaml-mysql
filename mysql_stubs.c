/*
 * This module provides access to MySQL from Objective Caml. 
 */

 
#include <stdio.h>              /* sprintf */
#include <string.h> 
#include <stdarg.h>
 
/* OCaml runtime system */

#include <caml/mlvalues.h>
#include <caml/memory.h>
#include <caml/fail.h>
#include <caml/alloc.h>
#include <caml/callback.h>
#include <caml/custom.h>
#include <caml/signals.h>

/* MySQL API */

#include <mysql/mysql.h>
#include <mysql/mysqld_error.h>
#include <mysql/errmsg.h>
/* type 'a option = None | Some of 'a */

#define NONE            Val_int(0)     
#define SOME            0             

#define EXTERNAL                /* dummy to highlight fn's exported to ML */

/* Abstract types 
 *
 * dbd - data base descriptor
 *
 *      header with Abstract_tag
 *      0:      MYSQL*
 *      1:      bool    (open == true, closed == false)
 *
 * res - result returned from query/exec
 *
 *      header with Final_tag
 *      0:      finalization function for 1
 *      1:      MYSQL_RES*
 *
 */

/* macros to access C values stored inside the abstract values */
 
#define DBDmysql(x) ((MYSQL*)(Field(x,1)))
#define DBDopen(x) (Field(x,2))
#define RESval(x) (*(MYSQL_RES**)Data_custom_val(x))

#define STMTval(x) ((MYSQL_STMT*)x)
#define ROWval(x) (*(row_t**)Data_custom_val(x))

static void mysqlfailwith(char *err) Noreturn;
static void mysqlfailmsg(const char *fmt, ...) Noreturn;

static void
mysqlfailwith(char *err) {
  raise_with_string(*caml_named_value("mysql error"), err);
}

static void
mysqlfailmsg(const char *fmt, ...) {
  char buf[1024];
  va_list args;

  va_start(args, fmt);
  vsnprintf(buf, sizeof buf, fmt, args);
  va_end(args);
  
  raise_with_string(*caml_named_value("mysql error"), buf);  

}

#define Val_none Val_int(0)

static inline value
Val_some( value v )
{
    CAMLparam1( v );
    CAMLlocal1( some );
    some = caml_alloc(1, 0);
    Store_field( some, 0, v );
    CAMLreturn( some );
}

static int
int_option(value v)
{
  if (v == NONE)
    return 0;
  else
    return  Int_val(Field(v,SOME));
}


/*
 * str_option gets a char* from an ML string option value.  Returns
 * NULL for NONE.
 */

static char*
str_option(value v)
{
  if (v == NONE)
    return (char*) NULL;
  else 
    return String_val(Field(v,SOME));
}

/*
 * val_str_option creates a string option value from a char* (NONE for
 * NULL) -- dual to str_option().
 */
 
static value
val_str_option(const char* s, unsigned long length)
{
  CAMLparam0();
  CAMLlocal2(res, v);

  if (!s)
    res = NONE;
  else {
    v = alloc_string(length);
    memcpy(String_val(v), s, length);
    
    res = alloc_small(1,SOME);
    Field(res,0) = v; 
  }
  CAMLreturn(res);
}

/*
 * val_some creates a (SOME v) from value v.
 */

static value
val_some(value some)
{
  CAMLparam1(some);
  CAMLlocal2(res, v);
        
  v = some;
  res = alloc_small(1,SOME);
  Field(res,0) = v;

  CAMLreturn(res);
}

/* check_dbd checks that the data base connection is still open.  The
 * open flag is reset by db_disconnect().
 */

static void
check_dbd(value dbd, char *fun)
{        
  if (!Bool_val(DBDopen(dbd))) 
    mysqlfailmsg("Mysql.%s called with closed connection", fun);
}


static void
conn_finalize(value dbd)
{
  if (Bool_val(DBDopen(dbd))) {
    caml_enter_blocking_section();
    mysql_close(DBDmysql(dbd));
    caml_leave_blocking_section();
  }
}

/* db_connect opens a data base connection and returns an abstract
 * connection object.
 */
 
EXTERNAL value
db_connect(value args)

{
  CAMLparam1(args);
  int i = 0;
  char *host      = str_option(Field(args,i++));
  char *db        = str_option(Field(args,i++));
  unsigned port   = (unsigned) int_option(Field(args,i++));
  char *pwd       = str_option(Field(args,i++));
  char *user      = str_option(Field(args,i++));
  CAMLlocal1(res);
  MYSQL *init;
  MYSQL *mysql;
        
  init = mysql_init(NULL);
  if (!init) {
    mysqlfailwith("connect failed");
  } else {
    caml_enter_blocking_section();
    mysql = mysql_real_connect(init ,host ,user
                               ,pwd ,db ,port
                               ,NULL, 0);
    caml_leave_blocking_section();
    if (!mysql) {
      mysqlfailwith(mysql_error(init));
    } else {
      res = alloc_final(3, conn_finalize, 100,1000);
      Field(res, 1) = (value)mysql;
      Field(res, 2) =  Val_true;
    }
  }
  CAMLreturn(res);
}


EXTERNAL value
db_change_user(value dbd, value args) {
  char *db        = str_option(Field(args,1));
  char *pwd       = str_option(Field(args,3));
  char *user      = str_option(Field(args,4));

  check_dbd(dbd,"change_user");

  caml_enter_blocking_section();
  if (mysql_change_user(DBDmysql(dbd), user, pwd, db)) {    
    caml_leave_blocking_section();
    mysqlfailmsg("Mysql.change_user: %s", mysql_error(DBDmysql(dbd)));
  }
  caml_leave_blocking_section();
  return Val_unit;
}

EXTERNAL value
db_list_dbs(value dbd, value pattern, value blah) {
  CAMLparam3(dbd, pattern, blah);
  CAMLlocal2(out, dbs);
  char *wild = str_option(pattern);
  int n, i;
  MYSQL_RES *res;
  MYSQL_ROW row;

  caml_enter_blocking_section();
  res = mysql_list_dbs(DBDmysql(dbd), wild);
  caml_leave_blocking_section();

  if (!res)
    CAMLreturn(NONE);

  n = mysql_num_rows(res);

  if (n == 0) {
    mysql_free_result(res);
    CAMLreturn(NONE);
  }

  dbs = alloc_tuple(n); /* Array */
  i = 0;
  while ((row = mysql_fetch_row(res)) != NULL) {
    Store_field(dbs, i, copy_string(row[0]));
    i++;
  }

  mysql_free_result(res);
  out = alloc_small(1, SOME);
  Field(out, 0) = dbs;
  CAMLreturn(out);

}

EXTERNAL value
db_select_db(value dbd, value newdb) {

  check_dbd(dbd, "select_db");
  
  caml_enter_blocking_section();
  if (mysql_select_db(DBDmysql(dbd), String_val(newdb))) {
    mysqlfailmsg("Mysql.select_db: %s", mysql_error(DBDmysql(dbd)));
    caml_leave_blocking_section();
  }
  caml_leave_blocking_section();
  return Val_unit;
}

/*
 * db_disconnect closes a db connection and marks the dbd closed.
 */

EXTERNAL value
db_disconnect(value dbd)
{
  CAMLparam1(dbd);
  check_dbd(dbd,"disconnect");
  caml_enter_blocking_section();
  mysql_close(DBDmysql(dbd));
  caml_leave_blocking_section();
  Field(dbd, 1) = Val_false;
  Field(dbd, 2) = Val_false; /* Mark closed */
  CAMLreturn(Val_unit);
}

EXTERNAL value
db_ping(value dbd)
{

  check_dbd(dbd,"ping");

  caml_enter_blocking_section();
  if (mysql_ping(DBDmysql(dbd))) {
    caml_leave_blocking_section();
    mysqlfailmsg("Mysql.ping: %s", mysql_error(DBDmysql(dbd)));
  }
  caml_leave_blocking_section();
  return Val_unit;

}

/*
 * finalize -- this is called when a data base result is garbage
 * collected -- frees memory allocated by MySQL.
 */

static void
res_finalize(value result)
{
  MYSQL_RES *res = RESval(result);
  if (res)
    mysql_free_result(res);
}


struct custom_operations res_ops = {
  "Mysql Query Results",
  res_finalize,
  custom_compare_default,
  custom_hash_default,
  custom_serialize_default,
  custom_deserialize_default
};

/*
 * db_exec -- execute a SQL query or command.  Returns a handle to
 * access the result.
 */

EXTERNAL value
db_exec(value dbd, value sql)
{
  CAMLparam2(dbd, sql);
  CAMLlocal2(res, v);
  MYSQL *mysql;
       
  check_dbd(dbd,"exec");
  mysql = DBDmysql(dbd);
        
  caml_enter_blocking_section();
  if (mysql_real_query(mysql, String_val(sql), string_length(sql))) {
    caml_leave_blocking_section();
    mysqlfailmsg("Mysql.exec: %s", mysql_error(mysql));
  } else {
    MYSQL_RES *saved;
    caml_leave_blocking_section();
    res = alloc_custom(&res_ops, sizeof(MYSQL_RES*), 1, 10);
    saved = mysql_store_result(DBDmysql(dbd));
    memcpy(Data_custom_val(res), &saved, sizeof(MYSQL_RES*));
  }

  CAMLreturn(res);
}

/* 
 * db_fetch -- fetch one result tuple, represented as array of string
 * options.  In case a value is Null, the respective value is None. 
 * Returns (Some v) in case there is such a result and None otherwise. 
 * Moves the internal result cursor to the next tuple.
 */

EXTERNAL value
db_fetch (value result)
{
  CAMLparam1(result);
  CAMLlocal2(fields, s);
  unsigned int i, n;
  unsigned long *length;  /* array of long */
  MYSQL_RES *res;
  MYSQL_ROW row;
  
  res = RESval(result);
  if (!res) 
    mysqlfailwith("Mysql.fetch: result did not return fetchable data");
  
  n = mysql_num_fields(res);
  if (n == 0)
    mysqlfailwith("Mysql.fetch: no columns");
  
  row = mysql_fetch_row(res);
  if (!row) 
    CAMLreturn(NONE);
  
  /* create Some([| f1; f2; .. ;fn |]) */
  
  length = mysql_fetch_lengths(res);      /* length[] */
  fields = alloc_tuple(n);                    /* array */
  for (i=0;i<n;i++) {
    s = val_str_option(row[i], length[i]);
    Store_field(fields, i, s);
  }
  
  CAMLreturn(val_some(fields));
}

EXTERNAL value
db_to_row(value result, value offset) {
  int64 off = Int64_val(offset);
  MYSQL_RES *res;

  res = RESval(result);
  if (!res) 
    mysqlfailwith("Mysql.to_row: result did not return fetchable data");

  if (off < 0 || off > (int64)mysql_num_rows(res)-1)
    invalid_argument("Mysql.to_row: offset out of range");

  mysql_data_seek(res, off);

  return Val_unit;
}

/*
 * db_status -- returns current status (simplistic)
 */

EXTERNAL value
db_status(value dbd)

{
  CAMLparam1(dbd);        
  check_dbd(dbd, "status");
  CAMLreturn(Val_int(mysql_errno(DBDmysql(dbd))));
}

/*
 * db_errmsg -- returns string option with last error message (None ==
 * no error)
 */

EXTERNAL value
db_errmsg(value dbd)
{
  CAMLparam1(dbd);
  CAMLlocal1(s);
  const char *msg;

  check_dbd(dbd,"errmsg");
  
  msg = mysql_error(DBDmysql(dbd));
  if (!msg || msg[0] == '\0')
    msg = (char*) NULL;
  s = val_str_option(msg, msg == (char *) NULL ? 0 : strlen(msg));
  CAMLreturn(s);
}

/*
 * db_escape - takes a string and escape all characters inside which
 * must be escaped inside MySQL strings.  This helps to construct SQL
 * statements easily.  Simply returns the new string including the
 * quotes. 
 */

EXTERNAL value
db_escape(value str)
{
  CAMLparam1(str);
  char *s;
  char *buf;
  int len, esclen;
  CAMLlocal1(res);
        
  s = String_val(str);
  len = string_length(str);
  buf = (char*) stat_alloc(2*len+1);
  esclen = mysql_escape_string(buf,s,len);

  res = alloc_string(esclen);
  memcpy(String_val(res), buf, esclen);
  stat_free(buf);
  
  CAMLreturn(res);
}

/*
 * db_size -- returns the size of the current result (number of rows).
 */

EXTERNAL value
db_size(value result)
{
  CAMLparam1(result);
  MYSQL_RES *res;
  int64 size;

  res = RESval(result);
  if (!res)
    size = 0;
  else
    size = (int64)(mysql_num_rows(res));
  
  CAMLreturn(copy_int64(size));
}

EXTERNAL value
db_affected(value dbd) {
  CAMLparam1(dbd);
  CAMLreturn(copy_int64(mysql_affected_rows(DBDmysql(dbd))));
}

EXTERNAL value
db_insert_id(value dbd) {
  CAMLparam1(dbd);
  CAMLreturn(copy_int64(mysql_insert_id(DBDmysql(dbd))));
}

EXTERNAL value
db_fields(value result)
{
  MYSQL_RES *res;
  long size;

  res = RESval(result);
  if (!res)
    size = 0;
  else
    size = (long)(mysql_num_fields(res));
  
  return Val_long(size);
}

EXTERNAL value
db_client_info(value unit) {
  CAMLparam1(unit);
  CAMLlocal1(info);
  info = copy_string(mysql_get_client_info());
  CAMLreturn(info);
}

EXTERNAL value
db_host_info(value dbd) {
  CAMLparam1(dbd);
  CAMLlocal1(info);
  info = copy_string(mysql_get_host_info(DBDmysql(dbd)));
  CAMLreturn(info);
}

EXTERNAL value
db_server_info(value dbd) {
  CAMLparam1(dbd);
  CAMLlocal1(info);
  info = copy_string(mysql_get_server_info(DBDmysql(dbd)));
  CAMLreturn(info);
}

EXTERNAL value
db_proto_info(value dbd) {
  long info = (long)mysql_get_proto_info(DBDmysql(dbd));
  return Val_long(info);
}


/*
 * type2dbty - maps column types to dbty values which describe the
 * column types in OCaml.
 */

#define INT_TY          0
#define FLOAT_TY        1
#define STRING_TY       2
#define SET_TY          3
#define ENUM_TY         4
#define DATETIME_TY     5
#define DATE_TY         6
#define TIME_TY         7
#define YEAR_TY         8
#define TIMESTAMP_TY    9
#define UNKNOWN_TY      10
#define INT64_TY        11
#define BLOB_TY         12
#define DECIMAL_TY			13

static value
type2dbty (int type)
{
  static struct {int mysql; value caml;} map[] = {
    {FIELD_TYPE_DECIMAL     , Val_long(DECIMAL_TY)},
    {FIELD_TYPE_TINY        , Val_long(INT_TY)},
    {FIELD_TYPE_SHORT       , Val_long(INT_TY)},
    {FIELD_TYPE_LONG        , Val_long(INT_TY)},
    {FIELD_TYPE_FLOAT       , Val_long(FLOAT_TY)},
    {FIELD_TYPE_DOUBLE      , Val_long(FLOAT_TY)},
    {FIELD_TYPE_NULL        , Val_long(STRING_TY)},
    {FIELD_TYPE_TIMESTAMP   , Val_long(TIMESTAMP_TY)},
    {FIELD_TYPE_LONGLONG    , Val_long(INT64_TY)},
    {FIELD_TYPE_INT24       , Val_long(INT_TY)},
    {FIELD_TYPE_DATE        , Val_long(DATE_TY)},
    {FIELD_TYPE_TIME        , Val_long(TIME_TY)},
    {FIELD_TYPE_DATETIME    , Val_long(DATETIME_TY)},
    {FIELD_TYPE_YEAR        , Val_long(YEAR_TY)},
    {FIELD_TYPE_NEWDATE     , Val_long(UNKNOWN_TY)},
    {FIELD_TYPE_ENUM        , Val_long(ENUM_TY)},
    {FIELD_TYPE_SET         , Val_long(SET_TY)},
    {FIELD_TYPE_TINY_BLOB   , Val_long(BLOB_TY)},
    {FIELD_TYPE_MEDIUM_BLOB , Val_long(BLOB_TY)},
    {FIELD_TYPE_LONG_BLOB   , Val_long(BLOB_TY)},
    {FIELD_TYPE_BLOB        , Val_long(BLOB_TY)},
    {FIELD_TYPE_VAR_STRING  , Val_long(STRING_TY)},
    {FIELD_TYPE_STRING      , Val_long(STRING_TY)},
    {-1 /*default*/         , Val_long(UNKNOWN_TY)}
  };
  int i;
  
  /* in principle using bsearch() would be better -- but how can
   * we know that the left side of the map is properly sorted? 
   */

  for (i=0; map[i].mysql != -1 && map[i].mysql != type; i++)
    /* empty */ ;
  
  return map[i].caml;
}

value 
make_field(MYSQL_FIELD *f) {
  CAMLparam0();
  CAMLlocal5(out, data, name, table, def);

  name = copy_string(f->name);

  if (f->table)
    table = val_str_option(f->table, strlen(f->table));
  else
    table = NONE;

  if (f->def)
    def = val_str_option(f->def, strlen(f->def));
  else
    def = NONE;

  data = alloc_small(7, 0);
  Field(data, 0) = name;
  Field(data, 1) = table;
  Field(data, 2) = def;
  Field(data, 3) = type2dbty(f->type);
  Field(data, 4) = Val_long(f->max_length);
  Field(data, 5) = Val_long(f->flags);
  Field(data, 6) = Val_long(f->decimals);

  CAMLreturn(data);
}
     
EXTERNAL value
db_fetch_field(value result) {
  CAMLparam1(result);
  CAMLlocal2(field, out);
  MYSQL_FIELD *f;
  MYSQL_RES *res = RESval(result);

  if (!res)
    CAMLreturn(NONE);

  f = mysql_fetch_field(res);
  if (!f)
    CAMLreturn(NONE);

  field = make_field(f);
  out = alloc_small(1, SOME);
  Field(out, SOME) = field;
  CAMLreturn(out);
}

EXTERNAL value
db_fetch_field_dir(value result, value pos) {
  CAMLparam2(result, pos);
  CAMLlocal2(field, out);
  MYSQL_FIELD *f;
  MYSQL_RES *res = RESval(result);

  if (!res)
    CAMLreturn(NONE);

  f = mysql_fetch_field_direct(res, Long_val(pos));
  if (!f)
    CAMLreturn(NONE);

  field = make_field(f);
  out = alloc_small(1, SOME);
  Field(out, SOME) = field;
  CAMLreturn(out);
}


EXTERNAL value
db_fetch_fields(value result) {
  CAMLparam1(result);
  CAMLlocal2(fields, out);
  MYSQL_RES *res = RESval(result);
  MYSQL_FIELD *f;
  int i, n;

  n = mysql_num_fields(res);

  if (n == 0)
    CAMLreturn(NONE);

  f = mysql_fetch_fields(res);

  fields = alloc_tuple(n);

  for (i = 0; i < n; i++) {
    Store_field(fields, i, make_field(f+i));
  }

  out = alloc_small(1, SOME);
  Field(out, 0) = fields;
  CAMLreturn(out);
}

EXTERNAL value
caml_mysql_stmt_prepare(value dbd, value sql)
{
  CAMLparam2(dbd,sql);
  check_dbd(dbd, "P.prepare");
  caml_enter_blocking_section();
  MYSQL_STMT* stmt = mysql_stmt_init(DBDmysql(dbd));
  if (!stmt)
  {
    caml_leave_blocking_section();
    mysqlfailwith("P.prepare : mysql_stmt_init");
  }
  int ret = mysql_stmt_prepare(stmt, String_val(sql), caml_string_length(sql));
  caml_leave_blocking_section();
  if (ret)
    mysqlfailwith("P.prepare : mysql_stmt_prepare");
  CAMLreturn(stmt);
}

EXTERNAL value
caml_mysql_stmt_close(value stmt)
{
  CAMLparam1(stmt);
  caml_enter_blocking_section();
  my_bool ret = mysql_stmt_close(STMTval(stmt));
  caml_leave_blocking_section();
  if (ret)
    mysqlfailwith("mysql_stmt_close");
  CAMLreturn(Val_unit);
}

typedef struct row_t_tag
{
  size_t count;
  MYSQL_STMT* stmt; /* not owned */

  MYSQL_BIND* bind;
  unsigned long* length;
  my_bool* error;
  my_bool* is_null;
} row_t;

row_t* create_row(MYSQL_STMT* stmt, size_t count)
{
  row_t* row = malloc(sizeof(row_t));
  if (row)
  {
    row->stmt = stmt;
    row->count = count;
    row->bind = calloc(count,sizeof(MYSQL_BIND));
    row->error = calloc(count,sizeof(my_bool));
    row->length = calloc(count,sizeof(unsigned long));
    row->is_null = calloc(count,sizeof(my_bool));
  }
  return row;
}

void set_param(row_t *r, char* str, size_t len, int index)
{
  MYSQL_BIND* bind = &r->bind[index];

  r->length[index] = len;
  bind->length = &r->length[index];
  bind->buffer_length = len;
  bind->buffer_type = MYSQL_TYPE_STRING;
  bind->buffer = (void*)str;
}

void bind_result(row_t* r, int index)
{
  MYSQL_BIND* bind = &r->bind[index];

  bind->buffer_type = MYSQL_TYPE_STRING;
  bind->buffer = 0;
  bind->buffer_length = 0;
  bind->is_null = &r->is_null[index];
  bind->length = &r->length[index];
  bind->error = &r->error[index];
}

value get_column(row_t* r, int index)
{
  CAMLparam0();
  CAMLlocal1(str);
  unsigned long length = r->length[index];
  MYSQL_BIND* bind = &r->bind[index];

  if (length > 0)
  {
    str = caml_alloc_string(length);
    bind->buffer = String_val(str);
    bind->buffer_length = length;
    mysql_stmt_fetch_column(r->stmt, r->bind, index, 0);
    CAMLreturn(Val_some(str));
  }

  CAMLreturn(Val_none);
}

void destroy_row(row_t* r)
{
  if (r)
  {
    free(r->bind);
    free(r->error);
    free(r->length);
    free(r->is_null);
    free(r);
  }
}

static void
stmt_result_finalize(value result)
{
  fprintf(stdout,"finalize");
  fflush(stdout);
  row_t *row = ROWval(result);
  destroy_row(row);
}

struct custom_operations stmt_result_ops = {
  "Mysql Prepared Statement Results",
  stmt_result_finalize,
  custom_compare_default,
  custom_hash_default,
  custom_serialize_default,
  custom_deserialize_default
};

EXTERNAL value
caml_mysql_stmt_execute(value stmt, value params)
{
  CAMLparam2(stmt,params);
  CAMLlocal1(res);
  int i = 0;
  int len = Wosize_val(params);
  if (len != mysql_stmt_param_count(STMTval(stmt)))
    mysqlfailmsg("P.execute : Got %i parameters, but expected %u", len, mysql_stmt_param_count(STMTval(stmt)));
  row_t* row = create_row(STMTval(stmt), len);
  if (!row)
    mysqlfailwith("P.execute : create_row for params");
  for (i = 0; i < len; i++)
  {
    /* Quick and dirty. 
     * Relies on the following :
     * - mysql doesn't read MYSQL_BIND buffers after mysql_stmt_execute finishes
     * - parameter strings are fixed in memory i.e. no GC till mysql_stmt_execute finishes 
     * i.e. no enter/leave_blocking_section */
    set_param(row,String_val(Field(params,i)),caml_string_length(Field(params,i)),i);
  }
  if (mysql_stmt_bind_param(STMTval(stmt), row->bind))
  {
    destroy_row(row);
    mysqlfailwith("P.execute : mysql_stmt_bind_param");
  }
  if (mysql_stmt_execute(STMTval(stmt)))
  {
    destroy_row(row);
    mysqlfailwith("P.execute : mysql_stmt_execute");
  }
  destroy_row(row);

  len = mysql_stmt_field_count(STMTval(stmt));
  row = create_row(STMTval(stmt), len);
  if (!row)
    mysqlfailwith("P.execute : create_row for results");
  if (len)
  {
    for (i = 0; i < len; i++)
    {
      bind_result(row,i);
    }
    if (mysql_stmt_bind_result(STMTval(stmt), row->bind))
    {
      destroy_row(row);
      mysqlfailwith("P.execute : mysql_stmt_bind_result");
    }
  }
  res = alloc_custom(&stmt_result_ops, sizeof(row_t*), 1, 10);
  memcpy(Data_custom_val(res),&row,sizeof(row_t*));
  CAMLreturn(res);
}

EXTERNAL value
caml_mysql_stmt_fetch(value result)
{
  CAMLparam1(result);
  CAMLlocal1(arr);
  int i = 0;
  row_t* r = ROWval(result);
  int res = mysql_stmt_fetch(r->stmt);
  if (0 != res && MYSQL_DATA_TRUNCATED != res) CAMLreturn(Val_none);
  arr = caml_alloc(r->count,0);
  for (i = 0; i < r->count; i++)
  {
    Store_field(arr,i,get_column(r,i));
  }
  CAMLreturn(Val_some(arr));
}

