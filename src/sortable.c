#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include "dep/libnu/libnu.h"
#include "rmutil/util.h"
#include "rmutil/strings.h"
#include "rmalloc.h"
#include "sortable.h"
#include "buffer.h"

/* Create a sorting vector of a given length for a document */
RSSortingVector *NewSortingVector(int len) {
  if (len > RS_SORTABLES_MAX) {
    return NULL;
  }
  RSSortingVector *ret = rm_calloc(1, sizeof(RSSortingVector) + len * (sizeof(RSSortableValue)));
  ret->len = len;
  // set all values to NIL
  for (int i = 0; i < len; i++) {
    ret->values[i].type = RS_SORTABLE_NIL;
  }
  return ret;
}

/* Internal compare function between members of the sorting vectors, sorted by sk */
inline int RSSortingVector_Cmp(RSSortingVector *self, RSSortingVector *other, RSSortingKey *sk) {

  RSSortableValue v1 = self->values[sk->index];
  RSSortableValue v2 = other->values[sk->index];
  int rc = 0;
  if (v2.type == RS_SORTABLE_NIL) {
    rc = v1.type == RS_SORTABLE_NIL ? 0 : 1;
  } else {

    assert(v1.type == v2.type || v1.type == RS_SORTABLE_NIL);
    switch (v1.type) {
      case RS_SORTABLE_NUM: {
        rc = v1.num < v2.num ? -1 : (v2.num < v1.num ? 1 : 0);
        break;
      }
      case RS_SORTABLE_STR: {
        rc = strcmp(v1.str, v2.str);
        break;
      }

      case RS_SORTABLE_NIL:
        rc = -1;
        break;
    }
  }
  return sk->ascending ? rc : -rc;
}

/* Normalize sorting string for storage. This folds everything to unicode equivalent strings. The
 * allocated return string needs to be freed later */
char *normalizeStr(const char *str) {

  size_t buflen = 2 * strlen(str) + 1;
  char *lower_buffer = rm_calloc(buflen, 1);
  char *lower = lower_buffer;
  char *end = lower + buflen;

  const char *p = str;
  size_t off = 0;
  while (*p != 0 && lower < end) {
    uint32_t in = 0;
    p = nu_utf8_read(p, &in);
    const char *lo = nu_tofold(in);

    if (lo != 0) {
      uint32_t u = 0;
      do {
        lo = nu_casemap_read(lo, &u);
        if (u == 0) break;
        lower = nu_utf8_write(u, lower);
      } while (u != 0 && lower < end);
    } else {
      lower = nu_utf8_write(in, lower);
    }
  }

  return lower_buffer;
}

/* Put a value in the sorting vector */
void RSSortingVector_Put(RSSortingVector *tbl, int idx, void *p, int type) {
  if (idx <= RS_SORTABLES_MAX) {
    switch (type) {
      case RS_SORTABLE_NUM:
        tbl->values[idx].num = *(double *)p;

        break;
      case RS_SORTABLE_STR:
        tbl->values[idx].str = normalizeStr((char *)p);
      case RS_SORTABLE_NIL:
      default:
        break;
    }
    tbl->values[idx].type = type;
  }
}
RSSortableValue *RSSortingVector_Get(RSSortingVector *v, RSSortingKey *k) {
  if (!v || !k) return NULL;
  if (k->index >= 0 && k->index < v->len) {
    return &v->values[k->index];
  }
  return NULL;
}

/* Free a sorting vector */
void SortingVector_Free(RSSortingVector *v) {
  for (int i = 0; i < v->len; i++) {
    if (v->values[i].type == RS_SORTABLE_STR) {
      rm_free(v->values[i].str);
    }
  }
  rm_free(v);
}

