/*
 * Copyright (c) 2009-2022, Google LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Google LLC nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL Google LLC BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "upb/io/tokenizer.h"

#include <stdio.h>

#include "upb/io/strtod.h"

// Must be included last.
#include "upb/port_def.inc"

typedef enum {
  // Started a line comment.
  LINE_COMMENT,

  // Started a block comment.
  BLOCK_COMMENT,

  // Consumed a slash, then realized it wasn't a comment.  current_ has
  // been filled in with a slash token.  The caller should return it.
  SLASH_NOT_COMMENT,

  // We do not appear to be starting a comment here.
  NO_COMMENT
} NextCommentStatus;

#define CHARACTER_CLASS(NAME, EXPRESSION) \
  static bool NAME(char c) { return EXPRESSION; }

CHARACTER_CLASS(Whitespace, c == ' ' || c == '\n' || c == '\t' || c == '\r' ||
                                c == '\v' || c == '\f');
CHARACTER_CLASS(WhitespaceNoNewline,
                c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f');

CHARACTER_CLASS(Unprintable, c<' ' && c> '\0');

CHARACTER_CLASS(Digit, '0' <= c && c <= '9');
CHARACTER_CLASS(OctalDigit, '0' <= c && c <= '7');
CHARACTER_CLASS(HexDigit, ('0' <= c && c <= '9') || ('a' <= c && c <= 'f') ||
                              ('A' <= c && c <= 'F'));

CHARACTER_CLASS(Letter,
                ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || (c == '_'));

CHARACTER_CLASS(Alphanumeric, ('a' <= c && c <= 'z') ||
                                  ('A' <= c && c <= 'Z') ||
                                  ('0' <= c && c <= '9') || (c == '_'));

CHARACTER_CLASS(Escape, c == 'a' || c == 'b' || c == 'f' || c == 'n' ||
                            c == 'r' || c == 't' || c == 'v' || c == '\\' ||
                            c == '?' || c == '\'' || c == '\"');

#undef CHARACTER_CLASS

// Since we count columns we need to interpret tabs somehow.  We'll take
// the standard 8-character definition for lack of any way to do better.
static const int kTabWidth = 8;

// Given a char, interpret it as a numeric digit and return its value.
// This supports any number base up to 36.
// Represents integer values of digits.
// Uses 36 to indicate an invalid character since we support
// bases up to 36.
static const int8_t kAsciiToInt[256] = {
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,  // 00-0F
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,  // 10-1F
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,  // ' '-'/'
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,                           // '0'-'9'
    36, 36, 36, 36, 36, 36, 36,                                      // ':'-'@'
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,  // 'A'-'P'
    26, 27, 28, 29, 30, 31, 32, 33, 34, 35,                          // 'Q'-'Z'
    36, 36, 36, 36, 36, 36,                                          // '['-'`'
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,  // 'a'-'p'
    26, 27, 28, 29, 30, 31, 32, 33, 34, 35,                          // 'q'-'z'
    36, 36, 36, 36, 36,                                              // '{'-DEL
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,  // 80-8F
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,  // 90-9F
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,  // A0-AF
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,  // B0-BF
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,  // C0-CF
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,  // D0-DF
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,  // E0-EF
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,  // F0-FF
};

// Maybe this belongs in port_def.inc instead?
UPB_INLINE uint32_t ghtonl(uint32_t host_int) {
  return (((host_int & (uint32_t)0xFF) << 24) |
          ((host_int & (uint32_t)0xFF00) << 8) |
          ((host_int & (uint32_t)0xFF0000) >> 8) |
          ((host_int & (uint32_t)0xFF000000) >> 24));
}

UPB_INLINE int DigitValue(char digit) { return kAsciiToInt[digit & 0xFF]; }

// Inline because it's only used in one place.
UPB_INLINE char TranslateEscape(char c) {
  switch (c) {
    case 'a':
      return '\a';
    case 'b':
      return '\b';
    case 'f':
      return '\f';
    case 'n':
      return '\n';
    case 'r':
      return '\r';
    case 't':
      return '\t';
    case 'v':
      return '\v';
    case '\\':
      return '\\';
    case '?':
      return '\?';  // Trigraphs = :(
    case '\'':
      return '\'';
    case '"':
      return '\"';

    // We expect escape sequences to have been validated separately.
    default:
      return '?';
  }
}

// ===================================================================

// Structure representing a token read from the token stream.
typedef struct {
  upb_TokenType type;

  // "line" and "column" specify the position of the first character of
  // the token within the input stream. They are zero-based.
  int line;
  int column;
  int end_column;

  // The exact text of the token as it appeared in the input.
  // e.g. tokens of TYPE_STRING will still be escaped and in quotes.
  upb_String text;
} upb_Token;

static upb_Token* upb_Token_Init(upb_Token* t, upb_Arena* arena) {
  upb_String_Init(&t->text, arena);

  t->type = UPB_TOKENTYPE_START;
  t->line = 0;
  t->column = 0;
  t->end_column = 0;
  return t;
}

static void upb_Token_Copy(upb_Token* des, const upb_Token* src) {
  upb_String_Copy(&des->text, &src->text);

  des->type = src->type;
  des->line = src->line;
  des->column = src->column;
  des->end_column = src->end_column;
}

// ===================================================================

struct upb_Tokenizer {
  upb_Token current;
  upb_Token previous;

  upb_ZeroCopyInputStream* input;
  upb_ErrorCollector* error_collector;

  upb_Arena* arena;

  char current_char;   // == buffer_[buffer_pos_], updated by NextChar().
  const char* buffer;  // Current buffer returned from input_.
  size_t buffer_size;  // Size of buffer_.
  size_t buffer_pos;   // Current position within the buffer.
  bool read_error;     // Did we previously encounter a read error?

  // Line and column number of current_char_ within the whole input stream.
  int line;

  // By "column number", the proto compiler refers to a count of the number
  // of bytes before a given byte, except that a tab character advances to
  // the next multiple of 8 bytes.  Note in particular that column numbers
  // are zero-based, while many user interfaces use one-based column numbers.
  int column;

  // String to which text should be appended as we advance through it.
  // Call RecordTo(&str) to start recording and StopRecording() to stop.
  // E.g. StartToken() calls RecordTo(&current_.text).  record_start_ is the
  // position within the current buffer where recording started.
  upb_String* record_target;
  int record_start;

  // Options.
  bool allow_f_after_float;
  upb_CommentStyle comment_style;
  bool require_space_after_number;
  bool allow_multiline_strings;
  bool report_whitespace;
  bool report_newlines;
};

void upb_Tokenizer_SetAllowFAfterFloat(upb_Tokenizer* t, bool allow) {
  t->allow_f_after_float = allow;
}

void upb_Tokenizer_SetCommentStyle(upb_Tokenizer* t, upb_CommentStyle style) {
  t->comment_style = style;
}

void upb_Tokenizer_SetRequireSpaceAfterNumber(upb_Tokenizer* t, bool require) {
  t->require_space_after_number = require;
}

void upb_Tokenizer_SetAllowMultilineStrings(upb_Tokenizer* t, bool allow) {
  t->allow_multiline_strings = allow;
}

bool upb_Tokenizer_ReportWhitespace(const upb_Tokenizer* t) {
  return t->report_whitespace;
}

// Note: `set_report_whitespace(false)` implies `set_report_newlines(false)`.
void upb_Tokenizer_SetReportWhitespace(upb_Tokenizer* t, bool report) {
  t->report_whitespace = report;
  t->report_newlines &= report;
}

// If true, newline tokens are reported by Next().
bool upb_Tokenizer_Reportnewlines(const upb_Tokenizer* t) {
  return t->report_newlines;
}

// Note: `set_report_newlines(true)` implies `set_report_whitespace(true)`.
void upb_Tokenizer_SetReportNewlines(upb_Tokenizer* t, bool report) {
  t->report_newlines = report;
  t->report_whitespace |= report;  // enable report_whitespace if necessary
}

// -------------------------------------------------------------------
// Internal helpers.

// Convenience method to add an error at the current line and column.
static void AddError(upb_Tokenizer* t, const char* message) {
  t->error_collector->AddError(t->line, t->column, message,
                               t->error_collector->context);
}

// Read a new buffer from the input.
static void Refresh(upb_Tokenizer* t) {
  if (t->read_error) {
    t->current_char = '\0';
    return;
  }

  // If we're in a token, append the rest of the buffer to it.
  if (t->record_target != NULL && t->record_start < t->buffer_size) {
    upb_String_Append(t->record_target, t->buffer + t->record_start,
                      t->buffer_size - t->record_start);
    t->record_start = 0;
  }

  t->buffer = NULL;
  t->buffer_pos = 0;

  upb_Status status;
  const void* data =
      upb_ZeroCopyInputStream_Next(t->input, &t->buffer_size, &status);

  if (t->buffer_size > 0) {
    t->buffer = data;
    t->current_char = t->buffer[0];
  } else {
    // end of stream (or read error)
    t->buffer_size = 0;
    t->read_error = true;
    t->current_char = '\0';
  }
}

// Consume this character and advance to the next one.
static void NextChar(upb_Tokenizer* t) {
  // Update our line and column counters based on the character being
  // consumed.
  if (t->current_char == '\n') {
    t->line++;
    t->column = 0;
  } else if (t->current_char == '\t') {
    t->column += kTabWidth - t->column % kTabWidth;
  } else {
    t->column++;
  }

  // Advance to the next character.
  t->buffer_pos++;
  if (t->buffer_pos < t->buffer_size) {
    t->current_char = t->buffer[t->buffer_pos];
  } else {
    Refresh(t);
  }
}

UPB_INLINE void RecordTo(upb_Tokenizer* t, upb_String* target) {
  t->record_target = target;
  t->record_start = t->buffer_pos;
}

UPB_INLINE void StopRecording(upb_Tokenizer* t) {
  upb_String_Append(t->record_target, t->buffer + t->record_start,
                    t->buffer_pos - t->record_start);
  t->record_target = NULL;
  t->record_start = -1;
}

// Called when the current character is the first character of a new
// token (not including whitespace or comments).
UPB_INLINE void StartToken(upb_Tokenizer* t) {
  t->current.type = UPB_TOKENTYPE_START;
  upb_String_Clear(&t->current.text);
  t->current.line = t->line;
  t->current.column = t->column;
  RecordTo(t, &t->current.text);
}

// Called when the current character is the first character after the
// end of the last token.  After this returns, current_.text will
// contain all text consumed since StartToken() was called.
UPB_INLINE void EndToken(upb_Tokenizer* t) {
  StopRecording(t);
  t->current.end_column = t->column;
}

// -----------------------------------------------------------------
// These helper methods make the parsing code more readable.
// The "character classes" referred to are defined at the top of the file.
// The method returns true if c is a member of this "class", like "Letter"
// or "Digit".

// Returns true if the current character is of the given character
// class, but does not consume anything.
UPB_INLINE bool LookingAt(const upb_Tokenizer* t, bool (*f)(char)) {
  return f(t->current_char);
}

// If the current character is in the given class, consume it and return true.
// Otherwise return false.
UPB_INLINE bool TryConsumeOne(upb_Tokenizer* t, bool (*f)(char)) {
  if (f(t->current_char)) {
    NextChar(t);
    return true;
  } else {
    return false;
  }
}

// Like above, but try to consume the specific character indicated.
UPB_INLINE bool TryConsume(upb_Tokenizer* t, char c) {
  if (t->current_char == c) {
    NextChar(t);
    return true;
  } else {
    return false;
  }
}

// Consume zero or more of the given character class.
UPB_INLINE void ConsumeZeroOrMore(upb_Tokenizer* t, bool (*f)(char)) {
  while (f(t->current_char)) {
    NextChar(t);
  }
}

// Consume one or more of the given character class or log the given
// error message.
UPB_INLINE void ConsumeOneOrMore(upb_Tokenizer* t, bool (*f)(char),
                                 const char* err_msg) {
  if (!f(t->current_char)) {
    AddError(t, err_msg);
  } else {
    do {
      NextChar(t);
    } while (f(t->current_char));
  }
}

// -----------------------------------------------------------------
// The following four methods are used to consume tokens of specific
// types.  They are actually used to consume all characters *after*
// the first, since the calling function consumes the first character
// in order to decide what kind of token is being read.

// Read and consume a string, ending when the given delimiter is consumed.
static void ConsumeString(upb_Tokenizer* t, char delimiter) {
  while (true) {
    switch (t->current_char) {
      case '\0':
        AddError(t, "Unexpected end of string.");
        return;

      case '\n': {
        if (!t->allow_multiline_strings) {
          AddError(t, "String literals cannot cross line boundaries.");
          return;
        }
        NextChar(t);
        break;
      }

      case '\\': {
        // An escape sequence.
        NextChar(t);
        if (TryConsumeOne(t, Escape)) {
          // Valid escape sequence.
        } else if (TryConsumeOne(t, OctalDigit)) {
          // Possibly followed by two more octal digits, but these will
          // just be consumed by the main loop anyway so we don't need
          // to do so explicitly here.
        } else if (TryConsume(t, 'x')) {
          if (!TryConsumeOne(t, HexDigit)) {
            AddError(t, "Expected hex digits for escape sequence.");
          }
          // Possibly followed by another hex digit, but again we don't care.
        } else if (TryConsume(t, 'u')) {
          if (!TryConsumeOne(t, HexDigit) || !TryConsumeOne(t, HexDigit) ||
              !TryConsumeOne(t, HexDigit) || !TryConsumeOne(t, HexDigit)) {
            AddError(t, "Expected four hex digits for \\u escape sequence.");
          }
        } else if (TryConsume(t, 'U')) {
          // We expect 8 hex digits; but only the range up to 0x10ffff is
          // legal.
          if (!TryConsume(t, '0') || !TryConsume(t, '0') ||
              !(TryConsume(t, '0') || TryConsume(t, '1')) ||
              !TryConsumeOne(t, HexDigit) || !TryConsumeOne(t, HexDigit) ||
              !TryConsumeOne(t, HexDigit) || !TryConsumeOne(t, HexDigit) ||
              !TryConsumeOne(t, HexDigit)) {
            AddError(t,
                     "Expected eight hex digits up to 10ffff for \\U escape "
                     "sequence");
          }
        } else {
          AddError(t, "Invalid escape sequence in string literal.");
        }
        break;
      }

      default: {
        if (t->current_char == delimiter) {
          NextChar(t);
          return;
        }
        NextChar(t);
        break;
      }
    }
  }
}

// Read and consume a number, returning TYPE_FLOAT or TYPE_INTEGER depending
// on what was read.  This needs to know if the first characer was a zero in
// order to correctly recognize hex and octal numbers.  It also needs to know
// whether the first character was a '.' to parse floating point correctly.
static upb_TokenType ConsumeNumber(upb_Tokenizer* t, bool started_with_zero,
                                   bool started_with_dot) {
  bool is_float = false;

  if (started_with_zero && (TryConsume(t, 'x') || TryConsume(t, 'X'))) {
    // A hex number (started with "0x").
    ConsumeOneOrMore(t, HexDigit, "\"0x\" must be followed by hex digits.");

  } else if (started_with_zero && LookingAt(t, Digit)) {
    // An octal number (had a leading zero).
    ConsumeZeroOrMore(t, OctalDigit);
    if (LookingAt(t, Digit)) {
      AddError(t, "Numbers starting with leading zero must be in octal.");
      ConsumeZeroOrMore(t, Digit);
    }

  } else {
    // A decimal number.
    if (started_with_dot) {
      is_float = true;
      ConsumeZeroOrMore(t, Digit);
    } else {
      ConsumeZeroOrMore(t, Digit);

      if (TryConsume(t, '.')) {
        is_float = true;
        ConsumeZeroOrMore(t, Digit);
      }
    }

    if (TryConsume(t, 'e') || TryConsume(t, 'E')) {
      is_float = true;
      if (!TryConsume(t, '-')) TryConsume(t, '+');
      ConsumeOneOrMore(t, Digit, "\"e\" must be followed by exponent.");
    }

    if (t->allow_f_after_float && (TryConsume(t, 'f') || TryConsume(t, 'F'))) {
      is_float = true;
    }
  }

  if (LookingAt(t, Letter) && t->require_space_after_number) {
    AddError(t, "Need space between number and identifier.");
  } else if (t->current_char == '.') {
    if (is_float) {
      AddError(
          t, "Already saw decimal point or exponent; can't have another one.");
    } else {
      AddError(t, "Hex and octal numbers must be integers.");
    }
  }

  return is_float ? UPB_TOKENTYPE_FLOAT : UPB_TOKENTYPE_INTEGER;
}

// Consume the rest of a line.
static void ConsumeLineComment(upb_Tokenizer* t, upb_String* content) {
  if (content != NULL) RecordTo(t, content);

  while (t->current_char != '\0' && t->current_char != '\n') {
    NextChar(t);
  }
  TryConsume(t, '\n');

  if (content != NULL) StopRecording(t);
}

static void ConsumeBlockComment(upb_Tokenizer* t, upb_String* content) {
  const int start_line = t->line;
  const int start_column = t->column - 2;

  if (content != NULL) RecordTo(t, content);

  while (true) {
    while (t->current_char != '\0' && t->current_char != '*' &&
           t->current_char != '/' && t->current_char != '\n') {
      NextChar(t);
    }

    if (TryConsume(t, '\n')) {
      if (content != NULL) StopRecording(t);

      // Consume leading whitespace and asterisk;
      ConsumeZeroOrMore(t, WhitespaceNoNewline);
      if (TryConsume(t, '*')) {
        if (TryConsume(t, '/')) {
          // End of comment.
          break;
        }
      }

      if (content != NULL) RecordTo(t, content);
    } else if (TryConsume(t, '*') && TryConsume(t, '/')) {
      // End of comment.
      if (content != NULL) {
        StopRecording(t);
        // Strip trailing "*/".
        upb_String_Erase(content, upb_String_Size(content) - 2, 2);
      }
      break;
    } else if (TryConsume(t, '/') && t->current_char == '*') {
      // Note:  We didn't consume the '*' because if there is a '/' after it
      //   we want to interpret that as the end of the comment.
      AddError(
          t, "\"/*\" inside block comment.  Block comments cannot be nested.");
    } else if (t->current_char == '\0') {
      AddError(t, "End-of-file inside block comment.");
      t->error_collector->AddError(start_line, start_column,
                                   "  Comment started here.",
                                   t->error_collector->context);
      if (content != NULL) StopRecording(t);
      break;
    }
  }
}

