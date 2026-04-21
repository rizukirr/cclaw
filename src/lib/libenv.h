#ifndef LIBENV_H
#define LIBENV_H

#include <stddef.h>

int libenv_load(char *env_file);
char *libenv_get(char *key);

#ifdef LIBENV_IMPLEMENTATION
#define ARENA_IMPLEMENTATION
#include "arena.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  char *key;
  char *value;
  size_t hash;
} libenv_entry;

typedef struct {
  libenv_entry *entries;
  size_t capacity;
  Arena *arena;
} libenv_store;

static libenv_store libenv_store_instance;

static size_t libenv_hash(const char *str) {
  size_t hash = 1469598103934665603ull;

  while (*str) {
    hash ^= (unsigned char)*str++;
    hash *= 1099511628211ull;
  }

  return hash;
}

static void libenv_store_reset(libenv_store *store) {
  arena_free(store->arena);
  store->entries = NULL;
  store->arena = NULL;
  store->capacity = 0;
}

static size_t libenv_next_capacity(size_t minimum) {
  size_t capacity = 16;

  while (capacity < minimum)
    capacity <<= 1;

  return capacity;
}

static void libenv_store_insert(libenv_store *store, char *key, char *value) {
  size_t mask = store->capacity - 1;
  size_t hash = libenv_hash(key);
  size_t index = hash & mask;

  while (store->entries[index].key != NULL) {
    if (store->entries[index].hash == hash &&
        strcmp(store->entries[index].key, key) == 0) {
      store->entries[index].value = value;
      return;
    }
    index = (index + 1) & mask;
  }

  store->entries[index].key = key;
  store->entries[index].value = value;
  store->entries[index].hash = hash;
}

static int libenv_read_file(Arena *arena, const char *path, char **buffer_out,
                            size_t *size_out) {
  FILE *file = fopen(path, "rb");
  long file_size;
  char *buffer;
  size_t read_size;

  if (file == NULL)
    return -1;

  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return -1;
  }

  file_size = ftell(file);
  if (file_size < 0) {
    fclose(file);
    return -1;
  }

  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return -1;
  }

  buffer = (char *)arena_alloc(arena, (size_t)file_size + 1, 1);
  if (buffer == NULL) {
    fclose(file);
    return -1;
  }

  read_size = fread(buffer, 1, (size_t)file_size, file);
  fclose(file);
  if (read_size != (size_t)file_size)
    return -1;

  buffer[read_size] = '\0';
  *buffer_out = buffer;
  *size_out = read_size;
  return 0;
}

int libenv_load(char *env_file) {
  libenv_store next = {0};
  char *buffer;
  char *cursor;
  char *buffer_end;
  size_t file_size = 0;
  size_t line_count = 1;

  next.arena = arena_create(4096);
  if (next.arena == NULL)
    return -1;

  if (libenv_read_file(next.arena, env_file, &buffer, &file_size) != 0) {
    arena_free(next.arena);
    return -1;
  }

  for (cursor = buffer; *cursor != '\0'; ++cursor) {
    if (*cursor == '\n')
      ++line_count;
  }

  next.capacity = libenv_next_capacity(line_count * 2);
  next.entries = (libenv_entry *)arena_alloc(
      next.arena, next.capacity * sizeof(libenv_entry), sizeof(void *));
  if (next.entries == NULL) {
    arena_free(next.arena);
    return -1;
  }
  memset(next.entries, 0, next.capacity * sizeof(libenv_entry));

  cursor = buffer;
  buffer_end = buffer + file_size;

  while (cursor < buffer_end) {
    char *line_start = cursor;
    char *line_end = cursor;
    char *trimmed_start;
    char *trimmed_end;
    char *eq;
    char *key_end;
    char *value_start;

    while (line_end < buffer_end && *line_end != '\n')
      ++line_end;

    if (line_end < buffer_end) {
      *line_end = '\0';
      cursor = line_end + 1;
    } else {
      cursor = line_end;
    }

    if (line_end > line_start && line_end[-1] == '\r')
      line_end[-1] = '\0';

    trimmed_start = line_start;
    while (*trimmed_start != '\0' && isspace((unsigned char)*trimmed_start))
      ++trimmed_start;

    if (*trimmed_start == '\0' || *trimmed_start == '#')
      continue;

    trimmed_end = trimmed_start + strlen(trimmed_start);
    while (trimmed_end > trimmed_start &&
           isspace((unsigned char)trimmed_end[-1])) {
      --trimmed_end;
    }
    *trimmed_end = '\0';

    eq = strchr(trimmed_start, '=');
    if (eq == NULL)
      continue;

    key_end = eq;
    while (key_end > trimmed_start && isspace((unsigned char)key_end[-1]))
      --key_end;
    *key_end = '\0';
    if (*trimmed_start == '\0')
      continue;

    value_start = eq + 1;
    while (*value_start != '\0' && isspace((unsigned char)*value_start))
      ++value_start;

    trimmed_end = value_start + strlen(value_start);
    while (trimmed_end > value_start &&
           isspace((unsigned char)trimmed_end[-1])) {
      --trimmed_end;
    }
    *trimmed_end = '\0';

    libenv_store_insert(&next, trimmed_start, value_start);
  }

  libenv_store_reset(&libenv_store_instance);
  libenv_store_instance = next;
  return 0;
}

char *libenv_get(char *key) {
  size_t index;
  size_t mask;
  size_t hash;

  if (key == NULL || libenv_store_instance.entries == NULL ||
      libenv_store_instance.capacity == 0) {
    return NULL;
  }

  hash = libenv_hash(key);
  mask = libenv_store_instance.capacity - 1;
  index = hash & mask;

  while (libenv_store_instance.entries[index].key != NULL) {
    if (libenv_store_instance.entries[index].hash == hash &&
        strcmp(libenv_store_instance.entries[index].key, key) == 0) {
      return libenv_store_instance.entries[index].value;
    }
    index = (index + 1) & mask;
  }

  return NULL;
}

#endif // LIBENV_IMPLEMENTATION

#endif // LIBENV_H
