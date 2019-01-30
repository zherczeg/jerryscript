/* Copyright 2015 University of Szeged.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <setjmp.h>

/* The utilites here are just for compiling purposes, JS
 * engines should have an optimized version for them. */

#define PARSER_DEBUG
#define PARSER_DUMP_BYTE_CODE

/* Malloc functions. */

#define PARSER_MALLOC(size) malloc (size)
#define PARSER_FREE(ptr) free (ptr)

#define PARSER_MALLOC_LOCAL(size) malloc (size)
#define PARSER_FREE_LOCAL(ptr) free (ptr)

/* UTF character management. Only ASCII characters are
 * supported for simplicity. */

int util_is_identifier_start (const uint8_t *);
int util_is_identifier_part (const uint8_t *);
int util_is_identifier_start_character (uint16_t);
int util_is_identifier_part_character (uint16_t);
size_t util_to_utf8_bytes (uint8_t *, uint16_t);
size_t util_get_utf8_length (uint16_t);

/* Immediate management. */

/**
 * Literal types.
 *
 * The LEXER_UNUSED_LITERAL type is internal and
 * used for various purposes.
 */
typedef enum
{
  LEXER_IDENT_LITERAL = 0,          /**< identifier literal */
  LEXER_STRING_LITERAL = 1,         /**< string literal */
  LEXER_NUMBER_LITERAL = 2,         /**< number literal */
  LEXER_FUNCTION_LITERAL = 3,       /**< function literal */
  LEXER_REGEXP_LITERAL = 4,         /**< regexp literal */
  LEXER_UNUSED_LITERAL = 5,         /**< unused literal, can only be
                                         used by the byte code generator. */
} lexer_literal_type_t;

/**
 * Flag bits for status_flags member of lexer_literal_t.
 */
typedef enum
{
  LEXER_FLAG_VAR = (1 << 0), /**< local identifier (var, function arg) */
  LEXER_FLAG_NO_REG_STORE = (1 << 1), /**< this local identifier cannot be stored in register */
  LEXER_FLAG_INITIALIZED = (1 << 2), /**< this local identifier is initialized with a value */
  LEXER_FLAG_FUNCTION_ARGUMENT = (1 << 3), /**< this local identifier is a function argument */
  LEXER_FLAG_UNUSED_IDENT = (1 << 4), /**< this identifier is referenced by sub-functions,
                                       *   but not referenced by the currently parsed function */
  LEXER_FLAG_SOURCE_PTR = (1 << 5), /**< the literal is directly referenced in the source code
                                     *   (no need to allocate memory) */
} lexer_literal_status_flags_t;

/**
 * Literal value.
 */
typedef union
{
  const uint8_t *char_p;      /**< char array */
  void *compiled_code_p;      /**< compiled code */
} literal_value_t;

/**
 * Literal data.
 */
typedef struct
{
  literal_value_t value;     /**< literal internal value */

#ifdef PARSER_DUMP_BYTE_CODE
  struct
#else
  union
#endif
  {
    uint16_t length;         /**< length of ident / string literal */
    uint16_t index;          /**< real index during post processing */
  } prop;

  uint8_t type;              /**< type of the literal */
  uint8_t status_flags;      /**< status flags */
} lexer_literal_t;

int util_compare_char_literals (lexer_literal_t *, const uint8_t *);
int32_t util_get_number (const uint8_t *, size_t);
int util_set_char_literal (lexer_literal_t *, const uint8_t *);
int util_set_number_literal (lexer_literal_t *, const uint8_t *);
int util_set_regexp_literal (lexer_literal_t *, const uint8_t *);
void util_set_function_literal (lexer_literal_t *, void *);
void util_free_literal (lexer_literal_t *);

#ifdef PARSER_DUMP_BYTE_CODE
void util_print_literal (lexer_literal_t *);
#endif /* PARSER_DUMP_BYTE_CODE */

/* Assertions */

#ifdef PARSER_DEBUG

#define PARSER_ASSERT(x) \
  do \
  { \
    if (!(x)) \
    { \
      printf ("Assertion failure in '%s' at line %d\n", __FILE__, __LINE__); \
      abort (); \
    } \
  } \
  while (0)

#else

#define PARSER_ASSERT(x)

#endif /* PARSER_DEBUG */

/* TRY/CATCH block */

#define PARSER_TRY_CONTEXT(context_name) \
  jmp_buf context_name

#define PARSER_THROW(context_name) \
  longjmp (context_name, 1);

#define PARSER_TRY(context_name) \
  { \
    if (!setjmp (context_name)) \
    { \

#define PARSER_CATCH \
    } \
    else \
    {

#define PARSER_TRY_END \
    } \
  }

/* Other */

#define PARSER_INLINE inline
#define PARSER_NOINLINE __attribute__ ((noinline))

#endif /* COMMON_H */