// If we're at the start of a new comment, consume it and return what kind
// of comment it is.
static NextCommentStatus TryConsumeCommentStart(upb_Tokenizer* t) {
  if (t->comment_style == UPB_COMMENT_STYLE_CPP && TryConsume(t, '/')) {
    if (TryConsume(t, '/')) {
      return LINE_COMMENT;
    } else if (TryConsume(t, '*')) {
      return BLOCK_COMMENT;
    } else {
      // Oops, it was just a slash.  Return it.
      t->current.type = UPB_TOKENTYPE_SYMBOL;
      upb_String_Assign(&t->current.text, "/", 1);
      t->current.line = t->line;
      t->current.column = t->column - 1;
      t->current.end_column = t->column;
      return SLASH_NOT_COMMENT;
    }
  } else if (t->comment_style == UPB_COMMENT_STYLE_SH && TryConsume(t, '#')) {
    return LINE_COMMENT;
  } else {
    return NO_COMMENT;
  }
}

// If we're looking at a TYPE_WHITESPACE token and `report_whitespace` is true,
// consume it and return true.
static bool TryConsumeWhitespace(upb_Tokenizer* t) {
  if (t->report_newlines) {
    if (TryConsumeOne(t, WhitespaceNoNewline)) {
      ConsumeZeroOrMore(t, WhitespaceNoNewline);
      t->current.type = UPB_TOKENTYPE_WHITESPACE;
      return true;
    }
    return false;
  }
  if (TryConsumeOne(t, Whitespace)) {
    ConsumeZeroOrMore(t, Whitespace);
    t->current.type = UPB_TOKENTYPE_WHITESPACE;
    return t->report_whitespace;
  }
  return false;
}

