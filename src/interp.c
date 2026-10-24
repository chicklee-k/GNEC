/*
 * interp.c — GNEC 트리 워킹 인터프리터
 *
 * 소유권/스코프 규칙은 1차와 동일(eval 은 +1 소유값 반환, 임시 루트 스택 사용).
 * 2차에서 추가된 제어 흐름:
 *   - 예외: 런타임 오류와 '종료' 외 예외는 gne_throw_str 로 던져지고,
 *     가장 안쪽 '시도/예외' 가 jmp_buf 로 받는다. try 진입 시 프레임/임시 스택의
 *     깊이를 기록해 두었다가, 예외로 되돌아오면 그 깊이까지 되감아 누수를 막는다.
 *   - 종료: 스택을 건너뛰어 최상위로 복귀하고 프로세스를 끝낸다 (try 로 잡히지 않음).
 */
#include "gnec.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MAX_CALL_DEPTH = 1000,
    LINE_BUF_INIT  = 128,
    MAX_FILL_GROW  = 1u << 20   /* 인덱스 대입으로 배열을 채울 수 있는 최대 간격 */
};

typedef enum { EX_NORMAL, EX_BREAK, EX_CONTINUE, EX_RETURN } ExecResult;

static _Noreturn void fatal_oom(void)
{
    fprintf(stderr, "치명적 오류: 메모리 부족\n");
    exit(EXIT_FAILURE);
}

/* ======================================================================
 * 임시 루트 스택
 * ==================================================================== */
static void temp_push(Interp *in, Value v)
{
    if (in->temp_count == in->temp_cap) {
        size_t nc = in->temp_cap ? in->temp_cap * 2 : 64;
        Value *g = realloc(in->temps, nc * sizeof *g);
        if (g == NULL) fatal_oom();
        in->temps = g; in->temp_cap = nc;
    }
    in->temps[in->temp_count++] = v;
}

static void temps_release_to(Interp *in, size_t base)
{
    while (in->temp_count > base) value_release(in->temps[--in->temp_count]);
}

/* ======================================================================
 * 예외 던지기
 * ==================================================================== */
_Noreturn void gne_throw_str(Interp *in, int line, const char *fmt, ...)
{
    char buf[512];
    int n = 0;
    if (line > 0) n = snprintf(buf, sizeof buf, "%d번째 줄: ", line);
    if (n < 0) n = 0;
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf + n, sizeof buf - (size_t)n, fmt, ap);
    va_end(ap);

    value_release(in->exc_value);
    in->exc_value = value_str_copy(buf, strlen(buf));
    in->has_exc = true;
    longjmp(in->catch_jmp ? *in->catch_jmp : *in->top_jmp, 1);
}

/* ======================================================================
 * 프레임 / 변수
 * ==================================================================== */
static Table *current_frame(Interp *in)
{
    return in->frame_count ? &in->frames[in->frame_count - 1] : NULL;
}

static void push_frame(Interp *in, int line)
{
    if (in->frame_count >= MAX_CALL_DEPTH)
        gne_throw_str(in, line, "함수 호출이 너무 깊습니다 (최대 %d단계)", MAX_CALL_DEPTH);
    if (in->frame_count == in->frame_cap) {
        size_t nc = in->frame_cap ? in->frame_cap * 2 : 16;
        Table *g = realloc(in->frames, nc * sizeof *g);
        if (g == NULL) fatal_oom();
        in->frames = g; in->frame_cap = nc;
    }
    if (!table_init(&in->frames[in->frame_count], 8)) fatal_oom();
    ++in->frame_count;
}

static void pop_frame(Interp *in) { table_free(&in->frames[--in->frame_count]); }

static void frames_pop_to(Interp *in, size_t base) { while (in->frame_count > base) pop_frame(in); }

static void assign_name(Interp *in, const char *name, size_t len, uint32_t hash, Value v)
{
    Table *f = current_frame(in);
    table_set(f ? f : &in->globals, name, len, hash, v);
}

