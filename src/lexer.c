/*
 * lexer.c — GNEC 렉서: UTF-8 소스 → 토큰 배열
 *
 * 1차 대비 달라진 점:
 *   - 주석: /ㄹ ... ㄹ/ (여러 줄) 과 // (한 줄)
 *   - 문자열: " 또는 ' 로 감싸고, ${식} 보간을 조각(StrPart)으로 미리 분해
 *   - 세미콜론 ';' 로 문장을 끝낸다 (줄바꿈은 공백 취급)
 *   - 키워드는 keywords.h 의 매크로로 정의 (중립 낱말)
 *
 * 식별자: ASCII 영숫자/_ 와 UTF-8 멀티바이트(한글 등).
 * 명세의 함수명 형태(K-계승, K-인공지능-로드맵)를 받기 위해 '-' 도 이름에 넣되,
 * "바로 뒤에 멀티바이트 글자(한글)가 올 때만" 이름의 일부로 본다.
 *   K-계승   → 이름 하나          i--  → i, --        n-1 → n, -, 1
 *   a-b      → a, -, b (뺄셈)     가-나 → 이름 하나 (한글끼리 뺄셈은 공백을 띄워 쓴다)
 */
#include "gnec.h"
#include "keywords.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

typedef struct { const char *word; TokenType type; } Keyword;

static const Keyword KEYWORDS[] = {
    { KW_TRUE, TOK_TRUE }, { KW_FALSE, TOK_FALSE }, { KW_NIL, TOK_NIL },
    { KW_VAR, TOK_VAR }, { KW_ASSIGN, TOK_ASSIGN_JOSA }, { KW_NEW, TOK_NEW },
    { KW_IMPORT, TOK_IMPORT }, { KW_EXPORT, TOK_EXPORT },
    { KW_STRUCT, TOK_STRUCT }, { KW_PROGRAM, TOK_PROGRAM },
    { KW_FUNC, TOK_FUNC }, { KW_RETURN, TOK_RETURN },
    { KW_IF, TOK_IF }, { KW_ELSE, TOK_ELSE }, { KW_WHILE, TOK_WHILE },
    { KW_BREAK, TOK_BREAK }, { KW_CONTINUE, TOK_CONTINUE },
    { KW_TRY, TOK_TRY }, { KW_CATCH, TOK_CATCH }, { KW_EXIT, TOK_EXIT },
    { KW_AND, TOK_AND }, { KW_OR, TOK_OR }, { KW_NOT, TOK_NOT },
    { KW_PLUS_WORD, TOK_PLUS },
};

typedef struct {
    const char *p;
    int         line;
    TokenList  *out;
    char       *err;
    size_t      errlen;
    jmp_buf     jb;
} Lexer;

static _Noreturn void lex_fail(Lexer *lx, const char *fmt, ...)
{
    va_list ap;
    int n = snprintf(lx->err, lx->errlen, "%d번째 줄: ", lx->line);
    if (n > 0 && (size_t)n < lx->errlen) {
        va_start(ap, fmt);
        vsnprintf(lx->err + n, lx->errlen - (size_t)n, fmt, ap);
        va_end(ap);
    }
    longjmp(lx->jb, 1);
}

static bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

static bool is_ident_start(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}

static bool is_ident_char(unsigned char c)
{
    return is_ident_start(c) || is_digit(c);
}

static Token *push_blank(Lexer *lx, TokenType type)
{
    TokenList *l = lx->out;
    if (l->count == l->cap) {
        size_t new_cap = l->cap ? l->cap * 2 : 256;
        Token *grown = realloc(l->items, new_cap * sizeof *grown);
        if (grown == NULL) lex_fail(lx, "메모리 부족");
        l->items = grown;
        l->cap = new_cap;
    }
    Token *t = &l->items[l->count++];
    memset(t, 0, sizeof *t);
    t->type = type;
    t->line = lx->line;
    return t;
}