// If we're looking at a TYPE_NEWLINE token and `report_newlines` is true,
// consume it and return true.
static bool TryConsumeNewline(upb_Tokenizer* t) {
  if (!t->report_whitespace || !t->report_newlines) {
    return false;
  }
  if (TryConsume(t, '\n')) {
    t->current.type = UPB_TOKENTYPE_NEWLINE;
    return true;
  }
  return false;
}

// -------------------------------------------------------------------

int upb_Tokenizer_CurrentColumn(const upb_Tokenizer* t) {
  return t->current.column;
}

int upb_Tokenizer_CurrentEndColumn(const upb_Tokenizer* t) {
  return t->current.end_column;
}

int upb_Tokenizer_CurrentLine(const upb_Tokenizer* t) {
  return t->current.line;
}

int upb_Tokenizer_CurrentTextSize(const upb_Tokenizer* t) {
  return t->current.text.size_;
}

const char* upb_Tokenizer_CurrentTextData(const upb_Tokenizer* t) {
  return t->current.text.data_;
}

upb_TokenType upb_Tokenizer_CurrentType(const upb_Tokenizer* t) {
  return t->current.type;
}

int upb_Tokenizer_PreviousColumn(const upb_Tokenizer* t) {
  return t->previous.column;
}

int upb_Tokenizer_PreviousEndColumn(const upb_Tokenizer* t) {
  return t->previous.end_column;
}

