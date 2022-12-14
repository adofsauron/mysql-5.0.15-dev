/******************************************************
SQL parser symbol table

(c) 1997 Innobase Oy

Created 12/15/1997 Heikki Tuuri
*******************************************************/

#ifndef pars0sym_h
#define pars0sym_h

#include "univ.i"
#include "que0types.h"
#include "usr0types.h"
#include "dict0types.h"
#include "pars0types.h"
#include "row0types.h"

/**********************************************************************
Creates a symbol table for a single stored procedure or query. */

sym_tab_t *sym_tab_create(
    /*===========*/
    /* out, own: symbol table */
    mem_heap_t *heap); /* in: memory heap where to create */
/**********************************************************************
Frees the memory allocated dynamically AFTER parsing phase for variables
etc. in the symbol table. Does not free the mem heap where the table was
originally created. Frees also SQL explicit cursor definitions. */

void sym_tab_free_private(
    /*=================*/
    sym_tab_t *sym_tab); /* in, own: symbol table */
/**********************************************************************
Adds an integer literal to a symbol table. */

sym_node_t *sym_tab_add_int_lit(
    /*================*/
    /* out: symbol table node */
    sym_tab_t *sym_tab, /* in: symbol table */
    ulint val);         /* in: integer value */
/**********************************************************************
Adds an string literal to a symbol table. */

sym_node_t *sym_tab_add_str_lit(
    /*================*/
    /* out: symbol table node */
    sym_tab_t *sym_tab, /* in: symbol table */
    byte *str,          /* in: string with no quotes around
                        it */
    ulint len);         /* in: string length */
/**********************************************************************
Adds an SQL null literal to a symbol table. */

sym_node_t *sym_tab_add_null_lit(
    /*=================*/
    /* out: symbol table node */
    sym_tab_t *sym_tab); /* in: symbol table */
/**********************************************************************
Adds an identifier to a symbol table. */

sym_node_t *sym_tab_add_id(
    /*===========*/
    /* out: symbol table node */
    sym_tab_t *sym_tab, /* in: symbol table */
    byte *name,         /* in: identifier name */
    ulint len);         /* in: identifier length */

#define SYM_CLUST_FIELD_NO 0
#define SYM_SEC_FIELD_NO 1

struct sym_node_struct
{
  que_common_t common; /* node type:
                       QUE_NODE_SYMBOL */
  /* NOTE: if the data field in 'common.val' is not NULL and the symbol
  table node is not for a temporary column, the memory for the value has
  been allocated from dynamic memory and it should be freed when the
  symbol table is discarded */

  sym_node_t *indirection; /* pointer to
                           another symbol table
                           node which contains
                           the value for this
                           node, NULL otherwise */
  sym_node_t *alias;       /* pointer to
                           another symbol table
                           node for which this
                           node is an alias,
                           NULL otherwise */
  UT_LIST_NODE_T(sym_node_t)
  col_var_list;            /* list of table
                           columns or a list of
                           input variables for an
                           explicit cursor */
  ibool copy_val;          /* TRUE if a column
                           and its value should
                           be copied to dynamic
                           memory when fetched */
  ulint field_nos[2];      /* if a column, in
                           the position
                           SYM_CLUST_FIELD_NO is
                           the field number in the
                           clustered index; in
                           the position
                           SYM_SEC_FIELD_NO
                           the field number in the
                           non-clustered index to
                           use first; if not found
                           from the index, then
                           ULINT_UNDEFINED */
  ibool resolved;          /* TRUE if the
                           meaning of a variable
                           or a column has been
                           resolved; for literals
                           this is always TRUE */
  ulint token_type;        /* SYM_VAR, SYM_COLUMN,
                           SYM_IMPLICIT_VAR,
                           SYM_LIT, SYM_TABLE,
                           SYM_CURSOR, ... */
  const char *name;        /* name of an id */
  ulint name_len;          /* id name length */
  dict_table_t *table;     /* table definition
                           if a table id or a
                           column id */
  ulint col_no;            /* column number if a
                           column */
  sel_buf_t *prefetch_buf; /* NULL, or a buffer
                           for cached column
                           values for prefetched
                           rows */
  sel_node_t *cursor_def;  /* cursor definition
                           select node if a
                           named cursor */
  ulint param_type;        /* PARS_INPUT,
                           PARS_OUTPUT, or
                           PARS_NOT_PARAM if not a
                           procedure parameter */
  sym_tab_t *sym_table;    /* back pointer to
                           the symbol table */
  UT_LIST_NODE_T(sym_node_t)
  sym_list; /* list of symbol
            nodes */
};

struct sym_tab_struct
{
  que_t *query_graph;
  /* query graph generated by the
  parser */
  const char *sql_string;
  /* SQL string to parse */
  size_t string_len;
  /* SQL string length */
  int next_char_pos;
  /* position of the next character in
  sql_string to give to the lexical
  analyzer */
  sym_node_list_t sym_list;
  /* list of symbol nodes in the symbol
  table */
  UT_LIST_BASE_NODE_T(func_node_t)
  func_node_list;
  /* list of function nodes in the
  parsed query graph */
  mem_heap_t *heap; /* memory heap from which we can
                    allocate space */
};

/* Types of a symbol table entry */
#define SYM_VAR                     \
  91 /* declared parameter or local \
     variable of a procedure */
#define SYM_IMPLICIT_VAR                                           \
  92                          /* storage for a intermediate result \
                              of a calculation */
#define SYM_LIT 93            /* literal */
#define SYM_TABLE 94          /* database table name */
#define SYM_COLUMN 95         /* database table name */
#define SYM_CURSOR 96         /* named cursor */
#define SYM_PROCEDURE_NAME 97 /* stored procedure name */
#define SYM_INDEX 98          /* database index name */

#ifndef UNIV_NONINL
#include "pars0sym.ic"
#endif

#endif
