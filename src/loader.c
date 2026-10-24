/*
 * loader.c — 소스 파일 로더 + 인코딩 변환
 *
 * 2차 명세는 "EUC-KR만 지원"을 표방하지만, 실제 예제 파일은 UTF-8/UTF-16 이 섞여 있다.
 * 그래서 입력 바이트를 모두 내부 UTF-8 로 정규화해 받아들인다.
 *   - BOM 으로 UTF-8 / UTF-16LE / UTF-16BE 를 판별
 *   - UTF-16 → UTF-8 은 서로게이트 페어까지 직접 변환 (외부 의존성 없음)
 *   - BOM 이 없고 UTF-8 로 유효하지 않으면 EUC-KR 로 보고 iconv 로 변환
 *     (iconv 는 POSIX 표준. GNE_NO_ICONV 로 끄면 EUC-KR 입력은 거부)
 *
 * 메모리 레이아웃 관점: 파일 전체를 힙(heap)에 한 번에 올린 뒤
 * 변환 결과를 새 힙 버퍼로 만들고 원본은 해제한다. 호출자는 반환 버퍼를 free().
 */
#include "gnec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GNE_NO_ICONV
#include <iconv.h>
#include <errno.h>
#endif

/* 파일 전체를 바이트로 읽는다. *out_len 에 길이. 실패 시 NULL. */
static unsigned char *read_all(const char *path, size_t *out_len, char *errbuf, size_t errlen)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        snprintf(errbuf, errlen, "파일을 열 수 없습니다: %s", path);
        return NULL;
    }
    size_t cap = 4096, len = 0;
    unsigned char *buf = malloc(cap);
    if (buf == NULL) { fclose(fp); snprintf(errbuf, errlen, "메모리 부족"); return NULL; }

    size_t n;
    while ((n = fread(buf + len, 1, cap - len, fp)) > 0) {
        len += n;
        if (len == cap) {
            unsigned char *grown = realloc(buf, cap * 2);
            if (grown == NULL) { free(buf); fclose(fp); snprintf(errbuf, errlen, "메모리 부족"); return NULL; }
            buf = grown;
            cap *= 2;
        }
    }
    if (ferror(fp)) {
        free(buf); fclose(fp);
        snprintf(errbuf, errlen, "파일 읽기 오류: %s", path);
        return NULL;
    }
    fclose(fp);
    *out_len = len;
    return buf;
}