int upb_Tokenizer_PreviousLine(const upb_Tokenizer* t) {
  return t->previous.line;
}

int upb_Tokenizer_PreviousTextSize(const upb_Tokenizer* t) {
  return t->previous.text.size_;
}

const char* upb_Tokenizer_PreviousTextData(const upb_Tokenizer* t) {
  return t->previous.text.data_;
}

upb_TokenType upb_Tokenizer_PreviousType(const upb_Tokenizer* t) {
  return t->previous.type;
}

bool upb_Tokenizer_Next(upb_Tokenizer* t) {
  upb_Token_Copy(&t->previous, &t->current);

  while (!t->read_error) {
    StartToken(t);
    bool report_token = TryConsumeWhitespace(t) || TryConsumeNewline(t);
    EndToken(t);
    if (report_token) {
      return true;
    }

    switch (TryConsumeCommentStart(t)) {
      case LINE_COMMENT:
        ConsumeLineComment(t, NULL);
        continue;
      case BLOCK_COMMENT:
        ConsumeBlockComment(t, NULL);
        continue;
      case SLASH_NOT_COMMENT:
        return true;
      case NO_COMMENT:
        break;
    }

    // Check for EOF before continuing.
    if (t->read_error) break;

    if (LookingAt(t, Unprintable) || t->current_char == '\0') {
      AddError(t, "Invalid control characters encountered in text.");
      NextChar(t);
      // Skip more unprintable characters, too.  But, remember that '\0' is
      // also what current_char_ is set to after EOF / read error.  We have
      // to be careful not to go into an infinite loop of trying to consume
      // it, so make sure to check read_error_ explicitly before consuming
      // '\0'.
      while (TryConsumeOne(t, Unprintable) ||
             (!t->read_error && TryConsume(t, '\0'))) {
        // Ignore.
      }

    } else {
      // Reading some sort of token.
      StartToken(t);

      if (TryConsumeOne(t, Letter)) {
        ConsumeZeroOrMore(t, Alphanumeric);
        t->current.type = UPB_TOKENTYPE_IDENTIFIER;
      } else if (TryConsume(t, '0')) {
        t->current.type = ConsumeNumber(t, true, false);
      } else if (TryConsume(t, '.')) {
        // This could be the beginning of a floating-point number, or it could
        // just be a '.' symbol.

        if (TryConsumeOne(t, Digit)) {
          // It's a floating-point number.
          if (t->previous.type == UPB_TOKENTYPE_IDENTIFIER &&
              t->current.line == t->previous.line &&
              t->current.column == t->previous.end_column) {
            // We don't accept syntax like "blah.123".
            t->error_collector->AddError(
                t->line, t->column - 2,
                "Need space between identifier and decimal point.",
                t->error_collector->context);
          }
          t->current.type = ConsumeNumber(t, false, true);
        } else {
          t->current.type = UPB_TOKENTYPE_SYMBOL;
        }
      } else if (TryConsumeOne(t, Digit)) {
        t->current.type = ConsumeNumber(t, false, false);
      } else if (TryConsume(t, '\"')) {
        ConsumeString(t, '\"');
        t->current.type = UPB_TOKENTYPE_STRING;
      } else if (TryConsume(t, '\'')) {
        ConsumeString(t, '\'');
        t->current.type = UPB_TOKENTYPE_STRING;
      } else {
        // Check if the high order bit is set.
        if (t->current_char & 0x80) {
          char temp[80];
          snprintf(temp, sizeof temp, "Interpreting non ascii codepoint %d.",
                   (uint8_t)t->current_char);
          t->error_collector->AddError(t->line, t->column, temp,
                                       t->error_collector->context);
        }
        NextChar(t);
        t->current.type = UPB_TOKENTYPE_SYMBOL;
      }

      EndToken(t);
      return true;
    }
  }

  // EOF
  t->current.type = UPB_TOKENTYPE_END;
  upb_String_Clear(&t->current.text);
  t->current.line = t->line;
  t->current.column = t->column;
  t->current.end_column = t->column;
  return false;
}

