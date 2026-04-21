/*
 * Copyright (c) 2025 Rizki Rakasiwi <rizkirr.xyz@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * libjson — zero-copy JSON parser (STB-style single-header library)
 *
 * In exactly ONE C file, do:
 *   #define LIBJSON_IMPLEMENTATION
 *   #include "libjson.h"
 *
 * All other files just #include "libjson.h" for declarations only.
 */

#ifndef LIBJSON_H
#define LIBJSON_H

#include <stdbool.h>
#include <stddef.h>
#ifndef JSON_NO_STDIO
#include <stdio.h>
#endif
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const char *data;
  size_t len;
} JsonSlice;

typedef struct {
  JsonSlice array;
  const char *cursor;
} JsonArrayIter;

typedef struct {
  JsonSlice object;
  const char *cursor;
} JsonObjectIter;

/*
 * Build a slice from an existing pointer and length.
 *
 * The slice does not own the memory. The caller must keep `data` alive for as
 * long as the slice is used.
 */
JsonSlice json_from_parts(const char *data, size_t len);

/*
 * Build a slice from a NUL-terminated JSON string.
 *
 * This is a convenience wrapper around `json_from_parts()`.
 */
JsonSlice json_from_cstr(const char *json);

/*
 * Copy a slice into a caller-provided buffer and append a trailing NUL byte.
 *
 * Returns false if `buffer` is NULL, the slice is invalid, or the buffer is
 * too small. This helper is optional and exists for callers that need a
 * conventional C string without using heap allocation.
 */
bool json_slice_copy(JsonSlice slice, char *buffer, size_t buffer_size);

/*
 * Look up `key` with an explicit key length.
 *
 * This avoids recomputing `strlen(key)` in hot lookup paths.
 */
bool json_getn(JsonSlice object, const char *key, size_t key_len,
               JsonSlice *out);

/*
 * Look up `key` in a JSON object slice and return the matched value as a
 * zero-copy slice.
 *
 * String results exclude the surrounding quotes. Object and array results keep
 * their delimiters. Number, boolean, and null results are returned as raw
 * tokens. Returns false if the key is missing or the input is not a valid
 * object-shaped slice.
 */
bool json_get(JsonSlice object, const char *key, JsonSlice *out);

/*
 * Initialize an iterator for a JSON object slice.
 *
 * The iterator walks key/value pairs in-place over the original input buffer.
 */
void json_object_iter_init(JsonSlice object, JsonObjectIter *iter);

/*
 * Return the next object key/value pair as zero-copy slices.
 *
 * Key slices exclude surrounding quotes. Returns true while entries remain.
 * Returns false when the object is exhausted or when invalid input is
 * encountered.
 */
bool json_object_iter_next(JsonObjectIter *iter, JsonSlice *key,
                           JsonSlice *value);

/*
 * Initialize an iterator for a JSON array slice.
 *
 * The iterator does not allocate and walks the array in-place over the
 * original input buffer.
 */
void json_array_iter_init(JsonSlice array, JsonArrayIter *iter);

/*
 * Return the next array item as a zero-copy slice.
 *
 * Returns true while elements remain. Returns false when the array is
 * exhausted or when invalid input is encountered.
 */
bool json_array_iter_next(JsonArrayIter *iter, JsonSlice *out);

/* ========================================================================= */
/*                          JSON WRITER (Serialization)                       */
/* ========================================================================= */

/*
 * JsonWriter — zero-allocation JSON serializer.
 *
 * Writes JSON into a caller-provided buffer. If the buffer runs out of space
 * the overflow flag is set and all subsequent writes become silent no-ops.
 * Check `json_writer_ok()` once after building the document.
 *
 * Comma insertion between values is fully automatic. A bitmask tracks the
 * "needs comma" state at each nesting depth, supporting up to 32 levels with
 * zero extra RAM.
 */
typedef struct {
  char *buf;
  size_t cap;
  size_t pos;
  bool overflow;
  unsigned int depth;
  unsigned int needs_comma;
} JsonWriter;

/*
 * Initialize a writer that targets `buf` with `cap` bytes of space.
 *
 * The buffer is NOT owned by the writer; the caller manages its lifetime.
 * One byte is always reserved for a trailing NUL, so the maximum JSON payload
 * length is `cap - 1`.
 */
void json_writer_init(JsonWriter *w, char *buf, size_t cap);