static Value lookup_name(Interp *in, const Node *var)
{
    Table *f = current_frame(in);
    Value *slot = NULL;
    if (f) slot = table_get(f, var->text, var->text_len, var->hash);
    if (slot == NULL) slot = table_get(&in->globals, var->text, var->text_len, var->hash);
    if (slot == NULL) gne_throw_str(in, var->line, "'%s' 은(는) 정의되지 않은 이름입니다", var->text);
    return value_retain(*slot);
}

static const Node *find_struct(Interp *in, const char *name, size_t len)
{
    for (size_t i = 0; i < in->struct_count; ++i) {
        const Node *s = in->structs[i];
        if (s->text_len == len && memcmp(s->text, name, len) == 0) return s;
    }
    return NULL;
}

/* ======================================================================
 * 내장 함수
 * ==================================================================== */
typedef Value (*NativeFn)(Interp *, int, const Value *, size_t);
typedef struct { const char *name; size_t min_args, max_args; NativeFn fn; } Builtin;

static void print_value(Value v)
{
    GneString *s = value_to_string(v, false);
    fwrite(s->chars, 1, s->len, stdout);
    value_release(value_str_take(s));
}

static Value bi_print(Interp *in, int line, const Value *args, size_t argc)
{
    (void)in; (void)line;
    for (size_t i = 0; i < argc; ++i) { if (i) fputc(' ', stdout); print_value(args[i]); }
    fputc('\n', stdout);
    return value_nil();
}

static Value bi_length(Interp *in, int line, const Value *args, size_t argc)
{
    (void)argc;
    if (args[0].type == V_STR)  return value_num((double)utf8_length(args[0].as.s->chars, args[0].as.s->len));
    if (args[0].type == V_LIST) return value_num((double)args[0].as.l->count);
    gne_throw_str(in, line, "길이: 문자열이나 배열이 필요합니다");
}

static Value bi_push(Interp *in, int line, const Value *args, size_t argc)
{
    (void)argc;
    if (args[0].type != V_LIST) gne_throw_str(in, line, "추가: 배열이 필요합니다");
    if (!list_push(args[0].as.l, value_retain(args[1]))) fatal_oom();
    return value_nil();
}

static Value bi_type(Interp *in, int line, const Value *args, size_t argc)
{
    (void)in; (void)line; (void)argc;
    const char *nm = value_type_name(args[0]);
    return value_str_copy(nm, strlen(nm));
}

static Value bi_number(Interp *in, int line, const Value *args, size_t argc)
{
    (void)in; (void)line; (void)argc;
    Value v = args[0];
    if (v.type == V_NUM) return v;
    if (v.type == V_BOOL) return value_num(v.as.b ? 1 : 0);
    if (v.type == V_STR) {
        char *end = NULL;
        double d = strtod(v.as.s->chars, &end);
        if (end == v.as.s->chars || (size_t)(end - v.as.s->chars) != v.as.s->len) return value_nil();
        return value_num(d);
    }
    return value_nil();
}

static Value bi_string(Interp *in, int line, const Value *args, size_t argc)
{
    (void)in; (void)line; (void)argc;
    return value_str_take(value_to_string(args[0], false));
}

static const Builtin BUILTINS[] = {
    { "출력", 0, SIZE_MAX, bi_print },
    { "길이", 1, 1, bi_length },
    { "추가", 2, 2, bi_push },
    { "종류", 1, 1, bi_type },
    { "숫자", 1, 1, bi_number },
    { "문자", 1, 1, bi_string },
};
enum { BUILTIN_COUNT = sizeof BUILTINS / sizeof BUILTINS[0] };

/* ======================================================================
 * 평가
 * ==================================================================== */
static Value eval(Interp *in, const Node *n);
static ExecResult exec_block(Interp *in, const Node *blk);

