/*
 * parser.c — GNEC 재귀 하강 파서: 토큰 → AST
 *
 * 문장 (대부분 ';' 로 종결, 블록형은 '}' 로 종결)
 *   선언문    : 선언 이름 ('[' ']')? ((= | 는) 식)? ;
 *   대입문    : lvalue (= | 는) 식 ;      (lvalue = 변수 | 필드 | 인덱스)
 *   증감문    : lvalue (++ | --) ;
 *   조건문    : 만약 ( 식 ) 블록 (아니면 (조건문 | 블록))?
 *   반복문    : 반복 ( 식 ) 블록
 *   함수      : 함수 이름 ( 매개변수들 ) 블록
 *   구조체    : 구조체 이름 { (선언 이름 ('[' ']')? ;)* }
 *   프로그램  : 프로그램 블록        (블록을 즉시 실행)
 *   예외      : 시도 블록 예외 ( 이름? ) 블록
 *   그외      : 돌려주기 식? ; | 완료 ; | 계속 ; | 종료 (식)? ; |
 *               가져오기 이름 ; | 내보내기 이름 ; | 식 ;
 *
 * 문자열 보간 토큰은 S_INTERP 노드로 바뀌며, ${} 안의 소스는 재귀적으로 다시
 * 렉싱·파싱되어 하위 식 노드가 된다. 그렇게 만든 임시 토큰열은 parse_program 이
 * 성공·실패와 무관하게 일괄 해제한다.
 */
#include "gnec.h"
#include "keywords.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <setjmp.h>

enum { MAX_NESTING = 200, ERR_SUB = 256 };

typedef struct {
    const Token *toks;
    size_t       pos;
    size_t       ntok;
    Program     *prog;
    int          depth;
    int          loop_depth;
    jmp_buf     *jb;
    char        *err;
    size_t       errlen;
    /* 보간 식에서 임시로 만든 토큰열들 (정리용) */
    TokenList  **subtoks;
    size_t       subtok_count;
    size_t       subtok_cap;
} Parser;

uint32_t gne_hash(const char *s, size_t len);   /* value.c */

static _Noreturn void perr(Parser *ps, const char *fmt, ...)
{
    int line = ps->pos < ps->ntok ? ps->toks[ps->pos].line : 0;
    int n = snprintf(ps->err, ps->errlen, "%d번째 줄: ", line);
    if (n > 0 && (size_t)n < ps->errlen) {
        va_list ap; va_start(ap, fmt);
        vsnprintf(ps->err + n, ps->errlen - (size_t)n, fmt, ap);
        va_end(ap);
    }
    longjmp(*ps->jb, 1);
}

static TokenType cur(const Parser *ps) { return ps->toks[ps->pos].type; }
static const Token *tok(const Parser *ps) { return &ps->toks[ps->pos]; }

static const Token *adv(Parser *ps)
{
    const Token *t = &ps->toks[ps->pos];
    if (t->type != TOK_EOF) ++ps->pos;
    return t;
}

static bool accept(Parser *ps, TokenType t) { if (cur(ps) == t) { adv(ps); return true; } return false; }

static const Token *expect(Parser *ps, TokenType t, const char *what)
{
    if (cur(ps) != t) perr(ps, "%s 이(가) 필요합니다", what);
    return adv(ps);
}

static Node *new_node(Parser *ps, NodeKind kind, int line)
{
    Program *pg = ps->prog;
    if (pg->node_count == pg->node_cap) {
        size_t nc = pg->node_cap ? pg->node_cap * 2 : 128;
        Node **g = realloc(pg->nodes, nc * sizeof *g);
        if (g == NULL) perr(ps, "메모리 부족");
        pg->nodes = g; pg->node_cap = nc;
    }
    Node *n = calloc(1, sizeof *n);
    if (n == NULL) perr(ps, "메모리 부족");
    n->kind = kind; n->line = line;
    pg->nodes[pg->node_count++] = n;
    return n;
}

static void list_add(Parser *ps, NodeList *l, Node *n)
{
    if (l->count == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 4;
        Node **g = realloc(l->items, nc * sizeof *g);
        if (g == NULL) perr(ps, "메모리 부족");
        l->items = g; l->cap = nc;
    }
    l->items[l->count++] = n;
}