// -------------------------------------------------------------------
// Token-parsing helpers.  Remember that these don't need to report
// errors since any errors should already have been reported while
// tokenizing.  Also, these can assume that whatever text they
// are given is text that the tokenizer actually parsed as a token
// of the given type.

bool upb_Parse_Integer(const char* text, uint64_t max_value, uint64_t* output) {
  // We can't just use strtoull() because (a) it accepts negative numbers,
  // (b) We want additional range checks, (c) it reports overflows via errno.

  const char* ptr = text;
  int base = 10;
  uint64_t overflow_if_mul_base = (UINT64_MAX / 10) + 1;
  if (ptr[0] == '0') {
    if (ptr[1] == 'x' || ptr[1] == 'X') {
      // This is hex.
      base = 16;
      overflow_if_mul_base = (UINT64_MAX / 16) + 1;
      ptr += 2;
    } else {
      // This is octal.
      base = 8;
      overflow_if_mul_base = (UINT64_MAX / 8) + 1;
    }
  }

  uint64_t result = 0;
  // For all the leading '0's, and also the first non-zero character, we
  // don't need to multiply.
  while (*ptr != '\0') {
    int digit = DigitValue(*ptr++);
    if (digit >= base) {
      // The token provided by Tokenizer is invalid. i.e., 099 is an invalid
      // token, but Tokenizer still think it's integer.
      return false;
    }
    if (digit != 0) {
      result = digit;
      break;
    }
  }
  for (; *ptr != '\0'; ptr++) {
    int digit = DigitValue(*ptr);
    if (digit < 0 || digit >= base) {
      // The token provided by Tokenizer is invalid. i.e., 099 is an invalid
      // token, but Tokenizer still think it's integer.
      return false;
    }
    if (result >= overflow_if_mul_base) {
      // We know the multiply we're about to do will overflow, so exit now.
      return false;
    }
    // We know that result * base won't overflow, but adding digit might...
    result = result * base + digit;
    // C++ guarantees defined "wrap" semantics when unsigned integer
    // operations overflow, making this a fast way to check if adding
    // digit made result overflow, and thus, wrap around.
    if (result < (uint64_t)base) return false;
  }
  if (result > max_value) return false;

  *output = result;
  return true;
}

