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

#include "js-parser-internal.h"

/**
 * Copy identifiers if needed.
 */
static void
parser_copy_identifiers (parser_context_t *context_p) /**< context */
{
  parser_saved_context_t *parent_p = context_p->last_context_p;

  if (parent_p == NULL || !(parent_p->status_flags & PARSER_IS_FUNCTION))
  {
    /* Return if this function is not a nested function. */
    return;
  }

  if (context_p->status_flags & PARSER_NO_REG_STORE)
  {
    /* This flag must affect all parent functions. */
    parent_p->status_flags |= PARSER_NO_REG_STORE;
    return;
  }

  parser_list_iterator_t literal_iterator;
  lexer_literal_t *literal_p;

  parser_list_t parent_literal_pool;

  /* Accessing the parent literal pool requires all data. */
  parent_literal_pool.data = parent_p->literal_pool_data;
  parent_literal_pool.page_size = context_p->literal_pool.page_size;
  parent_literal_pool.item_size = context_p->literal_pool.item_size;
  parent_literal_pool.item_count = context_p->literal_pool.item_count;

  parser_list_iterator_init (&context_p->literal_pool, &literal_iterator);
  while ((literal_p = (lexer_literal_t *) parser_list_iterator_next (&literal_iterator)))
  {
    if (literal_p->type != LEXER_IDENT_LITERAL || (literal_p->status_flags & LEXER_FLAG_VAR))
    {
      continue;
    }

    parser_list_iterator_t parent_literal_iterator;
    parser_list_iterator_init (&parent_literal_pool, &parent_literal_iterator);

    lexer_literal_t *parent_literal_p;
    const uint8_t *char_p = literal_p->value.char_p;
    size_t length = literal_p->prop.length;

    while ((parent_literal_p = (lexer_literal_t *) parser_list_iterator_next (&parent_literal_iterator)) != NULL)
    {
      if (parent_literal_p->type == LEXER_IDENT_LITERAL
          && parent_literal_p->prop.length == length
          && memcmp (parent_literal_p->value.char_p, char_p, length) == 0)
      {
        /* This literal is known by the parent. */
        parent_literal_p->status_flags |= LEXER_FLAG_NO_REG_STORE;
        break;
      }
    }

    if (parent_literal_p != NULL)
    {
      continue;
    }

    if (parent_p->literal_count >= PARSER_MAXIMUM_NUMBER_OF_LITERALS)
    {
      parser_raise_error (context_p, PARSER_ERR_LITERAL_LIMIT_REACHED);
    }

    parent_literal_p = (lexer_literal_t *) parser_list_append (context_p, &parent_literal_pool);

    /* The literal data is updated at every iteration to handle out-of memory. */
    parent_p->literal_pool_data = parent_literal_pool.data;

    parent_literal_p->prop.length = (uint16_t) length;
    parent_literal_p->type = LEXER_IDENT_LITERAL;
    parent_literal_p->status_flags = (uint8_t) (literal_p->status_flags & LEXER_FLAG_SOURCE_PTR);
    parent_literal_p->status_flags |= LEXER_FLAG_NO_REG_STORE | LEXER_FLAG_UNUSED_IDENT;
    parent_literal_p->value.char_p = char_p;

    /* The buffer ownership is passed to the parent by
     * setting this flag which prevents freeing the buffer. */
    literal_p->status_flags |= LEXER_FLAG_SOURCE_PTR;

    parent_p->literal_count++;
  }
} /* parser_copy_identifiers */

/**
 * Compute real literal indicies.
 *
 * @return length of the prefix opcodes
 */
static size_t
parser_compute_indicies (parser_context_t *context_p, /**< context */
                         uint16_t *ident_end, /**< end of the identifier group */
                         uint16_t *uninitialized_var_end, /**< end of the uninitialized var group */
                         uint16_t *initialized_var_end, /**< end of the initialized var group */
                         uint16_t *const_literal_end) /**< end of the const literal group */
{
  parser_list_iterator_t literal_iterator;
  lexer_literal_t *literal_p;
  lexer_literal_t *func_literal_p;
  size_t length = 0;
  uint16_t literal_one_byte_limit;
  uint32_t status_flags = context_p->status_flags;
  uint16_t argument_count;

  uint16_t register_count = context_p->register_count;
  uint16_t uninitialized_var_count = 0;
  uint16_t initialized_var_count = 0;
  uint16_t ident_count = 0;
  uint16_t const_literal_count = 0;

  uint16_t register_index;
  uint16_t uninitialized_var_index;
  uint16_t initialized_var_index;
  uint16_t ident_index;
  uint16_t const_literal_index;
  uint16_t literal_index;

  if (status_flags & PARSER_ARGUMENTS_NOT_NEEDED)
  {
    status_flags &= (uint32_t) ~PARSER_ARGUMENTS_NEEDED;
    context_p->status_flags = status_flags;
  }

  /* First phase: count the number of items in each group. */
  parser_list_iterator_init (&context_p->literal_pool, &literal_iterator);
  while ((literal_p = (lexer_literal_t *) parser_list_iterator_next (&literal_iterator)))
  {
    if (literal_p->status_flags & LEXER_FLAG_UNUSED_IDENT)
    {
#ifndef PARSER_DUMP_BYTE_CODE
      if (!(literal_p->status_flags & LEXER_FLAG_SOURCE_PTR))
      {
        jmem_heap_free_block ((void *) literal_p->u.char_p, literal_p->prop.length);
      }
#endif /* !PARSER_DUMP_BYTE_CODE */

      context_p->literal_count--;
      continue;
    }

    switch (literal_p->type)
    {
      case LEXER_IDENT_LITERAL:
      {
        if (literal_p->status_flags & LEXER_FLAG_VAR)
        {
          if (status_flags & PARSER_NO_REG_STORE)
          {
            literal_p->status_flags |= LEXER_FLAG_NO_REG_STORE;
          }

          if (literal_p->status_flags & LEXER_FLAG_INITIALIZED)
          {
            if (literal_p->status_flags & LEXER_FLAG_FUNCTION_ARGUMENT)
            {
              if ((status_flags & PARSER_ARGUMENTS_NEEDED)
                  && !(status_flags & PARSER_IS_STRICT))
              {
                literal_p->status_flags |= LEXER_FLAG_NO_REG_STORE;
              }

              /* Arguments are bound to their position, or
               * moved to the initialized var section. */
              if (literal_p->status_flags & LEXER_FLAG_NO_REG_STORE)
              {
                initialized_var_count++;
                context_p->literal_count++;
              }
            }
            else if (!(literal_p->status_flags & LEXER_FLAG_NO_REG_STORE)
                     && register_count < PARSER_MAXIMUM_NUMBER_OF_REGISTERS)
            {
              register_count++;
            }
            else
            {
              literal_p->status_flags |= LEXER_FLAG_NO_REG_STORE;
              initialized_var_count++;
            }

            if (context_p->literal_count >= PARSER_MAXIMUM_NUMBER_OF_LITERALS)
            {
              parser_raise_error (context_p, PARSER_ERR_LITERAL_LIMIT_REACHED);
            }
          }
          else if (!(literal_p->status_flags & LEXER_FLAG_NO_REG_STORE)
                   && register_count < PARSER_MAXIMUM_NUMBER_OF_REGISTERS)
          {
            register_count++;
          }
          else
          {
            literal_p->status_flags |= LEXER_FLAG_NO_REG_STORE;
            uninitialized_var_count++;
          }

          if (literal_p->status_flags & LEXER_FLAG_NO_REG_STORE)
          {
            status_flags |= PARSER_LEXICAL_ENV_NEEDED;
            context_p->status_flags = status_flags;
          }
        }
        else
        {
          ident_count++;
        }
        break;
      }
      case LEXER_STRING_LITERAL:
      case LEXER_NUMBER_LITERAL:
      {
        const_literal_count++;
        break;
      }
      case LEXER_UNUSED_LITERAL:
      {
        if (!(literal_p->status_flags & LEXER_FLAG_FUNCTION_ARGUMENT))
        {
          context_p->literal_count--;
        }
        break;
      }
    }
  }

  if (context_p->literal_count <= CBC_MAXIMUM_SMALL_VALUE)
  {
    literal_one_byte_limit = CBC_MAXIMUM_BYTE_VALUE - 1;
  }
  else
  {
    literal_one_byte_limit = CBC_LOWER_SEVEN_BIT_MASK;
  }

  if (uninitialized_var_count > 0)
  {
    /* Opcode byte and a literal argument. */
    length += 2;
    if ((register_count + uninitialized_var_count - 1) > literal_one_byte_limit)
    {
      length++;
    }
  }

  register_index = context_p->register_count;
  uninitialized_var_index = register_count;
  initialized_var_index = (uint16_t) (uninitialized_var_index + uninitialized_var_count);
  ident_index = (uint16_t) (initialized_var_index + initialized_var_count);
  const_literal_index = (uint16_t) (ident_index + ident_count);
  literal_index = (uint16_t) (const_literal_index + const_literal_count);

  if (initialized_var_count > 2)
  {
    status_flags |= PARSER_HAS_INITIALIZED_VARS;
    context_p->status_flags = status_flags;

    /* Opcode byte and two literal arguments. */
    length += 3;
    if (initialized_var_index > literal_one_byte_limit)
    {
      length++;
    }
    if (ident_index - 1 > literal_one_byte_limit)
    {
      length++;
    }
  }

  /* Second phase: Assign an index to each literal. */
  parser_list_iterator_init (&context_p->literal_pool, &literal_iterator);
  argument_count = 0;

  while ((literal_p = (lexer_literal_t *) parser_list_iterator_next (&literal_iterator)))
  {
    uint16_t init_index;

    if (literal_p->type != LEXER_IDENT_LITERAL)
    {
      if (literal_p->type == LEXER_STRING_LITERAL
          || literal_p->type == LEXER_NUMBER_LITERAL)
      {
        PARSER_ASSERT ((literal_p->status_flags & ~LEXER_FLAG_SOURCE_PTR) == 0);
        literal_p->prop.index = const_literal_index;
        const_literal_index++;
        continue;
      }

      if (literal_p->type != LEXER_UNUSED_LITERAL)
      {
        PARSER_ASSERT (literal_p->status_flags == 0);
        PARSER_ASSERT (literal_p->type == LEXER_FUNCTION_LITERAL
                       || literal_p->type == LEXER_REGEXP_LITERAL);

        literal_p->prop.index = literal_index;
        literal_index++;
        continue;
      }

      PARSER_ASSERT ((literal_p->status_flags & ~(LEXER_FLAG_FUNCTION_ARGUMENT | LEXER_FLAG_SOURCE_PTR)) == 0);

      if (literal_p->status_flags & LEXER_FLAG_FUNCTION_ARGUMENT)
      {
        argument_count++;
      }
      continue;
    }

    if (literal_p->status_flags & LEXER_FLAG_UNUSED_IDENT)
    {
      continue;
    }

    if (!(literal_p->status_flags & LEXER_FLAG_VAR))
    {
      literal_p->prop.index = ident_index;
      ident_index++;
      continue;
    }

    if (!(literal_p->status_flags & LEXER_FLAG_INITIALIZED))
    {
      if (!(literal_p->status_flags & LEXER_FLAG_NO_REG_STORE))
      {
        PARSER_ASSERT (register_count <= PARSER_MAXIMUM_NUMBER_OF_REGISTERS);
        /* This var literal can be stored in a register. */
        literal_p->prop.index = register_index;
        register_index++;
      }
      else
      {
        literal_p->prop.index = uninitialized_var_index;
        uninitialized_var_index++;
      }
      continue;
    }

    if (literal_p->status_flags & LEXER_FLAG_FUNCTION_ARGUMENT)
    {
      if (literal_p->status_flags & LEXER_FLAG_NO_REG_STORE)
      {
        literal_p->prop.index = initialized_var_index;
        initialized_var_index++;
        init_index = argument_count;
        argument_count++;
      }
      else
      {
        literal_p->prop.index = argument_count;
        argument_count++;
        continue;
      }
    }
    else
    {
      if (!(literal_p->status_flags & LEXER_FLAG_NO_REG_STORE))
      {
        PARSER_ASSERT (register_count <= PARSER_MAXIMUM_NUMBER_OF_REGISTERS);
        /* This var literal can be stored in a register. */
        literal_p->prop.index = register_index;
        register_index++;
      }
      else
      {
        literal_p->prop.index = initialized_var_index;
        initialized_var_index++;
      }

      init_index = literal_index;
      literal_index++;

      func_literal_p = (lexer_literal_t *) parser_list_iterator_next (&literal_iterator);

      PARSER_ASSERT (func_literal_p != NULL
                     && func_literal_p->type == LEXER_FUNCTION_LITERAL);
      func_literal_p->prop.index = init_index;
    }

    /* A CBC_INITIALIZE_VAR instruction or part of a CBC_INITIALIZE_VARS instruction. */
    if (!(status_flags & PARSER_HAS_INITIALIZED_VARS)
        || !(literal_p->status_flags & LEXER_FLAG_NO_REG_STORE))
    {
      length += 2;
      if (literal_p->prop.index > literal_one_byte_limit)
      {
        length++;
      }
    }

    length++;
    if (init_index > literal_one_byte_limit)
    {
      length++;
    }
  }

  PARSER_ASSERT (argument_count == context_p->argument_count);
  PARSER_ASSERT (register_index == register_count);
  PARSER_ASSERT (uninitialized_var_index == register_count + uninitialized_var_count);
  PARSER_ASSERT (initialized_var_index == uninitialized_var_index + initialized_var_count);
  PARSER_ASSERT (ident_index == initialized_var_index + ident_count);
  PARSER_ASSERT (const_literal_index == ident_index + const_literal_count);
  PARSER_ASSERT (literal_index == context_p->literal_count);

  *ident_end = ident_index;
  *uninitialized_var_end = uninitialized_var_index;
  *initialized_var_end = initialized_var_index;
  *const_literal_end = const_literal_index;
  context_p->register_count = register_index;

  return length;
} /* parser_compute_indicies */

