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

#define IS_UTF8_INTERMEDIATE_OCTET(byte) (((byte) & 0xc0) == 0x80)

/**
 * Align column to the next tab position.
 *
 * @return aligned position
 */
static parser_line_counter_t
align_column_to_tab (parser_line_counter_t column) /**< current column */
{
  /* Tab aligns to zero column start position. */
  return (parser_line_counter_t) (((column + (8u - 1u)) & ~0x7u) + 1u);
} /* align_column_to_tab */

/**
 * Parse hexadecimal character sequence
 *
 * @return character value
 */
static lexer_character_type_t
lexer_hex_to_character (parser_context_t *context_p, /**< context */
                        const uint8_t *source_p, /**< current source position */
                        int length)
{
  uint32_t result = 0;

  do
  {
    uint32_t byte = *source_p++;

    result <<= 4;

    if (byte >= '0' && byte <= '9')
    {
      result += byte - '0';
    }
    else
    {
      byte |= 0x20;
      if (byte >= 'a' && byte <= 'f')
      {
        result += byte - ('a' - 10);
      }
      else
      {
        parser_raise_error (context_p, PARSER_ERR_INVALID_ESCAPE_SEQUENCE);
      }
    }
  }
  while (--length > 0);

  return (lexer_character_type_t) result;
} /* lexer_hex_to_character */

/**
 * Skip space mode
 */
typedef enum
{
  LEXER_SKIP_SPACES,                 /**< skip spaces mode */
  LEXER_SKIP_SINGLE_LINE_COMMENT,    /**< parse single line comment */
  LEXER_SKIP_MULTI_LINE_COMMENT,     /**< parse multi line comment */
} skip_mode_t;

/**
 * Skip spaces.
 */
static void
lexer_skip_spaces (parser_context_t *context_p) /**< context */
{
  skip_mode_t mode = LEXER_SKIP_SPACES;
  const uint8_t *source_end_p = context_p->source_end_p;

  if (context_p->token.flags & LEXER_NO_SKIP_SPACES)
  {
    context_p->token.flags = (uint8_t) (context_p->token.flags & ~LEXER_NO_SKIP_SPACES);
    return;
  }

  context_p->token.flags = 0;

  while (PARSER_TRUE)
  {
    if (context_p->source_p >= source_end_p)
    {
      if (mode == LEXER_SKIP_MULTI_LINE_COMMENT)
      {
        parser_raise_error (context_p, PARSER_ERR_UNTERMINATED_MULTILINE_COMMENT);
      }
      return;
    }

    switch (context_p->source_p[0])
    {
      case LEXER_NEWLINE_CR:
      {
        if (context_p->source_p + 1 < source_end_p
            && context_p->source_p[1] == LEXER_NEWLINE_LF)
        {
          context_p->source_p++;
        }
        /* FALLTHRU */
      }

      case LEXER_NEWLINE_LF:
      {
        context_p->line++;
        context_p->column = 0;
        context_p->token.flags = LEXER_WAS_NEWLINE;

        if (mode == LEXER_SKIP_SINGLE_LINE_COMMENT)
        {
          mode = LEXER_SKIP_SPACES;
        }
        /* FALLTHRU */
      }

      case 0x0b:
      case 0x0c:
      case 0x20:
      {
        context_p->source_p++;
        context_p->column++;
        continue;
        /* FALLTHRU */
      }

      case LEXER_TAB:
      {
        context_p->column = align_column_to_tab (context_p->column);
        context_p->source_p++;
        continue;
        /* FALLTHRU */
      }

      case '/':
      {
        if (mode == LEXER_SKIP_SPACES
            && context_p->source_p + 1 < source_end_p)
        {
          if (context_p->source_p[1] == '/')
          {
            mode = LEXER_SKIP_SINGLE_LINE_COMMENT;
          }
          else if (context_p->source_p[1] == '*')
          {
            mode = LEXER_SKIP_MULTI_LINE_COMMENT;
            context_p->token.line = context_p->line;
            context_p->token.column = context_p->column;
          }

          if (mode != LEXER_SKIP_SPACES)
          {
            context_p->source_p += 2;
            PARSER_PLUS_EQUAL_LC (context_p->column, 2);
            continue;
          }
        }
        break;
      }

      case '*':
      {
        if (mode == LEXER_SKIP_MULTI_LINE_COMMENT
            && context_p->source_p + 1 < source_end_p
            && context_p->source_p[1] == '/')
        {
          mode = LEXER_SKIP_SPACES;
          context_p->source_p += 2;
          PARSER_PLUS_EQUAL_LC (context_p->column, 2);
          continue;
        }
        break;
      }

      case 0xc2:
      {
        if (context_p->source_p + 1 < source_end_p
            && context_p->source_p[1] == 0xa0)
        {
          /* Codepoint \u00A0 */
          context_p->source_p += 2;
          context_p->column++;
          continue;
        }
        break;
      }

      case LEXER_NEWLINE_LS_PS_BYTE_1:
      {
        PARSER_ASSERT (context_p->source_p + 2 < source_end_p);
        if (LEXER_NEWLINE_LS_PS_BYTE_23 (context_p->source_p))
        {
          /* Codepoint \u2028 and \u2029 */
          context_p->source_p += 3;
          context_p->line++;
          context_p->column = 1;
          context_p->token.flags = LEXER_WAS_NEWLINE;

          if (mode == LEXER_SKIP_SINGLE_LINE_COMMENT)
          {
            mode = LEXER_SKIP_SPACES;
          }
          continue;
        }
        break;
      }

      case 0xef:
      {
        if (context_p->source_p + 2 < source_end_p
            && context_p->source_p[1] == 0xbb
            && context_p->source_p[2] == 0xbf)
        {
          /* Codepoint \uFEFF */
          context_p->source_p += 3;
          context_p->column++;
          continue;
        }
        break;
      }

      default:
      {
        break;
      }
    }

    if (mode == LEXER_SKIP_SPACES)
    {
      return;
    }

    context_p->source_p++;

    if (context_p->source_p < source_end_p
        && IS_UTF8_INTERMEDIATE_OCTET (context_p->source_p[0]))
    {
      context_p->column++;
    }
  }
} /* lexer_skip_spaces */

#ifndef CONFIG_DISABLE_ES2015_CLASS
/**
 * Skip all the continuous empty statements.
 */
void
lexer_skip_empty_statements (parser_context_t *context_p) /**< context */
{
  lexer_skip_spaces (context_p);

  while (context_p->source_p < context_p->source_end_p
         && *context_p->source_p == ';')
  {
    context_p->source_p++;
    lexer_skip_spaces (context_p);
  }
} /* lexer_skip_empty_statements */
#endif /* !CONFIG_DISABLE_ES2015_CLASS */

/**
 * Keyword data.
 */
typedef struct
{
  const uint8_t *keyword_p;     /**< keyword string */
  lexer_token_type_t type;      /**< keyword token type */
} keyword_string_t;

#define LEXER_KEYWORD(name, type) { (const uint8_t *) (name), (type) }
#define LEXER_KEYWORD_LIST_LENGTH(name) (const uint8_t) (sizeof (name) / sizeof ((name)[0]))

/**
 * Keywords with 2 characters.
 */
static const keyword_string_t keywords_with_length_2[] =
{
  LEXER_KEYWORD ("do", LEXER_KEYW_DO),
  LEXER_KEYWORD ("if", LEXER_KEYW_IF),
  LEXER_KEYWORD ("in", LEXER_KEYW_IN)
};

/**
 * Keywords with 3 characters.
 */
static const keyword_string_t keywords_with_length_3[] =
{
  LEXER_KEYWORD ("for", LEXER_KEYW_FOR),
  LEXER_KEYWORD ("let", LEXER_KEYW_LET),
  LEXER_KEYWORD ("new", LEXER_KEYW_NEW),
  LEXER_KEYWORD ("try", LEXER_KEYW_TRY),
  LEXER_KEYWORD ("var", LEXER_KEYW_VAR)
};

/**
 * Keywords with 4 characters.
 */
static const keyword_string_t keywords_with_length_4[] =
{
  LEXER_KEYWORD ("case", LEXER_KEYW_CASE),
  LEXER_KEYWORD ("else", LEXER_KEYW_ELSE),
  LEXER_KEYWORD ("enum", LEXER_KEYW_ENUM),
  LEXER_KEYWORD ("null", LEXER_LIT_NULL),
  LEXER_KEYWORD ("this", LEXER_KEYW_THIS),
  LEXER_KEYWORD ("true", LEXER_LIT_TRUE),
  LEXER_KEYWORD ("void", LEXER_KEYW_VOID),
  LEXER_KEYWORD ("with", LEXER_KEYW_WITH)
};

/**
 * Keywords with 5 characters.
 */
static const keyword_string_t keywords_with_length_5[] =
{
#ifndef CONFIG_DISABLE_ES2015
  LEXER_KEYWORD ("await", LEXER_KEYW_AWAIT),
#endif /* !CONFIG_DISABLE_ES2015 */
  LEXER_KEYWORD ("break", LEXER_KEYW_BREAK),
  LEXER_KEYWORD ("catch", LEXER_KEYW_CATCH),
  LEXER_KEYWORD ("class", LEXER_KEYW_CLASS),
  LEXER_KEYWORD ("const", LEXER_KEYW_CONST),
  LEXER_KEYWORD ("false", LEXER_LIT_FALSE),
  LEXER_KEYWORD ("super", LEXER_KEYW_SUPER),
  LEXER_KEYWORD ("throw", LEXER_KEYW_THROW),
  LEXER_KEYWORD ("while", LEXER_KEYW_WHILE),
  LEXER_KEYWORD ("yield", LEXER_KEYW_YIELD)
};

