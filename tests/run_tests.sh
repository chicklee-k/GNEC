#!/bin/sh
# 예제 출력 비교 + 오류/예외가 올바른 종료 코드를 내는지 확인
BIN=${1:-./gnec}
DIR=$(dirname "$0")
pass=0; fail=0
for exp in "$DIR"/*.expected; do
    name=$(basename "$exp" .expected)
    if "$BIN" "$DIR/../examples/$name.ㄹ" | cmp -s - "$exp"; then
        pass=$((pass + 1))
    else
        echo "실패: $name"; fail=$((fail + 1))
    fi
done
# 문법 오류(종료코드 1)로 끝나야 하는 것들
while IFS= read -r code; do
    [ -z "$code" ] && continue
    if "$BIN" -e "$code" >/dev/null 2>&1; then
        echo "실패(오류 기대): $code"; fail=$((fail + 1))
    else
        pass=$((pass + 1))
    fi
done <<'CASES'
프로그램 { 선언 x 는 ; }
구조체 { }
프로그램 { 선언 a 는 [1] }
프로그램 { 만약 (참) { } 아니면 }
프로그램 { 콘솔.로그("미닫힘
CASES
echo "통과 $pass, 실패 $fail"
[ "$fail" -eq 0 ]