/**
 * Encode a literal argument.
 *
 * @return position after the encoded values
 */
static PARSER_INLINE uint8_t *
parser_encode_literal (uint8_t *dst_p, /**< destination buffer */
                       uint16_t literal_index, /**< literal index */
                       uint16_t literal_one_byte_limit) /**< maximum value of a literal
                                                          *   encoded in one byte */
{
  if (literal_index <= literal_one_byte_limit)
  {
    *dst_p++ = (uint8_t) (literal_index);
  }
  else
  {
    if (literal_one_byte_limit == CBC_MAXIMUM_BYTE_VALUE - 1)
    {
      *dst_p++ = (uint8_t) (CBC_MAXIMUM_BYTE_VALUE);
      *dst_p++ = (uint8_t) (literal_index - CBC_MAXIMUM_BYTE_VALUE);
    }
    else
    {
      *dst_p++ = (uint8_t) (literal_index >> 8) | CBC_HIGHEST_BIT_MASK;
      *dst_p++ = (uint8_t) (literal_index & CBC_MAXIMUM_BYTE_VALUE);
    }
  }
  return dst_p;
} /* parser_encode_literals */

/**
 * Generate initializer byte codes.
 *
 * @return the end of the initializer stream
 */
static uint8_t *
parser_generate_initializers (parser_context_t *context_p, /**< context */
                              uint8_t *dst_p, /**< destination buffer */
                              literal_value_t *literal_pool_p, /**< start of literal pool */
                              uint16_t uninitialized_var_end, /**< end of the uninitialized var group */
                              uint16_t initialized_var_end, /**< end of the initialized var group */
                              uint16_t literal_one_byte_limit) /**< maximum value of a literal
                                                                *   encoded in one byte */
{
  parser_list_iterator_t literal_iterator;
  lexer_literal_t *literal_p;
  uint16_t argument_count, register_count;

  if (uninitialized_var_end > context_p->register_count)
  {
    *dst_p++ = CBC_DEFINE_VARS;
    dst_p = parser_encode_literal (dst_p,
                                   (uint16_t) (uninitialized_var_end - 1),
                                   literal_one_byte_limit);
  }

  if (context_p->status_flags & PARSER_HAS_INITIALIZED_VARS)
  {
    const uint8_t expected_status_flags = LEXER_FLAG_VAR | LEXER_FLAG_NO_REG_STORE | LEXER_FLAG_INITIALIZED;
#ifdef PARSER_DEBUG
    uint16_t next_index = uninitialized_var_end;
#endif

    context_p->status_flags |= PARSER_LEXICAL_ENV_NEEDED;

    *dst_p++ = CBC_INITIALIZE_VARS;
    dst_p = parser_encode_literal (dst_p,
                                   (uint16_t) uninitialized_var_end,
                                   literal_one_byte_limit);
    dst_p = parser_encode_literal (dst_p,
                                   (uint16_t) (initialized_var_end - 1),
                                   literal_one_byte_limit);

    parser_list_iterator_init (&context_p->literal_pool, &literal_iterator);
    argument_count = 0;

    while ((literal_p = (lexer_literal_t *) parser_list_iterator_next (&literal_iterator)))
    {
      if (literal_p->status_flags & LEXER_FLAG_FUNCTION_ARGUMENT)
      {
        argument_count++;
      }

      if ((literal_p->status_flags & expected_status_flags) == expected_status_flags)
      {
        uint16_t init_index;

        PARSER_ASSERT (literal_p->type == LEXER_IDENT_LITERAL);
#ifdef PARSER_DEBUG
        PARSER_ASSERT (literal_p->prop.index == next_index);
        next_index++;
#endif

        literal_p->status_flags = (uint8_t) (literal_p->status_flags & ~LEXER_FLAG_INITIALIZED);

        if (literal_p->status_flags & LEXER_FLAG_FUNCTION_ARGUMENT)
        {
          init_index = (uint16_t) (argument_count - 1);
        }
        else
        {
          literal_p = (lexer_literal_t *) parser_list_iterator_next (&literal_iterator);

          PARSER_ASSERT (literal_p != NULL
                         && literal_p->type == LEXER_FUNCTION_LITERAL);
          init_index = literal_p->prop.index;
        }

        dst_p = parser_encode_literal (dst_p, init_index, literal_one_byte_limit);
      }
    }

    PARSER_ASSERT (argument_count == context_p->argument_count);
  }

  parser_list_iterator_init (&context_p->literal_pool, &literal_iterator);
  argument_count = 0;
  register_count = context_p->register_count;

  while ((literal_p = (lexer_literal_t *) parser_list_iterator_next (&literal_iterator)))
  {
    const uint8_t expected_status_flags = LEXER_FLAG_VAR | LEXER_FLAG_INITIALIZED;

    if (literal_p->type != LEXER_UNUSED_LITERAL
        && !(literal_p->status_flags & LEXER_FLAG_UNUSED_IDENT)
        && literal_p->prop.index >= register_count)
    {
      literal_pool_p[literal_p->prop.index] = literal_p->value;
    }

    if (literal_p->status_flags & LEXER_FLAG_FUNCTION_ARGUMENT)
    {
      argument_count++;
    }

    if ((literal_p->status_flags & expected_status_flags) == expected_status_flags)
    {
      uint16_t index = literal_p->prop.index;
      uint16_t init_index;

      PARSER_ASSERT (literal_p->type == LEXER_IDENT_LITERAL);

      if (literal_p->status_flags & LEXER_FLAG_FUNCTION_ARGUMENT)
      {
        init_index = (uint16_t) (argument_count - 1);

        if (init_index == literal_p->prop.index)
        {
          continue;
        }
      }
      else
      {
        literal_p = (lexer_literal_t *) parser_list_iterator_next (&literal_iterator);

        PARSER_ASSERT (literal_p != NULL
                       && literal_p->type == LEXER_FUNCTION_LITERAL);

        init_index = literal_p->prop.index;
        PARSER_ASSERT (init_index >= register_count);
      }

      *dst_p++ = CBC_INITIALIZE_VAR;
      dst_p = parser_encode_literal (dst_p, index, literal_one_byte_limit);
      dst_p = parser_encode_literal (dst_p, init_index, literal_one_byte_limit);
    }
  }

#ifndef CONFIG_DISABLE_ES2015_FUNCTION_REST_PARAMETER
  if (context_p->status_flags & PARSER_FUNCTION_HAS_REST_PARAM)
  {
    PARSER_ASSERT ((argument_count - 1) == context_p->argument_count);
    return dst_p;
  }
#endif /* !CONFIG_DISABLE_ES2015_FUNCTION_REST_PARAMETER */

  PARSER_ASSERT (argument_count == context_p->argument_count);
  return dst_p;
} /* parser_generate_initializers */

/*
 * During byte code post processing certain bytes are not
 * copied into the final byte code buffer. For example, if
 * one byte is enough for encoding a literal index, the
 * second byte is not copied. However, when a byte is skipped,
 * the offsets of those branches which crosses (jumps over)
 * that byte code should also be decreased by one. Instead
 * of finding these jumps every time when a byte is skipped,
 * all branch offset updates are computed in one step.
 *
 * Branch offset mapping example:
 *
 * Let's assume that each parser_mem_page of the byte_code
 * buffer is 8 bytes long and only 4 bytes are kept for a
 * given page:
 *
 * +---+---+---+---+---+---+---+---+
 * | X | 1 | 2 | 3 | X | 4 | X | X |
 * +---+---+---+---+---+---+---+---+
 *
 * X marks those bytes which are removed. The resulting
 * offset mapping is the following:
 *
 * +---+---+---+---+---+---+---+---+
 * | 0 | 1 | 2 | 3 | 3 | 4 | 4 | 4 |
 * +---+---+---+---+---+---+---+---+
 *
 * Each X is simply replaced by the index of the previous
 * index starting from zero. This shows the number of
 * copied bytes before a given byte including the byte
 * itself. The last byte always shows the number of bytes
 * copied from this page.
 *
 * This mapping allows recomputing all branch targets,
 * since mapping[to] - mapping[from] is the new argument
 * for forward branches. As for backward branches, the
 * equation is reversed to mapping[from] - mapping[to].
 *
 * The mapping is relative to one page, so distance
 * computation affecting multiple pages requires a loop.
 * We should also note that only argument bytes can
 * be skipped, so removed bytes cannot be targeted by
 * branches. Valid branches always target instruction
 * starts only.
 */