static char *dup_range(Lexer *lx, const char *s, size_t len)
{
    char *d = malloc(len + 1);
    if (d == NULL) lex_fail(lx, "메모리 부족");
    memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

static TokenType classify(const char *s, size_t len)
{
    for (size_t i = 0; i < sizeof KEYWORDS / sizeof KEYWORDS[0]; ++i)
        if (strlen(KEYWORDS[i].word) == len && memcmp(KEYWORDS[i].word, s, len) == 0)
            return KEYWORDS[i].type;
    return TOK_IDENT;
}

/* ---------------------------------------------------------------- 보간 문자열
 * quote 로 열린 문자열을 읽어 StrPart 배열로 분해한다. 닫는 따옴표 다음으로 p 를 옮긴다.
 * \n \t \r \\ \" \' \$ \{ 이스케이프와 ${ ... } 식 보간을 처리한다.
 */
typedef struct { StrPart *items; size_t count; size_t cap; } PartList;

static void part_add_literal(Lexer *lx, PartList *pl, const char *buf, size_t len)
{
    if (len == 0 && pl->count > 0) return;   /* 빈 리터럴은 생략 (단, 전체가 빈 문자열이면 하나 유지) */
    if (pl->count == pl->cap) {
        size_t nc = pl->cap ? pl->cap * 2 : 4;
        StrPart *g = realloc(pl->items, nc * sizeof *g);
        if (g == NULL) lex_fail(lx, "메모리 부족");
        pl->items = g; pl->cap = nc;
    }
    StrPart *sp = &pl->items[pl->count++];
    sp->is_expr = false;
    sp->str = dup_range(lx, buf, len);
    sp->str_len = len;
    sp->expr_src = NULL;
}

static void part_add_expr(Lexer *lx, PartList *pl, const char *src, size_t len)
{
    if (pl->count == pl->cap) {
        size_t nc = pl->cap ? pl->cap * 2 : 4;
        StrPart *g = realloc(pl->items, nc * sizeof *g);
        if (g == NULL) lex_fail(lx, "메모리 부족");
        pl->items = g; pl->cap = nc;
    }
    StrPart *sp = &pl->items[pl->count++];
    sp->is_expr = true;
    sp->str = NULL;
    sp->str_len = 0;
    sp->expr_src = dup_range(lx, src, len);
}

static void lex_string(Lexer *lx)
{
    char quote = *lx->p;
    int start_line = lx->line;
    ++lx->p;

    PartList pl = { NULL, 0, 0 };
    /* 현재 리터럴 조각을 모으는 동적 버퍼 */
    size_t cap = 32, blen = 0;
    char *buf = malloc(cap);
    if (buf == NULL) lex_fail(lx, "메모리 부족");

#define BUF_PUT(ch) do {                                           \
        if (blen + 1 > cap) { cap *= 2; char *g = realloc(buf, cap); \
            if (!g) { free(buf); lex_fail(lx, "메모리 부족"); } buf = g; } \
        buf[blen++] = (ch);                                        \
    } while (0)

    while (*lx->p != quote) {
        char c = *lx->p;
        if (c == '\0') { free(buf); for (size_t i=0;i<pl.count;++i){free(pl.items[i].str);free(pl.items[i].expr_src);} free(pl.items); lx->line = start_line; lex_fail(lx, "문자열이 닫히지 않았습니다"); }
        if (c == '\n') ++lx->line;

        if (c == '\\') {
            char e = lx->p[1];
            char out;
            switch (e) {
            case 'n': out = '\n'; break;
            case 't': out = '\t'; break;
            case 'r': out = '\r'; break;
            case '\\': out = '\\'; break;
            case '"': out = '"'; break;
            case '\'': out = '\''; break;
            case '$': out = '$'; break;
            case '{': out = '{'; break;
            case '0': out = '\0'; break;
            default: free(buf); lex_fail(lx, "알 수 없는 이스케이프 '\\%c'", e ? e : ' ');
            }
            BUF_PUT(out);
            lx->p += 2;
            continue;
        }

        if (c == '$' && lx->p[1] == '{') {
            /* 지금까지 모은 리터럴을 조각으로 넘기고, 중괄호 균형을 맞춰 식 소스를 추출 */
            part_add_literal(lx, &pl, buf, blen);
            blen = 0;
            lx->p += 2;
            const char *es = lx->p;
            int depth = 1;
            while (*lx->p != '\0' && depth > 0) {
                if (*lx->p == '{') ++depth;
                else if (*lx->p == '}') { if (--depth == 0) break; }
                else if (*lx->p == '\n') ++lx->line;
                ++lx->p;
            }
            if (*lx->p != '}') { free(buf); lex_fail(lx, "보간 식의 '}' 가 없습니다"); }
            part_add_expr(lx, &pl, es, (size_t)(lx->p - es));
            ++lx->p;   /* '}' 소비 */
            continue;
        }

        BUF_PUT(c);
        ++lx->p;
    }
    ++lx->p;   /* 닫는 따옴표 */

    /* 남은 리터럴. 전체가 비어 있어도 조각 하나는 유지해 빈 문자열을 표현 */
    if (blen > 0 || pl.count == 0) part_add_literal(lx, &pl, buf, blen);
    free(buf);
#undef BUF_PUT

    Token *t = push_blank(lx, TOK_STRING);
    t->line = start_line;
    t->parts = pl.items;
    t->part_count = pl.count;
}