/*
 * Return the number of JSON bytes written so far (excluding NUL terminator).
 */
size_t json_writer_len(const JsonWriter *w);

/*
 * Return true if the writer has not overflowed.
 */
bool json_writer_ok(const JsonWriter *w);

/*
 * Return the NUL-terminated output buffer. Only valid when `json_writer_ok()`
 * is true.
 */
const char *json_writer_output(const JsonWriter *w);

/*
 * Write a JSON null literal.
 */
void json_write_null(JsonWriter *w);

/*
 * Write a JSON boolean literal.
 */
void json_write_bool(JsonWriter *w, bool value);

/*
 * Write a signed integer as a JSON number.
 */
void json_write_int(JsonWriter *w, long value);

/*
 * Write an unsigned integer as a JSON number.
 */
void json_write_uint(JsonWriter *w, unsigned long value);

/*
 * Write a NUL-terminated C string as a JSON string with proper escaping.
 *
 * Characters that require escaping per RFC 8259 (", \, control characters)
 * are escaped automatically.
 */
void json_write_str(JsonWriter *w, const char *str);

/*
 * Write a `"key":"value"` pair inside an object in a single call.
 *
 * Equivalent to `json_write_key(w, key); json_write_str(w, value);`. Both the
 * key and the value are escaped per RFC 8259. If `value` is NULL the entry is
 * written as `"key":null`.
 */
void json_write_str_kv(JsonWriter *w, const char *key, const char *value);

/*
 * Write a string with explicit length as a JSON string with proper escaping.
 */
void json_write_strn(JsonWriter *w, const char *str, size_t len);

/*
 * Write a `"key":"value"` pair where the value has an explicit length.
 *
 * Equivalent to `json_write_key(w, key); json_write_strn(w, value, len);`. If
 * `value` is NULL the entry is written as `"key":null`.
 */
void json_write_strn_kv(JsonWriter *w, const char *key, const char *value,
                        size_t len);

/*
 * Write pre-formatted raw JSON bytes without any escaping or quoting.
 *
 * Useful for embedding pre-built JSON fragments or custom number formatting
 * (e.g. fixed-point representations).
 */
void json_write_raw(JsonWriter *w, const char *raw, size_t len);

/*
 * Write a `"key":<raw>` pair where the value is pre-formatted raw JSON bytes.
 */
void json_write_raw_kv(JsonWriter *w, const char *key, const char *raw,
                       size_t len);

/*
 * Write a `"key":null` pair inside an object in a single call.
 */
void json_write_null_kv(JsonWriter *w, const char *key);

/*
 * Write a `"key":<true|false>` pair inside an object in a single call.
 */
void json_write_bool_kv(JsonWriter *w, const char *key, bool value);

/*
 * Write a `"key":<signed integer>` pair inside an object in a single call.
 */
void json_write_int_kv(JsonWriter *w, const char *key, long value);

/*
 * Write a `"key":<unsigned integer>` pair inside an object in a single call.
 */
void json_write_uint_kv(JsonWriter *w, const char *key, unsigned long value);

/*
 * Begin a `"key":{` object entry inside an object. Pair with
 * `json_write_object_end()`.
 */
void json_write_object_begin_k(JsonWriter *w, const char *key);

/*
 * Begin a `"key":[` array entry inside an object. Pair with
 * `json_write_array_end()`.
 */
void json_write_array_begin_k(JsonWriter *w, const char *key);

/*
 * Begin a JSON object. Must be paired with `json_write_object_end()`.
 */
void json_write_object_begin(JsonWriter *w);

/*
 * End the current JSON object.
 */
void json_write_object_end(JsonWriter *w);

/*
 * Write an object key. The next write call produces the associated value.
 *
 * Writes `"key":` with automatic comma insertion before the key when needed.
 */
void json_write_key(JsonWriter *w, const char *key);

/*
 * Begin a JSON array. Must be paired with `json_write_array_end()`.
 */
void json_write_array_begin(JsonWriter *w);

/*
 * End the current JSON array.
 */
void json_write_array_end(JsonWriter *w);

#ifdef __cplusplus
}
#endif

#endif /* LIBJSON_H */

/* ========================================================================= */
/*                            IMPLEMENTATION                                 */
/* ========================================================================= */

#ifdef LIBJSON_IMPLEMENTATION