/**
 * Recompute the argument of a forward branch.
 *
 * @return the new distance
 */
static size_t
parser_update_forward_branch (parser_mem_page_t *page_p, /**< current page */
                              size_t full_distance, /**< full distance */
                              uint8_t bytes_copied_before_jump) /**< bytes copied before jump */
{
  size_t new_distance = 0;

  while (full_distance > PARSER_CBC_STREAM_PAGE_SIZE)
  {
    new_distance += page_p->bytes[PARSER_CBC_STREAM_PAGE_SIZE - 1] & CBC_LOWER_SEVEN_BIT_MASK;
    full_distance -= PARSER_CBC_STREAM_PAGE_SIZE;
    page_p = page_p->next_p;
  }

  new_distance += page_p->bytes[full_distance - 1] & CBC_LOWER_SEVEN_BIT_MASK;
  return new_distance - bytes_copied_before_jump;
} /* parser_update_forward_branch */

/**
 * Recompute the argument of a backward branch.
 *
 * @return the new distance
 */
static size_t
parser_update_backward_branch (parser_mem_page_t *page_p, /**< current page */
                               size_t full_distance, /**< full distance */
                               uint8_t bytes_copied_before_jump) /**< bytes copied before jump */
{
  size_t new_distance = bytes_copied_before_jump;

  while (full_distance >= PARSER_CBC_STREAM_PAGE_SIZE)
  {
    PARSER_ASSERT (page_p != NULL);
    new_distance += page_p->bytes[PARSER_CBC_STREAM_PAGE_SIZE - 1] & CBC_LOWER_SEVEN_BIT_MASK;
    full_distance -= PARSER_CBC_STREAM_PAGE_SIZE;
    page_p = page_p->next_p;
  }

  if (full_distance > 0)
  {
    size_t offset = PARSER_CBC_STREAM_PAGE_SIZE - full_distance;

    PARSER_ASSERT (page_p != NULL);

    new_distance += page_p->bytes[PARSER_CBC_STREAM_PAGE_SIZE - 1] & CBC_LOWER_SEVEN_BIT_MASK;
    new_distance -= page_p->bytes[offset - 1] & CBC_LOWER_SEVEN_BIT_MASK;
  }

  return new_distance;
} /* parser_update_backward_branch */

/**
 * Update targets of all branches in one step.
 */
static void
parse_update_branches (parser_context_t *context_p, /**< context */
                       uint8_t *byte_code_p) /**< byte code */
{
  parser_mem_page_t *page_p = context_p->byte_code.first_p;
  parser_mem_page_t *prev_page_p = NULL;
  parser_mem_page_t *last_page_p = context_p->byte_code.last_p;
  size_t last_position = context_p->byte_code.last_position;
  size_t offset = 0;
  size_t bytes_copied = 0;

  if (last_position >= PARSER_CBC_STREAM_PAGE_SIZE)
  {
    last_page_p = NULL;
    last_position = 0;
  }

  while (page_p != last_page_p || offset < last_position)
  {
    /* Branch instructions are marked to improve search speed. */
    if (page_p->bytes[offset] & CBC_HIGHEST_BIT_MASK)
    {
      uint8_t *bytes_p = byte_code_p + bytes_copied;
      uint8_t flags;
      uint8_t bytes_copied_before_jump = 0;
      size_t branch_argument_length;
      size_t target_distance;
      size_t length;

      if (offset > 0)
      {
        bytes_copied_before_jump = page_p->bytes[offset - 1] & CBC_LOWER_SEVEN_BIT_MASK;
      }
      bytes_p += bytes_copied_before_jump;

      if (*bytes_p == CBC_EXT_OPCODE)
      {
        bytes_p++;
        flags = cbc_ext_flags[*bytes_p];
      }
      else
      {
        flags = cbc_flags[*bytes_p];
      }

      PARSER_ASSERT (flags & CBC_HAS_BRANCH_ARG);
      branch_argument_length = CBC_BRANCH_OFFSET_LENGTH (*bytes_p);
      bytes_p++;

      /* Decoding target. */
      length = branch_argument_length;
      target_distance = 0;
      do
      {
        target_distance = (target_distance << 8) | *bytes_p;
        bytes_p++;
      }
      while (--length > 0);

      if (CBC_BRANCH_IS_FORWARD (flags))
      {
        /* Branch target was not set. */
        PARSER_ASSERT (target_distance > 0);

        target_distance = parser_update_forward_branch (page_p,
                                                        offset + target_distance,
                                                        bytes_copied_before_jump);
      }
      else
      {
        if (target_distance < offset)
        {
          uint8_t bytes_copied_before_target = page_p->bytes[offset - target_distance - 1];
          bytes_copied_before_target = bytes_copied_before_target & CBC_LOWER_SEVEN_BIT_MASK;

          target_distance = (size_t) (bytes_copied_before_jump - bytes_copied_before_target);
        }
        else if (target_distance == offset)
        {
          target_distance = bytes_copied_before_jump;
        }
        else
        {
          target_distance = parser_update_backward_branch (prev_page_p,
                                                           target_distance - offset,
                                                           bytes_copied_before_jump);
        }
      }

      /* Encoding target again. */
      do
      {
        bytes_p--;
        *bytes_p = (uint8_t) (target_distance & 0xff);
        target_distance >>= 8;
      }
      while (--branch_argument_length > 0);
    }

    offset++;
    if (offset >= PARSER_CBC_STREAM_PAGE_SIZE)
    {
      parser_mem_page_t *next_p = page_p->next_p;

      /* We reverse the pages before the current page. */
      page_p->next_p = prev_page_p;
      prev_page_p = page_p;

      bytes_copied += page_p->bytes[PARSER_CBC_STREAM_PAGE_SIZE - 1] & CBC_LOWER_SEVEN_BIT_MASK;
      page_p = next_p;
      offset = 0;
    }
  }

  /* After this point the pages of the byte code stream are
   * not used anymore. However, they needs to be freed during
   * cleanup, so the first and last pointers of the stream
   * descriptor are reversed as well. */
  if (last_page_p != NULL)
  {
    PARSER_ASSERT (last_page_p == context_p->byte_code.last_p);
    last_page_p->next_p = prev_page_p;
  }
  else
  {
    last_page_p = context_p->byte_code.last_p;
  }

  context_p->byte_code.last_p = context_p->byte_code.first_p;
  context_p->byte_code.first_p = last_page_p;
} /* parse_update_branches */

#ifdef PARSER_DUMP_BYTE_CODE

/**
 * Print literal.
 */
static void
parse_print_literal (cbc_compiled_code_t *compiled_code_p, /**< compiled code */
                     uint16_t literal_index, /**< literal index */
                     parser_list_t *literal_pool_p) /**< literal pool */
{
  lexer_literal_t *literal_p;
  parser_list_iterator_t literal_iterator;
  uint16_t argument_end;
  uint16_t register_end;
  uint16_t ident_end;
  uint16_t const_literal_end;

  if (compiled_code_p->status_flags & CBC_CODE_FLAGS_UINT16_ARGUMENTS)
  {
    cbc_uint16_arguments_t *args_p = (cbc_uint16_arguments_t *) compiled_code_p;
    argument_end = args_p->argument_end;
    register_end = args_p->register_end;
    ident_end = args_p->ident_end;
    const_literal_end = args_p->const_literal_end;
  }
  else
  {
    cbc_uint8_arguments_t *args_p = (cbc_uint8_arguments_t *) compiled_code_p;
    argument_end = args_p->argument_end;
    register_end = args_p->register_end;
    ident_end = args_p->ident_end;
    const_literal_end = args_p->const_literal_end;
  }

  parser_list_iterator_init (literal_pool_p, &literal_iterator);

  while (PARSER_TRUE)
  {
    literal_p = (lexer_literal_t *) parser_list_iterator_next (&literal_iterator);

    if (literal_p == NULL)
    {
      if (literal_index == const_literal_end)
      {
        printf (" idx:%d(self)->function", literal_index);
        break;
      }

      PARSER_ASSERT (literal_index < argument_end);
      printf (" idx:%d(arg)->undefined", literal_index);
      break;
    }

    if (literal_p->prop.index == literal_index
        && literal_p->type != LEXER_UNUSED_LITERAL
        && !(literal_p->status_flags & LEXER_FLAG_UNUSED_IDENT))
    {
      printf (" idx:%d", literal_index);

      if (literal_index < argument_end)
      {
        printf ("(arg)->");
      }
      else if (literal_index < register_end)
      {
        printf ("(reg)->");
      }
      else if (literal_index < ident_end)
      {
        printf ("(ident)->");
      }
      else
      {
        printf ("(lit)->");
      }

      util_print_literal (literal_p);
      return;
    }
  }
} /* parse_print_literal */

#define PARSER_READ_IDENTIFIER_INDEX(name) \
  name = *byte_code_p++; \
  if (name >= encoding_limit) \
  { \
    name = (uint16_t) (((name << 8) | byte_code_p[0]) - encoding_delta); \
    byte_code_p++; \
  }

/**
 * Print CBC_DEFINE_VARS instruction.
 *
 * @return next byte code position
 */
static uint8_t *
parse_print_define_vars (cbc_compiled_code_t *compiled_code_p, /**< compiled code */
                         uint8_t *byte_code_p, /**< byte code position */
                         uint16_t encoding_limit, /**< literal encoding limit */
                         uint16_t encoding_delta, /**< literal encoding delta */
                         parser_list_t *literal_pool_p) /**< literal pool */
{
  uint16_t identifier_index;
  uint16_t identifier_end;

  if (compiled_code_p->status_flags & CBC_CODE_FLAGS_UINT16_ARGUMENTS)
  {
    cbc_uint16_arguments_t *args_p = (cbc_uint16_arguments_t *) compiled_code_p;
    identifier_index = args_p->register_end;
  }
  else
  {
    cbc_uint8_arguments_t *args_p = (cbc_uint8_arguments_t *) compiled_code_p;
    identifier_index = args_p->register_end;
  }

  PARSER_READ_IDENTIFIER_INDEX (identifier_end);

  printf (" from: %d to: %d\n", identifier_index, identifier_end);

  while (identifier_index <= identifier_end)
  {
    printf ("        ");
    parse_print_literal (compiled_code_p, identifier_index, literal_pool_p);
    identifier_index++;
    printf ("\n");
  }

  return byte_code_p;
} /* parse_print_define_vars */

/**
 * Print CBC_INITIALIZE_VARS instruction.
 *
 * @return next byte code position
 */