static void set_text(Parser *ps, Node *n, const char *s, size_t len)
{
    n->text = malloc(len + 1);
    if (n->text == NULL) perr(ps, "메모리 부족");
    memcpy(n->text, s, len);
    n->text[len] = '\0';
    n->text_len = len;
    n->hash = gne_hash(s, len);
}

static void enter(Parser *ps) { if (++ps->depth > MAX_NESTING) perr(ps, "중첩이 너무 깊습니다"); }
static void leave(Parser *ps) { --ps->depth; }

/* ---------------------------------------------------------------- 보간 */

static Node *parse_expression(Parser *ps);
static Node *parse_tokens_as_expr(Parser *ps, TokenList *sub);

/* 보간 식 소스를 렉싱하여 ps->subtoks 에 보관하고 그 토큰열을 돌려준다 */
static TokenList *lex_sub(Parser *ps, const char *src, int line)
{
    TokenList *sub = malloc(sizeof *sub);
    if (sub == NULL) perr(ps, "메모리 부족");
    char suberr[ERR_SUB];
    if (!lex_source(src, sub, suberr, sizeof suberr)) {
        token_list_free(sub);
        free(sub);
        perr(ps, "%d번째 줄 보간식: %s", line, suberr);
    }
    if (ps->subtok_count == ps->subtok_cap) {
        size_t nc = ps->subtok_cap ? ps->subtok_cap * 2 : 8;
        TokenList **g = realloc(ps->subtoks, nc * sizeof *g);
        if (g == NULL) { token_list_free(sub); free(sub); perr(ps, "메모리 부족"); }
        ps->subtoks = g; ps->subtok_cap = nc;
    }
    ps->subtoks[ps->subtok_count++] = sub;
    return sub;
}

static Node *parse_string_token(Parser *ps, const Token *t)
{
    /* 조각이 하나뿐인 순수 리터럴이면 N_STR 로 단순화 */
    if (t->part_count == 1 && !t->parts[0].is_expr) {
        Node *n = new_node(ps, N_STR, t->line);
        set_text(ps, n, t->parts[0].str, t->parts[0].str_len);
        return n;
    }
    Node *n = new_node(ps, S_INTERP, t->line);
    for (size_t i = 0; i < t->part_count; ++i) {
        const StrPart *sp = &t->parts[i];
        if (!sp->is_expr) {
            Node *lit = new_node(ps, N_STR, t->line);
            set_text(ps, lit, sp->str, sp->str_len);
            list_add(ps, &n->list, lit);
        } else {
            TokenList *sub = lex_sub(ps, sp->expr_src, t->line);
            Node *e = parse_tokens_as_expr(ps, sub);
            list_add(ps, &n->list, e);
        }
    }
    return n;
}

/* ---------------------------------------------------------------- 식 */

static Node *parse_primary(Parser *ps)
{
    const Token *t = tok(ps);
    Node *n;
    switch (t->type) {
    case TOK_NUMBER:
        adv(ps); n = new_node(ps, N_NUM, t->line); n->num = t->number; return n;
    case TOK_STRING:
        adv(ps); return parse_string_token(ps, t);
    case TOK_TRUE: case TOK_FALSE:
        adv(ps); n = new_node(ps, N_BOOL, t->line); n->boolean = (t->type == TOK_TRUE); return n;
    case TOK_NIL:
        adv(ps); return new_node(ps, N_NIL, t->line);
    case TOK_IDENT:
        adv(ps); n = new_node(ps, N_VAR, t->line); set_text(ps, n, t->lexeme, t->lexeme_len); return n;
    case TOK_NEW: {
        adv(ps);
        const Token *name = expect(ps, TOK_IDENT, "구조체 이름");
        n = new_node(ps, N_NEW, t->line);
        set_text(ps, n, name->lexeme, name->lexeme_len);
        if (accept(ps, TOK_LPAREN)) {   /* 생성 인자는 받되 무시 */
            if (!accept(ps, TOK_RPAREN)) {
                do { (void)parse_expression(ps); } while (accept(ps, TOK_COMMA));
                expect(ps, TOK_RPAREN, "')'");
            }
        }
        return n;
    }
    case TOK_LPAREN:
        adv(ps); n = parse_expression(ps); expect(ps, TOK_RPAREN, "')'"); return n;
    case TOK_LBRACKET:
        adv(ps); n = new_node(ps, N_LIST, t->line);
        if (!accept(ps, TOK_RBRACKET)) {
            do {
                if (cur(ps) == TOK_RBRACKET) break;
                list_add(ps, &n->list, parse_expression(ps));
            } while (accept(ps, TOK_COMMA));
            expect(ps, TOK_RBRACKET, "']'");
        }
        return n;
    default:
        perr(ps, "식이 와야 할 자리입니다");
    }
}