/**
 * Keywords with 6 characters.
 */
static const keyword_string_t keywords_with_length_6[] =
{
  LEXER_KEYWORD ("delete", LEXER_KEYW_DELETE),
  LEXER_KEYWORD ("export", LEXER_KEYW_EXPORT),
  LEXER_KEYWORD ("import", LEXER_KEYW_IMPORT),
  LEXER_KEYWORD ("public", LEXER_KEYW_PUBLIC),
  LEXER_KEYWORD ("return", LEXER_KEYW_RETURN),
  LEXER_KEYWORD ("static", LEXER_KEYW_STATIC),
  LEXER_KEYWORD ("switch", LEXER_KEYW_SWITCH),
  LEXER_KEYWORD ("typeof", LEXER_KEYW_TYPEOF)
};

/**
 * Keywords with 7 characters.
 */
static const keyword_string_t keywords_with_length_7[] =
{
  LEXER_KEYWORD ("default", LEXER_KEYW_DEFAULT),
  LEXER_KEYWORD ("extends", LEXER_KEYW_EXTENDS),
  LEXER_KEYWORD ("finally", LEXER_KEYW_FINALLY),
  LEXER_KEYWORD ("package", LEXER_KEYW_PACKAGE),
  LEXER_KEYWORD ("private", LEXER_KEYW_PRIVATE)
};

/**
 * Keywords with 8 characters.
 */
static const keyword_string_t keywords_with_length_8[] =
{
  LEXER_KEYWORD ("continue", LEXER_KEYW_CONTINUE),
  LEXER_KEYWORD ("debugger", LEXER_KEYW_DEBUGGER),
  LEXER_KEYWORD ("function", LEXER_KEYW_FUNCTION)
};

/**
 * Keywords with 9 characters.
 */
static const keyword_string_t keywords_with_length_9[] =
{
  LEXER_KEYWORD ("interface", LEXER_KEYW_INTERFACE),
  LEXER_KEYWORD ("protected", LEXER_KEYW_PROTECTED)
};

/**
 * Keywords with 10 characters.
 */
static const keyword_string_t keywords_with_length_10[] =
{
  LEXER_KEYWORD ("implements", LEXER_KEYW_IMPLEMENTS),
  LEXER_KEYWORD ("instanceof", LEXER_KEYW_INSTANCEOF)
};

/**
 * List of the keyword groups.
 */
static const keyword_string_t * const keyword_strings_list[] =
{
  keywords_with_length_2,
  keywords_with_length_3,
  keywords_with_length_4,
  keywords_with_length_5,
  keywords_with_length_6,
  keywords_with_length_7,
  keywords_with_length_8,
  keywords_with_length_9,
  keywords_with_length_10
};

/**
 * List of the keyword groups length.
 */
static const uint8_t keyword_lengths_list[] =
{
  LEXER_KEYWORD_LIST_LENGTH (keywords_with_length_2),
  LEXER_KEYWORD_LIST_LENGTH (keywords_with_length_3),
  LEXER_KEYWORD_LIST_LENGTH (keywords_with_length_4),
  LEXER_KEYWORD_LIST_LENGTH (keywords_with_length_5),
  LEXER_KEYWORD_LIST_LENGTH (keywords_with_length_6),
  LEXER_KEYWORD_LIST_LENGTH (keywords_with_length_7),
  LEXER_KEYWORD_LIST_LENGTH (keywords_with_length_8),
  LEXER_KEYWORD_LIST_LENGTH (keywords_with_length_9),
  LEXER_KEYWORD_LIST_LENGTH (keywords_with_length_10)
};

#undef LEXER_KEYWORD
#undef LEXER_KEYWORD_LIST_LENGTH

/**
 * Parse identifier.
 */
static void
lexer_parse_identifier (parser_context_t *context_p, /**< context */
                        int check_keywords) /**< check keywords */
{
  /* Only very few identifiers contains \u escape sequences. */
  const uint8_t *source_p = context_p->source_p;
  const uint8_t *ident_start_p = context_p->source_p;
  /* Note: newline or tab cannot be part of an identifier. */
  parser_line_counter_t column = context_p->column;
  const uint8_t *source_end_p = context_p->source_end_p;
  size_t length = 0;

  context_p->token.type = LEXER_LITERAL;
  context_p->token.literal_is_reserved = PARSER_FALSE;
  context_p->token.lit_location.type = LEXER_IDENT_LITERAL;
  context_p->token.lit_location.has_escape = PARSER_FALSE;

  do
  {
    if (*source_p == '\\')
    {
      uint16_t character;

      context_p->token.lit_location.has_escape = PARSER_TRUE;
      context_p->source_p = source_p;
      context_p->token.column = column;

      if ((source_p + 6 > source_end_p) || (source_p[1] != 'u'))
      {
        parser_raise_error (context_p, PARSER_ERR_INVALID_UNICODE_ESCAPE_SEQUENCE);
      }

      character = lexer_hex_to_character (context_p, source_p + 2, 4);

      if (length == 0)
      {
        if (!util_is_identifier_start_character (character))
        {
          parser_raise_error (context_p, PARSER_ERR_INVALID_IDENTIFIER_START);
        }
      }
      else
      {
        if (!util_is_identifier_part_character (character))
        {
          parser_raise_error (context_p, PARSER_ERR_INVALID_IDENTIFIER_PART);
        }
      }

      length += util_get_utf8_length (character);
      source_p += 6;
      PARSER_PLUS_EQUAL_LC (column, 6);
      continue;
    }

    /* Valid identifiers cannot contain 4 byte long utf-8
     * characters, since those characters are represented
     * by 2 ecmascript (UTF-16) characters, and those
     * characters cannot be literal characters. */
    PARSER_ASSERT (source_p[0] < LEXER_UTF8_4BYTE_START);

    source_p++;
    length++;
    column++;

    while (source_p < source_end_p
           && IS_UTF8_INTERMEDIATE_OCTET (source_p[0]))
    {
      source_p++;
      length++;
    }
  }
  while (source_p < source_end_p
         && (util_is_identifier_part (source_p) || *source_p == '\\'));

  context_p->source_p = ident_start_p;
  context_p->token.column = context_p->column;

  if (length > PARSER_MAXIMUM_IDENT_LENGTH)
  {
    parser_raise_error (context_p, PARSER_ERR_IDENTIFIER_TOO_LONG);
  }

  /* Check keywords (Only if there is no \u escape sequence in the pattern). */
  if (check_keywords
      && !context_p->token.lit_location.has_escape
      && (length >= 2 && length <= 10))
  {
    const keyword_string_t *keyword_list_p = keyword_strings_list[length - 2];

    int start = 0;
    int end = keyword_lengths_list[length - 2];
    int middle = end / 2;

    do
    {
      const keyword_string_t *keyword_p = keyword_list_p + middle;
      int compare_result = ident_start_p[0] - keyword_p->keyword_p[0];

      if (compare_result == 0)
      {
        compare_result = memcmp (ident_start_p, keyword_p->keyword_p, length);

        if (compare_result == 0)
        {
          if (keyword_p->type >= LEXER_FIRST_FUTURE_STRICT_RESERVED_WORD)
          {
            if (context_p->status_flags & PARSER_IS_STRICT)
            {
              parser_raise_error (context_p, PARSER_ERR_STRICT_IDENT_NOT_ALLOWED);
            }

            context_p->token.literal_is_reserved = PARSER_TRUE;
            break;
          }

          context_p->token.type = (uint8_t) keyword_p->type;
          break;
        }
      }

      if (compare_result > 0)
      {
        start = middle + 1;
      }
      else
      {
        PARSER_ASSERT (compare_result < 0);
        end = middle;
      }

      middle = (start + end) / 2;
    }
    while (start < end);
  }

  if (context_p->token.type == LEXER_LITERAL)
  {
    /* Fill literal data. */
    context_p->token.lit_location.char_p = ident_start_p;
    context_p->token.lit_location.length = (uint16_t) length;
  }

  context_p->source_p = source_p;
  context_p->column = column;
} /* lexer_parse_identifier */

/**
 * Parse string.
 */