/* UTF-8 한 글자 인코딩. buf 에 쓰고 바이트 수 반환 (최대 4). */
static size_t encode_utf8(uint32_t cp, char buf[4])
{
    if (cp < 0x80) {
        buf[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    buf[0] = (char)(0xF0 | (cp >> 18));
    buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* UTF-16 → UTF-8. data/len 은 BOM 이후 바이트. 실패 시 NULL. */
static char *utf16_to_utf8(const unsigned char *data, size_t len, bool big_endian,
                           char *errbuf, size_t errlen)
{
    size_t units = len / 2;
    /* 최악의 경우 코드유닛당 3바이트, 서로게이트 페어는 2유닛→4바이트라 넉넉하다 */
    size_t cap = units * 3 + 1;
    char *out = malloc(cap);
    if (out == NULL) { snprintf(errbuf, errlen, "메모리 부족"); return NULL; }

    size_t o = 0;
    for (size_t i = 0; i < units; ++i) {
        size_t b = i * 2;
        uint32_t u = big_endian
            ? (uint32_t)((data[b] << 8) | data[b + 1])
            : (uint32_t)((data[b + 1] << 8) | data[b]);

        uint32_t cp;
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < units) {
            /* 상위 서로게이트 + 하위 서로게이트 → 보충 문자 */
            size_t b2 = (i + 1) * 2;
            uint32_t low = big_endian
                ? (uint32_t)((data[b2] << 8) | data[b2 + 1])
                : (uint32_t)((data[b2 + 1] << 8) | data[b2]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((u - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            } else {
                cp = 0xFFFD;   /* 짝 없는 서로게이트 → 대체 문자 */
            }
        } else if (u >= 0xD800 && u <= 0xDFFF) {
            cp = 0xFFFD;
        } else {
            cp = u;
        }
        char tmp[4];
        size_t k = encode_utf8(cp, tmp);
        memcpy(out + o, tmp, k);
        o += k;
    }
    out[o] = '\0';
    return out;
}

/* 바이트열이 올바른 UTF-8 인지 검사 */
static bool is_valid_utf8(const unsigned char *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        unsigned char c = s[i];
        size_t extra;
        if (c < 0x80)           { ++i; continue; }
        else if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else return false;
        if (i + extra >= len) return false;
        for (size_t k = 1; k <= extra; ++k)
            if ((s[i + k] & 0xC0) != 0x80) return false;
        i += extra + 1;
    }
    return true;
}

#ifndef GNE_NO_ICONV
static char *euckr_to_utf8(const unsigned char *data, size_t len, char *errbuf, size_t errlen)
{
    iconv_t cd = iconv_open("UTF-8", "EUC-KR");
    if (cd == (iconv_t)-1) {
        snprintf(errbuf, errlen, "EUC-KR 변환기를 열 수 없습니다");
        return NULL;
    }
    size_t out_cap = len * 3 + 1;
    char *out = malloc(out_cap);
    if (out == NULL) { iconv_close(cd); snprintf(errbuf, errlen, "메모리 부족"); return NULL; }

    /* iconv 는 비-const char** 를 요구한다 (POSIX). 원본은 수정되지 않는다. */
    char   *inbuf  = (char *)(uintptr_t)(const void *)data;
    size_t  inleft = len;
    char   *outbuf = out;
    size_t  outleft = out_cap - 1;

    while (inleft > 0) {
        size_t r = iconv(cd, &inbuf, &inleft, &outbuf, &outleft);
        if (r == (size_t)-1) {
            if (errno == E2BIG) {       /* 출력 버퍼 확장 */
                size_t used = (size_t)(outbuf - out);
                size_t new_cap = out_cap * 2;
                char *grown = realloc(out, new_cap);
                if (grown == NULL) { free(out); iconv_close(cd); snprintf(errbuf, errlen, "메모리 부족"); return NULL; }
                out = grown;
                outbuf = out + used;
                outleft = new_cap - used - 1;
                out_cap = new_cap;
                continue;
            }
            free(out); iconv_close(cd);
            snprintf(errbuf, errlen, "EUC-KR 디코딩 실패 (손상된 바이트)");
            return NULL;
        }
    }
    *outbuf = '\0';
    iconv_close(cd);
    return out;
}
#endif

char *load_source_file(const char *path, char *errbuf, size_t errlen)
{
    size_t len = 0;
    unsigned char *raw = read_all(path, &len, errbuf, errlen);
    if (raw == NULL) return NULL;

    char *result = NULL;

    if (len >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        result = utf16_to_utf8(raw + 2, len - 2, false, errbuf, errlen);
    } else if (len >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        result = utf16_to_utf8(raw + 2, len - 2, true, errbuf, errlen);
    } else if (len >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        result = malloc(len - 3 + 1);              /* UTF-8 BOM 제거 */
        if (result == NULL) snprintf(errbuf, errlen, "메모리 부족");
        else { memcpy(result, raw + 3, len - 3); result[len - 3] = '\0'; }
    } else if (is_valid_utf8(raw, len)) {
        result = malloc(len + 1);
        if (result == NULL) snprintf(errbuf, errlen, "메모리 부족");
        else { memcpy(result, raw, len); result[len] = '\0'; }
    } else {
#ifndef GNE_NO_ICONV
        result = euckr_to_utf8(raw, len, errbuf, errlen);   /* BOM 없는 비-UTF-8 → EUC-KR 추정 */
#else
        snprintf(errbuf, errlen, "EUC-KR 입력은 이 빌드에서 지원되지 않습니다 (iconv 비활성)");
#endif
    }

    free(raw);
    return result;
}
