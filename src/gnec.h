/*
 * gnec.h — GNEC 인터프리터 공용 선언 (원본 GNE 2차 명세의 검열판)
 *
 * 1차(식 중심, 줄바꿈 종결) 대비 2차에서 새로 들어온 것:
 *   - 세미콜론 종결 문장 + '{' '}' 블록
 *   - 구조체(필드 선언) 정의와 '새로' 를 통한 객체 생성
 *   - 객체 필드 접근/대입(obj.field), 배열 원소 대입(arr[i])
 *   - try/catch 예외 처리, 종료문
 *   - "${expr}" 문자열 보간
 *   - /ㄹ ... ㄹ/ 여러 줄 주석
 *   - EUC-KR / UTF-16(BOM) 소스 입력 → 내부 UTF-8
 *
 * 표준: C11, 외부 의존성은 libm 과 (EUC-KR 변환 시) POSIX iconv.
 */
#ifndef GNEC_H
#define GNEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>

#define GNEC_VERSION "1.0.0"   /* GNEC = 원본 GNE 2차 명세의 검열판 */

/* ======================================================================
 * 토큰
 * ==================================================================== */
typedef enum {
    TOK_EOF = 0,
    TOK_NUMBER, TOK_STRING, TOK_IDENT,
    /* 구두점 */
    TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE, TOK_LBRACKET, TOK_RBRACKET,
    TOK_COMMA, TOK_DOT, TOK_SEMI, TOK_ASSIGN_OP,   /* '=' */
    /* 산술 */
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_PLUSPLUS, TOK_MINUSMINUS,
    /* 비교 / 논리 */
    TOK_EQEQ, TOK_NEQ, TOK_LT, TOK_LE, TOK_GT, TOK_GE,
    TOK_AND, TOK_OR, TOK_NOT,
    /* 키워드 */
    TOK_TRUE, TOK_FALSE, TOK_NIL,
    TOK_VAR, TOK_ASSIGN_JOSA, TOK_NEW,
    TOK_IMPORT, TOK_EXPORT,
    TOK_STRUCT, TOK_PROGRAM,
    TOK_FUNC, TOK_RETURN,
    TOK_IF, TOK_ELSE, TOK_WHILE, TOK_BREAK, TOK_CONTINUE,
    TOK_TRY, TOK_CATCH, TOK_EXIT
} TokenType;

/*
 * 문자열 토큰은 보간 조각들로 미리 쪼개 둔다.
 *   "바쁜 ${꿀벌}은" → parts: ["바쁜 ", <식 꿀벌>, "은"]
 * 리터럴 조각은 str 에, 식 조각은 expr_src(원본 소스 조각)에 담는다.
 */
typedef struct {
    bool        is_expr;
    char       *str;       /* is_expr=false: 리터럴 (소유) */
    size_t      str_len;
    char       *expr_src;  /* is_expr=true:  ${} 안 소스 (소유, NUL 종료) */
} StrPart;

typedef struct {
    TokenType   type;
    char       *lexeme;    /* 식별자/숫자 원문 (소유, NUL 종료). 그 외엔 NULL */
    size_t      lexeme_len;
    double      number;
    StrPart    *parts;     /* TOK_STRING 전용 (소유) */
    size_t      part_count;
    int         line;
} Token;

typedef struct {
    Token  *items;
    size_t  count;
    size_t  cap;
} TokenList;

bool lex_source(const char *src, TokenList *out, char *errbuf, size_t errlen);
void token_list_free(TokenList *list);

/* ======================================================================
 * AST
 * ==================================================================== */
typedef enum {
    /* 식 */
    N_NUM, N_STR, N_BOOL, N_NIL, N_VAR, N_LIST, N_INDEX, N_FIELD,
    N_CALL, N_METHOD, N_NEW, N_NEG, N_NOT, N_BINARY, N_AND, N_OR,
    N_ASSIGN, N_PREOP,   /* ++/-- 를 복합 대입으로 환원 */
    /* 문 */
    S_EXPR, S_VARDECL, S_BLOCK, S_IF, S_WHILE, S_FUNC, S_STRUCT, S_PROGRAM,
    S_RETURN, S_BREAK, S_CONTINUE, S_TRY, S_EXIT, S_IMPORT, S_INTERP
} NodeKind;

typedef struct Node Node;

typedef struct {
    Node  **items;
    size_t  count;
    size_t  cap;
} NodeList;

/*
 * 공용 필드 노드.
 *   N_STR/N_VAR/N_FIELD/N_METHOD/N_NEW/S_FUNC/S_STRUCT/S_VARDECL/S_TRY : text
 *   N_BINARY : op, a, b   / N_INDEX : a[b]   / N_FIELD : a.text
 *   N_CALL   : a(list)    / N_METHOD : a.text(list)
 *   N_ASSIGN : a(대상) <- b   / N_PREOP : op('+'/'-'), a(대상)
 *   S_IF     : a=조건,b=참,c=거짓   / S_WHILE : a=조건,b=본문
 *   S_FUNC   : text=이름,list=매개변수,a=본문
 *   S_STRUCT : text=이름,list=필드명(N_VAR)
 *   S_TRY    : a=시도블록, text=예외변수(없으면 NULL), b=예외블록
 *   S_INTERP : list=조각들(N_STR 또는 임의 식), 문자열 보간 결과
 */