#ifdef __cplusplus
extern "C" {
#endif

#ifdef JSON_NO_STDIO
static void json__error(const char *message) { (void)message; }
#else
static void json__error(const char *message) {
  fprintf(stderr, "%s\n", message);
}
#endif

static const char *json__skip_whitespace(const char *cursor, const char *end) {
  while (cursor < end) {
    unsigned char ch = (unsigned char)*cursor;
    if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r')
      break;
    cursor++;
  }
  return cursor;
}

static const char *json__skip_string(const char *cursor, const char *end) {
  if (cursor >= end || *cursor != '"') {
    json__error("json: expected string");
    return NULL;
  }

  cursor++;
  while (cursor < end) {
    size_t remaining = (size_t)(end - cursor);
    const char *next_quote = (const char *)memchr(cursor, '"', remaining);
    const char *next_escape = (const char *)memchr(cursor, '\\', remaining);
    const char *next = next_quote;

    if (next_escape && (!next || next_escape < next))
      next = next_escape;

    if (!next)
      break;

    if (*next == '"')
      return next + 1;

    cursor = next + 1;
    if (cursor >= end) {
      json__error("json: unterminated escape");
      return NULL;
    }
    cursor++;
  }

  json__error("json: unterminated string");
  return NULL;
}

static const char *json__skip_compound(const char *cursor, const char *end,
                                       char open_ch, char close_ch) {
  int depth = 1;
  bool in_string = false;
  bool escaped = false;

  cursor++;
  while (cursor < end) {
    char ch = *cursor;

    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        in_string = false;
      }
      cursor++;
      continue;
    }

    if (ch == '"') {
      in_string = true;
    } else if (ch == open_ch) {
      depth++;
    } else if (ch == close_ch) {
      depth--;
      if (depth == 0)
        return cursor + 1;
    }

    cursor++;
  }

  json__error("json: unterminated compound value");
  return NULL;
}

static const char *json__skip_primitive(const char *cursor, const char *end) {
  while (cursor < end) {
    char ch = *cursor;
    if (ch == ',' || ch == ']' || ch == '}' || ch == ' ' || ch == '\t' ||
        ch == '\n' || ch == '\r') {
      return cursor;
    }
    cursor++;
  }
  return cursor;
}

static const char *json__skip_value(const char *cursor, const char *end) {
  cursor = json__skip_whitespace(cursor, end);
  if (cursor >= end) {
    json__error("json: expected value");
    return NULL;
  }

  if (*cursor == '"')
    return json__skip_string(cursor, end);
  if (*cursor == '{')
    return json__skip_compound(cursor, end, '{', '}');
  if (*cursor == '[')
    return json__skip_compound(cursor, end, '[', ']');
  return json__skip_primitive(cursor, end);
}

static JsonSlice json__value_slice(const char *start, const char *end) {
  if (!start || !end || end < start)
    return json_from_parts(NULL, 0);

  if (*start == '"' && end > start + 1 && *(end - 1) == '"')
    return json_from_parts(start + 1, (size_t)((end - start) - 2));

  return json_from_parts(start, (size_t)(end - start));
}

static bool json__object_next(const char **cursor_ptr, const char *end,
                              JsonSlice *key, JsonSlice *value) {
  const char *cursor = *cursor_ptr;
  const char *key_start;
  const char *key_end;
  const char *value_start;
  const char *value_end;

  cursor = json__skip_whitespace(cursor, end);
  if (cursor >= end) {
    json__error("json: unterminated object");
    return false;
  }

  if (*cursor == '}') {
    *cursor_ptr = cursor;
    return false;
  }
  if (*cursor == ',') {
    cursor++;
    cursor = json__skip_whitespace(cursor, end);
  }

  if (cursor >= end) {
    json__error("json: unterminated object");
    return false;
  }
  if (*cursor == '}') {
    *cursor_ptr = cursor;
    return false;
  }
  if (*cursor != '"') {
    json__error("json: expected object key");
    return false;
  }

  key_start = cursor + 1;
  key_end = json__skip_string(cursor, end);
  if (!key_end)
    return false;

  cursor = json__skip_whitespace(key_end, end);
  if (cursor >= end || *cursor != ':') {
    json__error("json: expected ':' after key");
    return false;
  }

  cursor++;
  value_start = json__skip_whitespace(cursor, end);
  value_end = json__skip_value(value_start, end);
  if (!value_end)
    return false;

  if (key)
    *key = json_from_parts(key_start, (size_t)((key_end - 1) - key_start));
  if (value)
    *value = json__value_slice(value_start, value_end);

  cursor = json__skip_whitespace(value_end, end);
  if (cursor < end && *cursor == ',')
    cursor++;
  *cursor_ptr = cursor;
  return true;
}

