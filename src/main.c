/*
 * main.c — GNEC 실행기
 *
  *   gnec 파일.ㄹ       파일 실행 (EUC-KR/UTF-8/UTF-16 자동 인식)
  *   gnec -e "코드"     코드 직접 실행 (UTF-8)
 */
#include "gnec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ERR_BUF = 512 };

static Program *compile(const char *src)
{
    char err[ERR_BUF];
    TokenList toks;
    if (!lex_source(src, &toks, err, sizeof err)) {
        fprintf(stderr, "[GNE 문법 오류] %s\n", err);
        token_list_free(&toks);
        return NULL;
    }
    Program *prog = parse_program(&toks, err, sizeof err);
    token_list_free(&toks);
    if (prog == NULL) fprintf(stderr, "[GNE 문법 오류] %s\n", err);
    return prog;
}

static int run_source(const char *src)
{
    Program *prog = compile(src);
    if (prog == NULL) return EXIT_FAILURE;

    Interp in;
    if (!interp_init(&in)) {
        program_free(prog);
        fprintf(stderr, "치명적 오류: 메모리 부족\n");
        return EXIT_FAILURE;
    }
    bool ok = interp_run(&in, prog);
    int code = in.exit_code;
    interp_free(&in);
    program_free(prog);
    if (!ok) return EXIT_FAILURE;
    return code;
}

static void usage(const char *prog)
{
    printf("GNEC 인터프리터 %s (원본 GNE 2차 명세의 검열판)\n\n"
           "사용법:\n"
           "  %s <파일.ㄹ>       파일 실행 (EUC-KR/UTF-8/UTF-16 자동 인식)\n"
           "  %s -e \"<코드>\"    코드 직접 실행\n",
           GNEC_VERSION, prog, prog);
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(argv[0]); return EXIT_FAILURE; }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) { usage(argv[0]); return EXIT_SUCCESS; }
    if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) { printf("GNEC %s\n", GNEC_VERSION); return EXIT_SUCCESS; }

    if (strcmp(argv[1], "-e") == 0) {
        if (argc < 3) { usage(argv[0]); return EXIT_FAILURE; }
        return run_source(argv[2]);
    }

    char err[ERR_BUF];
    char *src = load_source_file(argv[1], err, sizeof err);
    if (src == NULL) { fprintf(stderr, "%s\n", err); return EXIT_FAILURE; }
    int rc = run_source(src);
    free(src);
    return rc;
}
