/*
 * value.c — 값 표현, 참조 카운트, 해시 테이블, UTF-8 유틸 (2차)
 *
 * 1차와 동일한 참조 카운트 모델에 객체(V_OBJECT)를 추가했다.
 *   - 객체는 필드 테이블을 힙에 소유하고, 테이블의 각 값도 참조 카운트로 관리된다.
 *   - 객체가 다른 객체를 필드로 참조하는 트리 구조는 정상 회수된다.
 *     (순환 참조는 회수되지 않음 — 1차와 동일한 한계)
 */
#include "gnec.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NUM_BUF_SIZE = 64, TABLE_MIN_CAP = 8 };
static const double INT_PRINT_LIMIT = 1e15;

/* ---------------------------------------------------------------- 해시 */

uint32_t gne_hash(const char *s, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

/* ---------------------------------------------------------------- 테이블 */

bool table_init(Table *t, size_t cap)
{
    t->entries = calloc(cap, sizeof *t->entries);
    t->count = 0;
    t->cap = t->entries ? cap : 0;
    return t->entries != NULL;
}

void table_free(Table *t)
{
    if (t->entries == NULL) return;
    for (size_t i = 0; i < t->cap; ++i) {
        if (t->entries[i].key != NULL) {
            free(t->entries[i].key);
            value_release(t->entries[i].value);
        }
    }
    free(t->entries);
    t->entries = NULL;
    t->count = t->cap = 0;
}

static Entry *table_slot(const Table *t, const char *key, size_t len, uint32_t hash)
{
    size_t mask = t->cap - 1;
    for (size_t i = hash & mask;; i = (i + 1) & mask) {
        Entry *e = &t->entries[i];
        if (e->key == NULL) return e;
        if (e->hash == hash && e->key_len == len && memcmp(e->key, key, len) == 0) return e;
    }
}

Value *table_get(const Table *t, const char *key, size_t len, uint32_t hash)
{
    if (t->cap == 0) return NULL;
    Entry *e = table_slot(t, key, len, hash);
    return e->key ? &e->value : NULL;
}

static _Noreturn void fatal_oom(void)
{
    fprintf(stderr, "치명적 오류: 메모리 부족\n");
    exit(EXIT_FAILURE);
}

static void table_grow(Table *t)
{
    Table bigger;
    size_t new_cap = t->cap ? t->cap * 2 : TABLE_MIN_CAP;
    if (!table_init(&bigger, new_cap)) fatal_oom();
    for (size_t i = 0; i < t->cap; ++i) {
        Entry *old = &t->entries[i];
        if (old->key == NULL) continue;
        *table_slot(&bigger, old->key, old->key_len, old->hash) = *old;
        ++bigger.count;
    }
    free(t->entries);
    *t = bigger;
}

void table_set(Table *t, const char *key, size_t len, uint32_t hash, Value v)
{
    if (t->cap == 0 || (t->count + 1) * 4 > t->cap * 3) table_grow(t);
    Entry *e = table_slot(t, key, len, hash);
    if (e->key != NULL) {
        value_release(e->value);
        e->value = v;
        return;
    }
    e->key = malloc(len + 1);
    if (e->key == NULL) fatal_oom();
    memcpy(e->key, key, len);
    e->key[len] = '\0';
    e->key_len = len;
    e->hash = hash;
    e->value = v;
    ++t->count;
}

/* ---------------------------------------------------------------- 값 생성 */

Value value_nil(void)              { Value v; v.type = V_NIL;  v.as.n = 0; return v; }
Value value_bool(bool b)           { Value v; v.type = V_BOOL; v.as.b = b; return v; }
Value value_num(double n)          { Value v; v.type = V_NUM;  v.as.n = n; return v; }
Value value_str_take(GneString *s) { Value v; v.type = V_STR;  v.as.s = s; return v; }

GneString *gstr_new(const char *s, size_t len)
{
    if (len > SIZE_MAX - sizeof(GneString) - 1) return NULL;
    GneString *g = malloc(sizeof(GneString) + len + 1);
    if (g == NULL) return NULL;
    g->refcount = 1;
    g->len = len;
    if (len > 0 && s != NULL) memcpy(g->chars, s, len);
    g->chars[len] = '\0';
    return g;
}

GneString *gstr_concat(const GneString *a, const GneString *b)
{
    size_t len = a->len + b->len;
    GneString *g = gstr_new(NULL, len);
    if (g == NULL) return NULL;
    memcpy(g->chars, a->chars, a->len);
    memcpy(g->chars + a->len, b->chars, b->len);
    return g;
}

Value value_str_copy(const char *s, size_t len)
{
    GneString *g = gstr_new(s, len);
    if (g == NULL) fatal_oom();
    return value_str_take(g);
}

Value value_list_new(size_t cap)
{
    GneList *l = malloc(sizeof *l);
    if (l == NULL) fatal_oom();
    l->refcount = 1;
    l->count = 0;
    l->cap = cap;
    l->items = NULL;
    if (cap > 0) {
        l->items = malloc(cap * sizeof *l->items);
        if (l->items == NULL) { free(l); fatal_oom(); }
    }
    Value v; v.type = V_LIST; v.as.l = l;
    return v;
}

bool list_push(GneList *l, Value v)
{
    if (l->count == l->cap) {
        size_t new_cap = l->cap ? l->cap * 2 : 4;
        Value *grown = realloc(l->items, new_cap * sizeof *grown);
        if (grown == NULL) return false;
        l->items = grown;
        l->cap = new_cap;
    }
    l->items[l->count++] = v;
    return true;
}

/* ---------------------------------------------------------------- 참조 카운트 */

Value value_retain(Value v)
{
    switch (v.type) {
    case V_STR:    ++v.as.s->refcount; break;
    case V_LIST:   ++v.as.l->refcount; break;
    case V_OBJECT: ++v.as.o->refcount; break;
    default: break;
    }
    return v;
}

void value_release(Value v)
{
    switch (v.type) {
    case V_STR:
        if (--v.as.s->refcount == 0) free(v.as.s);
        break;
    case V_LIST: {
        GneList *l = v.as.l;
        if (--l->refcount == 0) {
            for (size_t i = 0; i < l->count; ++i) value_release(l->items[i]);
            free(l->items);
            free(l);
        }
        break;
    }
    case V_OBJECT: {
        GneObject *o = v.as.o;
        if (--o->refcount == 0) {
            table_free(o->fields);   /* 필드 값들도 연쇄 해제 */
            free(o->fields);
            free(o);
        }
        break;
    }
    default: break;
    }
}

/* ---------------------------------------------------------------- 의미 */

bool value_truthy(Value v)
{
    switch (v.type) {
    case V_NIL:    return false;
    case V_BOOL:   return v.as.b;
    case V_NUM:    return v.as.n != 0.0;
    case V_STR:    return v.as.s->len > 0;
    case V_LIST:   return v.as.l->count > 0;
    default:       return true;
    }
}

bool value_equal(Value a, Value b)
{
    if (a.type != b.type) return false;
    switch (a.type) {
    case V_NIL:    return true;
    case V_BOOL:   return a.as.b == b.as.b;
    case V_NUM:    return a.as.n == b.as.n;
    case V_STR:    return a.as.s->len == b.as.s->len &&
                          memcmp(a.as.s->chars, b.as.s->chars, a.as.s->len) == 0;
    case V_LIST: {
        const GneList *x = a.as.l, *y = b.as.l;
        if (x == y) return true;
        if (x->count != y->count) return false;
        for (size_t i = 0; i < x->count; ++i)
            if (!value_equal(x->items[i], y->items[i])) return false;
        return true;
    }
    case V_OBJECT: return a.as.o == b.as.o;   /* 객체는 동일성 비교 */
    case V_FUNC:   return a.as.fn == b.as.fn;
    case V_NATIVE: return a.as.native == b.as.native;
    }
    return false;
}

const char *value_type_name(Value v)
{
    switch (v.type) {
    case V_NIL:    return "없음";
    case V_BOOL:   return "논리";
    case V_NUM:    return "숫자";
    case V_STR:    return "문자열";
    case V_LIST:   return "배열";
    case V_FUNC:   return "함수";
    case V_NATIVE: return "내장함수";
    case V_OBJECT: return v.as.o->type_name ? v.as.o->type_name : "객체";
    }
    return "?";
}

/* ---------------------------------------------------------------- 문자열화 */

typedef struct { char *data; size_t len; size_t cap; } StrBuf;

static void sb_append(StrBuf *sb, const char *s, size_t n)
{
    if (sb->len + n + 1 > sb->cap) {
        size_t new_cap = sb->cap ? sb->cap : 32;
        while (new_cap < sb->len + n + 1) new_cap *= 2;
        char *grown = realloc(sb->data, new_cap);
        if (grown == NULL) fatal_oom();
        sb->data = grown;
        sb->cap = new_cap;
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void format_number(double n, char *buf, size_t size)
{
    if (isnan(n))               snprintf(buf, size, "숫자아님");
    else if (isinf(n))          snprintf(buf, size, n > 0 ? "무한" : "-무한");
    else if (n == floor(n) && fabs(n) < INT_PRINT_LIMIT)
                                snprintf(buf, size, "%lld", (long long)n);
    else                        snprintf(buf, size, "%.15g", n);
}

static void append_value(StrBuf *sb, Value v, bool repr, int depth)
{
    char num[NUM_BUF_SIZE];
    switch (v.type) {
    case V_NIL:  sb_append(sb, "없음", strlen("없음")); break;
    case V_BOOL: { const char *s = v.as.b ? "참" : "거짓"; sb_append(sb, s, strlen(s)); break; }
    case V_NUM:  format_number(v.as.n, num, sizeof num); sb_append(sb, num, strlen(num)); break;
    case V_STR:
        if (repr) sb_append(sb, "\"", 1);
        sb_append(sb, v.as.s->chars, v.as.s->len);
        if (repr) sb_append(sb, "\"", 1);
        break;
    case V_LIST: {
        const GneList *l = v.as.l;
        if (depth > 32) { sb_append(sb, "[...]", 5); break; }
        sb_append(sb, "[", 1);
        for (size_t i = 0; i < l->count; ++i) {
            if (i > 0) sb_append(sb, ", ", 2);
            append_value(sb, l->items[i], true, depth + 1);
        }
        sb_append(sb, "]", 1);
        break;
    }
    case V_OBJECT: {
        const char *nm = value_type_name(v);
        sb_append(sb, "<", 1);
        sb_append(sb, nm, strlen(nm));
        sb_append(sb, " 객체>", strlen(" 객체>"));
        break;
    }
    case V_FUNC: {
        sb_append(sb, "<함수 ", strlen("<함수 "));
        sb_append(sb, v.as.fn->text, v.as.fn->text_len);
        sb_append(sb, ">", 1);
        break;
    }
    case V_NATIVE: sb_append(sb, "<내장함수>", strlen("<내장함수>")); break;
    }
}

GneString *value_to_string(Value v, bool repr)
{
    if (v.type == V_STR && !repr) return value_retain(v).as.s;
    StrBuf sb = { NULL, 0, 0 };
    append_value(&sb, v, repr, 0);
    GneString *g = gstr_new(sb.data ? sb.data : "", sb.len);
    free(sb.data);
    if (g == NULL) fatal_oom();
    return g;
}

/* ---------------------------------------------------------------- UTF-8 */

static size_t utf8_seq_len(unsigned char c)
{
    if (c < 0x80)           return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

size_t utf8_length(const char *s, size_t len)
{
    size_t count = 0;
    for (size_t i = 0; i < len; ++count) {
        size_t step = utf8_seq_len((unsigned char)s[i]);
        i += (i + step <= len) ? step : 1;
    }
    return count;
}

bool utf8_slice_at(const char *s, size_t len, size_t index, size_t *off, size_t *blen)
{
    size_t i = 0, cp = 0;
    while (i < len) {
        size_t step = utf8_seq_len((unsigned char)s[i]);
        if (i + step > len) step = 1;
        if (cp == index) { *off = i; *blen = step; return true; }
        i += step;
        ++cp;
    }
    return false;
}