static Node *parse_postfix(Parser *ps)
{
    Node *n = parse_primary(ps);
    for (;;) {
        int line = tok(ps)->line;
        if (accept(ps, TOK_LPAREN)) {
            Node *call = new_node(ps, N_CALL, line);
            call->a = n;
            if (!accept(ps, TOK_RPAREN)) {
                do { list_add(ps, &call->list, parse_expression(ps)); } while (accept(ps, TOK_COMMA));
                expect(ps, TOK_RPAREN, "')'");
            }
            n = call;
        } else if (accept(ps, TOK_LBRACKET)) {
            Node *idx = new_node(ps, N_INDEX, line);
            idx->a = n; idx->b = parse_expression(ps);
            expect(ps, TOK_RBRACKET, "']'");
            n = idx;
        } else if (accept(ps, TOK_DOT)) {
            const Token *fld = expect(ps, TOK_IDENT, "필드 이름");
            if (cur(ps) == TOK_LPAREN) {       /* 메서드 호출 obj.name(...) */
                adv(ps);
                Node *m = new_node(ps, N_METHOD, line);
                m->a = n;
                set_text(ps, m, fld->lexeme, fld->lexeme_len);
                if (!accept(ps, TOK_RPAREN)) {
                    do { list_add(ps, &m->list, parse_expression(ps)); } while (accept(ps, TOK_COMMA));
                    expect(ps, TOK_RPAREN, "')'");
                }
                n = m;
            } else {
                Node *f = new_node(ps, N_FIELD, line);
                f->a = n;
                set_text(ps, f, fld->lexeme, fld->lexeme_len);
                n = f;
            }
        } else {
            return n;
        }
    }
}

static Node *parse_unary(Parser *ps)
{
    int line = tok(ps)->line;
    if (accept(ps, TOK_MINUS)) {
        enter(ps);
        Node *n = new_node(ps, N_NEG, line);
        n->a = parse_unary(ps);
        leave(ps);
        return n;
    }
    return parse_postfix(ps);
}

static Node *mk_bin(Parser *ps, int op, Node *a, Node *b, int line)
{
    Node *n = new_node(ps, N_BINARY, line);
    n->op = op; n->a = a; n->b = b;
    return n;
}

static Node *parse_term(Parser *ps)
{
    Node *n = parse_unary(ps);
    for (;;) {
        TokenType t = cur(ps);
        if (t != TOK_STAR && t != TOK_SLASH && t != TOK_PERCENT) return n;
        int line = adv(ps)->line;
        n = mk_bin(ps, (int)t, n, parse_unary(ps), line);
    }
}

static Node *parse_additive(Parser *ps)
{
    Node *n = parse_term(ps);
    for (;;) {
        TokenType t = cur(ps);
        if (t != TOK_PLUS && t != TOK_MINUS) return n;
        int line = adv(ps)->line;
        n = mk_bin(ps, (int)t, n, parse_term(ps), line);
    }
}

static Node *parse_comparison(Parser *ps)
{
    Node *n = parse_additive(ps);
    for (;;) {
        TokenType t = cur(ps);
        if (t != TOK_EQEQ && t != TOK_NEQ && t != TOK_LT && t != TOK_LE && t != TOK_GT && t != TOK_GE)
            return n;
        int line = adv(ps)->line;
        n = mk_bin(ps, (int)t, n, parse_additive(ps), line);
    }
}