void
lexer_parse_string (parser_context_t *context_p) /**< context */
{
  uint8_t str_end_character = context_p->source_p[0];
  const uint8_t *source_p = context_p->source_p + 1;
  const uint8_t *string_start_p = source_p;
  const uint8_t *source_end_p = context_p->source_end_p;
  parser_line_counter_t line = context_p->line;
  parser_line_counter_t column = (parser_line_counter_t) (context_p->column + 1);
  parser_line_counter_t original_line = line;
  parser_line_counter_t original_column = column;
  size_t length = 0;
  uint8_t has_escape = PARSER_FALSE;

#ifndef CONFIG_DISABLE_ES2015_TEMPLATE_STRINGS
  if (str_end_character == '}')
  {
    str_end_character = '`';
  }
#endif /* !CONFIG_DISABLE_ES2015_TEMPLATE_STRINGS */

  while (PARSER_TRUE)
  {
    if (source_p >= source_end_p)
    {
      context_p->token.line = original_line;
      context_p->token.column = (parser_line_counter_t) (original_column - 1);
      parser_raise_error (context_p, PARSER_ERR_UNTERMINATED_STRING);
    }

    if (*source_p == str_end_character)
    {
      break;
    }

    if (*source_p == '\\')
    {
      source_p++;
      column++;
      if (source_p >= source_end_p)
      {
        /* Will throw an unterminated string error. */
        continue;
      }

      has_escape = PARSER_TRUE;

      /* Newline is ignored. */
      if (*source_p == LEXER_NEWLINE_CR)
      {
        source_p++;
        if (source_p < source_end_p
            && *source_p == LEXER_NEWLINE_LF)
        {
          source_p++;
        }

        line++;
        column = 1;
        continue;
      }
      else if (*source_p == LEXER_NEWLINE_LF)
      {
        source_p++;
        line++;
        column = 1;
        continue;
      }
      else if (*source_p == LEXER_NEWLINE_LS_PS_BYTE_1 && LEXER_NEWLINE_LS_PS_BYTE_23 (source_p))
      {
        source_p += 3;
        line++;
        column = 1;
        continue;
      }

      /* Except \x, \u, and octal numbers, everything is
       * converted to a character which has the same byte length. */
      if (*source_p >= '0' && *source_p <= '3')
      {
        if (context_p->status_flags & PARSER_IS_STRICT)
        {
          parser_raise_error (context_p, PARSER_ERR_OCTAL_ESCAPE_NOT_ALLOWED);
        }

        source_p++;
        column++;

        if (source_p < source_end_p && *source_p >= '0' && *source_p <= '7')
        {
          source_p++;
          column++;

          if (source_p < source_end_p && *source_p >= '0' && *source_p <= '7')
          {
            /* Numbers >= 0x200 (0x80) requires
             * two bytes for encoding in UTF-8. */
            if (source_p[-2] >= '2')
            {
              length++;
            }

            source_p++;
            column++;
          }
        }

        length++;
        continue;
      }

      if (*source_p >= '4' && *source_p <= '7')
      {
        if (context_p->status_flags & PARSER_IS_STRICT)
        {
          parser_raise_error (context_p, PARSER_ERR_OCTAL_ESCAPE_NOT_ALLOWED);
        }

        source_p++;
        column++;

        if (source_p < source_end_p && *source_p >= '0' && *source_p <= '7')
        {
          source_p++;
          column++;
        }

        /* The maximum number is 0x4d so the UTF-8
         * representation is always one byte. */
        length++;
        continue;
      }

      if (*source_p == 'x' || *source_p == 'u')
      {
        uint8_t hex_part_length = (*source_p == 'x') ? 2 : 4;

        context_p->token.line = line;
        context_p->token.column = (parser_line_counter_t) (column - 1);
        if (source_p + 1 + hex_part_length > source_end_p)
        {
          parser_raise_error (context_p, PARSER_ERR_INVALID_ESCAPE_SEQUENCE);
        }

        length += util_get_utf8_length (lexer_hex_to_character (context_p,
                                                                source_p + 1,
                                                                hex_part_length));
        source_p += hex_part_length + 1;
        PARSER_PLUS_EQUAL_LC (column, hex_part_length + 1u);
        continue;
      }
    }

    if (*source_p >= LEXER_UTF8_4BYTE_START)
    {
      /* Processing 4 byte unicode sequence (even if it is
       * after a backslash). Always converted to two 3 byte
       * long sequence. */
      length += 2 * 3;
      has_escape = PARSER_TRUE;
      source_p += 4;
      column++;
      continue;
    }
    else if (*source_p == LEXER_TAB)
    {
      column = align_column_to_tab (column);
      /* Subtract -1 because column is increased below. */
      column--;
    }
#ifndef CONFIG_DISABLE_ES2015_TEMPLATE_STRINGS
    else if (str_end_character == '`')
    {
      if (source_p[0] == '{'
          && source_p[-1] == '$'
          && source_p[-2] != '\\')
      {
        length--;
        break;
      }

      /* Newline (without backslash) is part of the string. */
      if (*source_p == LEXER_NEWLINE_CR)
      {
        source_p++;
        length++;
        if (source_p < source_end_p
            && *source_p == LEXER_NEWLINE_LF)
        {
          source_p++;
          length++;
        }
        line++;
        column = 1;
        continue;
      }
      else if (*source_p == LEXER_NEWLINE_LF)
      {
        source_p++;
        length++;
        line++;
        column = 1;
        continue;
      }
      else if (*source_p == LEXER_NEWLINE_LS_PS_BYTE_1 && LEXER_NEWLINE_LS_PS_BYTE_23 (source_p))
      {
        source_p += 3;
        length += 3;
        line++;
        column = 1;
        continue;
      }
    }
#endif /* !CONFIG_DISABLE_ES2015_TEMPLATE_STRINGS */
    else if (*source_p == LEXER_NEWLINE_CR
             || *source_p == LEXER_NEWLINE_LF
             || (*source_p == LEXER_NEWLINE_LS_PS_BYTE_1 && LEXER_NEWLINE_LS_PS_BYTE_23 (source_p)))
    {
      context_p->token.line = line;
      context_p->token.column = column;
      parser_raise_error (context_p, PARSER_ERR_NEWLINE_NOT_ALLOWED);
    }

    source_p++;
    column++;
    length++;

    while (source_p < source_end_p
           && IS_UTF8_INTERMEDIATE_OCTET (*source_p))
    {
      source_p++;
      length++;
    }
  }

  if (length > PARSER_MAXIMUM_STRING_LENGTH)
  {
    parser_raise_error (context_p, PARSER_ERR_STRING_TOO_LONG);
  }

#ifndef CONFIG_DISABLE_ES2015_TEMPLATE_STRINGS
  context_p->token.type = ((str_end_character != '`') ? LEXER_LITERAL
                                                      : LEXER_TEMPLATE_LITERAL);
#else /* CONFIG_DISABLE_ES2015_TEMPLATE_STRINGS */
  context_p->token.type = LEXER_LITERAL;
#endif /* !CONFIG_DISABLE_ES2015_TEMPLATE_STRINGS */

  /* Fill literal data. */
  context_p->token.lit_location.char_p = string_start_p;
  context_p->token.lit_location.length = (uint16_t) length;
  context_p->token.lit_location.type = LEXER_STRING_LITERAL;
  context_p->token.lit_location.has_escape = has_escape;

  context_p->source_p = source_p + 1;
  context_p->line = line;
  context_p->column = (parser_line_counter_t) (column + 1);
} /* lexer_parse_string */

/**
 * Checks whether the character is hex digit.
 *
 * @return non-zero if the character is hex digit.
 */
static int
lexer_is_hex_digit (uint8_t character)
{
  return (character >= '0' && character <= '9') || ((character | 0x20) >= 'a' && (character | 0x20) <= 'f');
} /* lexer_is_hex_digit */

/**
 * Parse number.
 */
static void
lexer_parse_number (parser_context_t *context_p) /**< context */
{
  const uint8_t *source_p = context_p->source_p;
  const uint8_t *source_end_p = context_p->source_end_p;
  int can_be_float = PARSER_FALSE;
  size_t length;

  context_p->token.type = LEXER_LITERAL;
  context_p->token.literal_is_reserved = PARSER_FALSE;
  context_p->token.extra_value = LEXER_NUMBER_DECIMAL;
  context_p->token.lit_location.char_p = source_p;
  context_p->token.lit_location.type = LEXER_NUMBER_LITERAL;
  context_p->token.lit_location.has_escape = PARSER_FALSE;

  if (source_p[0] == '0'
      && source_p + 1 < source_end_p)
  {
    if ((source_p[1] | 0x20) == 'x')
    {
      context_p->token.extra_value = LEXER_NUMBER_HEXADECIMAL;
      source_p += 2;

      if (source_p >= source_end_p
          || !lexer_is_hex_digit (source_p[0]))
      {
        parser_raise_error (context_p, PARSER_ERR_INVALID_HEX_DIGIT);
      }

      do
      {
        source_p ++;
      }
      while (source_p < source_end_p
             && lexer_is_hex_digit (source_p[0]));
    }
    else if (source_p[1] >= '0'
             && source_p[1] <= '7')
    {
      context_p->token.extra_value = LEXER_NUMBER_OCTAL;

      if (context_p->status_flags & PARSER_IS_STRICT)
      {
        parser_raise_error (context_p, PARSER_ERR_OCTAL_NUMBER_NOT_ALLOWED);
      }

      do
      {
        source_p ++;
      }
      while (source_p < source_end_p
             && source_p[0] >= '0'
             && source_p[0] <= '7');

      if (source_p < source_end_p
          && source_p[0] >= '8'
          && source_p[0] <= '9')
      {
        parser_raise_error (context_p, PARSER_ERR_INVALID_NUMBER);
      }
    }
    else if (source_p[1] >= '8'
             && source_p[1] <= '9')
    {
      parser_raise_error (context_p, PARSER_ERR_INVALID_NUMBER);
    }
    else
    {
      can_be_float = PARSER_TRUE;
      source_p++;
    }
  }
  else
  {
    while (source_p < source_end_p
           && source_p[0] >= '0'
           && source_p[0] <= '9')
    {
      source_p++;
    }

    can_be_float = PARSER_TRUE;
  }

  if (can_be_float)
  {
    if (source_p < source_end_p
        && source_p[0] == '.')
    {
      source_p++;
      while (source_p < source_end_p
             && source_p[0] >= '0'
             && source_p[0] <= '9')
      {
        source_p++;
      }
    }

    if (source_p < source_end_p
        && (source_p[0] | 0x20) == 'e')
    {
      source_p++;

      if (source_p < source_end_p
          && (source_p[0] == '+' || source_p[0] == '-'))
      {
        source_p++;
      }

      if (source_p >= source_end_p
          || source_p[0] < '0'
          || source_p[0] > '9')
      {
        parser_raise_error (context_p, PARSER_ERR_MISSING_EXPONENT);
      }

      do
      {
        source_p++;
      }
      while (source_p < source_end_p
             && source_p[0] >= '0'
             && source_p[0] <= '9');
    }
  }

  if (source_p < source_end_p
      && (util_is_identifier_start (source_p) || source_p[0] == '\\'))
  {
    parser_raise_error (context_p, PARSER_ERR_IDENTIFIER_AFTER_NUMBER);
  }

  length = (size_t) (source_p - context_p->source_p);
  if (length > PARSER_MAXIMUM_IDENT_LENGTH)
  {
    parser_raise_error (context_p, PARSER_ERR_NUMBER_TOO_LONG);
  }

  context_p->token.lit_location.length = (uint16_t) length;
  PARSER_PLUS_EQUAL_LC (context_p->column, length);
  context_p->source_p = source_p;
} /* lexer_parse_number */