JsonSlice json_from_parts(const char *data, size_t len) {
  JsonSlice slice;
  slice.data = data;
  slice.len = len;
  return slice;
}

JsonSlice json_from_cstr(const char *json) {
  if (!json)
    return json_from_parts(NULL, 0);
  return json_from_parts(json, strlen(json));
}

bool json_slice_copy(JsonSlice slice, char *buffer, size_t buffer_size) {
  if (!buffer) {
    json__error("json_slice_copy: buffer is NULL");
    return false;
  }

  if (!slice.data && slice.len != 0) {
    json__error("json_slice_copy: invalid slice");
    return false;
  }

  if (buffer_size <= slice.len) {
    json__error("json_slice_copy: buffer too small");
    return false;
  }

  if (slice.len > 0)
    memcpy(buffer, slice.data, slice.len);
  buffer[slice.len] = '\0';
  return true;
}

bool json_getn(JsonSlice object, const char *key, size_t key_len,
               JsonSlice *out) {
  const char *cursor;
  const char *end;
  JsonSlice current_key;
  JsonSlice current_value;

  if (!out || !key) {
    json__error("json_getn: invalid arguments");
    return false;
  }

  *out = json_from_parts(NULL, 0);
  if (!object.data) {
    json__error("json_getn: object slice is NULL");
    return false;
  }

  cursor = json__skip_whitespace(object.data, object.data + object.len);
  end = object.data + object.len;

  if (cursor >= end || *cursor != '{') {
    json__error("json_getn: input is not an object");
    return false;
  }

  cursor++;
  while (cursor < end) {
    if (!json__object_next(&cursor, end, &current_key, &current_value))
      return false;

    if (current_key.len == key_len &&
        strncmp(current_key.data, key, key_len) == 0) {
      *out = current_value;
      return true;
    }
  }

  json__error("json_getn: unterminated object");
  return false;
}

bool json_get(JsonSlice object, const char *key, JsonSlice *out) {
  if (!key) {
    json__error("json_get: invalid arguments");
    return false;
  }
  return json_getn(object, key, strlen(key), out);
}

void json_object_iter_init(JsonSlice object, JsonObjectIter *iter) {
  if (!iter) {
    json__error("json_object_iter_init: iter is NULL");
    return;
  }

  iter->object = object;
  iter->cursor = object.data;
}

bool json_object_iter_next(JsonObjectIter *iter, JsonSlice *key,
                           JsonSlice *value) {
  const char *cursor;
  const char *end;

  if (!iter || !key || !value) {
    json__error("json_object_iter_next: invalid arguments");
    return false;
  }

  *key = json_from_parts(NULL, 0);
  *value = json_from_parts(NULL, 0);
  if (!iter->object.data) {
    json__error("json_object_iter_next: object slice is NULL");
    return false;
  }

  end = iter->object.data + iter->object.len;
  cursor = iter->cursor ? iter->cursor : iter->object.data;
  cursor = json__skip_whitespace(cursor, end);

  if (cursor == iter->object.data) {
    if (cursor >= end || *cursor != '{') {
      json__error("json_object_iter_next: input is not an object");
      return false;
    }
    cursor++;
  }

  if (!json__object_next(&cursor, end, key, value)) {
    iter->cursor = cursor;
    return false;
  }

  iter->cursor = cursor;
  return true;
}

void json_array_iter_init(JsonSlice array, JsonArrayIter *iter) {
  if (!iter) {
    json__error("json_array_iter_init: iter is NULL");
    return;
  }

  iter->array = array;
  iter->cursor = array.data;
}