static Value make_object(const Node *def, int line)
{
    GneObject *o = malloc(sizeof *o);
    if (o == NULL) fatal_oom();
    o->refcount = 1;
    o->type_name = def->text;
    o->fields = malloc(sizeof(Table));
    if (o->fields == NULL) { free(o); fatal_oom(); }
    if (!table_init(o->fields, 8)) { free(o->fields); free(o); fatal_oom(); }
    Value ov; ov.type = V_OBJECT; ov.as.o = o;

    /* 선언된 필드를 없음(또는 빈 배열)으로 초기화 */
    for (size_t i = 0; i < def->list.count; ++i) {
        const Node *f = def->list.items[i];
        Value init = f->boolean ? value_list_new(0) : value_nil();
        table_set(o->fields, f->text, f->text_len, f->hash, init);
    }
    (void)line;
    return ov;
}

static Value call_function(Interp *in, const Node *fn, const Value *args, size_t argc, int line)
{
    if (argc != fn->list.count)
        gne_throw_str(in, line, "함수 '%s' 는 인자 %zu개가 필요하지만 %zu개를 받았습니다",
                      fn->text, fn->list.count, argc);
    push_frame(in, line);
    Table *frame = current_frame(in);
    for (size_t i = 0; i < argc; ++i) {
        const Node *p = fn->list.items[i];
        table_set(frame, p->text, p->text_len, p->hash, value_retain(args[i]));
    }
    ExecResult r = exec_block(in, fn->a);
    pop_frame(in);
    if (r == EX_RETURN) {
        Value v = in->ret_value;
        in->ret_value = value_nil();
        return v;
    }
    return value_nil();
}

static Value call_value(Interp *in, Value callee, const Value *args, size_t argc, int line)
{
    if (callee.type == V_NATIVE) {
        const Builtin *b = &BUILTINS[callee.as.native];
        if (argc < b->min_args || argc > b->max_args)
            gne_throw_str(in, line, "%s: 인자 개수가 맞지 않습니다", b->name);
        return b->fn(in, line, args, argc);
    }
    if (callee.type == V_FUNC) return call_function(in, callee.as.fn, args, argc, line);
    gne_throw_str(in, line, "%s 은(는) 호출할 수 없습니다", value_type_name(callee));
}

static Value eval_call(Interp *in, const Node *n)
{
    size_t base = in->temp_count;
    size_t argc = n->list.count;
    temp_push(in, eval(in, n->a));
    for (size_t i = 0; i < argc; ++i) temp_push(in, eval(in, n->list.items[i]));
    Value callee = in->temps[base];
    Value result = call_value(in, callee, &in->temps[base + 1], argc, n->line);
    temps_release_to(in, base);
    return result;
}

/* 메서드: 콘솔.로그/콘솔.오류 는 내장, 그 외는 객체 필드가 함수일 때 호출 */
static Value eval_method(Interp *in, const Node *n)
{
    if (n->a->kind == N_VAR && strcmp(n->a->text, "콘솔") == 0) {
        bool to_err = (strcmp(n->text, "오류") == 0);
        bool is_log = (strcmp(n->text, "로그") == 0);
        if (to_err || is_log) {
            size_t base = in->temp_count;
            for (size_t i = 0; i < n->list.count; ++i) temp_push(in, eval(in, n->list.items[i]));
            for (size_t i = 0; i < n->list.count; ++i) {
                GneString *s = value_to_string(in->temps[base + i], false);
                FILE *out = to_err ? stderr : stdout;
                if (i) fputc(' ', out);
                fwrite(s->chars, 1, s->len, out);
                value_release(value_str_take(s));
            }
            fputc('\n', to_err ? stderr : stdout);
            temps_release_to(in, base);
            return value_nil();
        }
        gne_throw_str(in, n->line, "콘솔.%s 는 없는 메서드입니다", n->text);
    }

    size_t base = in->temp_count;
    temp_push(in, eval(in, n->a));
    Value obj = in->temps[base];
    if (obj.type != V_OBJECT) gne_throw_str(in, n->line, "%s 에는 메서드가 없습니다", value_type_name(obj));
    Value *slot = table_get(obj.as.o->fields, n->text, n->text_len, n->hash);
    if (slot == NULL || slot->type != V_FUNC)
        gne_throw_str(in, n->line, "'%s' 메서드를 찾을 수 없습니다", n->text);
    const Node *fn = slot->as.fn;
    for (size_t i = 0; i < n->list.count; ++i) temp_push(in, eval(in, n->list.items[i]));
    Value result = call_function(in, fn, &in->temps[base + 1], n->list.count, n->line);
    temps_release_to(in, base);
    return result;
}