#define LEXER_TYPE_A_TOKEN(char1, type1) \
  case (uint8_t) (char1) : \
  { \
    context_p->token.type = (type1); \
    length = 1; \
    break; \
  }

#define LEXER_TYPE_B_TOKEN(char1, type1, char2, type2) \
  case (uint8_t) (char1) : \
  { \
    if (length >= 2 && context_p->source_p[1] == (uint8_t) (char2)) \
    { \
      context_p->token.type = (type2); \
      length = 2; \
      break; \
    } \
    \
    context_p->token.type = (type1); \
    length = 1; \
    break; \
  }

#define LEXER_TYPE_C_TOKEN(char1, type1, char2, type2, char3, type3) \
  case (uint8_t) (char1) : \
  { \
    if (length >= 2) \
    { \
      if (context_p->source_p[1] == (uint8_t) (char2)) \
      { \
        context_p->token.type = (type2); \
        length = 2; \
        break; \
      } \
      \
      if (context_p->source_p[1] == (uint8_t) (char3)) \
      { \
        context_p->token.type = (type3); \
        length = 2; \
        break; \
      } \
    } \
    \
    context_p->token.type = (type1); \
    length = 1; \
    break; \
  }

/**
 * Get next token.
 */
void
lexer_next_token (parser_context_t *context_p) /**< context */
{
  size_t length;

  lexer_skip_spaces (context_p);

  context_p->token.line = context_p->line;
  context_p->token.column = context_p->column;

  length = (size_t) (context_p->source_end_p - context_p->source_p);
  if (length == 0)
  {
    context_p->token.type = LEXER_EOS;
    return;
  }

  if (util_is_identifier_start (context_p->source_p)
      || context_p->source_p[0] == '\\')
  {
    lexer_parse_identifier (context_p, PARSER_TRUE);
    return;
  }

  if (context_p->source_p[0] >= '0' && context_p->source_p[0] <= '9')
  {
    lexer_parse_number (context_p);
    return;
  }

  switch (context_p->source_p[0])
  {
    LEXER_TYPE_A_TOKEN ('{', LEXER_LEFT_BRACE);
    LEXER_TYPE_A_TOKEN ('(', LEXER_LEFT_PAREN);
    LEXER_TYPE_A_TOKEN ('[', LEXER_LEFT_SQUARE);
    LEXER_TYPE_A_TOKEN ('}', LEXER_RIGHT_BRACE);
    LEXER_TYPE_A_TOKEN (')', LEXER_RIGHT_PAREN);
    LEXER_TYPE_A_TOKEN (']', LEXER_RIGHT_SQUARE);
    LEXER_TYPE_A_TOKEN (';', LEXER_SEMICOLON);
    LEXER_TYPE_A_TOKEN (',', LEXER_COMMA);

    case (uint8_t) '.':
    {
      if (length >= 2
          && (context_p->source_p[1] >= '0' && context_p->source_p[1] <= '9'))
      {
        lexer_parse_number (context_p);
        return;
      }

#ifndef CONFIG_DISABLE_ES2015_FUNCTION_REST_PARAMETER
      if (length >= 3
          && context_p->source_p[1] == (uint8_t) '.'
          && context_p->source_p[2] == (uint8_t) '.')
      {
        context_p->token.type = LEXER_THREE_DOTS;
        length = 3;
        break;
      }
#endif /* !CONFIG_DISABLE_ES2015_FUNCTION_REST_PARAMETER */

      context_p->token.type = LEXER_DOT;
      length = 1;
      break;
    }

    case (uint8_t) '<':
    {
      if (length >= 2)
      {
        if (context_p->source_p[1] == (uint8_t) '=')
        {
          context_p->token.type = LEXER_LESS_EQUAL;
          length = 2;
          break;
        }

        if (context_p->source_p[1] == (uint8_t) '<')
        {
          if (length >= 3 && context_p->source_p[2] == (uint8_t) '=')
          {
            context_p->token.type = LEXER_ASSIGN_LEFT_SHIFT;
            length = 3;
            break;
          }

          context_p->token.type = LEXER_LEFT_SHIFT;
          length = 2;
          break;
        }
      }

      context_p->token.type = LEXER_LESS;
      length = 1;
      break;
    }

    case (uint8_t) '>':
    {
      if (length >= 2)
      {
        if (context_p->source_p[1] == (uint8_t) '=')
        {
          context_p->token.type = LEXER_GREATER_EQUAL;
          length = 2;
          break;
        }

        if (context_p->source_p[1] == (uint8_t) '>')
        {
          if (length >= 3)
          {
            if (context_p->source_p[2] == (uint8_t) '=')
            {
              context_p->token.type = LEXER_ASSIGN_RIGHT_SHIFT;
              length = 3;
              break;
            }

            if (context_p->source_p[2] == (uint8_t) '>')
            {
              if (length >= 4 && context_p->source_p[3] == (uint8_t) '=')
              {
                context_p->token.type = LEXER_ASSIGN_UNS_RIGHT_SHIFT;
                length = 4;
                break;
              }

              context_p->token.type = LEXER_UNS_RIGHT_SHIFT;
              length = 3;
              break;
            }
          }

          context_p->token.type = LEXER_RIGHT_SHIFT;
          length = 2;
          break;
        }
      }

      context_p->token.type = LEXER_GREATER;
      length = 1;
      break;
    }

    case (uint8_t) '=':
    {
      if (length >= 2)
      {
        if (context_p->source_p[1] == (uint8_t) '=')
        {
          if (length >= 3 && context_p->source_p[2] == (uint8_t) '=')
          {
            context_p->token.type = LEXER_STRICT_EQUAL;
            length = 3;
            break;
          }

          context_p->token.type = LEXER_EQUAL;
          length = 2;
          break;
        }

#ifndef CONFIG_DISABLE_ES2015_ARROW_FUNCTION
        if (context_p->source_p[1] == (uint8_t) '>')
        {
          context_p->token.type = LEXER_ARROW;
          length = 2;
          break;
        }
#endif /* !CONFIG_DISABLE_ES2015_ARROW_FUNCTION */
      }

      context_p->token.type = LEXER_ASSIGN;
      length = 1;
      break;
    }

    case (uint8_t) '!':
    {
      if (length >= 2 && context_p->source_p[1] == (uint8_t) '=')
      {
        if (length >= 3 && context_p->source_p[2] == (uint8_t) '=')
        {
          context_p->token.type = LEXER_STRICT_NOT_EQUAL;
          length = 3;
          break;
        }

        context_p->token.type = LEXER_NOT_EQUAL;
        length = 2;
        break;
      }

      context_p->token.type = LEXER_LOGICAL_NOT;
      length = 1;
      break;
    }

    LEXER_TYPE_C_TOKEN ('+', LEXER_ADD, '=', LEXER_ASSIGN_ADD, '+', LEXER_INCREASE)
    LEXER_TYPE_C_TOKEN ('-', LEXER_SUBTRACT, '=', LEXER_ASSIGN_SUBTRACT, '-', LEXER_DECREASE)

    LEXER_TYPE_B_TOKEN ('*', LEXER_MULTIPLY, '=', LEXER_ASSIGN_MULTIPLY)
    LEXER_TYPE_B_TOKEN ('/', LEXER_DIVIDE, '=', LEXER_ASSIGN_DIVIDE)
    LEXER_TYPE_B_TOKEN ('%', LEXER_MODULO, '=', LEXER_ASSIGN_MODULO)

    LEXER_TYPE_C_TOKEN ('&', LEXER_BIT_AND, '=', LEXER_ASSIGN_BIT_AND, '&', LEXER_LOGICAL_AND)
    LEXER_TYPE_C_TOKEN ('|', LEXER_BIT_OR, '=', LEXER_ASSIGN_BIT_OR, '|', LEXER_LOGICAL_OR)

    LEXER_TYPE_B_TOKEN ('^', LEXER_BIT_XOR, '=', LEXER_ASSIGN_BIT_XOR)

    LEXER_TYPE_A_TOKEN ('~', LEXER_BIT_NOT);
    LEXER_TYPE_A_TOKEN ('?', LEXER_QUESTION_MARK);
    LEXER_TYPE_A_TOKEN (':', LEXER_COLON);

    case '\'':
    case '"':
#ifndef CONFIG_DISABLE_ES2015_TEMPLATE_STRINGS
    case '`':
#endif /* !CONFIG_DISABLE_ES2015_TEMPLATE_STRINGS */
    {
      lexer_parse_string (context_p);
      return;
    }

    default:
    {
      parser_raise_error (context_p, PARSER_ERR_INVALID_CHARACTER);
    }
  }

  context_p->source_p += length;
  PARSER_PLUS_EQUAL_LC (context_p->column, length);
} /* lexer_next_token */

