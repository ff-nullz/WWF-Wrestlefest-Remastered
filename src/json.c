/* Minimal JSON reader — see json.h. Recursive descent over the text. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "json.h"

typedef struct { const char *p, *end; int line; char *err; size_t errlen; int failed; } parser;

static void fail(parser *ps, const char *msg)
{
    if (!ps->failed) { ps->failed = 1; if (ps->err) snprintf(ps->err, ps->errlen, "line %d: %s", ps->line, msg); }
}
static void ws(parser *ps)
{
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c == '\n') { ps->line++; ps->p++; }
        else if (c == ' ' || c == '\t' || c == '\r') ps->p++;
        else break;
    }
}
static json_val *new_val(json_type t) { json_val *v = calloc(1, sizeof *v); v->type = t; return v; }

static void put_utf8(char **o, unsigned cp)
{
    if (cp < 0x80) *(*o)++ = (char)cp;
    else if (cp < 0x800) { *(*o)++ = (char)(0xC0 | (cp >> 6)); *(*o)++ = (char)(0x80 | (cp & 0x3F)); }
    else { *(*o)++ = (char)(0xE0 | (cp >> 12)); *(*o)++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *(*o)++ = (char)(0x80 | (cp & 0x3F)); }
}

static char *parse_string(parser *ps)
{
    const char *s;
    char *out, *o;
    if (ps->p >= ps->end || *ps->p != '"') { fail(ps, "expected string"); return NULL; }
    ps->p++;
    s = ps->p;
    {   /* size the buffer from the raw span (escapes only shrink it) */
        const char *q = s;
        while (q < ps->end && *q != '"') { if (*q == '\\' && q + 1 < ps->end) q++; q++; }
        out = malloc((size_t)(q - s) * 1 + 4); o = out;
    }
    while (ps->p < ps->end && *ps->p != '"') {
        char c = *ps->p++;
        if (c == '\\') {
            if (ps->p >= ps->end) break;
            c = *ps->p++;
            switch (c) {
            case 'n': *o++ = '\n'; break; case 't': *o++ = '\t'; break; case 'r': *o++ = '\r'; break;
            case 'b': *o++ = '\b'; break; case 'f': *o++ = '\f'; break;
            case '"': case '\\': case '/': *o++ = c; break;
            case 'u': {
                unsigned cp = 0;
                for (int i = 0; i < 4 && ps->p < ps->end; i++, ps->p++) {
                    char h = *ps->p; cp <<= 4;
                    cp |= (unsigned)(isdigit((unsigned char)h) ? h - '0' : (tolower((unsigned char)h) - 'a' + 10));
                }
                put_utf8(&o, cp); break; }
            default: fail(ps, "bad escape"); free(out); return NULL;
            }
        } else { if (c == '\n') ps->line++; *o++ = c; }
    }
    if (ps->p >= ps->end) { fail(ps, "unterminated string"); free(out); return NULL; }
    ps->p++;
    *o = 0;
    return out;
}

static json_val *parse_value(parser *ps);

static json_val *parse_number(parser *ps)
{
    const char *s = ps->p;
    char *e;
    json_val *v = new_val(JSON_NUMBER);
    int isint = 1;
    while (ps->p < ps->end && (isdigit((unsigned char)*ps->p) || strchr("+-.eE", *ps->p))) {
        if (*ps->p == '.' || *ps->p == 'e' || *ps->p == 'E') isint = 0;
        ps->p++;
    }
    v->num = strtod(s, &e);
    if (e != ps->p) { fail(ps, "bad number"); free(v); return NULL; }
    if (isint) { v->is_int = 1; v->inum = strtoll(s, NULL, 10); }
    else if (floor(v->num) == v->num && fabs(v->num) < 9e15) { v->is_int = 1; v->inum = (int64_t)v->num; }
    return v;
}