static size_t check_index(Interp *in, int line, Value idx, size_t count)
{
    if (idx.type != V_NUM || idx.as.n != floor(idx.as.n))
        gne_throw_str(in, line, "인덱스는 정수여야 합니다");
    double i = idx.as.n;
    if (i < 0) i += (double)count;
    if (i < 0 || i >= (double)count)
        gne_throw_str(in, line, "인덱스 %g 이(가) 범위를 벗어났습니다", idx.as.n);
    return (size_t)i;
}

static Value eval_index(Interp *in, const Node *n)
{
    size_t base = in->temp_count;
    temp_push(in, eval(in, n->a));
    temp_push(in, eval(in, n->b));
    Value obj = in->temps[base], idx = in->temps[base + 1];
    Value result;
    if (obj.type == V_LIST) {
        result = value_retain(obj.as.l->items[check_index(in, n->line, idx, obj.as.l->count)]);
    } else if (obj.type == V_STR) {
        const GneString *s = obj.as.s;
        size_t cp = check_index(in, n->line, idx, utf8_length(s->chars, s->len));
        size_t off = 0, blen = 0;
        utf8_slice_at(s->chars, s->len, cp, &off, &blen);
        result = value_str_copy(s->chars + off, blen);
    } else {
        gne_throw_str(in, n->line, "%s 은(는) 인덱싱할 수 없습니다", value_type_name(obj));
    }
    temps_release_to(in, base);
    return result;
}

static Value eval_field(Interp *in, const Node *n)
{
    size_t base = in->temp_count;
    temp_push(in, eval(in, n->a));
    Value obj = in->temps[base];
    if (obj.type != V_OBJECT) gne_throw_str(in, n->line, "%s 에는 필드가 없습니다", value_type_name(obj));
    Value *slot = table_get(obj.as.o->fields, n->text, n->text_len, n->hash);
    if (slot == NULL) gne_throw_str(in, n->line, "'%s' 필드가 없습니다 (나쁜 변수)", n->text);
    Value result = value_retain(*slot);
    temps_release_to(in, base);
    return result;
}

static Value concat(Value a, Value b)
{
    GneString *sa = value_to_string(a, false);
    GneString *sb = value_to_string(b, false);
    GneString *r = gstr_concat(sa, sb);
    value_release(value_str_take(sa));
    value_release(value_str_take(sb));
    if (r == NULL) fatal_oom();
    return value_str_take(r);
}

static int cmp_str(const GneString *a, const GneString *b)
{
    size_t m = a->len < b->len ? a->len : b->len;
    int c = memcmp(a->chars, b->chars, m);
    if (c) return c;
    return (a->len > b->len) - (a->len < b->len);
}