#undef LEXER_TYPE_A_TOKEN
#undef LEXER_TYPE_B_TOKEN
#undef LEXER_TYPE_C_TOKEN
#undef LEXER_TYPE_D_TOKEN

/**
 * Checks whether the next token is the specified character.
 *
 * @return true - if the next is the specified character
 *         false - otherwise
 */
int
lexer_check_next_character (parser_context_t *context_p, /**< context */
                            uint8_t character) /**< specified character */
{
  lexer_skip_spaces (context_p);

  context_p->token.flags = (uint8_t) (context_p->token.flags | LEXER_NO_SKIP_SPACES);

  return (context_p->source_p < context_p->source_end_p
          && context_p->source_p[0] == character);
} /* lexer_check_next_character */

#ifndef CONFIG_DISABLE_ES2015_ARROW_FUNCTION

/**
 * Checks whether the next token is a type used for detecting arrow functions.
 *
 * @return identified token type
 */
lexer_token_type_t
lexer_check_arrow (parser_context_t *context_p) /**< context */
{
  lexer_skip_spaces (context_p);

  context_p->token.flags = (uint8_t) (context_p->token.flags | LEXER_NO_SKIP_SPACES);

  if (context_p->source_p < context_p->source_end_p)
  {
    switch (context_p->source_p[0])
    {
      case (uint8_t) ',':
      {
        return LEXER_COMMA;
      }
      case (uint8_t) ')':
      {
        return LEXER_RIGHT_PAREN;
      }
      case (uint8_t) '=':
      {
        if (!(context_p->token.flags & LEXER_WAS_NEWLINE)
            && context_p->source_p + 1 < context_p->source_end_p
            && context_p->source_p[1] == (uint8_t) '>')
        {
          return LEXER_ARROW;
        }
        break;
      }
      default:
      {
        break;
      }
    }
  }

  return LEXER_EOS;
} /* lexer_check_arrow */

#endif /* !CONFIG_DISABLE_ES2015_ARROW_FUNCTION */

/**
 * Search or append the string to the literal pool.
 */
static void
lexer_process_char_literal (parser_context_t *context_p, /**< context */
                            const uint8_t *char_p, /**< characters */
                            size_t length, /**< length of string */
                            uint8_t literal_type, /**< final literal type */
                            uint8_t has_escape) /**< has escape sequences */
{
  parser_list_iterator_t literal_iterator;
  lexer_literal_t *literal_p;
  uint32_t literal_index = 0;

  PARSER_ASSERT (literal_type == LEXER_IDENT_LITERAL
                 || literal_type == LEXER_STRING_LITERAL);

  PARSER_ASSERT (literal_type != LEXER_IDENT_LITERAL || length <= PARSER_MAXIMUM_IDENT_LENGTH);
  PARSER_ASSERT (literal_type != LEXER_STRING_LITERAL || length <= PARSER_MAXIMUM_STRING_LENGTH);

  parser_list_iterator_init (&context_p->literal_pool, &literal_iterator);

  while ((literal_p = (lexer_literal_t *) parser_list_iterator_next (&literal_iterator)) != NULL)
  {
    if (literal_p->type == literal_type
        && literal_p->prop.length == length
        && util_compare_char_literals (literal_p, char_p))
    {
      context_p->lit_object.literal_p = literal_p;
      context_p->lit_object.index = (uint16_t) literal_index;
      literal_p->status_flags = (uint8_t) (literal_p->status_flags & ~LEXER_FLAG_UNUSED_IDENT);
      return;
    }

    literal_index++;
  }

  PARSER_ASSERT (literal_index == context_p->literal_count);

  if (literal_index >= PARSER_MAXIMUM_NUMBER_OF_LITERALS)
  {
    parser_raise_error (context_p, PARSER_ERR_LITERAL_LIMIT_REACHED);
  }

  if (length == 0)
  {
    has_escape = 0;
  }

  literal_p = (lexer_literal_t *) parser_list_append (context_p, &context_p->literal_pool);
  literal_p->prop.length = (uint16_t) length;
  literal_p->type = literal_type;
  literal_p->status_flags = has_escape ? 0 : LEXER_FLAG_SOURCE_PTR;

  if (util_set_char_literal (literal_p, char_p) != 0)
  {
    parser_raise_error (context_p, PARSER_ERR_OUT_OF_MEMORY);
  }

  context_p->lit_object.literal_p = literal_p;
  context_p->lit_object.index = (uint16_t) literal_index;
  context_p->literal_count++;
} /* lexer_process_char_literal */

/* Maximum buffer size for identifiers which contains escape sequences. */
#define LEXER_MAX_LITERAL_LOCAL_BUFFER_SIZE 48

/**
 * Construct a literal object from an identifier.
 */
void
lexer_construct_literal_object (parser_context_t *context_p, /**< context */
                                lexer_lit_location_t *literal_p, /**< literal location */
                                uint8_t literal_type) /**< final literal type */
{
  uint8_t *destination_start_p;
  const uint8_t *source_p;
  uint8_t local_byte_array[LEXER_MAX_LITERAL_LOCAL_BUFFER_SIZE];

  PARSER_ASSERT (literal_p->type == LEXER_IDENT_LITERAL
                 || literal_p->type == LEXER_STRING_LITERAL);
  PARSER_ASSERT (context_p->allocated_buffer_p == NULL);

  destination_start_p = local_byte_array;
  source_p = literal_p->char_p;

  if (literal_p->has_escape)
  {
    uint8_t *destination_p;

    if (literal_p->length > LEXER_MAX_LITERAL_LOCAL_BUFFER_SIZE)
    {
      destination_start_p = (uint8_t *) parser_malloc_local (context_p, literal_p->length);
      context_p->allocated_buffer_p = destination_start_p;
    }

    destination_p = destination_start_p;

    if (literal_p->type == LEXER_IDENT_LITERAL)
    {
      const uint8_t *source_end_p = context_p->source_end_p;

      PARSER_ASSERT (literal_p->length <= PARSER_MAXIMUM_IDENT_LENGTH);

      do
      {
        if (*source_p == '\\')
        {
          destination_p += util_to_utf8_bytes (destination_p,
                                               lexer_hex_to_character (context_p, source_p + 2, 4));
          source_p += 6;
          continue;
        }

        *destination_p++ = *source_p++;

        while (source_p < source_end_p
               && IS_UTF8_INTERMEDIATE_OCTET (*source_p))
        {
          *destination_p++ = *source_p++;
        }
      }
      while (source_p < source_end_p
             && (util_is_identifier_part (source_p) || *source_p == '\\'));

      PARSER_ASSERT (destination_p == destination_start_p + literal_p->length);
    }
    else
    {
      uint8_t str_end_character = source_p[-1];

#ifndef CONFIG_DISABLE_ES2015_TEMPLATE_STRINGS
      if (str_end_character == '}')
      {
        str_end_character = '`';
      }
#endif /* !CONFIG_DISABLE_ES2015_TEMPLATE_STRINGS */

      while (PARSER_TRUE)
      {
        if (*source_p == str_end_character)
        {
          break;
        }

        if (*source_p == '\\')
        {
          uint8_t conv_character;

          source_p++;
          PARSER_ASSERT (source_p < context_p->source_end_p);

          /* Newline is ignored. */
          if (*source_p == LEXER_NEWLINE_CR)
          {
            source_p++;
            PARSER_ASSERT (source_p < context_p->source_end_p);

            if (*source_p == LEXER_NEWLINE_LF)
            {
              source_p++;
            }
            continue;
          }
          else if (*source_p == LEXER_NEWLINE_LF)
          {
            source_p++;
            continue;
          }
          else if (*source_p == LEXER_NEWLINE_LS_PS_BYTE_1 && LEXER_NEWLINE_LS_PS_BYTE_23 (source_p))
          {
            source_p += 3;
            continue;
          }

          if (*source_p >= '0' && *source_p <= '3')
          {
            uint32_t octal_number = (uint32_t) (*source_p - '0');

            source_p++;
            PARSER_ASSERT (source_p < context_p->source_end_p);

            if (*source_p >= '0' && *source_p <= '7')
            {
              octal_number = octal_number * 8 + (uint32_t) (*source_p - '0');
              source_p++;
              PARSER_ASSERT (source_p < context_p->source_end_p);

              if (*source_p >= '0' && *source_p <= '7')
              {
                octal_number = octal_number * 8 + (uint32_t) (*source_p - '0');
                source_p++;
                PARSER_ASSERT (source_p < context_p->source_end_p);
              }
            }

            destination_p += util_to_utf8_bytes (destination_p, (uint16_t) octal_number);
            continue;
          }

          if (*source_p >= '4' && *source_p <= '7')
          {
            uint32_t octal_number = (uint32_t) (*source_p - '0');

            source_p++;
            PARSER_ASSERT (source_p < context_p->source_end_p);

            if (*source_p >= '0' && *source_p <= '7')
            {
              octal_number = octal_number * 8 + (uint32_t) (*source_p - '0');
              source_p++;
              PARSER_ASSERT (source_p < context_p->source_end_p);
            }

            *destination_p++ = (uint8_t) octal_number;
            continue;
          }

          if (*source_p == 'x' || *source_p == 'u')
          {
            int hex_part_length = (*source_p == 'x') ? 2 : 4;
            PARSER_ASSERT (source_p + 1 + hex_part_length <= context_p->source_end_p);

            destination_p += util_to_utf8_bytes (destination_p,
                                                 lexer_hex_to_character (context_p,
                                                                         source_p + 1,
                                                                         hex_part_length));
            source_p += hex_part_length + 1;
            continue;
          }

          conv_character = *source_p;
          switch (*source_p)
          {
            case 'b':
            {
              conv_character = 0x08;
              break;
            }
            case 't':
            {
              conv_character = 0x09;
              break;
            }
            case 'n':
            {
              conv_character = 0x0a;
              break;
            }
            case 'v':
            {
              conv_character = 0x0b;
              break;
            }
            case 'f':
            {
              conv_character = 0x0c;
              break;
            }
            case 'r':
            {
              conv_character = 0x0d;
              break;
            }
          }

          if (conv_character != *source_p)
          {
            *destination_p++ = conv_character;
            source_p++;
            continue;
          }
        }
#ifndef CONFIG_DISABLE_ES2015_TEMPLATE_STRINGS
        else if (str_end_character == '`'
                 && source_p[0] == '$'
                 && source_p[1] == '{')
        {
          source_p++;
          PARSER_ASSERT (source_p < context_p->source_end_p);
          break;
        }
#endif /* !CONFIG_DISABLE_ES2015_TEMPLATE_STRINGS */

        if (*source_p >= LEXER_UTF8_4BYTE_START)
        {
          /* Processing 4 byte unicode sequence (even if it is
           * after a backslash). Always converted to two 3 byte
           * long sequence. */

          uint32_t character = ((((uint32_t) source_p[0]) & 0x7) << 18);
          character |= ((((uint32_t) source_p[1]) & 0x3f) << 12);
          character |= ((((uint32_t) source_p[2]) & 0x3f) << 6);
          character |= (((uint32_t) source_p[3]) & 0x3f);

          PARSER_ASSERT (character >= 0x10000);
          character -= 0x10000;
          destination_p += util_to_utf8_bytes (destination_p,
                                               (lexer_character_type_t) (0xd800 | (character >> 10)));
          destination_p += util_to_utf8_bytes (destination_p,
                                               (lexer_character_type_t) (0xdc00 | (character & 0x3ff)));
          source_p += 4;
          continue;
        }

        *destination_p++ = *source_p++;

        /* There is no need to check the source_end_p
         * since the string is terminated by a quotation mark. */
        while (IS_UTF8_INTERMEDIATE_OCTET (*source_p))
        {
          *destination_p++ = *source_p++;
        }
      }

      PARSER_ASSERT (destination_p == destination_start_p + literal_p->length);
    }

    source_p = destination_start_p;
  }

  lexer_process_char_literal (context_p,
                              source_p,
                              literal_p->length,
                              literal_type,
                              literal_p->has_escape);

  context_p->lit_object.type = LEXER_LITERAL_OBJECT_ANY;

  if (literal_type == LEXER_IDENT_LITERAL
      && (context_p->status_flags & PARSER_INSIDE_WITH)
      && context_p->lit_object.literal_p->type == LEXER_IDENT_LITERAL)
  {
    context_p->lit_object.literal_p->status_flags |= LEXER_FLAG_NO_REG_STORE;
  }

  if (literal_p->length == 4
      && source_p[0] == 'e'
      && source_p[3] == 'l'
      && source_p[1] == 'v'
      && source_p[2] == 'a')
  {
    context_p->lit_object.type = LEXER_LITERAL_OBJECT_EVAL;
  }

  if (literal_p->length == 9
      && source_p[0] == 'a'
      && source_p[8] == 's'
      && memcmp (source_p + 1, "rgument", 7) == 0)
  {
    context_p->lit_object.type = LEXER_LITERAL_OBJECT_ARGUMENTS;
    if (!(context_p->status_flags & PARSER_ARGUMENTS_NOT_NEEDED)
        && literal_type == LEXER_IDENT_LITERAL)
    {
      context_p->status_flags |= PARSER_ARGUMENTS_NEEDED | PARSER_LEXICAL_ENV_NEEDED;
      context_p->lit_object.literal_p->status_flags |= LEXER_FLAG_NO_REG_STORE;
    }
  }

  if (destination_start_p != local_byte_array)
  {
    PARSER_ASSERT (context_p->allocated_buffer_p == destination_start_p);

    context_p->allocated_buffer_p = NULL;
    parser_free_local (destination_start_p);
  }

  PARSER_ASSERT (context_p->allocated_buffer_p == NULL);
} /* lexer_construct_literal_object */