static uint8_t *
parse_print_initialize_vars (cbc_compiled_code_t *compiled_code_p, /**< compiled code */
                             uint8_t *byte_code_p, /**< byte code position */
                             uint16_t encoding_limit, /**< literal encoding limit */
                             uint16_t encoding_delta, /**< literal encoding delta */
                             parser_list_t *literal_pool_p) /**< literal pool */
{
  uint16_t identifier_index;
  uint16_t identifier_end;

  PARSER_READ_IDENTIFIER_INDEX (identifier_index);
  PARSER_READ_IDENTIFIER_INDEX (identifier_end);

  printf (" from: %d to: %d\n", identifier_index, identifier_end);

  while (identifier_index <= identifier_end)
  {
    uint16_t literal_index;

    printf ("        ");
    parse_print_literal (compiled_code_p, identifier_index, literal_pool_p);
    printf (" =");

    PARSER_READ_IDENTIFIER_INDEX (literal_index);

    parse_print_literal (compiled_code_p, literal_index, literal_pool_p);
    identifier_index++;
    printf ("\n");
  }

  return byte_code_p;
} /* parse_print_initialize_vars */

/**
 * Print byte code.
 */
static void
parse_print_final_cbc (cbc_compiled_code_t *compiled_code_p, /**< compiled code */
                       parser_list_t *literal_pool_p, /**< literal pool */
                       size_t length) /**< length of byte code */
{
  cbc_opcode_t opcode;
  cbc_ext_opcode_t ext_opcode;
  uint8_t flags;
  uint8_t *byte_code_start_p;
  uint8_t *byte_code_end_p;
  uint8_t *byte_code_p;
  uint16_t encoding_limit;
  uint16_t encoding_delta;
  uint16_t stack_limit;
  uint16_t argument_end;
  uint16_t register_end;
  uint16_t ident_end;
  uint16_t const_literal_end;
  uint16_t literal_end;

  if (compiled_code_p->status_flags & CBC_CODE_FLAGS_UINT16_ARGUMENTS)
  {
    cbc_uint16_arguments_t *args = (cbc_uint16_arguments_t *) compiled_code_p;
    stack_limit = args->stack_limit;
    argument_end = args->argument_end;
    register_end = args->register_end;
    ident_end = args->ident_end;
    const_literal_end = args->const_literal_end;
    literal_end = args->literal_end;
  }
  else
  {
    cbc_uint8_arguments_t *args = (cbc_uint8_arguments_t *) compiled_code_p;
    stack_limit = args->stack_limit;
    argument_end = args->argument_end;
    register_end = args->register_end;
    ident_end = args->ident_end;
    const_literal_end = args->const_literal_end;
    literal_end = args->literal_end;
  }

  printf ("\nFinal byte code dump:\n\n  Maximum stack depth: %d\n  Flags: [",
          (int) (stack_limit + register_end));

  if (!(compiled_code_p->status_flags & CBC_CODE_FLAGS_FULL_LITERAL_ENCODING))
  {
    printf ("small_lit_enc");
    encoding_limit = 255;
    encoding_delta = 0xfe01;
  }
  else
  {
    printf ("full_lit_enc");
    encoding_limit = 128;
    encoding_delta = 0x8000;
  }

  if (compiled_code_p->status_flags & CBC_CODE_FLAGS_UINT16_ARGUMENTS)
  {
    printf (",uint16_arguments");
  }

  if (compiled_code_p->status_flags & CBC_CODE_FLAGS_STRICT_MODE)
  {
    printf (",strict_mode");
  }

  if (compiled_code_p->status_flags & CBC_CODE_FLAGS_ARGUMENTS_NEEDED)
  {
    printf (",arguments_needed");
  }

  if (compiled_code_p->status_flags & CBC_CODE_FLAGS_LEXICAL_ENV_NOT_NEEDED)
  {
    printf (",no_lexical_env");
  }

#ifndef CONFIG_DISABLE_ES2015_ARROW_FUNCTION
  if (compiled_code_p->status_flags & CBC_CODE_FLAGS_ARROW_FUNCTION)
  {
    printf (",arrow");
  }
#endif /* !CONFIG_DISABLE_ES2015_ARROW_FUNCTION */

#ifndef CONFIG_DISABLE_ES2015_CLASS
  if (compiled_code_p->status_flags & CBC_CODE_FLAGS_CONSTRUCTOR)
  {
    printf (",constructor");
  }
#endif /* !CONFIG_DISABLE_ES2015_CLASS */

  printf ("]\n");

  printf ("  Argument range end: %d\n", (int) argument_end);
  printf ("  Register range end: %d\n", (int) register_end);
  printf ("  Identifier range end: %d\n", (int) ident_end);
  printf ("  Const literal range end: %d\n", (int) const_literal_end);
  printf ("  Literal range end: %d\n\n", (int) literal_end);

  byte_code_start_p = (uint8_t *) compiled_code_p;

  if (compiled_code_p->status_flags & CBC_CODE_FLAGS_UINT16_ARGUMENTS)
  {
    byte_code_start_p += sizeof (cbc_uint16_arguments_t);
  }
  else
  {
    byte_code_start_p += sizeof (cbc_uint8_arguments_t);
  }

  byte_code_start_p += (unsigned int) (literal_end - register_end) * sizeof (literal_value_t);

  byte_code_end_p = byte_code_start_p + length;
  byte_code_p = byte_code_start_p;

  while (byte_code_p < byte_code_end_p)
  {
    size_t cbc_offset;

    opcode = (cbc_opcode_t) *byte_code_p;
    ext_opcode = CBC_EXT_NOP;
    cbc_offset = (size_t) (byte_code_p - byte_code_start_p);

    if (opcode != CBC_EXT_OPCODE)
    {
      flags = cbc_flags[opcode];
      printf (" %3d : %s", (int) cbc_offset, cbc_names[opcode]);
      byte_code_p++;

      if (opcode == CBC_INITIALIZE_VARS)
      {
        byte_code_p = parse_print_initialize_vars (compiled_code_p,
                                                   byte_code_p,
                                                   encoding_limit,
                                                   encoding_delta,
                                                   literal_pool_p);
        continue;
      }

      if (opcode == CBC_DEFINE_VARS)
      {
        byte_code_p = parse_print_define_vars (compiled_code_p,
                                               byte_code_p,
                                               encoding_limit,
                                               encoding_delta,
                                               literal_pool_p);
        continue;
      }
    }
    else
    {
      ext_opcode = (cbc_ext_opcode_t) byte_code_p[1];
      flags = cbc_ext_flags[ext_opcode];
      printf (" %3d : %s", (int) cbc_offset, cbc_ext_names[ext_opcode]);
      byte_code_p += 2;
    }

    if (flags & (CBC_HAS_LITERAL_ARG | CBC_HAS_LITERAL_ARG2))
    {
      uint16_t literal_index;

      PARSER_READ_IDENTIFIER_INDEX (literal_index);
      parse_print_literal (compiled_code_p, literal_index, literal_pool_p);
    }

    if (flags & CBC_HAS_LITERAL_ARG2)
    {
      uint16_t literal_index;

      PARSER_READ_IDENTIFIER_INDEX (literal_index);
      parse_print_literal (compiled_code_p, literal_index, literal_pool_p);

      if (!(flags & CBC_HAS_LITERAL_ARG))
      {
        PARSER_READ_IDENTIFIER_INDEX (literal_index);
        parse_print_literal (compiled_code_p, literal_index, literal_pool_p);
      }
    }

    if (flags & CBC_HAS_BYTE_ARG)
    {
      if (opcode == CBC_PUSH_NUMBER_POS_BYTE
          || ext_opcode == CBC_EXT_PUSH_LITERAL_PUSH_NUMBER_POS_BYTE)
      {
        printf (" number:%d", (int) *byte_code_p + 1);
      }
      else if (opcode == CBC_PUSH_NUMBER_NEG_BYTE
               || ext_opcode == CBC_EXT_PUSH_LITERAL_PUSH_NUMBER_NEG_BYTE)
      {
        printf (" number:%d", -((int) *byte_code_p + 1));
      }
      else
      {
        printf (" byte_arg:%d", *byte_code_p);
      }
      byte_code_p++;
    }

    if (flags & CBC_HAS_BRANCH_ARG)
    {
      size_t branch_offset_length = (opcode != CBC_EXT_OPCODE ? CBC_BRANCH_OFFSET_LENGTH (opcode)
                                                              : CBC_BRANCH_OFFSET_LENGTH (ext_opcode));
      size_t offset = 0;

      do
      {
        offset = (offset << 8) | *byte_code_p++;
      }
      while (--branch_offset_length > 0);

      printf (" offset:%d(->%d)",
              (int) offset,
              (int) (cbc_offset + (CBC_BRANCH_IS_FORWARD (flags) ? offset : -offset)));
    }
    printf ("\n");
  }
} /* parse_print_final_cbc */

#undef PARSER_READ_IDENTIFIER_INDEX

#endif /* PARSER_DUMP_BYTE_CODE */

#define PARSER_NEXT_BYTE(page_p, offset) \
  do { \
    if (++(offset) >= PARSER_CBC_STREAM_PAGE_SIZE) \
    { \
      offset = 0; \
      page_p = page_p->next_p; \
    } \
  } while (0)

#define PARSER_NEXT_BYTE_UPDATE(page_p, offset, real_offset) \
  do { \
    page_p->bytes[offset] = real_offset; \
    if (++(offset) >= PARSER_CBC_STREAM_PAGE_SIZE) \
    { \
      offset = 0; \
      real_offset = 0; \
      page_p = page_p->next_p; \
    } \
  } while (0)

/**
 * Post processing main function.
 *
 * @return compiled code
 */