static Node *parse_not(Parser *ps)
{
    int line = tok(ps)->line;
    if (accept(ps, TOK_NOT)) {
        enter(ps);
        Node *n = new_node(ps, N_NOT, line);
        n->a = parse_not(ps);
        leave(ps);
        return n;
    }
    return parse_comparison(ps);
}

static Node *parse_and(Parser *ps)
{
    Node *n = parse_not(ps);
    while (cur(ps) == TOK_AND) {
        int line = adv(ps)->line;
        Node *x = new_node(ps, N_AND, line);
        x->a = n; x->b = parse_not(ps);
        n = x;
    }
    return n;
}

static Node *parse_expression(Parser *ps)
{
    enter(ps);
    Node *n = parse_and(ps);
    while (cur(ps) == TOK_OR) {
        int line = adv(ps)->line;
        Node *x = new_node(ps, N_OR, line);
        x->a = n; x->b = parse_and(ps);
        n = x;
    }
    leave(ps);
    return n;
}

static Node *parse_tokens_as_expr(Parser *ps, TokenList *sub)
{
    /* 보간 하위식: 같은 Program/jb 를 공유하되 토큰열만 교체 */
    const Token *save_toks = ps->toks;
    size_t save_pos = ps->pos, save_ntok = ps->ntok;
    ps->toks = sub->items; ps->pos = 0; ps->ntok = sub->count;
    Node *e = parse_expression(ps);
    if (cur(ps) != TOK_EOF) perr(ps, "보간식이 올바르지 않습니다");
    ps->toks = save_toks; ps->pos = save_pos; ps->ntok = save_ntok;
    return e;
}

/* ---------------------------------------------------------------- 문 */

static Node *parse_statement(Parser *ps);

static bool is_lvalue(const Node *n)
{
    return n->kind == N_VAR || n->kind == N_FIELD || n->kind == N_INDEX;
}

static Node *parse_block(Parser *ps)
{
    int line = tok(ps)->line;
    expect(ps, TOK_LBRACE, "'{'");
    Node *blk = new_node(ps, S_BLOCK, line);
    enter(ps);
    while (cur(ps) != TOK_RBRACE) {
        if (cur(ps) == TOK_EOF) perr(ps, "블록이 '}' 로 닫히지 않았습니다");
        list_add(ps, &blk->list, parse_statement(ps));
    }
    leave(ps);
    expect(ps, TOK_RBRACE, "'}'");
    return blk;
}

static Node *parse_if(Parser *ps, int line)
{
    Node *n = new_node(ps, S_IF, line);
    expect(ps, TOK_LPAREN, "'('");
    n->a = parse_expression(ps);
    expect(ps, TOK_RPAREN, "')'");
    n->b = parse_block(ps);
    if (accept(ps, TOK_ELSE)) {
        if (cur(ps) == TOK_IF) {
            int l2 = adv(ps)->line;
            enter(ps);
            n->c = parse_if(ps, l2);
            leave(ps);
        } else {
            n->c = parse_block(ps);
        }
    }
    return n;
}

static Node *parse_struct(Parser *ps, int line)
{
    const Token *name = expect(ps, TOK_IDENT, "구조체 이름");
    Node *n = new_node(ps, S_STRUCT, line);
    set_text(ps, n, name->lexeme, name->lexeme_len);
    expect(ps, TOK_LBRACE, "'{'");
    while (cur(ps) != TOK_RBRACE) {
        if (cur(ps) == TOK_EOF) perr(ps, "구조체가 '}' 로 닫히지 않았습니다");
        expect(ps, TOK_VAR, "필드 선언('" KW_VAR "')");
        const Token *f = expect(ps, TOK_IDENT, "필드 이름");
        Node *field = new_node(ps, N_VAR, f->line);
        set_text(ps, field, f->lexeme, f->lexeme_len);
        if (accept(ps, TOK_LBRACKET)) { expect(ps, TOK_RBRACKET, "']'"); field->boolean = true; } /* 배열 필드 */
        expect(ps, TOK_SEMI, "';'");
        list_add(ps, &n->list, field);
    }
    expect(ps, TOK_RBRACE, "'}'");
    return n;
}

