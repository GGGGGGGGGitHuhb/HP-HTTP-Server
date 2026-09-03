#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: http_smoke_test.sh <hp_http_server-path>" >&2
    exit 2
fi

server_path="$(realpath "$1")"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
task_root="${HP_S3_TEST_TMP_ROOT:-$repo_root/.cache/olympus-v0.1-s3/tests}"
mkdir -p "$task_root"
workspace="$(mktemp -d "$task_root/smoke-XXXXXX")"
root="$workspace/root"
log="$workspace/server.log"
headers="$workspace/headers"
body="$workspace/body"
mkdir -p "$root"
printf '<h1>smoke index</h1>\n' > "$root/index.html"
printf 'smoke text\n' > "$root/note.txt"
printf 'S3-SMOKE-SIBLING-SECRET\n' > "$workspace/sibling-secret.txt"
ln -s "$workspace/sibling-secret.txt" "$root/escape.txt"

server_pid=""
cleanup() {
    if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -rf -- "$workspace"
}
trap cleanup EXIT

"$server_path" --port 0 --root "$root" >"$log" 2>&1 &
server_pid=$!

port=""
for _ in $(seq 1 100); do
    if ! kill -0 "$server_pid" 2>/dev/null; then
        echo "server exited during smoke startup" >&2
        sed -n '1,20p' "$log" >&2
        exit 1
    fi
    port="$(sed -n 's/.*listening on port \([0-9][0-9]*\).*/\1/p' "$log" | head -n 1)"
    [[ -n "$port" ]] && break
    sleep 0.05
done
if [[ -z "$port" ]]; then
    echo "server startup port was not reported" >&2
    exit 1
fi

request() {
    local expected="$1"
    shift
    local actual
    actual="$(curl --silent --show-error --path-as-is \
        --dump-header "$headers" --output "$body" \
        --write-out '%{http_code}' "$@")"
    [[ "$actual" == "$expected" ]]
    grep -q $'Connection: close\r$' "$headers"
}

base="http://127.0.0.1:$port"
request 200 "$base/"
cmp -s "$body" "$root/index.html"
request 200 "$base/note.txt?x=1"
cmp -s "$body" "$root/note.txt"
request 404 "$base/missing.txt"
request 405 --request POST "$base/"
grep -q $'Allow: GET\r$' "$headers"
request 403 "$base/../sibling-secret.txt"
! grep -q 'S3-SMOKE-SIBLING-SECRET' "$body"
request 400 "$base/%2e%2e/sibling-secret.txt"
! grep -q 'S3-SMOKE-SIBLING-SECRET' "$body"
request 403 "$base/escape.txt"
! grep -q 'S3-SMOKE-SIBLING-SECRET' "$body"

echo "HTTP smoke passed; status_hits={200:2,400:1,403:2,404:1,405:1} traversal_hits=3 secret_leaks=0"