struct Node {
    NodeKind  kind;
    int       line;
    int       op;
    double    num;
    bool      boolean;
    char     *text;
    size_t    text_len;
    uint32_t  hash;
    Node     *a, *b, *c;
    NodeList  list;
};

typedef struct {
    Node   *root;        /* S_BLOCK (최상위) */
    Node  **nodes;
    size_t  node_count;
    size_t  node_cap;
} Program;

Program *parse_program(const TokenList *tokens, char *errbuf, size_t errlen);
void program_free(Program *prog);
uint32_t gne_hash(const char *s, size_t len);

/* ======================================================================
 * 값 (참조 카운트)
 * ==================================================================== */
typedef enum { V_NIL, V_BOOL, V_NUM, V_STR, V_LIST, V_FUNC, V_NATIVE, V_OBJECT } ValueType;

typedef struct GneString GneString;
typedef struct GneList   GneList;
typedef struct GneObject GneObject;
typedef struct Table     Table;

typedef struct {
    ValueType type;
    union {
        bool        b;
        double      n;
        GneString  *s;
        GneList    *l;
        GneObject  *o;
        const Node *fn;
        int         native;
    } as;
} Value;

struct GneString { size_t refcount; size_t len; char chars[]; };
struct GneList   { size_t refcount; size_t count; size_t cap; Value *items; };

/* 객체: 구조체 이름(빌려씀) + 필드 테이블(소유) */
struct GneObject {
    size_t       refcount;
    const char  *type_name;   /* S_STRUCT 노드의 text, 빌려씀 */
    Table       *fields;      /* 소유 (힙) */
};

Value value_nil(void);
Value value_bool(bool b);
Value value_num(double n);
Value value_str_take(GneString *s);
Value value_str_copy(const char *s, size_t len);
Value value_list_new(size_t cap);

Value value_retain(Value v);
void  value_release(Value v);

GneString *gstr_new(const char *s, size_t len);
GneString *gstr_concat(const GneString *a, const GneString *b);
bool  value_truthy(Value v);
bool  value_equal(Value a, Value b);
const char *value_type_name(Value v);
GneString *value_to_string(Value v, bool repr);
bool  list_push(GneList *l, Value v);

size_t utf8_length(const char *s, size_t len);
bool   utf8_slice_at(const char *s, size_t len, size_t index, size_t *off, size_t *blen);

/* ======================================================================
 * 해시 테이블 (전역/지역/필드 공용)
 * ==================================================================== */
typedef struct {
    char     *key;
    size_t    key_len;
    uint32_t  hash;
    Value     value;
} Entry;

struct Table {
    Entry  *entries;
    size_t  count;
    size_t  cap;
};

bool   table_init(Table *t, size_t cap);
void   table_free(Table *t);
Value *table_get(const Table *t, const char *key, size_t len, uint32_t hash);
void   table_set(Table *t, const char *key, size_t len, uint32_t hash, Value v);

/* ======================================================================
 * 인터프리터
 * ==================================================================== */
typedef struct {
    Table    globals;
    const Node **structs;   /* 등록된 구조체 정의 (S_STRUCT 노드, 빌려씀) */
    size_t   struct_count;
    size_t   struct_cap;
    Table   *frames;
    size_t   frame_count;
    size_t   frame_cap;
    Value   *temps;       /* 임시 루트 스택 (longjmp 안전) */
    size_t   temp_count;
    size_t   temp_cap;
    Value    ret_value;
    /* 예외 처리: try 가 중첩될 수 있으므로 복귀 지점을 스택으로 */
    jmp_buf *catch_jmp;   /* 현재 가장 안쪽 try 의 복귀 지점 (없으면 NULL) */
    Value    exc_value;   /* 던져진 예외 값 */
    bool     has_exc;
    jmp_buf *top_jmp;     /* 최상위 오류 복귀 지점 */
    int      exit_code;   /* 종료문으로 설정 */
    bool     exiting;
} Interp;

bool interp_init(Interp *in);
void interp_free(Interp *in);
bool interp_run(Interp *in, const Program *prog);

/* 런타임 오류를 예외로 던진다 (가장 안쪽 try 로, 없으면 최상위로) */
_Noreturn void gne_throw_str(Interp *in, int line, const char *fmt, ...);

/* ======================================================================
 * 소스 로더 (인코딩 변환)
 * ==================================================================== */
/* 파일을 읽어 UTF-8 NUL 종료 문자열로. 실패 시 NULL + errbuf. 호출자 free. */
char *load_source_file(const char *path, char *errbuf, size_t errlen);

#endif /* GNEC_H */