/* Save a sorting vector to rdb. This is called from the doc table */
void SortingVector_RdbSave(RedisModuleIO *rdb, RSSortingVector *v) {
  if (!v) {
    RedisModule_SaveUnsigned(rdb, 0);
    return;
  }
  RedisModule_SaveUnsigned(rdb, v->len);
  for (int i = 0; i < v->len; i++) {
    RSSortableValue *val = &v->values[i];
    RedisModule_SaveUnsigned(rdb, val->type);
    switch (val->type) {
      case RS_SORTABLE_STR:
        // save string - one extra byte for null terminator
        RedisModule_SaveStringBuffer(rdb, val->str, strlen(val->str) + 1);
        break;

      case RS_SORTABLE_NUM:
        // save numeric value
        RedisModule_SaveDouble(rdb, val->num);
        break;
      // for nil we write nothing
      case RS_SORTABLE_NIL:
      default:
        break;
    }
  }
}

/* Load a sorting vector from RDB */
RSSortingVector *SortingVector_RdbLoad(RedisModuleIO *rdb, int encver) {

  int len = (int)RedisModule_LoadUnsigned(rdb);
  if (len > RS_SORTABLES_MAX || len <= 0) {
    return NULL;
  }
  RSSortingVector *vec = NewSortingVector(len);
  for (int i = 0; i < len; i++) {
    vec->values[i].type = RedisModule_LoadUnsigned(rdb);
    switch (vec->values[i].type) {
      case RS_SORTABLE_STR: {
        size_t len;
        // strings include an extra character for null terminator. we set it to zero just in case
        vec->values[i].str = RedisModule_LoadStringBuffer(rdb, &len);
        vec->values[i].str[len - 1] = '\0';

        break;
      }
      case RS_SORTABLE_NUM:
        // load numeric value
        vec->values[i].num = RedisModule_LoadDouble(rdb);
        break;
      // for nil we read nothing
      case RS_SORTABLE_NIL:
      default:
        break;
    }
  }
  return vec;
}

// We know these won't change
#define SV_SERIALIZED_STRING 1
#define SV_SERIALIZED_NUMBER 2
#define SV_SERIALIZED_NIL 4

#define SV_SERIALIZED_UNKNOWN 0xff

#define SV_MINSIZE 4  // Minimum size for serialized sort vector: This is 4 bytes

void SortingVector_Serialize(const RSSortingVector *sv, Buffer *out) {
  // Write the number of sorting vectors in the table
  BufferWriter bw = NewBufferWriter(out);
  Buffer_WriteU32(&bw, sv->len);
  for (size_t ii = 0; ii < sv->len; ++ii) {
    const RSSortableValue *val = sv->values + ii;
    char *buf, buf_s[1024];
    size_t slen;

    if (val->type == RS_SORTABLE_STR) {
      Buffer_WriteU8(&bw, SV_SERIALIZED_STRING);
      buf = val->str;
      slen = strlen(val->str);
    } else if (val->type == RS_SORTABLE_NUM) {
      Buffer_WriteU8(&bw, SV_SERIALIZED_NUMBER);
      buf = buf_s;
      slen = sprintf(buf_s, "%lf", val->num);
    } else if (val->type == RS_SORTABLE_NIL) {
      Buffer_WriteU8(&bw, SV_SERIALIZED_NIL);
      continue;

    } else {
      // wtf?
      Buffer_WriteU8(&bw, SV_SERIALIZED_UNKNOWN);
      continue;
    }
    Buffer_WriteU16(&bw, slen);
    Buffer_Write(&bw, buf, slen);
  }
}