static json_val *parse_value(parser *ps)
{
    ws(ps);
    if (ps->p >= ps->end) { fail(ps, "unexpected end"); return NULL; }
    switch (*ps->p) {
    case '{': {
        json_val *v = new_val(JSON_OBJECT), **tail = &v->child;
        ps->p++; ws(ps);
        if (ps->p < ps->end && *ps->p == '}') { ps->p++; return v; }
        for (;;) {
            char *key; json_val *m;
            ws(ps);
            key = parse_string(ps);
            if (!key) { json_free(v); return NULL; }
            ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') { fail(ps, "expected ':'"); free(key); json_free(v); return NULL; }
            ps->p++;
            m = parse_value(ps);
            if (!m) { free(key); json_free(v); return NULL; }
            m->key = key; *tail = m; tail = &m->next; v->n++;
            ws(ps);
            if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
            if (ps->p < ps->end && *ps->p == '}') { ps->p++; return v; }
            fail(ps, "expected ',' or '}'"); json_free(v); return NULL;
        }
    }
    case '[': {
        json_val *v = new_val(JSON_ARRAY), **tail = &v->child;
        ps->p++; ws(ps);
        if (ps->p < ps->end && *ps->p == ']') { ps->p++; return v; }
        for (;;) {
            json_val *e = parse_value(ps);
            if (!e) { json_free(v); return NULL; }
            *tail = e; tail = &e->next; v->n++;
            ws(ps);
            if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
            if (ps->p < ps->end && *ps->p == ']') { ps->p++; return v; }
            fail(ps, "expected ',' or ']'"); json_free(v); return NULL;
        }
    }
    case '"': { json_val *v = new_val(JSON_STRING); v->str = parse_string(ps); if (!v->str) { free(v); return NULL; } return v; }
    case 't': if (ps->end - ps->p >= 4 && !strncmp(ps->p, "true", 4)) { json_val *v = new_val(JSON_BOOL); v->inum = 1; v->is_int = 1; v->num = 1; ps->p += 4; return v; } break;
    case 'f': if (ps->end - ps->p >= 5 && !strncmp(ps->p, "false", 5)) { json_val *v = new_val(JSON_BOOL); ps->p += 5; return v; } break;
    case 'n': if (ps->end - ps->p >= 4 && !strncmp(ps->p, "null", 4)) { ps->p += 4; return new_val(JSON_NULL); } break;
    default: if (*ps->p == '-' || isdigit((unsigned char)*ps->p)) return parse_number(ps);
    }
    fail(ps, "unexpected character");
    return NULL;
}

json_val *json_parse(const char *text, size_t len, char *err, size_t errlen)
{
    parser ps = { text, text + len, 1, err, errlen, 0 };
    json_val *v = parse_value(&ps);
    if (v) { ws(&ps); if (ps.p != ps.end) { fail(&ps, "trailing characters"); json_free(v); return NULL; } }
    return v;
}

json_val *json_parse_file(const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    long sz; char *buf; json_val *v;
    if (!f) { if (err) snprintf(err, errlen, "cannot open %s", path); return NULL; }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)sz + 1);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); if (err) snprintf(err, errlen, "short read %s", path); return NULL; }
    fclose(f);
    buf[sz] = 0;
    v = json_parse(buf, (size_t)sz, err, errlen);
    free(buf);
    return v;
}

void json_free(json_val *v)
{
    while (v) {
        json_val *n = v->next;
        json_free(v->child);
        free(v->str); free(v->key); free(v);
        v = n;
    }
}

const json_val *json_get(const json_val *obj, const char *key)
{
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (const json_val *m = obj->child; m; m = m->next)
        if (m->key && !strcmp(m->key, key)) return m;
    return NULL;
}
const json_val *json_at(const json_val *arr, int i)
{
    if (!arr || arr->type != JSON_ARRAY || i < 0) return NULL;
    for (const json_val *e = arr->child; e; e = e->next, i--) if (!i) return e;
    return NULL;
}
int64_t json_int(const json_val *v, int64_t def)
{
    if (!v) return def;
    if (v->type == JSON_NUMBER || v->type == JSON_BOOL) return v->is_int ? v->inum : (int64_t)v->num;
    if (v->type == JSON_STRING && v->str) { char *e; long long x = strtoll(v->str, &e, 0); if (e != v->str && !*e) return x; }
    return def;
}
const char *json_str(const json_val *v, const char *def) { return (v && v->type == JSON_STRING) ? v->str : def; }