static cbc_compiled_code_t *
parser_post_processing (parser_context_t *context_p) /**< context */
{
  uint16_t literal_one_byte_limit;
  uint16_t ident_end;
  uint16_t uninitialized_var_end;
  uint16_t initialized_var_end;
  uint16_t const_literal_end;
  parser_mem_page_t *page_p;
  parser_mem_page_t *last_page_p;
  size_t last_position;
  size_t offset;
  size_t length;
  size_t literal_length;
  size_t total_size;
  size_t initializers_length;
  uint8_t real_offset;
  uint8_t *byte_code_p;
  int needs_uint16_arguments;
  cbc_opcode_t last_opcode = CBC_EXT_OPCODE;
  cbc_compiled_code_t *compiled_code_p;
  literal_value_t *literal_pool_p;
  uint8_t *dst_p;

  if ((size_t) context_p->stack_limit + (size_t) context_p->register_count > PARSER_MAXIMUM_STACK_LIMIT)
  {
    parser_raise_error (context_p, PARSER_ERR_STACK_LIMIT_REACHED);
  }

  PARSER_ASSERT (context_p->literal_count <= PARSER_MAXIMUM_NUMBER_OF_LITERALS);

  parser_copy_identifiers (context_p);

  initializers_length = parser_compute_indicies (context_p,
                                                 &ident_end,
                                                 &uninitialized_var_end,
                                                 &initialized_var_end,
                                                 &const_literal_end);
  length = initializers_length;

  if (context_p->literal_count <= CBC_MAXIMUM_SMALL_VALUE)
  {
    literal_one_byte_limit = CBC_MAXIMUM_BYTE_VALUE - 1;
  }
  else
  {
    literal_one_byte_limit = CBC_LOWER_SEVEN_BIT_MASK;
  }

  last_page_p = context_p->byte_code.last_p;
  last_position = context_p->byte_code.last_position;

  if (last_position >= PARSER_CBC_STREAM_PAGE_SIZE)
  {
    last_page_p = NULL;
    last_position = 0;
  }

  page_p = context_p->byte_code.first_p;
  offset = 0;

  while (page_p != last_page_p || offset < last_position)
  {
    uint8_t *opcode_p;
    uint8_t flags;
    size_t branch_offset_length;

    opcode_p = page_p->bytes + offset;
    last_opcode = (cbc_opcode_t) (*opcode_p);
    PARSER_NEXT_BYTE (page_p, offset);
    branch_offset_length = CBC_BRANCH_OFFSET_LENGTH (last_opcode);
    flags = cbc_flags[last_opcode];
    length++;

    if (last_opcode == CBC_EXT_OPCODE)
    {
      cbc_ext_opcode_t ext_opcode;

      ext_opcode = (cbc_ext_opcode_t) page_p->bytes[offset];
      branch_offset_length = CBC_BRANCH_OFFSET_LENGTH (ext_opcode);
      flags = cbc_ext_flags[ext_opcode];
      PARSER_NEXT_BYTE (page_p, offset);
      length++;

#ifndef CONFIG_DISABLE_ES2015_CLASS
      if (ext_opcode == CBC_EXT_CONSTRUCTOR_RETURN)
      {
        last_opcode = CBC_RETURN;
      }
#endif /* !CONFIG_DISABLE_ES2015 */
    }

    while (flags & (CBC_HAS_LITERAL_ARG | CBC_HAS_LITERAL_ARG2))
    {
      uint8_t *first_byte = page_p->bytes + offset;
      size_t literal_index = *first_byte;
      lexer_literal_t *literal_p;

      PARSER_NEXT_BYTE (page_p, offset);
      length++;

      literal_index |= ((size_t) page_p->bytes[offset]) << 8;
      literal_p = PARSER_GET_LITERAL (literal_index);

      if (literal_p->type == LEXER_UNUSED_LITERAL)
      {
        /* In a few cases uninitialized literals may have been converted to initialized
         * literals later. Byte code references to the old (uninitialized) literals
         * must be redirected to the new instance of the literal. */
        literal_p = PARSER_GET_LITERAL (literal_p->prop.index);

        PARSER_ASSERT (literal_p != NULL && literal_p->type != LEXER_UNUSED_LITERAL);
      }

      if (literal_p->prop.index <= literal_one_byte_limit)
      {
        *first_byte = (uint8_t) literal_p->prop.index;
      }
      else
      {
        if (context_p->literal_count <= CBC_MAXIMUM_SMALL_VALUE)
        {
          PARSER_ASSERT (literal_index <= CBC_MAXIMUM_SMALL_VALUE);
          *first_byte = CBC_MAXIMUM_BYTE_VALUE;
          page_p->bytes[offset] = (uint8_t) (literal_p->prop.index - CBC_MAXIMUM_BYTE_VALUE);
          length++;
        }
        else
        {
          PARSER_ASSERT (literal_index <= CBC_MAXIMUM_FULL_VALUE);
          *first_byte = (uint8_t) (literal_p->prop.index >> 8) | CBC_HIGHEST_BIT_MASK;
          page_p->bytes[offset] = (uint8_t) (literal_p->prop.index & 0xff);
          length++;
        }
      }
      PARSER_NEXT_BYTE (page_p, offset);

      if (flags & CBC_HAS_LITERAL_ARG2)
      {
        if (flags & CBC_HAS_LITERAL_ARG)
        {
          flags = CBC_HAS_LITERAL_ARG;
        }
        else
        {
          flags = CBC_HAS_LITERAL_ARG | CBC_HAS_LITERAL_ARG2;
        }
      }
      else
      {
        break;
      }
    }

    if (flags & CBC_HAS_BYTE_ARG)
    {
      /* This argument will be copied without modification. */
      PARSER_NEXT_BYTE (page_p, offset);
      length++;
    }

    if (flags & CBC_HAS_BRANCH_ARG)
    {
      int prefix_zero = PARSER_TRUE;
#if PARSER_MAXIMUM_CODE_SIZE <= 65535
      cbc_opcode_t jump_forward = CBC_JUMP_FORWARD_2;
#else /* PARSER_MAXIMUM_CODE_SIZE <= 65535 */
      cbc_opcode_t jump_forward = CBC_JUMP_FORWARD_3;
#endif /* PARSER_MAXIMUM_CODE_SIZE <= 65535 */

      /* The leading zeroes are dropped from the stream.
       * Although dropping these zeroes for backward
       * branches are unnecessary, we use the same
       * code path for simplicity. */
      PARSER_ASSERT (branch_offset_length > 0 && branch_offset_length <= 3);

      while (--branch_offset_length > 0)
      {
        uint8_t byte = page_p->bytes[offset];
        if (byte > 0 || !prefix_zero)
        {
          prefix_zero = PARSER_FALSE;
          length++;
        }
        else
        {
          PARSER_ASSERT (CBC_BRANCH_IS_FORWARD (flags));
        }
        PARSER_NEXT_BYTE (page_p, offset);
      }

      if (last_opcode == jump_forward
          && prefix_zero
          && page_p->bytes[offset] == CBC_BRANCH_OFFSET_LENGTH (jump_forward) + 1)
      {
        /* Uncoditional jumps which jump right after the instruction
         * are effectively NOPs. These jumps are removed from the
         * stream. The 1 byte long CBC_JUMP_FORWARD form marks these
         * instructions, since this form is constructed during post
         * processing and cannot be emitted directly. */
        *opcode_p = CBC_JUMP_FORWARD;
        length --;
      }
      else
      {
        /* Other last bytes are always copied. */
        length++;
      }

      PARSER_NEXT_BYTE (page_p, offset);
    }
  }

  if (!(context_p->status_flags & PARSER_NO_END_LABEL)
      || !(PARSER_OPCODE_IS_RETURN (last_opcode)))
  {
    context_p->status_flags &= (uint32_t) ~PARSER_NO_END_LABEL;
    length++;
  }

  needs_uint16_arguments = PARSER_FALSE;
  total_size = sizeof (cbc_uint8_arguments_t);

  if (context_p->stack_limit > CBC_MAXIMUM_BYTE_VALUE
      || context_p->register_count > CBC_MAXIMUM_BYTE_VALUE
      || context_p->literal_count > CBC_MAXIMUM_BYTE_VALUE)
  {
    needs_uint16_arguments = PARSER_TRUE;
    total_size = sizeof (cbc_uint16_arguments_t);
  }

  literal_length = (size_t) (context_p->literal_count - context_p->register_count) * sizeof (literal_value_t);

  total_size += literal_length + length;

  if ((context_p->status_flags & PARSER_ARGUMENTS_NEEDED)
      && !(context_p->status_flags & PARSER_IS_STRICT))
  {
    total_size += context_p->argument_count * sizeof (literal_value_t);
  }

  compiled_code_p = (cbc_compiled_code_t *) parser_malloc (context_p, total_size);

  byte_code_p = (uint8_t *) compiled_code_p;
  compiled_code_p->status_flags = CBC_CODE_FLAGS_FUNCTION;

#ifndef CONFIG_DISABLE_ES2015_FUNCTION_REST_PARAMETER
  if (context_p->status_flags & PARSER_FUNCTION_HAS_REST_PARAM)
  {
    PARSER_ASSERT (context_p->argument_count > 0);
    context_p->argument_count--;
  }
#endif /* !CONFIG_DISABLE_ES2015_FUNCTION_REST_PARAMETER */

  if (needs_uint16_arguments)
  {
    cbc_uint16_arguments_t *args_p = (cbc_uint16_arguments_t *) compiled_code_p;

    args_p->stack_limit = context_p->stack_limit;
    args_p->argument_end = context_p->argument_count;
    args_p->register_end = context_p->register_count;
    args_p->ident_end = ident_end;
    args_p->const_literal_end = const_literal_end;
    args_p->literal_end = context_p->literal_count;

    compiled_code_p->status_flags |= CBC_CODE_FLAGS_UINT16_ARGUMENTS;
    byte_code_p += sizeof (cbc_uint16_arguments_t);
  }
  else
  {
    cbc_uint8_arguments_t *args_p = (cbc_uint8_arguments_t *) compiled_code_p;

    args_p->stack_limit = (uint8_t) context_p->stack_limit;
    args_p->argument_end = (uint8_t) context_p->argument_count;
    args_p->register_end = (uint8_t) context_p->register_count;
    args_p->ident_end = (uint8_t) ident_end;
    args_p->const_literal_end = (uint8_t) const_literal_end;
    args_p->literal_end = (uint8_t) context_p->literal_count;

    byte_code_p += sizeof (cbc_uint8_arguments_t);
  }

  if (context_p->literal_count > CBC_MAXIMUM_SMALL_VALUE)
  {
    compiled_code_p->status_flags |= CBC_CODE_FLAGS_FULL_LITERAL_ENCODING;
  }

  if (context_p->status_flags & PARSER_IS_STRICT)
  {
    compiled_code_p->status_flags |= CBC_CODE_FLAGS_STRICT_MODE;
  }

  if (context_p->status_flags & PARSER_ARGUMENTS_NEEDED)
  {
    compiled_code_p->status_flags |= CBC_CODE_FLAGS_ARGUMENTS_NEEDED;

    /* Arguments is stored in the lexical environment. */
    context_p->status_flags |= PARSER_LEXICAL_ENV_NEEDED;
  }

  if (!(context_p->status_flags & PARSER_LEXICAL_ENV_NEEDED))
  {
    compiled_code_p->status_flags |= CBC_CODE_FLAGS_LEXICAL_ENV_NOT_NEEDED;
  }

#ifndef CONFIG_DISABLE_ES2015_ARROW_FUNCTION
  if (context_p->status_flags & PARSER_IS_ARROW_FUNCTION)
  {
    compiled_code_p->status_flags |= CBC_CODE_FLAGS_ARROW_FUNCTION;
  }
#endif /* !CONFIG_DISABLE_ES2015_ARROW_FUNCTION */

#ifndef CONFIG_DISABLE_ES2015_CLASS
  if (context_p->status_flags & PARSER_CLASS_CONSTRUCTOR)
  {
    compiled_code_p->status_flags |= CBC_CODE_FLAGS_CONSTRUCTOR;
  }
#endif /* !CONFIG_DISABLE_ES2015_CLASS */

#ifndef CONFIG_DISABLE_ES2015_FUNCTION_REST_PARAMETER
  if (context_p->status_flags & PARSER_FUNCTION_HAS_REST_PARAM)
  {
    compiled_code_p->status_flags |= CBC_CODE_FLAGS_REST_PARAMETER;
  }
#endif /* !CONFIG_DISABLE_ES2015_FUNCTION_REST_PARAMETER */

  literal_pool_p = (literal_value_t *) byte_code_p;
  literal_pool_p -= context_p->register_count;
  byte_code_p += literal_length;

  dst_p = parser_generate_initializers (context_p,
                                        byte_code_p,
                                        literal_pool_p,
                                        uninitialized_var_end,
                                        initialized_var_end,
                                        literal_one_byte_limit);

  PARSER_ASSERT (dst_p == byte_code_p + initializers_length);

  page_p = context_p->byte_code.first_p;
  offset = 0;
  real_offset = 0;

  while (page_p != last_page_p || offset < last_position)
  {
    uint8_t flags;
    uint8_t *opcode_p;
    uint8_t *branch_mark_p;
    cbc_opcode_t opcode;
    size_t branch_offset_length;

    opcode_p = dst_p;
    branch_mark_p = page_p->bytes + offset;
    opcode = (cbc_opcode_t) (*branch_mark_p);
    branch_offset_length = CBC_BRANCH_OFFSET_LENGTH (opcode);

    if (opcode == CBC_JUMP_FORWARD)
    {
      /* These opcodes are deleted from the stream. */
#if PARSER_MAXIMUM_CODE_SIZE <= 65535
      size_t counter = 3;
#else /* PARSER_MAXIMUM_CODE_SIZE <= 65535 */
      size_t counter = 4;
#endif /* PARSER_MAXIMUM_CODE_SIZE <= 65535 */

      do
      {
        PARSER_NEXT_BYTE_UPDATE (page_p, offset, real_offset);
      }
      while (--counter > 0);

      continue;
    }

    /* Storing the opcode */
    *dst_p++ = opcode;
    real_offset++;
    PARSER_NEXT_BYTE_UPDATE (page_p, offset, real_offset);
    flags = cbc_flags[opcode];

    if (opcode == CBC_EXT_OPCODE)
    {
      cbc_ext_opcode_t ext_opcode;

      ext_opcode = (cbc_ext_opcode_t) page_p->bytes[offset];
      flags = cbc_ext_flags[ext_opcode];
      branch_offset_length = CBC_BRANCH_OFFSET_LENGTH (ext_opcode);

      /* Storing the extended opcode */
      *dst_p++ = ext_opcode;
      opcode_p++;
      real_offset++;
      PARSER_NEXT_BYTE_UPDATE (page_p, offset, real_offset);
    }

    if (flags & CBC_HAS_BRANCH_ARG)
    {
      *branch_mark_p |= CBC_HIGHEST_BIT_MASK;
    }

    /* Only literal and call arguments can be combined. */
    PARSER_ASSERT (!(flags & CBC_HAS_BRANCH_ARG)
                   || !(flags & (CBC_HAS_BYTE_ARG | CBC_HAS_LITERAL_ARG)));

    while (flags & (CBC_HAS_LITERAL_ARG | CBC_HAS_LITERAL_ARG2))
    {
      uint8_t first_byte = page_p->bytes[offset];

      *dst_p++ = first_byte;
      real_offset++;
      PARSER_NEXT_BYTE_UPDATE (page_p, offset, real_offset);

      if (first_byte > literal_one_byte_limit)
      {
        *dst_p++ = page_p->bytes[offset];
        real_offset++;
      }
      PARSER_NEXT_BYTE_UPDATE (page_p, offset, real_offset);

      if (flags & CBC_HAS_LITERAL_ARG2)
      {
        if (flags & CBC_HAS_LITERAL_ARG)
        {
          flags = CBC_HAS_LITERAL_ARG;
        }
        else
        {
          flags = CBC_HAS_LITERAL_ARG | CBC_HAS_LITERAL_ARG2;
        }
      }
      else
      {
        break;
      }
    }

    if (flags & CBC_HAS_BYTE_ARG)
    {
      /* This argument will be copied without modification. */
      *dst_p++ = page_p->bytes[offset];
      real_offset++;
      PARSER_NEXT_BYTE_UPDATE (page_p, offset, real_offset);
    }

    if (flags & CBC_HAS_BRANCH_ARG)
    {
      int prefix_zero = PARSER_TRUE;

      /* The leading zeroes are dropped from the stream. */
      PARSER_ASSERT (branch_offset_length > 0 && branch_offset_length <= 3);

      while (--branch_offset_length > 0)
      {
        uint8_t byte = page_p->bytes[offset];
        if (byte > 0 || !prefix_zero)
        {
          prefix_zero = PARSER_FALSE;
          *dst_p++ = page_p->bytes[offset];
          real_offset++;
        }
        else
        {
          /* When a leading zero is dropped, the branch
           * offset length must be decreased as well. */
          (*opcode_p)--;
        }
        PARSER_NEXT_BYTE_UPDATE (page_p, offset, real_offset);
      }

      *dst_p++ = page_p->bytes[offset];
      real_offset++;
      PARSER_NEXT_BYTE_UPDATE (page_p, offset, real_offset);
    }
  }

  if (!(context_p->status_flags & PARSER_NO_END_LABEL))
  {
    *dst_p++ = CBC_RETURN_WITH_BLOCK;
  }
  PARSER_ASSERT (dst_p == byte_code_p + length);

  parse_update_branches (context_p,
                         byte_code_p + initializers_length);

#ifdef PARSER_DUMP_BYTE_CODE
  if (context_p->is_show_opcodes)
  {
    parse_print_final_cbc (compiled_code_p, &context_p->literal_pool, length);
    printf ("\nByte code size: %d bytes\n", (int) length);
    context_p->total_byte_code_size += (uint32_t) length;
  }
#endif /* PARSER_DUMP_BYTE_CODE */

  if ((context_p->status_flags & PARSER_ARGUMENTS_NEEDED)
      && !(context_p->status_flags & PARSER_IS_STRICT))
  {
    parser_list_iterator_t literal_iterator;
    uint16_t argument_count = 0;
    uint16_t register_count = context_p->register_count;
    literal_value_t *argument_base_p = (literal_value_t *) (((uint8_t *) compiled_code_p) + total_size);
    argument_base_p -= context_p->argument_count;

    parser_list_iterator_init (&context_p->literal_pool, &literal_iterator);
    while (argument_count < context_p->argument_count)
    {
      lexer_literal_t *literal_p;
      literal_p = (lexer_literal_t *) parser_list_iterator_next (&literal_iterator);

      PARSER_ASSERT (literal_p != NULL);

      if (!(literal_p->status_flags & LEXER_FLAG_FUNCTION_ARGUMENT))
      {
        continue;
      }

      /* All arguments must be moved to initialized registers. */
      if (literal_p->type == LEXER_UNUSED_LITERAL)
      {
        if (literal_p->value.char_p == NULL)
        {
          argument_base_p[argument_count].char_p = NULL;
          argument_count++;
          continue;
        }

        literal_p = PARSER_GET_LITERAL (literal_p->prop.index);

        PARSER_ASSERT (literal_p != NULL);
      }

      PARSER_ASSERT (literal_p->type == LEXER_IDENT_LITERAL
                     && (literal_p->status_flags & LEXER_FLAG_VAR));

      PARSER_ASSERT (literal_p->prop.index >= register_count);

      argument_base_p[argument_count] = literal_pool_p[literal_p->prop.index];
      argument_count++;
    }
  }

  return compiled_code_p;
} /* parser_post_processing */