static Value eval_binary(Interp *in, const Node *n)
{
    size_t base = in->temp_count;
    temp_push(in, eval(in, n->a));
    temp_push(in, eval(in, n->b));
    Value a = in->temps[base], b = in->temps[base + 1];
    Value r = value_nil();
    TokenType op = (TokenType)n->op;

    switch (op) {
    case TOK_EQEQ: r = value_bool(value_equal(a, b)); break;
    case TOK_NEQ:  r = value_bool(!value_equal(a, b)); break;
    case TOK_PLUS:
        if (a.type == V_NUM && b.type == V_NUM) r = value_num(a.as.n + b.as.n);
        else if (a.type == V_STR || b.type == V_STR) r = concat(a, b);
        else if (a.type == V_LIST && b.type == V_LIST) {
            r = value_list_new(a.as.l->count + b.as.l->count);
            for (size_t i = 0; i < a.as.l->count; ++i) list_push(r.as.l, value_retain(a.as.l->items[i]));
            for (size_t i = 0; i < b.as.l->count; ++i) list_push(r.as.l, value_retain(b.as.l->items[i]));
        } else gne_throw_str(in, n->line, "%s 와(과) %s 은(는) 더할 수 없습니다", value_type_name(a), value_type_name(b));
        break;
    case TOK_MINUS: case TOK_STAR: case TOK_SLASH: case TOK_PERCENT:
        if (a.type != V_NUM || b.type != V_NUM)
            gne_throw_str(in, n->line, "산술 연산은 숫자끼리만 가능합니다 (%s, %s)", value_type_name(a), value_type_name(b));
        if (op == TOK_MINUS) r = value_num(a.as.n - b.as.n);
        else if (op == TOK_STAR) r = value_num(a.as.n * b.as.n);
        else {
            if (b.as.n == 0.0) gne_throw_str(in, n->line, "0 으로 나눌 수 없습니다");
            r = value_num(op == TOK_SLASH ? a.as.n / b.as.n : fmod(a.as.n, b.as.n));
        }
        break;
    case TOK_LT: case TOK_LE: case TOK_GT: case TOK_GE: {
        int c;
        if (a.type == V_NUM && b.type == V_NUM) c = (a.as.n > b.as.n) - (a.as.n < b.as.n);
        else if (a.type == V_STR && b.type == V_STR) c = cmp_str(a.as.s, b.as.s);
        else gne_throw_str(in, n->line, "%s 와(과) %s 은(는) 비교할 수 없습니다", value_type_name(a), value_type_name(b));
        r = value_bool(op == TOK_LT ? c < 0 : op == TOK_LE ? c <= 0 : op == TOK_GT ? c > 0 : c >= 0);
        break;
    }
    default: gne_throw_str(in, n->line, "내부 오류: 알 수 없는 연산자");
    }
    temps_release_to(in, base);
    return r;
}

/* 배열 인덱스 대입: index==count 면 추가, 범위 밖이면 없음으로 채운다 */
static void list_set_index(Interp *in, int line, GneList *l, Value idxv, Value v)
{
    if (idxv.type != V_NUM || idxv.as.n != floor(idxv.as.n) || idxv.as.n < 0)
        gne_throw_str(in, line, "배열 인덱스는 0 이상의 정수여야 합니다");
    double di = idxv.as.n;
    if (di > (double)(l->count + MAX_FILL_GROW))
        gne_throw_str(in, line, "배열 인덱스가 너무 큽니다");
    size_t i = (size_t)di;
    while (l->count <= i) { if (!list_push(l, value_nil())) fatal_oom(); }
    value_release(l->items[i]);
    l->items[i] = value_retain(v);
}

static Value eval_assign(Interp *in, const Node *n)
{
    const Node *target = n->a;

    if (target->kind == N_VAR) {
        Value v = eval(in, n->b);
        assign_name(in, target->text, target->text_len, target->hash, value_retain(v));
        return v;
    }
    if (target->kind == N_FIELD) {
        size_t base = in->temp_count;
        temp_push(in, eval(in, target->a));
        temp_push(in, eval(in, n->b));
        Value obj = in->temps[base], v = in->temps[base + 1];
        if (obj.type != V_OBJECT) gne_throw_str(in, n->line, "%s 에는 필드를 대입할 수 없습니다", value_type_name(obj));
        table_set(obj.as.o->fields, target->text, target->text_len, target->hash, value_retain(v));
        Value result = value_retain(v);
        temps_release_to(in, base);
        return result;
    }
    /* N_INDEX */
    size_t base = in->temp_count;
    temp_push(in, eval(in, target->a));
    temp_push(in, eval(in, target->b));
    temp_push(in, eval(in, n->b));
    Value obj = in->temps[base], idx = in->temps[base + 1], v = in->temps[base + 2];
    if (obj.type != V_LIST) gne_throw_str(in, n->line, "%s 의 원소는 바꿀 수 없습니다", value_type_name(obj));
    list_set_index(in, n->line, obj.as.l, idx, v);
    Value result = value_retain(v);
    temps_release_to(in, base);
    return result;
}