void json_write_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\t') fputs("\\t", f);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static void ind(FILE *f, int n) { while (n-- > 0) fputc(' ', f); }
void json_write(FILE *f, const json_val *v, int indent)
{
    if (!v) { fputs("null", f); return; }
    switch (v->type) {
    case JSON_NULL: fputs("null", f); break;
    case JSON_BOOL: fputs(v->inum ? "true" : "false", f); break;
    case JSON_NUMBER:
        if (v->is_int) fprintf(f, "%lld", (long long)v->inum);
        else fprintf(f, "%.17g", v->num);
        break;
    case JSON_STRING: json_write_string(f, v->str ? v->str : ""); break;
    case JSON_ARRAY: {
        int scalar = 1;
        for (const json_val *e = v->child; e; e = e->next) if (e->type == JSON_ARRAY || e->type == JSON_OBJECT) { scalar = 0; break; }
        fputc('[', f);
        if (scalar) {                               /* numbers on one line */
            for (const json_val *e = v->child; e; e = e->next) { json_write(f, e, 0); if (e->next) fputs(", ", f); }
        } else {
            fputc('\n', f);
            for (const json_val *e = v->child; e; e = e->next) { ind(f, indent + 2); json_write(f, e, indent + 2); if (e->next) fputc(',', f); fputc('\n', f); }
            ind(f, indent);
        }
        fputc(']', f);
        break; }
    case JSON_OBJECT:
        fputs("{\n", f);
        for (const json_val *m = v->child; m; m = m->next) {
            ind(f, indent + 2); json_write_string(f, m->key ? m->key : ""); fputs(": ", f);
            json_write(f, m, indent + 2);
            if (m->next) fputc(',', f);
            fputc('\n', f);
        }
        ind(f, indent); fputc('}', f);
        break;
    }
}
int json_write_file(const char *path, const json_val *v)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    json_write(f, v, 0); fputc('\n', f);
    return fclose(f) == 0 ? 0 : -1;
}
json_val *json_set_number(json_val *obj, const char *key, int64_t val)
{
    json_val *m;
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (m = obj->child; m; m = m->next) if (m->key && !strcmp(m->key, key)) break;
    if (!m) {
        json_val **tail = &obj->child;
        while (*tail) tail = &(*tail)->next;
        m = calloc(1, sizeof *m); m->key = strdup(key); *tail = m; obj->n++;
    }
    free(m->str); m->str = NULL; json_free(m->child); m->child = NULL;
    m->type = JSON_NUMBER; m->is_int = 1; m->inum = val; m->num = (double)val;
    return m;
}

static json_val *json_member(json_val *obj, const char *key)
{
    json_val *m;
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (m = obj->child; m; m = m->next) if (m->key && !strcmp(m->key, key)) return m;
    { json_val **tail = &obj->child; while (*tail) tail = &(*tail)->next;
      m = calloc(1, sizeof *m); m->key = strdup(key); *tail = m; obj->n++; }
    return m;
}
json_val *json_set_string(json_val *obj, const char *key, const char *s)
{
    json_val *m = json_member(obj, key);
    if (!m) return NULL;
    free(m->str); json_free(m->child); m->child = NULL; m->n = 0;
    m->type = JSON_STRING; m->str = strdup(s ? s : "");
    return m;
}
json_val *json_set_object(json_val *obj, const char *key)
{
    json_val *m = json_member(obj, key);
    if (!m) return NULL;
    if (m->type != JSON_OBJECT) { free(m->str); m->str = NULL; json_free(m->child); m->child = NULL; m->n = 0; m->type = JSON_OBJECT; }
    return m;
}
void json_remove(json_val *obj, const char *key)
{
    json_val **pp;
    if (!obj || obj->type != JSON_OBJECT) return;
    for (pp = &obj->child; *pp; pp = &(*pp)->next)
        if ((*pp)->key && !strcmp((*pp)->key, key)) { json_val *m = *pp; *pp = m->next; m->next = NULL; json_free(m); obj->n--; return; }
}

json_val *json_set_array(json_val *obj, const char *key)
{
    json_val *m;
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (m = obj->child; m; m = m->next) if (m->key && !strcmp(m->key, key)) break;
    if (!m) {
        json_val **tail = &obj->child;
        while (*tail) tail = &(*tail)->next;
        m = calloc(1, sizeof *m); m->key = strdup(key); *tail = m; obj->n++;
    }
    free(m->str); m->str = NULL; json_free(m->child); m->child = NULL;
    m->type = JSON_ARRAY; m->n = 0;
    return m;
}
json_val *json_array_push_object(json_val *arr)
{
    json_val *e, **tail;
    if (!arr || arr->type != JSON_ARRAY) return NULL;
    e = calloc(1, sizeof *e); e->type = JSON_OBJECT;
    tail = &arr->child; while (*tail) tail = &(*tail)->next; *tail = e; arr->n++;
    return e;
}