#undef PARSER_NEXT_BYTE
#undef PARSER_NEXT_BYTE_UPDATE

/**
 * Free identifiers and literals.
 */
static void
parser_free_literals (parser_list_t *literal_pool_p) /**< literals */
{
  parser_list_iterator_t literal_iterator;
  lexer_literal_t *literal_p;

  parser_list_iterator_init (literal_pool_p, &literal_iterator);
  while ((literal_p = (lexer_literal_t *) parser_list_iterator_next (&literal_iterator)) != NULL)
  {
    util_free_literal (literal_p);
  }

  parser_list_free (literal_pool_p);
} /* parser_free_literals */

/**
 * Parse function arguments
 */
static void
parser_parse_function_arguments (parser_context_t *context_p, /**< context */
                                 lexer_token_type_t end_type) /**< expected end type */
{
#ifndef CONFIG_DISABLE_ES2015_FUNCTION_PARAMETER_INITIALIZER
  int duplicated_argument_names = PARSER_FALSE;
  int initializer_found = PARSER_FALSE;
#endif /* !CONFIG_DISABLE_ES2015_FUNCTION_PARAMETER_INITIALIZER */

  if (context_p->token.type == end_type)
  {
    return;
  }

  while (PARSER_TRUE)
  {
    uint16_t literal_count = context_p->literal_count;

#ifndef CONFIG_DISABLE_ES2015_FUNCTION_REST_PARAMETER
    if (context_p->status_flags & PARSER_FUNCTION_HAS_REST_PARAM)
    {
      parser_raise_error (context_p, PARSER_ERR_FORMAL_PARAM_AFTER_REST_PARAMETER);
    }
    else if (context_p->token.type == LEXER_THREE_DOTS)
    {
      lexer_expect_identifier (context_p, LEXER_IDENT_LITERAL);
      context_p->status_flags |= PARSER_FUNCTION_HAS_REST_PARAM;
    }
#endif /* !CONFIG_DISABLE_ES2015_FUNCTION_REST_PARAMETER */

    if (context_p->token.type != LEXER_LITERAL
        || context_p->token.lit_location.type != LEXER_IDENT_LITERAL)
    {
      parser_raise_error (context_p, PARSER_ERR_IDENTIFIER_EXPECTED);
    }

    lexer_construct_literal_object (context_p,
                                    &context_p->token.lit_location,
                                    LEXER_IDENT_LITERAL);

    if (literal_count == context_p->literal_count
        || context_p->token.literal_is_reserved
        || context_p->lit_object.type != LEXER_LITERAL_OBJECT_ANY)
    {
      context_p->status_flags |= PARSER_HAS_NON_STRICT_ARG;
    }

    if (context_p->lit_object.type == LEXER_LITERAL_OBJECT_ARGUMENTS)
    {
      uint8_t literal_status_flags = context_p->lit_object.literal_p->status_flags;

      literal_status_flags = (uint8_t) (literal_status_flags & ~LEXER_FLAG_NO_REG_STORE);
      context_p->lit_object.literal_p->status_flags = literal_status_flags;

      context_p->status_flags |= PARSER_ARGUMENTS_NOT_NEEDED;
      context_p->status_flags &= (uint32_t) ~(PARSER_LEXICAL_ENV_NEEDED | PARSER_ARGUMENTS_NEEDED);
    }

    if (context_p->literal_count == literal_count)
    {
      lexer_literal_t *literal_p;

#ifndef CONFIG_DISABLE_ES2015_FUNCTION_PARAMETER_INITIALIZER
      if (initializer_found)
      {
        parser_raise_error (context_p, PARSER_ERR_DUPLICATED_ARGUMENT_NAMES);
      }
      duplicated_argument_names = PARSER_TRUE;
#endif /* !CONFIG_DISABLE_ES2015_FUNCTION_PARAMETER_INITIALIZER */

      if (context_p->literal_count >= PARSER_MAXIMUM_NUMBER_OF_LITERALS)
      {
        parser_raise_error (context_p, PARSER_ERR_LITERAL_LIMIT_REACHED);
      }

      literal_p = (lexer_literal_t *) parser_list_append (context_p, &context_p->literal_pool);
      *literal_p = *context_p->lit_object.literal_p;

      literal_p->status_flags &= LEXER_FLAG_SOURCE_PTR;
      literal_p->status_flags |= LEXER_FLAG_VAR | LEXER_FLAG_INITIALIZED | LEXER_FLAG_FUNCTION_ARGUMENT;

      context_p->literal_count++;

      /* There cannot be references from the byte code to these literals
       * since no byte code has been emitted yet. Therefore there is no
       * need to set the index field. */
      context_p->lit_object.literal_p->type = LEXER_UNUSED_LITERAL;

      /* Only the LEXER_FLAG_FUNCTION_ARGUMENT flag is kept. */
      context_p->lit_object.literal_p->status_flags &= LEXER_FLAG_FUNCTION_ARGUMENT;
      context_p->lit_object.literal_p->value.char_p = NULL;
    }
    else
    {
      uint8_t lexer_flags = LEXER_FLAG_VAR | LEXER_FLAG_INITIALIZED | LEXER_FLAG_FUNCTION_ARGUMENT;
      context_p->lit_object.literal_p->status_flags |= lexer_flags;
    }

    context_p->argument_count++;
    if (context_p->argument_count >= PARSER_MAXIMUM_NUMBER_OF_REGISTERS)
    {
      parser_raise_error (context_p, PARSER_ERR_REGISTER_LIMIT_REACHED);
    }

    lexer_next_token (context_p);

#ifndef CONFIG_DISABLE_ES2015_FUNCTION_PARAMETER_INITIALIZER
    if (context_p->token.type == LEXER_ASSIGN)
    {
      parser_branch_t skip_init;

#ifndef CONFIG_DISABLE_ES2015_FUNCTION_REST_PARAMETER
      if (context_p->status_flags & PARSER_FUNCTION_HAS_REST_PARAM)
      {
        parser_raise_error (context_p, PARSER_ERR_REST_PARAMETER_DEFAULT_INITIALIZER);
      }
#endif /* !CONFIG_DISABLE_ES2015_FUNCTION_REST_PARAMETER */

      if (duplicated_argument_names)
      {
        parser_raise_error (context_p, PARSER_ERR_DUPLICATED_ARGUMENT_NAMES);
      }
      initializer_found = PARSER_TRUE;

     /* LEXER_ASSIGN does not overwrite lit_object. */
      parser_emit_cbc (context_p, CBC_PUSH_UNDEFINED);
      parser_emit_cbc_literal (context_p, CBC_STRICT_EQUAL_RIGHT_LITERAL, context_p->lit_object.index);
      parser_emit_cbc_forward_branch (context_p, CBC_BRANCH_IF_FALSE_FORWARD, &skip_init);

      parser_emit_cbc_literal_from_token (context_p, CBC_PUSH_LITERAL);
      parser_parse_expression (context_p, PARSE_EXPR_STATEMENT | PARSE_EXPR_NO_COMMA | PARSE_EXPR_HAS_LITERAL);

      parser_set_branch_to_current_position (context_p, &skip_init);
    }
#endif /* !CONFIG_DISABLE_ES2015_FUNCTION_PARAMETER_INITIALIZER */

    if (context_p->token.type != LEXER_COMMA)
    {
      break;
    }

    lexer_next_token (context_p);
  }

  if (context_p->token.type != end_type)
  {
    parser_error_t error = ((end_type == LEXER_RIGHT_PAREN) ? PARSER_ERR_RIGHT_PAREN_EXPECTED
                                                            : PARSER_ERR_IDENTIFIER_EXPECTED);

    parser_raise_error (context_p, error);
  }

  context_p->register_count = context_p->argument_count;
} /* parser_parse_function_arguments */