static Value eval_preop(Interp *in, const Node *n)
{
    /* lvalue (++/--) 를 "lvalue = lvalue ± 1" 로 환원 */
    Value cur_v = eval(in, n->a);
    if (cur_v.type != V_NUM) {
        const char *tn = value_type_name(cur_v);
        value_release(cur_v);
        gne_throw_str(in, n->line, "%s 에는 증감을 쓸 수 없습니다", tn);
    }
    double nv = cur_v.as.n + (n->op == '+' ? 1.0 : -1.0);
    value_release(cur_v);

    const Node *t = n->a;
    if (t->kind == N_VAR) {
        assign_name(in, t->text, t->text_len, t->hash, value_num(nv));
    } else if (t->kind == N_FIELD) {
        size_t base = in->temp_count;
        temp_push(in, eval(in, t->a));
        Value obj = in->temps[base];
        if (obj.type != V_OBJECT) gne_throw_str(in, n->line, "필드를 증감할 수 없습니다");
        table_set(obj.as.o->fields, t->text, t->text_len, t->hash, value_num(nv));
        temps_release_to(in, base);
    } else { /* N_INDEX */
        size_t base = in->temp_count;
        temp_push(in, eval(in, t->a));
        temp_push(in, eval(in, t->b));
        Value obj = in->temps[base], idx = in->temps[base + 1];
        if (obj.type != V_LIST) gne_throw_str(in, n->line, "원소를 증감할 수 없습니다");
        list_set_index(in, n->line, obj.as.l, idx, value_num(nv));
        temps_release_to(in, base);
    }
    return value_num(nv);
}

static Value eval_interp(Interp *in, const Node *n)
{
    /* 조각들을 모두 문자열로 바꿔 이어붙인다 */
    GneString *acc = gstr_new("", 0);
    if (acc == NULL) fatal_oom();
    for (size_t i = 0; i < n->list.count; ++i) {
        Value piece = eval(in, n->list.items[i]);
        GneString *ps = value_to_string(piece, false);
        GneString *merged = gstr_concat(acc, ps);
        value_release(value_str_take(acc));
        value_release(value_str_take(ps));
        value_release(piece);
        if (merged == NULL) fatal_oom();
        acc = merged;
    }
    return value_str_take(acc);
}

static Value eval(Interp *in, const Node *n)
{
    switch (n->kind) {
    case N_NUM:  return value_num(n->num);
    case N_STR:  return value_str_copy(n->text, n->text_len);
    case N_BOOL: return value_bool(n->boolean);
    case N_NIL:  return value_nil();
    case N_VAR:  return lookup_name(in, n);
    case S_INTERP: return eval_interp(in, n);
    case N_LIST: {
        size_t base = in->temp_count;
        Value l = value_list_new(n->list.count);
        temp_push(in, l);
        for (size_t i = 0; i < n->list.count; ++i)
            if (!list_push(l.as.l, eval(in, n->list.items[i]))) fatal_oom();
        Value result = value_retain(l);
        temps_release_to(in, base);
        return result;
    }
    case N_INDEX:  return eval_index(in, n);
    case N_FIELD:  return eval_field(in, n);
    case N_CALL:   return eval_call(in, n);
    case N_METHOD: return eval_method(in, n);
    case N_NEW: {
        const Node *def = find_struct(in, n->text, n->text_len);
        if (def == NULL) gne_throw_str(in, n->line, "'%s' 구조체가 정의되지 않았습니다", n->text);
        return make_object(def, n->line);
    }
    case N_NEG: {
        Value v = eval(in, n->a);
        if (v.type != V_NUM) { const char *tn = value_type_name(v); value_release(v); gne_throw_str(in, n->line, "%s 에는 '-' 를 쓸 수 없습니다", tn); }
        return value_num(-v.as.n);
    }
    case N_NOT: {
        Value v = eval(in, n->a);
        bool t = value_truthy(v);
        value_release(v);
        return value_bool(!t);
    }
    case N_AND: case N_OR: {
        Value l = eval(in, n->a);
        bool lt = value_truthy(l);
        value_release(l);
        if (n->kind == N_AND && !lt) return value_bool(false);
        if (n->kind == N_OR && lt)   return value_bool(true);
        Value r = eval(in, n->b);
        bool rt = value_truthy(r);
        value_release(r);
        return value_bool(rt);
    }
    case N_BINARY: return eval_binary(in, n);
    case N_ASSIGN: return eval_assign(in, n);
    case N_PREOP:  return eval_preop(in, n);
    default: gne_throw_str(in, n->line, "내부 오류: 식이 아닌 노드");
    }
}