static Node *parse_function(Parser *ps, int line)
{
    const Token *name = expect(ps, TOK_IDENT, "함수 이름");
    Node *fn = new_node(ps, S_FUNC, line);
    set_text(ps, fn, name->lexeme, name->lexeme_len);
    expect(ps, TOK_LPAREN, "'('");
    if (!accept(ps, TOK_RPAREN)) {
        do {
            const Token *p = expect(ps, TOK_IDENT, "매개변수 이름");
            for (size_t i = 0; i < fn->list.count; ++i)
                if (fn->list.items[i]->text_len == p->lexeme_len &&
                    memcmp(fn->list.items[i]->text, p->lexeme, p->lexeme_len) == 0)
                    perr(ps, "매개변수 이름이 중복됩니다");
            Node *param = new_node(ps, N_VAR, p->line);
            set_text(ps, param, p->lexeme, p->lexeme_len);
            list_add(ps, &fn->list, param);
        } while (accept(ps, TOK_COMMA));
        expect(ps, TOK_RPAREN, "')'");
    }
    int saved = ps->loop_depth;
    ps->loop_depth = 0;
    fn->a = parse_block(ps);
    ps->loop_depth = saved;
    return fn;
}

static Node *parse_try(Parser *ps, int line)
{
    Node *n = new_node(ps, S_TRY, line);
    n->a = parse_block(ps);
    expect(ps, TOK_CATCH, "'" KW_CATCH "'");
    if (accept(ps, TOK_LPAREN)) {
        if (cur(ps) == TOK_IDENT) {
            const Token *v = adv(ps);
            set_text(ps, n, v->lexeme, v->lexeme_len);   /* 예외 변수명 */
        }
        expect(ps, TOK_RPAREN, "')'");
    }
    n->b = parse_block(ps);
    return n;
}

static Node *parse_vardecl(Parser *ps, int line)
{
    const Token *name = expect(ps, TOK_IDENT, "변수 이름");
    Node *n = new_node(ps, S_VARDECL, line);
    set_text(ps, n, name->lexeme, name->lexeme_len);
    if (accept(ps, TOK_LBRACKET)) { expect(ps, TOK_RBRACKET, "']'"); n->boolean = true; } /* 배열 초기화 */
    if (accept(ps, TOK_ASSIGN_OP) || accept(ps, TOK_ASSIGN_JOSA))
        n->a = parse_expression(ps);
    expect(ps, TOK_SEMI, "';'");
    return n;
}

