/* Minimal JSON DOM reader/writer for the data pipeline (packer, exporter
 * self-check, package loader). Numbers are kept as double + int64 (exact for
 * every integer the tables hold); strings are unescaped for \" \\ \/ \n \t
 * \uXXXX (BMP, UTF-8 out). Not a general-purpose library: no streaming,
 * whole document in memory, errors report line numbers. */
#ifndef WF_JSON_H
#define WF_JSON_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } json_type;

typedef struct json_val json_val;
struct json_val {
    json_type type;
    double    num;
    int64_t   inum;          /* valid when is_int */
    int       is_int;
    char     *str;           /* STRING; for OBJECT members, the key lives in `key` */
    char     *key;           /* member name when inside an object */
    json_val *child;         /* first element/member */
    json_val *next;          /* sibling */
    int       n;             /* element/member count */
};

json_val *json_parse(const char *text, size_t len, char *err, size_t errlen);
json_val *json_parse_file(const char *path, char *err, size_t errlen);   /* NULL on error */
void      json_free(json_val *v);

const json_val *json_get(const json_val *obj, const char *key);   /* object member or NULL */
const json_val *json_at(const json_val *arr, int i);              /* array element or NULL */
int64_t         json_int(const json_val *v, int64_t def);         /* number (or "0x.." string) */
const char     *json_str(const json_val *v, const char *def);

/* Writer helpers: escaped string; pretty-print a whole value (the editor
 * round-trips JSON files with it); set/add members. */
void json_write_string(FILE *f, const char *s);
void json_write(FILE *f, const json_val *v, int indent);
int  json_write_file(const char *path, const json_val *v);   /* 0 = ok */
json_val *json_set_number(json_val *obj, const char *key, int64_t v);   /* member (created if absent) */
json_val *json_set_string(json_val *obj, const char *key, const char *s);
json_val *json_set_object(json_val *obj, const char *key);              /* member made an (empty) object unless it is one */
void      json_remove(json_val *obj, const char *key);
json_val *json_set_array(json_val *obj, const char *key);               /* member made an (empty) array */
json_val *json_array_push_object(json_val *arr);                        /* append an empty object, returns it */
#endif