RSSortingVector *SortingVector_LoadSerialized(const void *s, size_t n) {
  Buffer b = {.data = (void *)s, .cap = n, .offset = 0};
  if (n < SV_MINSIZE) {
    return NULL;
  }

  BufferReader r = NewBufferReader(&b);

  // Read the length of sorting vectors
  uint32_t len = Buffer_ReadU32(&r);
  if (len > RS_SORTABLES_MAX) {
    return NULL;
  }

  RSSortingVector *v = NewSortingVector(len);
  for (size_t ii = 0; ii < len; ++ii) {
    if (BufferReader_Remaining(&r) < 1) {
      // Need at least {type(1),len(2)}
      goto error;
    }

    RSSortableValue *cur = v->values + ii;

    uint8_t type = Buffer_ReadU8(&r);

    if (type == SV_SERIALIZED_NIL) {
      cur->type = RS_SORTABLE_NIL;
      continue;
    }

    if (BufferReader_Remaining(&r) < 2) {
      goto error;
    }

    uint16_t len = Buffer_ReadU16(&r);
    if (BufferReader_Remaining(&r) < len) {
      goto error;
    }
    if (type == SV_SERIALIZED_STRING) {
      cur->type = RS_SORTABLE_STR;
      cur->str = rm_malloc(len + 1);
      cur->str[len] = 0;
      Buffer_Read(&r, cur->str, len);
    } else if (type == SV_SERIALIZED_NUMBER) {
      cur->type = RS_SORTABLE_NUM;
      char buf[1024];
      if (len > sizeof(buf)) {
        goto error;
      }
      if (sscanf(buf, "%lf", &cur->num) != 1) {
        goto error;
      }
    } else {
      // Unknown type!
      goto error;
    }
  }

  if (r.pos != n) {
    // Some bytes not consumed
    goto error;
  }
  return v;

error:
  if (v) {
    SortingVector_Free(v);
  }
  return NULL;
}

/* Create a new sorting table of a given length */
RSSortingTable *NewSortingTable(int len) {
  RSSortingTable *tbl = rm_calloc(1, sizeof(RSSortingTable) + len * sizeof(const char *));
  tbl->len = len;
  return tbl;
}

void SortingTable_Free(RSSortingTable *t) {
  rm_free(t);
}

/* Set a field in the table by index. This is called during the schema parsing */
void SortingTable_SetFieldName(RSSortingTable *tbl, int idx, const char *name) {
  if (!tbl) return;
  if (idx >= tbl->len) {
    return;
  }
  tbl->fields[idx] = name;
}

/* Get the field index by name from the sorting table. Returns -1 if the field was not found */
int RSSortingTable_GetFieldIdx(RSSortingTable *tbl, const char *field) {

  if (!tbl) return -1;
  for (int i = 0; i < tbl->len; i++) {
    if (!strcasecmp(tbl->fields[i], field)) {
      return i;
    }
  }
  return -1;
}

/* Parse the sorting key of a query from redis arguments. We expect SORTBY {filed} [ASC/DESC]. The
 * default is ASC if not specified.  This function returns 1 if we found sorting args, they are
 * valid and the field name exists */
int RSSortingTable_ParseKey(RSSortingTable *tbl, RSSortingKey *k, RedisModuleString **argv,
                            int argc) {
  const char *field = NULL;
  k->index = -1;
  k->ascending = 1;
  if (!tbl) return 0;

  int sortPos = RMUtil_ArgIndex("SORTBY", argv, argc);
  if (sortPos >= 0 && sortPos + 1 < argc) {

    // parse the sorting field
    RMUtil_ParseArgs(argv, argc, sortPos + 1, "c", &field);

    // if we've found a field...
    if (field) {
      // parse optional ASC/DESC
      if (sortPos + 2 < argc) {

        if (RMUtil_StringEqualsCaseC(argv[sortPos + 2], "ASC")) {
          k->ascending = 1;
        } else if (RMUtil_StringEqualsCaseC(argv[sortPos + 2], "DESC")) {
          k->ascending = 0;
        }
      }
      // Get the actual field index from the descriptor
      k->index = RSSortingTable_GetFieldIdx(tbl, field);
    }
  }
  // return 1 on successful parse, 0 if not found or no sorting key
  return k->index == -1 ? 0 : 1;
}

void RSSortingKey_Free(RSSortingKey *k) {
  free(k);
}