/**
 * Parse and compile EcmaScript source code
 *
 * Note:
 *      if arg_list_p is not NULL, a function body is parsed
 *
 * @return compiled code
 */
cbc_compiled_code_t *
parser_parse_source (const uint8_t *arg_list_p, /**< function argument list */
                     size_t arg_list_size, /**< size of function argument list */
                     const uint8_t *source_p, /**< valid UTF-8 source code */
                     size_t source_size, /**< size of the source code */
                     parser_error_location_t *error_location_p) /**< error location */
{
  parser_context_t context;
  cbc_compiled_code_t *compiled_code;

  context.error = PARSER_ERR_NO_ERROR;
  context.allocated_buffer_p = NULL;

  if (error_location_p != NULL)
  {
    error_location_p->error = PARSER_ERR_NO_ERROR;
  }

  if (arg_list_p == NULL)
  {
    context.status_flags = PARSER_NO_REG_STORE | PARSER_LEXICAL_ENV_NEEDED | PARSER_ARGUMENTS_NOT_NEEDED;
    context.source_p = source_p;
    context.source_end_p = source_p + source_size;
  }
  else
  {
    context.status_flags = PARSER_IS_FUNCTION;
    context.source_p = arg_list_p;
    context.source_end_p = arg_list_p + arg_list_size;
  }

  context.stack_depth = 0;
  context.stack_limit = 0;
  context.last_context_p = NULL;
  context.last_statement.current_p = NULL;

  context.token.flags = 0;
  context.line = 1;
  context.column = 1;

  context.last_cbc_opcode = PARSER_CBC_UNAVAILABLE;

  context.argument_count = 0;
  context.register_count = 0;
  context.literal_count = 0;

  parser_cbc_stream_init (&context.byte_code);
  context.byte_code_size = 0;
  parser_list_init (&context.literal_pool, sizeof (lexer_literal_t), 15);
  parser_stack_init (&context);

#ifdef PARSER_DEBUG
  context.context_stack_depth = 0;
#endif /* PARSER_DEBUG */

#ifdef PARSER_DUMP_BYTE_CODE
  context.is_show_opcodes = PARSER_TRUE;
  context.total_byte_code_size = 0;

  if (context.is_show_opcodes)
  {
    printf ("\n--- %s parsing start ---\n\n",
            (arg_list_p == NULL) ? "Script"
                                 : "Function");
  }
#endif /* PARSER_DUMP_BYTE_CODE */

  PARSER_TRY (context.try_buffer)
  {
    /* Pushing a dummy value ensures the stack is never empty.
     * This simplifies the stack management routines. */
    parser_stack_push_uint8 (&context, CBC_MAXIMUM_BYTE_VALUE);
    /* The next token must always be present to make decisions
     * in the parser. Therefore when a token is consumed, the
     * lexer_next_token() must be immediately called. */
    lexer_next_token (&context);

    if (arg_list_p != NULL)
    {
      parser_parse_function_arguments (&context, LEXER_EOS);

      context.source_p = source_p;
      context.source_end_p = source_p + source_size;
      context.line = 1;
      context.column = 1;

      lexer_next_token (&context);
    }

    parser_parse_statements (&context);

    /* When the parsing is successful, only the
     * dummy value can be remained on the stack. */
    PARSER_ASSERT (context.stack_top_uint8 == CBC_MAXIMUM_BYTE_VALUE
                   && context.stack.last_position == 1
                   && context.stack.first_p != NULL
                   && context.stack.first_p->next_p == NULL
                   && context.stack.last_p == NULL);
    PARSER_ASSERT (context.last_statement.current_p == NULL);

    PARSER_ASSERT (context.last_cbc_opcode == PARSER_CBC_UNAVAILABLE);
    PARSER_ASSERT (context.allocated_buffer_p == NULL);

    compiled_code = parser_post_processing (&context);

#ifdef PARSER_DUMP_BYTE_CODE
    if (context.is_show_opcodes)
    {
      printf ("\n%s parsing successfully completed. Total byte code size: %d bytes\n",
              (arg_list_p == NULL) ? "Script"
                                   : "Function",
              (int) context.total_byte_code_size);
    }
#endif /* PARSER_DUMP_BYTE_CODE */
  }
  PARSER_CATCH
  {
    if (context.last_statement.current_p != NULL)
    {
      parser_free_jumps (context.last_statement);
    }

    if (context.allocated_buffer_p != NULL)
    {
      parser_free_local (context.allocated_buffer_p);
    }

    if (error_location_p != NULL)
    {
      error_location_p->error = context.error;
      error_location_p->line = context.token.line;
      error_location_p->column = context.token.column;
    }

    compiled_code = NULL;
  }
  PARSER_TRY_END

#ifdef PARSER_DUMP_BYTE_CODE
  if (context.is_show_opcodes)
  {
    printf ("\n--- %s parsing end ---\n\n",
            (arg_list_p == NULL) ? "Script"
                                 : "Function");
  }
#endif /* PARSER_DUMP_BYTE_CODE */

  parser_cbc_stream_free (&context.byte_code);
  parser_free_literals (&context.literal_pool);
  parser_stack_free (&context);

  return compiled_code;
} /* parser_parse_script */

/**
 * Save parser context before function parsing.
 */
static void
parser_save_context (parser_context_t *context_p, /**< context */
                     parser_saved_context_t *saved_context_p) /**< target for saving the context */
{
  PARSER_ASSERT (context_p->last_cbc_opcode == PARSER_CBC_UNAVAILABLE);

  /* Save private part of the context. */

  saved_context_p->status_flags = context_p->status_flags;
  saved_context_p->stack_depth = context_p->stack_depth;
  saved_context_p->stack_limit = context_p->stack_limit;
  saved_context_p->prev_context_p = context_p->last_context_p;
  saved_context_p->last_statement = context_p->last_statement;

  saved_context_p->argument_count = context_p->argument_count;
  saved_context_p->register_count = context_p->register_count;
  saved_context_p->literal_count = context_p->literal_count;

  saved_context_p->byte_code = context_p->byte_code;
  saved_context_p->byte_code_size = context_p->byte_code_size;
  saved_context_p->literal_pool_data = context_p->literal_pool.data;

#ifndef JERRY_NDEBUG
  saved_context_p->context_stack_depth = context_p->context_stack_depth;
#endif /* !JERRY_NDEBUG */

  /* Reset private part of the context. */

  context_p->status_flags &= PARSER_IS_STRICT;
  context_p->stack_depth = 0;
  context_p->stack_limit = 0;
  context_p->last_context_p = saved_context_p;
  context_p->last_statement.current_p = NULL;

  context_p->argument_count = 0;
  context_p->register_count = 0;
  context_p->literal_count = 0;

  parser_cbc_stream_init (&context_p->byte_code);
  context_p->byte_code_size = 0;
  parser_list_reset (&context_p->literal_pool);

#ifndef JERRY_NDEBUG
  context_p->context_stack_depth = 0;
#endif /* !JERRY_NDEBUG */
} /* parser_save_context */