static Node *parse_statement(Parser *ps)
{
    const Token *t = tok(ps);
    int line = t->line;
    Node *n;

    enter(ps);
    switch (t->type) {
    case TOK_VAR:     adv(ps); n = parse_vardecl(ps, line); leave(ps); return n;
    case TOK_IF:      adv(ps); n = parse_if(ps, line);      leave(ps); return n;
    case TOK_STRUCT:  adv(ps); n = parse_struct(ps, line);  leave(ps); return n;
    case TOK_FUNC:    adv(ps); n = parse_function(ps, line); leave(ps); return n;
    case TOK_TRY:     adv(ps); n = parse_try(ps, line);     leave(ps); return n;
    case TOK_PROGRAM:
        adv(ps);
        n = new_node(ps, S_PROGRAM, line);
        n->a = parse_block(ps);
        leave(ps);
        return n;
    case TOK_WHILE:
        adv(ps);
        n = new_node(ps, S_WHILE, line);
        expect(ps, TOK_LPAREN, "'('");
        n->a = parse_expression(ps);
        expect(ps, TOK_RPAREN, "')'");
        ++ps->loop_depth;
        n->b = parse_block(ps);
        --ps->loop_depth;
        leave(ps);
        return n;
    case TOK_RETURN:
        adv(ps);
        n = new_node(ps, S_RETURN, line);
        if (cur(ps) != TOK_SEMI) n->a = parse_expression(ps);
        expect(ps, TOK_SEMI, "';'");
        leave(ps);
        return n;
    case TOK_BREAK:
        if (ps->loop_depth == 0) perr(ps, "'" KW_BREAK "' 은 반복문 안에서만 쓸 수 있습니다");
        adv(ps); n = new_node(ps, S_BREAK, line); expect(ps, TOK_SEMI, "';'"); leave(ps); return n;
    case TOK_CONTINUE:
        if (ps->loop_depth == 0) perr(ps, "'" KW_CONTINUE "' 은 반복문 안에서만 쓸 수 있습니다");
        adv(ps); n = new_node(ps, S_CONTINUE, line); expect(ps, TOK_SEMI, "';'"); leave(ps); return n;
    case TOK_EXIT:
        adv(ps);
        n = new_node(ps, S_EXIT, line);
        if (accept(ps, TOK_LPAREN)) {
            if (cur(ps) != TOK_RPAREN) n->a = parse_expression(ps);
            expect(ps, TOK_RPAREN, "')'");
        }
        expect(ps, TOK_SEMI, "';'");
        leave(ps);
        return n;
    case TOK_IMPORT:
    case TOK_EXPORT:
        adv(ps);
        n = new_node(ps, S_IMPORT, line);
        /* 모듈 경로: 이름(.이름)* 를 건너뛴다 */
        expect(ps, TOK_IDENT, "모듈 이름");
        while (accept(ps, TOK_DOT)) expect(ps, TOK_IDENT, "모듈 이름");
        expect(ps, TOK_SEMI, "';'");
        leave(ps);
        return n;
    default:
        break;
    }

    /* 식으로 시작하는 문장: 대입 / 증감 / 식 */
    Node *expr = parse_expression(ps);
    if (cur(ps) == TOK_ASSIGN_OP || cur(ps) == TOK_ASSIGN_JOSA) {
        if (!is_lvalue(expr)) perr(ps, "대입할 수 없는 대상입니다");
        int l2 = adv(ps)->line;
        Node *as = new_node(ps, N_ASSIGN, l2);
        as->a = expr;
        as->b = parse_expression(ps);
        expect(ps, TOK_SEMI, "';'");
        n = new_node(ps, S_EXPR, l2);
        n->a = as;
        leave(ps);
        return n;
    }
    if (cur(ps) == TOK_PLUSPLUS || cur(ps) == TOK_MINUSMINUS) {
        if (!is_lvalue(expr)) perr(ps, "증감 대상이 올바르지 않습니다");
        TokenType op = cur(ps);
        int l2 = adv(ps)->line;
        Node *pre = new_node(ps, N_PREOP, l2);
        pre->op = (op == TOK_PLUSPLUS) ? '+' : '-';
        pre->a = expr;
        expect(ps, TOK_SEMI, "';'");
        n = new_node(ps, S_EXPR, l2);
        n->a = pre;
        leave(ps);
        return n;
    }
    expect(ps, TOK_SEMI, "';'");
    n = new_node(ps, S_EXPR, line);
    n->a = expr;
    leave(ps);
    return n;
}

/* ---------------------------------------------------------------- 진입 */

Program *parse_program(const TokenList *tokens, char *errbuf, size_t errlen)
{
    Program *volatile prog = calloc(1, sizeof *prog);
    if (prog == NULL) { snprintf(errbuf, errlen, "메모리 부족"); return NULL; }

    Parser ps;
    memset(&ps, 0, sizeof ps);
    ps.toks = tokens->items;
    ps.ntok = tokens->count;
    ps.prog = prog;
    ps.err = errbuf;
    ps.errlen = errlen;

    jmp_buf jb;
    ps.jb = &jb;

    if (setjmp(jb) != 0) {
        for (size_t i = 0; i < ps.subtok_count; ++i) { token_list_free(ps.subtoks[i]); free(ps.subtoks[i]); }
        free(ps.subtoks);
        program_free(prog);
        return NULL;
    }

    Node *root = new_node(&ps, S_BLOCK, 1);
    while (cur(&ps) != TOK_EOF) list_add(&ps, &root->list, parse_statement(&ps));
    prog->root = root;

    for (size_t i = 0; i < ps.subtok_count; ++i) { token_list_free(ps.subtoks[i]); free(ps.subtoks[i]); }
    free(ps.subtoks);
    return prog;
}

void program_free(Program *prog)
{
    if (prog == NULL) return;
    for (size_t i = 0; i < prog->node_count; ++i) {
        Node *n = prog->nodes[i];
        free(n->text);
        free(n->list.items);
        free(n);
    }
    free(prog->nodes);
    free(prog);
}