#undef LEXER_MAX_LITERAL_LOCAL_BUFFER_SIZE

/**
 * Construct a number object.
 *
 * @return PARSER_TRUE if number is small number
 */
int
lexer_construct_number_object (parser_context_t *context_p, /**< context */
                               int is_expr, /**< expression is parsed */
                               int is_negative_number) /**< sign is negative */
{
  lexer_literal_t *literal_p;
  uint16_t literal_count = context_p->literal_count;

  if (is_expr)
  {
    int32_t number = util_get_number (context_p->token.lit_location.char_p,
                                      context_p->token.lit_location.length);

    if (number <= CBC_PUSH_NUMBER_BYTE_RANGE_END && (number != 0 || !is_negative_number))
    {
      context_p->lit_object.index = (uint16_t) number;
      return PARSER_TRUE;
    }
  }

  if (literal_count >= PARSER_MAXIMUM_NUMBER_OF_LITERALS)
  {
    parser_raise_error (context_p, PARSER_ERR_LITERAL_LIMIT_REACHED);
  }

  literal_p = (lexer_literal_t *) parser_list_append (context_p, &context_p->literal_pool);
  literal_p->prop.length = context_p->token.lit_location.length;
  literal_p->type = LEXER_UNUSED_LITERAL;
  literal_p->status_flags = 0;

  context_p->literal_count++;

  if (util_set_number_literal (literal_p, context_p->token.lit_location.char_p))
  {
    parser_raise_error (context_p, PARSER_ERR_OUT_OF_MEMORY);
  }

  literal_p->type = LEXER_NUMBER_LITERAL;

  context_p->lit_object.literal_p = literal_p;
  context_p->lit_object.index = literal_count;
  context_p->lit_object.type = LEXER_LITERAL_OBJECT_ANY;

  return PARSER_FALSE;
} /* lexer_construct_number_object */

/**
 * Convert a push number opcode to push literal opcode
 */
void
lexer_convert_push_number_to_push_literal (parser_context_t *context_p) /**< context */
{
  int value;
  int two_literals = !PARSER_IS_BASIC_OPCODE (context_p->last_cbc_opcode);

  if (context_p->last_cbc_opcode == CBC_PUSH_NUMBER_0
      || context_p->last_cbc_opcode == PARSER_TO_EXT_OPCODE (CBC_EXT_PUSH_LITERAL_PUSH_NUMBER_0))
  {
    value = 0;
  }
  else if (context_p->last_cbc_opcode == CBC_PUSH_NUMBER_POS_BYTE
           || context_p->last_cbc_opcode == PARSER_TO_EXT_OPCODE (CBC_EXT_PUSH_LITERAL_PUSH_NUMBER_POS_BYTE))
  {
    value = ((int) context_p->last_cbc.value) + 1;
  }
  else
  {
    PARSER_ASSERT (context_p->last_cbc_opcode == CBC_PUSH_NUMBER_NEG_BYTE
                   || context_p->last_cbc_opcode == PARSER_TO_EXT_OPCODE (CBC_EXT_PUSH_LITERAL_PUSH_NUMBER_NEG_BYTE));
    value = -((int) context_p->last_cbc.value) - 1;
  }

  const uint8_t *lit_value = (const uint8_t *) (uintptr_t) (256 + value);

  parser_list_iterator_t literal_iterator;
  parser_list_iterator_init (&context_p->literal_pool, &literal_iterator);

  context_p->last_cbc_opcode = two_literals ? CBC_PUSH_TWO_LITERALS : CBC_PUSH_LITERAL;

  uint32_t literal_index = 0;
  lexer_literal_t *literal_p;

  while ((literal_p = (lexer_literal_t *) parser_list_iterator_next (&literal_iterator)) != NULL)
  {
    if (literal_p->type == LEXER_NUMBER_LITERAL
        && literal_p->prop.length == 0
        && literal_p->value.char_p == lit_value)
    {
      if (two_literals)
      {
        context_p->last_cbc.value = (uint16_t) literal_index;
      }
      else
      {
        context_p->last_cbc.literal_index = (uint16_t) literal_index;
      }
      return;
    }

    literal_index++;
  }

  PARSER_ASSERT (literal_index == context_p->literal_count);

  if (literal_index >= PARSER_MAXIMUM_NUMBER_OF_LITERALS)
  {
    parser_raise_error (context_p, PARSER_ERR_LITERAL_LIMIT_REACHED);
  }

  literal_p = (lexer_literal_t *) parser_list_append (context_p, &context_p->literal_pool);
  literal_p->value.char_p = lit_value;
  literal_p->prop.length = 0;
  literal_p->type = LEXER_NUMBER_LITERAL;
  literal_p->status_flags = 0;

  context_p->literal_count++;

  if (two_literals)
  {
    context_p->last_cbc.value = (uint16_t) literal_index;
  }
  else
  {
    context_p->last_cbc.literal_index = (uint16_t) literal_index;
  }
} /* lexer_convert_push_number_to_push_literal */

/**
 * Construct a function literal object.
 *
 * @return function object literal index
 */
