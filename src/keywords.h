/*
 * keywords.h — GNEC 키워드 표 (중립 한국어)
 *
 * 2차 명세(GNE.g4 포함)의 "구조"만 가져오고, 키워드 자체는 중립적인 한국어로 정했다.
 * 다른 낱말을 쓰고 싶으면 이 파일의 문자열만 바꾸면 된다. 렉서/파서는 아래 매크로만 참조한다.
 * (한 토큰에 여러 표기를 허용하려면 lexer 의 KEYWORDS 표에 줄을 더 추가한다.)
 */
#ifndef GNE_KEYWORDS_H
#define GNE_KEYWORDS_H

/* 리터럴 */
#define KW_TRUE       "참"
#define KW_FALSE      "거짓"
#define KW_NIL        "없음"

/* 선언 / 대입 */
#define KW_VAR        "선언"      /* 변수 선언:  선언 x 는 10;        */
#define KW_ASSIGN     "는"        /* 대입 조사:  x 는 10;  obj.f 는 3; */
#define KW_NEW        "새로"      /* 객체 생성:  새로 사람()           */

/* 모듈 (구조만: 실행 시 무시) */
#define KW_IMPORT     "가져오기"
#define KW_EXPORT     "내보내기"

/* 자료형 정의 / 프로그램 */
#define KW_STRUCT     "구조체"    /* 구조체 사람 { 선언 나이; }        */
#define KW_PROGRAM    "프로그램"  /* 진입점 블록                       */

/* 함수 */
#define KW_FUNC       "함수"      /* 함수 더하기(a, b) { 돌려주기 a + b; } */
#define KW_RETURN     "돌려주기"

/* 제어 흐름 */
#define KW_IF         "만약"
#define KW_ELSE       "아니면"
#define KW_WHILE      "반복"
#define KW_BREAK      "완료"      /* break  */
#define KW_CONTINUE   "계속"      /* continue */

/* 예외 처리 / 종료 */
#define KW_TRY        "시도"
#define KW_CATCH      "예외"
#define KW_EXIT       "종료"      /* 종료;  또는  종료(코드); */

/* 논리 연산 (기호 && || ! 도 함께 허용) */
#define KW_AND        "그리고"
#define KW_OR         "또는"
#define KW_NOT        "아닌"

/* 덧셈 낱말 (기호 + 와 동치) */
#define KW_PLUS_WORD  "더하기"

#endif /* GNE_KEYWORDS_H */