/**
 * Restore parser context after function parsing.
 */
static void
parser_restore_context (parser_context_t *context_p, /**< context */
                        parser_saved_context_t *saved_context_p) /**< target for saving the context */
{
  parser_cbc_stream_free (&context_p->byte_code);
  parser_free_literals (&context_p->literal_pool);

  /* Restore private part of the context. */

  PARSER_ASSERT (context_p->last_cbc_opcode == PARSER_CBC_UNAVAILABLE);

  context_p->status_flags = saved_context_p->status_flags;
  context_p->stack_depth = saved_context_p->stack_depth;
  context_p->stack_limit = saved_context_p->stack_limit;
  context_p->last_context_p = saved_context_p->prev_context_p;
  context_p->last_statement = saved_context_p->last_statement;

  context_p->argument_count = saved_context_p->argument_count;
  context_p->register_count = saved_context_p->register_count;
  context_p->literal_count = saved_context_p->literal_count;

  context_p->byte_code = saved_context_p->byte_code;
  context_p->byte_code_size = saved_context_p->byte_code_size;
  context_p->literal_pool.data = saved_context_p->literal_pool_data;

#ifndef JERRY_NDEBUG
  context_p->context_stack_depth = saved_context_p->context_stack_depth;
#endif /* !JERRY_NDEBUG */
} /* parser_restore_context */

/**
 * Parse function code
 *
 * @return compiled code
 */
cbc_compiled_code_t *
parser_parse_function (parser_context_t *context_p, /**< context */
                       uint32_t status_flags) /**< extra status flags */
{
  parser_saved_context_t saved_context;
  cbc_compiled_code_t *compiled_code_p;

  PARSER_ASSERT (status_flags & PARSER_IS_FUNCTION);
  parser_save_context (context_p, &saved_context);
  context_p->status_flags |= status_flags;

#ifdef PARSER_DUMP_BYTE_CODE
  if (context_p->is_show_opcodes)
  {
#ifndef CONFIG_DISABLE_ES2015_CLASS
    int is_constructor = context_p->status_flags & PARSER_CLASS_CONSTRUCTOR;
    printf (is_constructor ? "\n--- Class constructor parsing start ---\n\n"
                           : "\n--- Function parsing start ---\n\n");
#else /* CONFIG_DISABLE_ES2015_CLASS */
    printf ("\n--- Function parsing start ---\n\n");
#endif /* !CONFIG_DISABLE_ES2015_CLASS */
  }
#endif /* PARSER_DUMP_BYTE_CODE */

  lexer_next_token (context_p);

  if (context_p->token.type != LEXER_LEFT_PAREN)
  {
    parser_raise_error (context_p, PARSER_ERR_ARGUMENT_LIST_EXPECTED);
  }

  lexer_next_token (context_p);

  parser_parse_function_arguments (context_p, LEXER_RIGHT_PAREN);
  lexer_next_token (context_p);

  if ((context_p->status_flags & PARSER_IS_PROPERTY_GETTER)
      && context_p->argument_count != 0)
  {
    parser_raise_error (context_p, PARSER_ERR_NO_ARGUMENTS_EXPECTED);
  }

  if ((context_p->status_flags & PARSER_IS_PROPERTY_SETTER)
      && context_p->argument_count != 1)
  {
    parser_raise_error (context_p, PARSER_ERR_ONE_ARGUMENT_EXPECTED);
  }

#ifdef PARSER_DUMP_BYTE_CODE
  if (context_p->is_show_opcodes
      && (context_p->status_flags & PARSER_HAS_NON_STRICT_ARG))
  {
    printf ("  Note: legacy (non-strict) argument definition\n\n");
  }
#endif /* PARSER_DUMP_BYTE_CODE */

  if (context_p->token.type != LEXER_LEFT_BRACE)
  {
    parser_raise_error (context_p, PARSER_ERR_LEFT_BRACE_EXPECTED);
  }

  lexer_next_token (context_p);

#ifndef CONFIG_DISABLE_ES2015_CLASS
  if ((context_p->status_flags & PARSER_CLASS_CONSTRUCTOR_SUPER) == PARSER_CLASS_CONSTRUCTOR_SUPER)
  {
    context_p->status_flags |= PARSER_LEXICAL_ENV_NEEDED;
  }
#endif /* !CONFIG_DISABLE_ES2015_CLASS */

  parser_parse_statements (context_p);
  compiled_code_p = parser_post_processing (context_p);

#ifdef PARSER_DUMP_BYTE_CODE
  if (context_p->is_show_opcodes)
  {
#ifndef CONFIG_DISABLE_ES2015_CLASS
    int is_constructor = context_p->status_flags & PARSER_CLASS_CONSTRUCTOR;
    printf (is_constructor ? "\n--- Class constructor parsing end ---\n\n"
                           : "\n--- Function parsing end ---\n\n");
#else /* CONFIG_DISABLE_ES2015_CLASS */
    printf ("\n--- Function parsing end ---\n\n");
#endif /* !CONFIG_DISABLE_ES2015_CLASS */
  }
#endif /* PARSER_DUMP_BYTE_CODE */

  parser_restore_context (context_p, &saved_context);

  return compiled_code_p;
} /* parser_parse_function */

#ifndef CONFIG_DISABLE_ES2015_ARROW_FUNCTION

/**
 * Parse arrow function code
 *
 * @return compiled code
 */
cbc_compiled_code_t *
parser_parse_arrow_function (parser_context_t *context_p, /**< context */
                             uint32_t status_flags) /**< extra status flags */
{
  parser_saved_context_t saved_context;
  cbc_compiled_code_t *compiled_code_p;

  PARSER_ASSERT ((status_flags & PARSER_IS_FUNCTION)
                 && (status_flags & PARSER_IS_ARROW_FUNCTION));
  parser_save_context (context_p, &saved_context);
  context_p->status_flags |= status_flags | PARSER_ARGUMENTS_NOT_NEEDED;
#ifndef CONFIG_DISABLE_ES2015_CLASS
  context_p->status_flags |= saved_context.status_flags & PARSER_CLASS_HAS_SUPER;
#endif /* !CONFIG_DISABLE_ES2015_CLASS */

#ifdef PARSER_DUMP_BYTE_CODE
  if (context_p->is_show_opcodes)
  {
    printf ("\n--- Arrow function parsing start ---\n\n");
  }
#endif /* PARSER_DUMP_BYTE_CODE */

#ifdef JERRY_DEBUGGER
  if ((JERRY_CONTEXT (debugger_flags) & JERRY_DEBUGGER_CONNECTED)
      && jerry_debugger_send_parse_function (context_p->token.line, context_p->token.column))
  {
    /* This option has a high memory and performance costs,
     * but it is necessary for executing eval operations by the debugger. */
    context_p->status_flags |= PARSER_LEXICAL_ENV_NEEDED | PARSER_NO_REG_STORE;
  }
#endif /* JERRY_DEBUGGER */

  if (status_flags & PARSER_ARROW_PARSE_ARGS)
  {
    parser_parse_function_arguments (context_p, LEXER_RIGHT_PAREN);
  }
  else
  {
    PARSER_ASSERT (context_p->token.type == LEXER_LITERAL
                   && context_p->token.lit_location.type == LEXER_IDENT_LITERAL);

    lexer_construct_literal_object (context_p,
                                    &context_p->token.lit_location,
                                    LEXER_IDENT_LITERAL);

    PARSER_ASSERT (context_p->argument_count == 0 && context_p->literal_count == 1);

    if (context_p->token.literal_is_reserved
        || context_p->lit_object.type != LEXER_LITERAL_OBJECT_ANY)
    {
      context_p->status_flags |= PARSER_HAS_NON_STRICT_ARG;
    }

    uint8_t lexer_flags = LEXER_FLAG_VAR | LEXER_FLAG_INITIALIZED | LEXER_FLAG_FUNCTION_ARGUMENT;
    context_p->lit_object.literal_p->status_flags |= lexer_flags;

    context_p->argument_count = 1;
    context_p->register_count = 1;
  }

  lexer_next_token (context_p);
  PARSER_ASSERT (context_p->token.type == LEXER_ARROW);

  lexer_next_token (context_p);

  if (context_p->token.type == LEXER_LEFT_BRACE)
  {
    lexer_next_token (context_p);

    context_p->status_flags |= PARSER_IS_CLOSURE;
    parser_parse_statements (context_p);

    /* Unlike normal function, arrow functions consume their close brace. */
    PARSER_ASSERT (context_p->token.type == LEXER_RIGHT_BRACE);
    lexer_next_token (context_p);
  }
  else
  {
    if (context_p->status_flags & PARSER_IS_STRICT
        && context_p->status_flags & PARSER_HAS_NON_STRICT_ARG)
    {
      parser_raise_error (context_p, PARSER_ERR_NON_STRICT_ARG_DEFINITION);
    }

    parser_parse_expression (context_p, PARSE_EXPR_NO_COMMA);

    if (context_p->last_cbc_opcode == CBC_PUSH_LITERAL)
    {
      context_p->last_cbc_opcode = CBC_RETURN_WITH_LITERAL;
    }
    else
    {
      parser_emit_cbc (context_p, CBC_RETURN);
    }
    parser_flush_cbc (context_p);
  }

  compiled_code_p = parser_post_processing (context_p);

#ifdef PARSER_DUMP_BYTE_CODE
  if (context_p->is_show_opcodes)
  {
    printf ("\n--- Arrow function parsing end ---\n\n");
  }
#endif /* PARSER_DUMP_BYTE_CODE */

  parser_restore_context (context_p, &saved_context);

  return compiled_code_p;
} /* parser_parse_arrow_function */

#endif /* !CONFIG_DISABLE_ES2015_ARROW_FUNCTION */

/**
 * Raise a parse error
 */
void
parser_raise_error (parser_context_t *context_p, /**< context */
                    parser_error_t error) /**< error code */
{
  parser_saved_context_t *saved_context_p = context_p->last_context_p;

  while (saved_context_p != NULL)
  {
    parser_cbc_stream_free (&saved_context_p->byte_code);

    /* First the current literal pool is freed, and then it is replaced
     * by the literal pool coming from the saved context. Since literals
     * are not used anymore, this is a valid replacement. The last pool
     * is freed by parser_parse_script. */

    parser_free_literals (&context_p->literal_pool);
    context_p->literal_pool.data = saved_context_p->literal_pool_data;

    if (saved_context_p->last_statement.current_p != NULL)
    {
      parser_free_jumps (saved_context_p->last_statement);
    }

    saved_context_p = saved_context_p->prev_context_p;
  }

  context_p->error = error;
  PARSER_THROW (context_p->try_buffer);
  /* Should never been reached. */
  PARSER_ASSERT (0);
} /* parser_raise_error */