uint16_t
lexer_construct_function_object (parser_context_t *context_p, /**< context */
                                 uint32_t extra_status_flags) /**< extra status flags */
{
  cbc_compiled_code_t *compiled_code_p;
  lexer_literal_t *literal_p;
  uint16_t result_index;

  if (context_p->literal_count >= PARSER_MAXIMUM_NUMBER_OF_LITERALS)
  {
    parser_raise_error (context_p, PARSER_ERR_LITERAL_LIMIT_REACHED);
  }

  if (context_p->status_flags & (PARSER_RESOLVE_BASE_FOR_CALLS | PARSER_INSIDE_WITH))
  {
    extra_status_flags |= PARSER_RESOLVE_BASE_FOR_CALLS;
  }

  literal_p = (lexer_literal_t *) parser_list_append (context_p, &context_p->literal_pool);
  literal_p->type = LEXER_UNUSED_LITERAL;
  literal_p->status_flags = 0;

  result_index = context_p->literal_count;
  context_p->literal_count++;

#ifndef CONFIG_DISABLE_ES2015_ARROW_FUNCTION
  if (!(extra_status_flags & PARSER_IS_ARROW_FUNCTION))
  {
    compiled_code_p = parser_parse_function (context_p, extra_status_flags);
  }
  else
  {
    compiled_code_p = parser_parse_arrow_function (context_p, extra_status_flags);
  }
#else /* CONFIG_DISABLE_ES2015_ARROW_FUNCTION */
  compiled_code_p = parser_parse_function (context_p, extra_status_flags);
#endif /* !CONFIG_DISABLE_ES2015_ARROW_FUNCTION */

  util_set_function_literal (literal_p, compiled_code_p);
  literal_p->type = LEXER_FUNCTION_LITERAL;

  return result_index;
} /* lexer_construct_function_object */

/**
 * Construct a regular expression object.
 */
void
lexer_construct_regexp_object (parser_context_t *context_p, /**< context */
                               int parse_only) /**< parse only */
{
  const uint8_t *source_p = context_p->source_p;
  const uint8_t *regex_start_p = context_p->source_p - 1;
  const uint8_t *source_end_p = context_p->source_end_p;
  parser_line_counter_t column = context_p->column;
  lexer_literal_t *literal_p;
  int in_class = PARSER_FALSE;
  uint32_t current_flags;
  size_t length;

  PARSER_ASSERT (context_p->token.type == LEXER_DIVIDE
                 || context_p->token.type == LEXER_ASSIGN_DIVIDE);

  if (context_p->token.type == LEXER_ASSIGN_DIVIDE)
  {
    regex_start_p--;
  }

  while (PARSER_TRUE)
  {
    if (source_p >= source_end_p)
    {
      parser_raise_error (context_p, PARSER_ERR_UNTERMINATED_REGEXP);
    }

    if (!in_class && source_p[0] == '/')
    {
      source_p++;
      column++;
      break;
    }

    switch (source_p[0])
    {
      case LEXER_NEWLINE_CR:
      case LEXER_NEWLINE_LF:
      case LEXER_NEWLINE_LS_PS_BYTE_1:
      {
        if (source_p[0] != LEXER_NEWLINE_LS_PS_BYTE_1
            || LEXER_NEWLINE_LS_PS_BYTE_23 (source_p))
        {
          parser_raise_error (context_p, PARSER_ERR_NEWLINE_NOT_ALLOWED);
        }
        break;
      }
      case LEXER_TAB:
      {
        column = align_column_to_tab (column);
         /* Subtract -1 because column is increased below. */
        column--;
        break;
      }
      case '[':
      {
        in_class = PARSER_TRUE;
        break;
      }
      case ']':
      {
        in_class = PARSER_FALSE;
        break;
      }
      case '\\':
      {
        if (source_p + 1 >= source_end_p)
        {
          parser_raise_error (context_p, PARSER_ERR_UNTERMINATED_REGEXP);
        }

        if (source_p[1] >= 0x20 && source_p[1] <= 0x7f)
        {
          source_p++;
          column++;
        }
      }
    }

    source_p++;
    column++;

    while (source_p < source_end_p
           && IS_UTF8_INTERMEDIATE_OCTET (source_p[0]))
    {
      source_p++;
    }
  }

  current_flags = 0;
  while (source_p < source_end_p)
  {
    uint32_t flag = 0;

    if (source_p[0] == 'g')
    {
      flag = 0x1;
    }
    else if (source_p[0] == 'i')
    {
      flag = 0x2;
    }
    else if (source_p[0] == 'm')
    {
      flag = 0x4;
    }

    if (flag == 0)
    {
      break;
    }

    if (current_flags & flag)
    {
      parser_raise_error (context_p, PARSER_ERR_DUPLICATED_REGEXP_FLAG);
    }

    current_flags |= flag;
    source_p++;
    column++;
  }

  if (source_p < source_end_p
      && util_is_identifier_part (source_p))
  {
    parser_raise_error (context_p, PARSER_ERR_UNKNOWN_REGEXP_FLAG);
  }

  context_p->source_p = source_p;
  context_p->column = column;

  length = (size_t) (source_p - regex_start_p);
  if (length > PARSER_MAXIMUM_STRING_LENGTH)
  {
    parser_raise_error (context_p, PARSER_ERR_REGEXP_TOO_LONG);
  }

  context_p->column = column;
  context_p->source_p = source_p;

  if (parse_only)
  {
    return;
  }

  if (context_p->literal_count >= PARSER_MAXIMUM_NUMBER_OF_LITERALS)
  {
    parser_raise_error (context_p, PARSER_ERR_LITERAL_LIMIT_REACHED);
  }

  literal_p = (lexer_literal_t *) parser_list_append (context_p, &context_p->literal_pool);
  literal_p->prop.length = (uint16_t) length;
  literal_p->type = LEXER_UNUSED_LITERAL;
  literal_p->status_flags = 0;

  context_p->literal_count++;

  if (util_set_regexp_literal (literal_p, regex_start_p))
  {
    parser_raise_error (context_p, PARSER_ERR_INVALID_REGEXP);
  }

  literal_p->type = LEXER_REGEXP_LITERAL;

  context_p->token.type = LEXER_LITERAL;
  context_p->token.literal_is_reserved = PARSER_FALSE;
  context_p->token.lit_location.type = LEXER_REGEXP_LITERAL;

  context_p->lit_object.literal_p = literal_p;
  context_p->lit_object.index = (uint16_t) (context_p->literal_count - 1);
  context_p->lit_object.type = LEXER_LITERAL_OBJECT_ANY;
} /* lexer_construct_regexp_object */

/**
 * Next token must be an identifier.
 */
void
lexer_expect_identifier (parser_context_t *context_p, /**< context */
                         uint8_t literal_type) /**< literal type */
{
  PARSER_ASSERT (literal_type == LEXER_STRING_LITERAL
                 || literal_type == LEXER_IDENT_LITERAL);

  lexer_skip_spaces (context_p);
  context_p->token.line = context_p->line;
  context_p->token.column = context_p->column;

  if (context_p->source_p < context_p->source_end_p
      && (util_is_identifier_start (context_p->source_p) || context_p->source_p[0] == '\\'))
  {
    lexer_parse_identifier (context_p, literal_type != LEXER_STRING_LITERAL);

    if (context_p->token.type == LEXER_LITERAL)
    {
      lexer_construct_literal_object (context_p,
                                      &context_p->token.lit_location,
                                      literal_type);

      if (literal_type == LEXER_IDENT_LITERAL
          && (context_p->status_flags & PARSER_IS_STRICT)
          && context_p->lit_object.type != LEXER_LITERAL_OBJECT_ANY)
      {
        parser_error_t error;

        if (context_p->lit_object.type == LEXER_LITERAL_OBJECT_EVAL)
        {
          error = PARSER_ERR_EVAL_NOT_ALLOWED;
        }
        else
        {
          PARSER_ASSERT (context_p->lit_object.type == LEXER_LITERAL_OBJECT_ARGUMENTS);
          error = PARSER_ERR_ARGUMENTS_NOT_ALLOWED;
        }

        parser_raise_error (context_p, error);
      }

      context_p->token.lit_location.type = literal_type;
      return;
    }
  }

  parser_raise_error (context_p, PARSER_ERR_IDENTIFIER_EXPECTED);
} /* lexer_expect_identifier */

/**
 * Next token must be an identifier.
 */