/* ======================================================================
 * 실행
 * ==================================================================== */
static bool eval_cond(Interp *in, const Node *n)
{
    Value v = eval(in, n);
    bool t = value_truthy(v);
    value_release(v);
    return t;
}

static void define_struct(Interp *in, const Node *n)
{
    if (in->struct_count == in->struct_cap) {
        size_t nc = in->struct_cap ? in->struct_cap * 2 : 8;
        const Node **g = realloc(in->structs, nc * sizeof *g);
        if (g == NULL) fatal_oom();
        in->structs = g; in->struct_cap = nc;
    }
    /* 같은 이름 재정의는 덮어쓴다 */
    for (size_t i = 0; i < in->struct_count; ++i)
        if (in->structs[i]->text_len == n->text_len && memcmp(in->structs[i]->text, n->text, n->text_len) == 0) {
            in->structs[i] = n; return;
        }
    in->structs[in->struct_count++] = n;
}

static void define_func(Interp *in, const Node *n)
{
    Value f; f.type = V_FUNC; f.as.fn = n;
    assign_name(in, n->text, n->text_len, n->hash, f);
}

static ExecResult exec(Interp *in, const Node *n);

static ExecResult exec_block(Interp *in, const Node *blk)
{
    for (size_t i = 0; i < blk->list.count; ++i) {
        ExecResult r = exec(in, blk->list.items[i]);
        if (r != EX_NORMAL) return r;
    }
    return EX_NORMAL;
}

static ExecResult exec(Interp *in, const Node *n)
{
    switch (n->kind) {
    case S_EXPR:    value_release(eval(in, n->a)); return EX_NORMAL;
    case S_VARDECL: {
        Value v = n->a ? eval(in, n->a) : (n->boolean ? value_list_new(0) : value_nil());
        assign_name(in, n->text, n->text_len, n->hash, v);
        return EX_NORMAL;
    }
    case S_BLOCK:   return exec_block(in, n);
    case S_PROGRAM: return exec_block(in, n->a);
    case S_IF:
        if (eval_cond(in, n->a)) return exec_block(in, n->b);
        if (n->c == NULL) return EX_NORMAL;
        return (n->c->kind == S_IF) ? exec(in, n->c) : exec_block(in, n->c);
    case S_WHILE:
        while (eval_cond(in, n->a)) {
            ExecResult r = exec_block(in, n->b);
            if (r == EX_BREAK) break;
            if (r == EX_RETURN) return r;
        }
        return EX_NORMAL;
    case S_FUNC:   define_func(in, n); return EX_NORMAL;
    case S_STRUCT: define_struct(in, n); return EX_NORMAL;
    case S_IMPORT: return EX_NORMAL;      /* 모듈: 구조만, 실행 시 무시 */
    case S_RETURN:
        value_release(in->ret_value);
        in->ret_value = n->a ? eval(in, n->a) : value_nil();
        return EX_RETURN;
    case S_BREAK:    return EX_BREAK;
    case S_CONTINUE: return EX_CONTINUE;
    case S_EXIT: {
        int code = 0;
        if (n->a) {
            Value v = eval(in, n->a);
            if (v.type == V_NUM) code = (int)v.as.n;
            value_release(v);
        }
        in->exit_code = code;
        in->exiting = true;
        fflush(stdout);
        longjmp(*in->top_jmp, 1);       /* try 로 잡히지 않고 즉시 종료 */
    }
    case S_TRY: {
        /* 예외 복귀 지점을 설치. 되돌아오면 프레임/임시 스택을 try 진입 시점으로 되감는다. */
        size_t save_frames = in->frame_count;
        size_t save_temps  = in->temp_count;
        jmp_buf *prev_catch = in->catch_jmp;
        jmp_buf here;
        if (setjmp(here) == 0) {
            in->catch_jmp = &here;
            ExecResult r = exec_block(in, n->a);
            in->catch_jmp = prev_catch;
            return r;
        }
        /* 예외로 복귀 */
        in->catch_jmp = prev_catch;
        frames_pop_to(in, save_frames);
        temps_release_to(in, save_temps);
        in->has_exc = false;
        if (n->text != NULL) {   /* 예외 변수 바인딩 */
            Value exc = in->exc_value;
            in->exc_value = value_nil();
            assign_name(in, n->text, n->text_len, gne_hash(n->text, n->text_len), exc);
        } else {
            value_release(in->exc_value);
            in->exc_value = value_nil();
        }
        return exec_block(in, n->b);
    }
    default: value_release(eval(in, n)); return EX_NORMAL;
    }
}