bool json_array_iter_next(JsonArrayIter *iter, JsonSlice *out) {
  const char *cursor;
  const char *end;
  const char *value_start;
  const char *value_end;

  if (!iter || !out) {
    json__error("json_array_iter_next: invalid arguments");
    return false;
  }

  *out = json_from_parts(NULL, 0);
  if (!iter->array.data) {
    json__error("json_array_iter_next: array slice is NULL");
    return false;
  }

  end = iter->array.data + iter->array.len;
  cursor = iter->cursor ? iter->cursor : iter->array.data;
  cursor = json__skip_whitespace(cursor, end);

  if (cursor == iter->array.data) {
    if (cursor >= end || *cursor != '[') {
      json__error("json_array_iter_next: input is not an array");
      return false;
    }
    cursor++;
  }
  cursor = json__skip_whitespace(cursor, end);

  if (cursor < end && *cursor == ',') {
    cursor++;
    cursor = json__skip_whitespace(cursor, end);
  }

  if (cursor >= end) {
    json__error("json_array_iter_next: unterminated array");
    return false;
  }

  if (*cursor == ']') {
    iter->cursor = cursor;
    return false;
  }

  value_start = cursor;
  value_end = json__skip_value(value_start, end);
  if (!value_end)
    return false;

  *out = json__value_slice(value_start, value_end);
  cursor = json__skip_whitespace(value_end, end);
  if (cursor < end && *cursor == ',')
    cursor++;
  iter->cursor = cursor;
  return true;
}

/* ========================================================================= */
/*                       WRITER IMPLEMENTATION                               */
/* ========================================================================= */

static void json__write_bytes(JsonWriter *w, const char *data, size_t len) {
  if (w->overflow || len == 0)
    return;
  if (w->pos + len >= w->cap) {
    w->overflow = true;
    return;
  }
  memcpy(w->buf + w->pos, data, len);
  w->pos += len;
  w->buf[w->pos] = '\0';
}

static void json__write_char(JsonWriter *w, char ch) {
  if (w->overflow)
    return;
  if (w->pos + 1 >= w->cap) {
    w->overflow = true;
    return;
  }
  w->buf[w->pos++] = ch;
  w->buf[w->pos] = '\0';
}

static void json__write_comma_if_needed(JsonWriter *w) {
  if (w->needs_comma & (1u << w->depth)) {
    json__write_char(w, ',');
  }
  w->needs_comma |= (1u << w->depth);
}

static void json__write_escaped_string(JsonWriter *w, const char *str,
                                       size_t len) {
  static const char hex_digits[] = "0123456789abcdef";
  size_t i;

  json__write_char(w, '"');
  for (i = 0; i < len && !w->overflow; i++) {
    unsigned char ch = (unsigned char)str[i];
    switch (ch) {
    case '"':
      json__write_bytes(w, "\\\"", 2);
      break;
    case '\\':
      json__write_bytes(w, "\\\\", 2);
      break;
    case '\b':
      json__write_bytes(w, "\\b", 2);
      break;
    case '\f':
      json__write_bytes(w, "\\f", 2);
      break;
    case '\n':
      json__write_bytes(w, "\\n", 2);
      break;
    case '\r':
      json__write_bytes(w, "\\r", 2);
      break;
    case '\t':
      json__write_bytes(w, "\\t", 2);
      break;
    default:
      if (ch < 0x20) {
        char esc[6] = {'\\', 'u', '0', '0', 0, 0};
        esc[4] = hex_digits[ch >> 4];
        esc[5] = hex_digits[ch & 0x0f];
        json__write_bytes(w, esc, 6);
      } else {
        json__write_char(w, (char)ch);
      }
      break;
    }
  }
  json__write_char(w, '"');
}

static void json__write_long(JsonWriter *w, unsigned long val, bool negative) {
  char tmp[21];
  int pos = (int)sizeof(tmp);

  if (val == 0) {
    json__write_char(w, '0');
    return;
  }

  while (val > 0) {
    tmp[--pos] = (char)('0' + (val % 10));
    val /= 10;
  }

  if (negative)
    tmp[--pos] = '-';

  json__write_bytes(w, tmp + pos, (size_t)(sizeof(tmp) - (size_t)pos));
}

void json_writer_init(JsonWriter *w, char *buf, size_t cap) {
  if (!w)
    return;
  w->buf = buf;
  w->cap = cap;
  w->pos = 0;
  w->overflow = (buf == NULL || cap == 0);
  w->depth = 0;
  w->needs_comma = 0;
  if (buf && cap > 0)
    buf[0] = '\0';
}

size_t json_writer_len(const JsonWriter *w) { return w ? w->pos : 0; }

bool json_writer_ok(const JsonWriter *w) { return w && !w->overflow; }

const char *json_writer_output(const JsonWriter *w) {
  if (!w || w->overflow)
    return NULL;
  return w->buf;
}

void json_write_null(JsonWriter *w) {
  if (!w)
    return;
  json__write_comma_if_needed(w);
  json__write_bytes(w, "null", 4);
}