void
lexer_expect_object_literal_id (parser_context_t *context_p, /**< context */
                                uint32_t ident_opts) /**< lexer_obj_ident_opts_t option bits */
{
  lexer_skip_spaces (context_p);

#ifndef CONFIG_DISABLE_ES2015_CLASS
  int is_class_method = ((ident_opts & LEXER_OBJ_IDENT_CLASS_METHOD)
                         && !(ident_opts & LEXER_OBJ_IDENT_ONLY_IDENTIFIERS)
                         && (context_p->token.type != LEXER_KEYW_STATIC));
#endif /* !CONFIG_DISABLE_ES2015_CLASS */

  context_p->token.line = context_p->line;
  context_p->token.column = context_p->column;

  if (context_p->source_p < context_p->source_end_p)
  {
    int create_literal_object = PARSER_FALSE;

    if (util_is_identifier_start (context_p->source_p) || context_p->source_p[0] == '\\')
    {
      lexer_parse_identifier (context_p, PARSER_FALSE);

      if (!(ident_opts & LEXER_OBJ_IDENT_ONLY_IDENTIFIERS)
          && context_p->token.lit_location.length == 3)
      {
        lexer_skip_spaces (context_p);

        if (context_p->source_p < context_p->source_end_p
            && context_p->source_p[0] != ':')
        {
          if (lexer_compare_raw_identifier_to_current (context_p, "get", 3))
          {
            context_p->token.type = LEXER_PROPERTY_GETTER;
            return;
          }
          else if (lexer_compare_raw_identifier_to_current (context_p, "set", 3))
          {
            context_p->token.type = LEXER_PROPERTY_SETTER;
            return;
          }
        }
      }

#ifndef CONFIG_DISABLE_ES2015_CLASS
      if (is_class_method
          && lexer_compare_raw_identifier_to_current (context_p, "static", 6))
      {
        context_p->token.type = LEXER_KEYW_STATIC;
        return;
      }
#endif /* !CONFIG_DISABLE_ES2015_CLASS */

      create_literal_object = PARSER_TRUE;
    }
    else if (context_p->source_p[0] == '"'
             || context_p->source_p[0] == '\'')
    {
      lexer_parse_string (context_p);
      create_literal_object = PARSER_TRUE;
    }
#ifndef CONFIG_DISABLE_ES2015_OBJECT_INITIALIZER
    else if (context_p->source_p[0] == '[')
    {
      context_p->source_p += 1;
      context_p->column++;

      lexer_next_token (context_p);
      parser_parse_expression (context_p, PARSE_EXPR_NO_COMMA);

      if (context_p->token.type != LEXER_RIGHT_SQUARE)
      {
        parser_raise_error (context_p, PARSER_ERR_RIGHT_SQUARE_EXPECTED);
      }
      return;
    }
#endif /* CONFIG_DISABLE_ES2015_OBJECT_INITIALIZER */
    else if (!(ident_opts & LEXER_OBJ_IDENT_ONLY_IDENTIFIERS) && context_p->source_p[0] == '}')
    {
      context_p->token.type = LEXER_RIGHT_BRACE;
      context_p->source_p += 1;
      context_p->column++;
      return;
    }
    else
    {
      const uint8_t *char_p = context_p->source_p;

      if (char_p[0] == '.')
      {
        char_p++;
      }

      if (char_p < context_p->source_end_p
          && char_p[0] >= '0'
          && char_p[0] <= '9')
      {
        lexer_parse_number (context_p);
        lexer_construct_number_object (context_p, PARSER_FALSE, PARSER_FALSE);
        return;
      }
    }

    if (create_literal_object)
    {
#ifndef CONFIG_DISABLE_ES2015_CLASS
      if (is_class_method
          && lexer_compare_raw_identifier_to_current (context_p, "constructor", 11))
      {
        context_p->token.type = LEXER_CLASS_CONSTRUCTOR;
        return;
      }
#endif /* !CONFIG_DISABLE_ES2015_CLASS */

      lexer_construct_literal_object (context_p,
                                      &context_p->token.lit_location,
                                      LEXER_STRING_LITERAL);
      return;
    }
  }

  parser_raise_error (context_p, PARSER_ERR_PROPERTY_IDENTIFIER_EXPECTED);
} /* lexer_expect_object_literal_id */

/**
 * Next token must be an identifier.
 */
void
lexer_scan_identifier (parser_context_t *context_p, /**< context */
                       int property_name) /**< property name */
{
  lexer_skip_spaces (context_p);
  context_p->token.line = context_p->line;
  context_p->token.column = context_p->column;

  if (context_p->source_p < context_p->source_end_p
      && (util_is_identifier_start (context_p->source_p) || context_p->source_p[0] == '\\'))
  {
    lexer_parse_identifier (context_p, PARSER_FALSE);

    if (property_name && context_p->token.lit_location.length == 3)
    {
      lexer_skip_spaces (context_p);

      if (context_p->source_p < context_p->source_end_p
          && context_p->source_p[0] != ':')
      {
        if (lexer_compare_raw_identifier_to_current (context_p, "get", 3))
        {
          context_p->token.type = LEXER_PROPERTY_GETTER;
        }
        else if (lexer_compare_raw_identifier_to_current (context_p, "set", 3))
        {
          context_p->token.type = LEXER_PROPERTY_SETTER;
        }
      }
    }
    return;
  }

  if (property_name)
  {
    lexer_next_token (context_p);

    if (context_p->token.type == LEXER_LITERAL
#ifndef CONFIG_DISABLE_ES2015_OBJECT_INITIALIZER
        || context_p->token.type == LEXER_LEFT_SQUARE
#endif /* !CONFIG_DISABLE_ES2015_OBJECT_INITIALIZER */
        || context_p->token.type == LEXER_RIGHT_BRACE)
    {
      return;
    }
  }

  parser_raise_error (context_p, PARSER_ERR_IDENTIFIER_EXPECTED);
} /* lexer_scan_identifier */

/**
 * Converts a "\uxxxx" sequence into a unicode character
 *
 * @return the decoded 16 bit unicode character
 */
static lexer_character_type_t
lexer_decode_unicode_sequence (const uint8_t *source_p)
{
  lexer_character_type_t chr = 0;
  const uint8_t *source_end_p = source_p + 6;

  source_p += 2;
  do
  {
    uint8_t byte = *source_p++;
    chr = (lexer_character_type_t) (chr << 4);
    if (byte <= '9')
    {
      chr = (lexer_character_type_t) (chr + byte - '0');
    }
    else
    {
      chr = (lexer_character_type_t) (chr + LEXER_TO_ASCII_LOWERCASE (byte) - ('a' - 10));
    }
  }
  while (source_p < source_end_p);

  return chr;
} /* lexer_decode_unicode_sequence */

/**
 * Compares two identifiers.
 *
 * @return non-zero if the input identifiers are the same
 */
int
lexer_compare_identifier_to_current (parser_context_t *context_p, /**< context */
                                     const lexer_lit_location_t *right_ident_p) /**< identifier */
{
  lexer_lit_location_t *left_ident_p = &context_p->token.lit_location;
  const uint8_t *left_p;
  const uint8_t *right_p;
  size_t count;

  PARSER_ASSERT (left_ident_p->length > 0 && right_ident_p->length > 0);

  if (left_ident_p->length != right_ident_p->length)
  {
    return 0;
  }

  if (!left_ident_p->has_escape && !right_ident_p->has_escape)
  {
    return memcmp (left_ident_p->char_p, right_ident_p->char_p, left_ident_p->length) == 0;
  }

  left_p = left_ident_p->char_p;
  right_p = right_ident_p->char_p;
  count = left_ident_p->length;

  do
  {
    uint8_t utf8_buf[3];
    size_t utf8_len, offset;

    /* Backslash cannot be part of a multibyte UTF-8 character. */
    if (*left_p != '\\' && *right_p != '\\')
    {
      if (*left_p++ != *right_p++)
      {
        return PARSER_FALSE;
      }
      count--;
      continue;
    }

    if (*left_p == '\\' && *right_p == '\\')
    {
      uint16_t left_chr = lexer_decode_unicode_sequence (left_p);

      if (left_chr != lexer_decode_unicode_sequence (right_p))
      {
        return PARSER_FALSE;
      }

      left_p += 6;
      right_p += 6;
      count += util_get_utf8_length (left_chr);
      continue;
    }

    /* One character is encoded as unicode sequence. */
    if (*right_p == '\\')
    {
      /* The pointers can be swapped. */
      const uint8_t *swap_p = left_p;
      left_p = right_p;
      right_p = swap_p;
    }

    utf8_len = util_to_utf8_bytes (utf8_buf, lexer_decode_unicode_sequence (left_p));
    PARSER_ASSERT (utf8_len > 0);
    count -= utf8_len;
    offset = 0;

    do
    {
      if (utf8_buf[offset] != *right_p++)
      {
        return PARSER_FALSE;
      }
      offset++;
    }
    while (offset < utf8_len);

    left_p += 6;
  }
  while (count > 0);

  return PARSER_TRUE;
} /* lexer_compare_identifier_to_current */

/**
 * Compares the current identifier in the context to the parameter identifier
 *
 * Note:
 *   Escape sequences are not allowed.
 *
 * @return non-zero if the input identifiers are the same
 */
int
lexer_compare_raw_identifier_to_current (parser_context_t *context_p, /**< context */
                                         const char *right_ident_p, /**< identifier */
                                         size_t right_ident_length) /**< identifier length */
{
  lexer_lit_location_t *left_ident_p = &context_p->token.lit_location;

  if (left_ident_p->length != right_ident_length || left_ident_p->has_escape)
  {
    return 0;
  }

  return memcmp (left_ident_p->char_p, right_ident_p, right_ident_length) == 0;
} /* lexer_compare_raw_identifier_to_current */

/**
 * Convert binary lvalue token to binary token
 * e.g. += -> +
 *      ^= -> ^
 *
 * @return binary token
 */
uint8_t
lexer_convert_binary_lvalue_token_to_binary (uint8_t token) /**< binary lvalue token */
{
  PARSER_ASSERT (LEXER_IS_BINARY_LVALUE_TOKEN (token));
  PARSER_ASSERT (token != LEXER_ASSIGN);

  if (token <= LEXER_ASSIGN_MODULO)
  {
    return (uint8_t) (LEXER_ADD + (token - LEXER_ASSIGN_ADD));
  }

  if (token <= LEXER_ASSIGN_UNS_RIGHT_SHIFT)
  {
    return (uint8_t) (LEXER_LEFT_SHIFT + (token - LEXER_ASSIGN_LEFT_SHIFT));
  }

 switch (token)
  {
    case LEXER_ASSIGN_BIT_AND:
    {
      return LEXER_BIT_AND;
    }
    case LEXER_ASSIGN_BIT_OR:
    {
      return LEXER_BIT_OR;
    }
    default:
    {
      PARSER_ASSERT (token == LEXER_ASSIGN_BIT_XOR);
      return LEXER_BIT_XOR;
    }
  }
} /* lexer_convert_binary_lvalue_token_to_binary */