bool lex_source(const char *src, TokenList *out, char *errbuf, size_t errlen)
{
    out->items = NULL; out->count = 0; out->cap = 0;

    Lexer lx;
    lx.p = src; lx.line = 1; lx.out = out; lx.err = errbuf; lx.errlen = errlen;

    if (setjmp(lx.jb) != 0) return false;

    while (*lx.p != '\0') {
        unsigned char c = (unsigned char)*lx.p;

        if (c == ' ' || c == '\t' || c == '\r') { ++lx.p; continue; }
        if (c == '\n') { ++lx.line; ++lx.p; continue; }

        /* 주석 */
        if (c == '/' && lx.p[1] == '/') { while (*lx.p && *lx.p != '\n') ++lx.p; continue; }
        if (c == '/' && (unsigned char)lx.p[1] == 0xE3) {
            /* '/ㄹ' 여러 줄 주석: 'ㄹ' = U+3139 = E3 84 B9 */
            if ((unsigned char)lx.p[2] == 0x84 && (unsigned char)lx.p[3] == 0xB9) {
                lx.p += 4;
                while (*lx.p != '\0') {
                    if ((unsigned char)lx.p[0] == 0xE3 && (unsigned char)lx.p[1] == 0x84 &&
                        (unsigned char)lx.p[2] == 0xB9 && lx.p[3] == '/') { lx.p += 4; break; }
                    if (*lx.p == '\n') ++lx.line;
                    ++lx.p;
                }
                continue;
            }
        }

        if (is_digit(c)) {
            const char *s = lx.p;
            while (is_digit((unsigned char)*lx.p)) ++lx.p;
            if (*lx.p == '.' && is_digit((unsigned char)lx.p[1])) {
                ++lx.p;
                while (is_digit((unsigned char)*lx.p)) ++lx.p;
            }
            size_t len = (size_t)(lx.p - s);
            char *num = dup_range(&lx, s, len);
            Token *t = push_blank(&lx, TOK_NUMBER);
            t->number = strtod(num, NULL);
            t->lexeme = num;
            t->lexeme_len = len;
            continue;
        }

        if (is_ident_start(c)) {
            const char *s = lx.p;
            ++lx.p;
            for (;;) {
                if (is_ident_char((unsigned char)*lx.p)) { ++lx.p; continue; }
                /* '-' 다음이 멀티바이트(한글)일 때만 이름에 포함 — 그 외엔 뺄셈/감소 연산자 */
                if (*lx.p == '-' && (unsigned char)lx.p[1] >= 0x80) { lx.p += 1; continue; }
                break;
            }
            size_t len = (size_t)(lx.p - s);
            TokenType tt = classify(s, len);
            Token *t = push_blank(&lx, tt);
            if (tt == TOK_IDENT) { t->lexeme = dup_range(&lx, s, len); t->lexeme_len = len; }
            continue;
        }

        if (c == '"' || c == '\'') { lex_string(&lx); continue; }

        /* 기호 */
        switch (c) {
        case '(': push_blank(&lx, TOK_LPAREN); ++lx.p; break;
        case ')': push_blank(&lx, TOK_RPAREN); ++lx.p; break;
        case '{': push_blank(&lx, TOK_LBRACE); ++lx.p; break;
        case '}': push_blank(&lx, TOK_RBRACE); ++lx.p; break;
        case '[': push_blank(&lx, TOK_LBRACKET); ++lx.p; break;
        case ']': push_blank(&lx, TOK_RBRACKET); ++lx.p; break;
        case ',': push_blank(&lx, TOK_COMMA); ++lx.p; break;
        case '.': push_blank(&lx, TOK_DOT); ++lx.p; break;
        case ';': push_blank(&lx, TOK_SEMI); ++lx.p; break;
        case '%': push_blank(&lx, TOK_PERCENT); ++lx.p; break;
        case '*': push_blank(&lx, TOK_STAR); ++lx.p; break;
        case '/': push_blank(&lx, TOK_SLASH); ++lx.p; break;
        case '+':
            if (lx.p[1] == '+') { push_blank(&lx, TOK_PLUSPLUS); lx.p += 2; }
            else { push_blank(&lx, TOK_PLUS); ++lx.p; }
            break;
        case '-':
            if (lx.p[1] == '-') { push_blank(&lx, TOK_MINUSMINUS); lx.p += 2; }
            else { push_blank(&lx, TOK_MINUS); ++lx.p; }
            break;
        case '=':
            if (lx.p[1] == '=') { push_blank(&lx, TOK_EQEQ); lx.p += 2; }
            else { push_blank(&lx, TOK_ASSIGN_OP); ++lx.p; }
            break;
        case '!':
            if (lx.p[1] == '=') { push_blank(&lx, TOK_NEQ); lx.p += 2; }
            else { push_blank(&lx, TOK_NOT); ++lx.p; }
            break;
        case '<':
            if (lx.p[1] == '=') { push_blank(&lx, TOK_LE); lx.p += 2; }
            else { push_blank(&lx, TOK_LT); ++lx.p; }
            break;
        case '>':
            if (lx.p[1] == '=') { push_blank(&lx, TOK_GE); lx.p += 2; }
            else { push_blank(&lx, TOK_GT); ++lx.p; }
            break;
        case '&':
            if (lx.p[1] == '&') { push_blank(&lx, TOK_AND); lx.p += 2; break; }
            lex_fail(&lx, "알 수 없는 문자 '&'");
            break;
        case '|':
            if (lx.p[1] == '|') { push_blank(&lx, TOK_OR); lx.p += 2; break; }
            lex_fail(&lx, "알 수 없는 문자 '|'");
            break;
        default:
            lex_fail(&lx, "알 수 없는 문자 (바이트 0x%02X)", c);
        }
    }

    push_blank(&lx, TOK_EOF);
    return true;
}

void token_list_free(TokenList *list)
{
    for (size_t i = 0; i < list->count; ++i) {
        Token *t = &list->items[i];
        free(t->lexeme);
        for (size_t k = 0; k < t->part_count; ++k) {
            free(t->parts[k].str);
            free(t->parts[k].expr_src);
        }
        free(t->parts);
    }
    free(list->items);
    list->items = NULL;
    list->count = list->cap = 0;
}