/* ======================================================================
 * 공개 API
 * ==================================================================== */
bool interp_init(Interp *in)
{
    memset(in, 0, sizeof *in);
    in->ret_value = value_nil();
    in->exc_value = value_nil();
    if (!table_init(&in->globals, 16)) return false;
    for (int i = 0; i < (int)BUILTIN_COUNT; ++i) {
        Value v; v.type = V_NATIVE; v.as.native = i;
        const char *nm = BUILTINS[i].name;
        size_t len = strlen(nm);
        table_set(&in->globals, nm, len, gne_hash(nm, len), v);
    }
    return true;
}

void interp_free(Interp *in)
{
    frames_pop_to(in, 0);
    free(in->frames);
    temps_release_to(in, 0);
    free(in->temps);
    free(in->structs);
    table_free(&in->globals);
    value_release(in->ret_value);
    value_release(in->exc_value);
    memset(in, 0, sizeof *in);
}

bool interp_run(Interp *in, const Program *prog)
{
    jmp_buf top;
    in->top_jmp = &top;
    in->catch_jmp = NULL;

    if (setjmp(top) != 0) {
        /* 종료문 또는 잡히지 않은 예외로 복귀 */
        frames_pop_to(in, 0);
        temps_release_to(in, 0);
        bool ok = in->exiting;      /* 종료문은 정상 종료로 간주 */
        if (in->has_exc) {
            GneString *s = value_to_string(in->exc_value, false);
            fflush(stdout);
            fprintf(stderr, "[GNE 예외] %.*s\n", (int)s->len, s->chars);
            value_release(value_str_take(s));
            in->has_exc = false;
            ok = false;
        }
        value_release(in->exc_value); in->exc_value = value_nil();
        value_release(in->ret_value); in->ret_value = value_nil();
        in->top_jmp = NULL;
        fflush(stdout);
        return ok;
    }

    /* 1차 패스: 최상위 함수/구조체 선언을 끌어올린다 */
    const Node *root = prog->root;
    for (size_t i = 0; i < root->list.count; ++i) {
        const Node *s = root->list.items[i];
        if (s->kind == S_FUNC)   define_func(in, s);
        else if (s->kind == S_STRUCT) define_struct(in, s);
    }
    /* 2차 패스: 나머지 문장 실행 (프로그램 블록 포함) */
    for (size_t i = 0; i < root->list.count; ++i) {
        const Node *s = root->list.items[i];
        if (s->kind == S_FUNC || s->kind == S_STRUCT) continue;
        exec(in, s);
    }

    value_release(in->ret_value); in->ret_value = value_nil();
    in->top_jmp = NULL;
    fflush(stdout);
    return true;
}