void json_write_bool(JsonWriter *w, bool value) {
  if (!w)
    return;
  json__write_comma_if_needed(w);
  if (value)
    json__write_bytes(w, "true", 4);
  else
    json__write_bytes(w, "false", 5);
}

void json_write_int(JsonWriter *w, long value) {
  if (!w)
    return;
  json__write_comma_if_needed(w);
  if (value < 0)
    json__write_long(w, (unsigned long)(-(value + 1)) + 1, true);
  else
    json__write_long(w, (unsigned long)value, false);
}

void json_write_uint(JsonWriter *w, unsigned long value) {
  if (!w)
    return;
  json__write_comma_if_needed(w);
  json__write_long(w, value, false);
}

void json_write_str(JsonWriter *w, const char *str) {
  if (!w)
    return;
  json__write_comma_if_needed(w);
  if (!str) {
    json__write_bytes(w, "null", 4);
    return;
  }
  json__write_escaped_string(w, str, strlen(str));
}

void json_write_str_kv(JsonWriter *w, const char *key, const char *value) {
  if (!w || !key)
    return;
  json_write_key(w, key);
  json_write_str(w, value);
}

void json_write_null_kv(JsonWriter *w, const char *key) {
  if (!w || !key)
    return;
  json_write_key(w, key);
  json_write_null(w);
}

void json_write_bool_kv(JsonWriter *w, const char *key, bool value) {
  if (!w || !key)
    return;
  json_write_key(w, key);
  json_write_bool(w, value);
}

void json_write_int_kv(JsonWriter *w, const char *key, long value) {
  if (!w || !key)
    return;
  json_write_key(w, key);
  json_write_int(w, value);
}

void json_write_uint_kv(JsonWriter *w, const char *key, unsigned long value) {
  if (!w || !key)
    return;
  json_write_key(w, key);
  json_write_uint(w, value);
}

void json_write_strn(JsonWriter *w, const char *str, size_t len) {
  if (!w)
    return;
  json__write_comma_if_needed(w);
  if (!str) {
    json__write_bytes(w, "null", 4);
    return;
  }
  json__write_escaped_string(w, str, len);
}

void json_write_strn_kv(JsonWriter *w, const char *key, const char *value,
                        size_t len) {
  if (!w || !key)
    return;
  json_write_key(w, key);
  json_write_strn(w, value, len);
}

void json_write_raw(JsonWriter *w, const char *raw, size_t len) {
  if (!w)
    return;
  json__write_comma_if_needed(w);
  json__write_bytes(w, raw, len);
}

void json_write_raw_kv(JsonWriter *w, const char *key, const char *raw,
                       size_t len) {
  if (!w || !key)
    return;
  json_write_key(w, key);
  json_write_raw(w, raw, len);
}

void json_write_object_begin(JsonWriter *w) {
  if (!w)
    return;
  json__write_comma_if_needed(w);
  json__write_char(w, '{');
  w->depth++;
  w->needs_comma &= ~(1u << w->depth);
}

void json_write_object_begin_k(JsonWriter *w, const char *key) {
  if (!w || !key)
    return;
  json_write_key(w, key);
  json_write_object_begin(w);
}

void json_write_object_end(JsonWriter *w) {
  if (!w || w->depth == 0)
    return;
  w->needs_comma &= ~(1u << w->depth);
  w->depth--;
  json__write_char(w, '}');
}

void json_write_key(JsonWriter *w, const char *key) {
  if (!w || !key)
    return;
  json__write_comma_if_needed(w);
  json__write_escaped_string(w, key, strlen(key));
  json__write_char(w, ':');
  w->needs_comma &= ~(1u << w->depth);
}

void json_write_array_begin(JsonWriter *w) {
  if (!w)
    return;
  json__write_comma_if_needed(w);
  json__write_char(w, '[');
  w->depth++;
  w->needs_comma &= ~(1u << w->depth);
}

void json_write_array_begin_k(JsonWriter *w, const char *key) {
  if (!w || !key)
    return;
  json_write_key(w, key);
  json_write_array_begin(w);
}

void json_write_array_end(JsonWriter *w) {
  if (!w || w->depth == 0)
    return;
  w->needs_comma &= ~(1u << w->depth);
  w->depth--;
  json__write_char(w, ']');
}

#ifdef __cplusplus
}
#endif

#endif /* LIBJSON_IMPLEMENTATION */