double upb_Parse_Float(const char* text) {
  char* end;
  double result = NoLocaleStrtod(text, &end);

  // "1e" is not a valid float, but if the tokenizer reads it, it will
  // report an error but still return it as a valid token.  We need to
  // accept anything the tokenizer could possibly return, error or not.
  if (*end == 'e' || *end == 'E') {
    ++end;
    if (*end == '-' || *end == '+') ++end;
  }

  // If the Tokenizer had allow_f_after_float_ enabled, the float may be
  // suffixed with the letter 'f'.
  if (*end == 'f' || *end == 'F') {
    ++end;
  }

  if ((end - text) != strlen(text) || *text == '-') {
    fprintf(stderr,
            "upb_Parse_Float() passed text that could not have"
            " been tokenized as a float: %s\n",
            text);
    UPB_ASSERT(0);
  }
  return result;
}

// Helper to append a Unicode code point to a string as UTF8, without bringing
// in any external dependencies.
static void AppendUTF8(uint32_t code_point, upb_String* output) {
  uint32_t tmp = 0;
  int len = 0;
  if (code_point <= 0x7f) {
    tmp = code_point;
    len = 1;
  } else if (code_point <= 0x07ff) {
    tmp = 0x0000c080 | ((code_point & 0x07c0) << 2) | (code_point & 0x003f);
    len = 2;
  } else if (code_point <= 0xffff) {
    tmp = 0x00e08080 | ((code_point & 0xf000) << 4) |
          ((code_point & 0x0fc0) << 2) | (code_point & 0x003f);
    len = 3;
  } else if (code_point <= 0x10ffff) {
    tmp = 0xf0808080 | ((code_point & 0x1c0000) << 6) |
          ((code_point & 0x03f000) << 4) | ((code_point & 0x000fc0) << 2) |
          (code_point & 0x003f);
    len = 4;
  } else {
    char temp[24];
    // ConsumeString permits hex values up to 0x1FFFFF, and FetchUnicodePoint
    // doesn't perform a range check.
    // Unicode code points end at 0x10FFFF, so this is out-of-range.
    const int size = snprintf(temp, sizeof temp, "\\U%08x", code_point);
    upb_String_Append(output, temp, size);
    return;
  }
  tmp = ghtonl(tmp);
  upb_String_Append(output, ((char*)&tmp) + (sizeof tmp) - len, len);
}

// Try to read <len> hex digits from ptr, and stuff the numeric result into
// *result. Returns true if that many digits were successfully consumed.
static bool ReadHexDigits(const char* ptr, int len, uint32_t* result) {
  *result = 0;
  if (len == 0) return false;
  for (const char* end = ptr + len; ptr < end; ++ptr) {
    if (*ptr == '\0') return false;
    *result = (*result << 4) + DigitValue(*ptr);
  }
  return true;
}

// Handling UTF-16 surrogate pairs. UTF-16 encodes code points in the range
// 0x10000...0x10ffff as a pair of numbers, a head surrogate followed by a trail
// surrogate. These numbers are in a reserved range of Unicode code points, so
// if we encounter such a pair we know how to parse it and convert it into a
// single code point.
static const uint32_t kMinHeadSurrogate = 0xd800;
static const uint32_t kMaxHeadSurrogate = 0xdc00;
static const uint32_t kMinTrailSurrogate = 0xdc00;
static const uint32_t kMaxTrailSurrogate = 0xe000;

UPB_INLINE bool IsHeadSurrogate(uint32_t code_point) {
  return (code_point >= kMinHeadSurrogate) && (code_point < kMaxHeadSurrogate);
}

UPB_INLINE bool IsTrailSurrogate(uint32_t code_point) {
  return (code_point >= kMinTrailSurrogate) &&
         (code_point < kMaxTrailSurrogate);
}

// Combine a head and trail surrogate into a single Unicode code point.
static uint32_t AssembleUTF16(uint32_t head_surrogate,
                              uint32_t trail_surrogate) {
  UPB_ASSERT(IsHeadSurrogate(head_surrogate));
  UPB_ASSERT(IsTrailSurrogate(trail_surrogate));
  return 0x10000 + (((head_surrogate - kMinHeadSurrogate) << 10) |
                    (trail_surrogate - kMinTrailSurrogate));
}

// Convert the escape sequence parameter to a number of expected hex digits.
UPB_INLINE int UnicodeLength(char key) {
  if (key == 'u') return 4;
  if (key == 'U') return 8;
  return 0;
}

// Given a pointer to the 'u' or 'U' starting a Unicode escape sequence, attempt
// to parse that sequence. On success, returns a pointer to the first char
// beyond that sequence, and fills in *code_point. On failure, returns ptr
// itself.
static const char* FetchUnicodePoint(const char* ptr, uint32_t* code_point) {
  const char* p = ptr;
  // Fetch the code point.
  const int len = UnicodeLength(*p++);
  if (!ReadHexDigits(p, len, code_point)) return ptr;
  p += len;

  // Check if the code point we read is a "head surrogate." If so, then we
  // expect it to be immediately followed by another code point which is a valid
  // "trail surrogate," and together they form a UTF-16 pair which decodes into
  // a single Unicode point. Trail surrogates may only use \u, not \U.
  if (IsHeadSurrogate(*code_point) && *p == '\\' && *(p + 1) == 'u') {
    uint32_t trail_surrogate;
    if (ReadHexDigits(p + 2, 4, &trail_surrogate) &&
        IsTrailSurrogate(trail_surrogate)) {
      *code_point = AssembleUTF16(*code_point, trail_surrogate);
      p += 6;
    }
    // If this failed, then we just emit the head surrogate as a code point.
    // It's bogus, but so is the string.
  }

  return p;
}

// The text string must begin and end with single or double quote characters.
void upb_Parse_StringAppend(const char* text, upb_String* output) {
  const size_t size = strlen(text);

  // Reminder: text[0] is always a quote character.  (If text is
  // empty, it's invalid, so we'll just return).
  if (size == 0) {
    fprintf(stderr,
            "Tokenizer::ParseStringAppend() passed text that could not"
            " have been tokenized as a string: %s",
            text);
    UPB_ASSERT(0);
    return;
  }

  // Reserve room for new string.
  const size_t new_len = size + upb_String_Size(output);
  upb_String_Reserve(output, new_len);

  // Loop through the string copying characters to "output" and
  // interpreting escape sequences.  Note that any invalid escape
  // sequences or other errors were already reported while tokenizing.
  // In this case we do not need to produce valid results.
  for (const char* ptr = text + 1; *ptr != '\0'; ptr++) {
    if (*ptr == '\\' && ptr[1] != '\0') {
      // An escape sequence.
      ++ptr;

      if (OctalDigit(*ptr)) {
        // An octal escape.  May one, two, or three digits.
        int code = DigitValue(*ptr);
        if (OctalDigit(ptr[1])) {
          ++ptr;
          code = code * 8 + DigitValue(*ptr);
        }
        if (OctalDigit(ptr[1])) {
          ++ptr;
          code = code * 8 + DigitValue(*ptr);
        }
        upb_String_PushBack(output, (char)code);

      } else if (*ptr == 'x') {
        // A hex escape.  May zero, one, or two digits.  (The zero case
        // will have been caught as an error earlier.)
        int code = 0;
        if (HexDigit(ptr[1])) {
          ++ptr;
          code = DigitValue(*ptr);
        }
        if (HexDigit(ptr[1])) {
          ++ptr;
          code = code * 16 + DigitValue(*ptr);
        }
        upb_String_PushBack(output, (char)code);

      } else if (*ptr == 'u' || *ptr == 'U') {
        uint32_t unicode;
        const char* end = FetchUnicodePoint(ptr, &unicode);
        if (end == ptr) {
          // Failure: Just dump out what we saw, don't try to parse it.
          upb_String_PushBack(output, *ptr);
        } else {
          AppendUTF8(unicode, output);
          ptr = end - 1;  // Because we're about to ++ptr.
        }
      } else {
        // Some other escape code.
        upb_String_PushBack(output, TranslateEscape(*ptr));
      }

    } else if (*ptr == text[0] && ptr[1] == '\0') {
      // Ignore final quote matching the starting quote.
    } else {
      upb_String_PushBack(output, *ptr);
    }
  }
}

UPB_INLINE bool AllInClass(bool (*f)(char), const char* text, int size) {
  for (int i = 0; i < size; i++) {
    if (!f(text[i])) return false;
  }
  return true;
}

bool upb_Tokenizer_IsIdentifier(const char* text, int size) {
  // Mirrors IDENTIFIER definition in Tokenizer::Next() above.
  if (size == 0) return false;
  if (!Letter(text[0])) return false;
  if (!AllInClass(Alphanumeric, text + 1, size - 1)) return false;
  return true;
}

upb_Tokenizer* upb_Tokenizer_New(const void* data, size_t size,
                                 upb_ZeroCopyInputStream* input,
                                 upb_ErrorCollector* error_collector,
                                 upb_Arena* arena) {
  upb_Tokenizer* t = upb_Arena_Malloc(arena, sizeof(upb_Tokenizer));
  if (!t) return NULL;

  t->input = input;
  t->error_collector = error_collector;
  t->arena = arena;
  t->buffer = data;
  t->buffer_size = size;
  t->buffer_pos = 0;
  t->read_error = false;
  t->line = 0;
  t->column = 0;
  t->record_target = NULL;
  t->record_start = -1;
  t->allow_f_after_float = false;
  t->comment_style = UPB_COMMENT_STYLE_CPP;
  t->require_space_after_number = true;
  t->allow_multiline_strings = false;
  t->report_whitespace = false;
  t->report_newlines = false;

  upb_Token_Init(&t->current, arena);
  upb_Token_Init(&t->previous, arena);

  if (size) {
    t->current_char = t->buffer[0];
  } else {
    Refresh(t);
  }
  return t;
}

void upb_Tokenizer_Fini(upb_Tokenizer* t) {
  // If we had any buffer left unread, return it to the underlying stream
  // so that someone else can read it.
  if (t->buffer_size > t->buffer_pos) {
    upb_ZeroCopyInputStream_BackUp(t->input, t->buffer_size - t->buffer_pos);
  }
}
