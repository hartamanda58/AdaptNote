#!/usr/bin/env bash
# Drives the Windows build on GitHub Actions end to end.
#
#   export GH_TOKEN=<fine-grained or classic PAT>
#   bash scripts/gh_ci.sh            # 建仓(可跳过) → push → 触发 workflow → 等 → 下产物
#
# Needs: git, curl, python3 (json), unzip. No gh CLI required.
set -euo pipefail

: "${GH_TOKEN:?需要 GH_TOKEN（repo/contents+actions+workflows 权限）}"
OWNER="${OWNER:-$(curl -sS -H "Authorization: Bearer $GH_TOKEN" https://api.github.com/user | python3 -c 'import json,sys;print(json.load(sys.stdin)["login"])')}"
REPO="${REPO:-AdaptNote}"
PRIVATE="${PRIVATE:-true}"
BRANCH="${BRANCH:-main}"
WORKFLOW="windows-portable.yml"
OUT="${OUT:-dist/github}"

api() { # api <method> <path> [json]
    local m="$1" p="$2" body="${3:-}"
    if [ -n "$body" ]; then
        curl -sS -X "$m" -H "Authorization: Bearer $GH_TOKEN" \
             -H "Accept: application/vnd.github+json" \
             -H "X-GitHub-Api-Version: 2022-11-28" \
             -d "$body" "https://api.github.com$p"
    else
        curl -sS -X "$m" -H "Authorization: Bearer $GH_TOKEN" \
             -H "Accept: application/vnd.github+json" \
             -H "X-GitHub-Api-Version: 2022-11-28" \
             "https://api.github.com$p"
    fi
}

echo "== 账号：$OWNER / 仓库：$REPO"

if api GET "/repos/$OWNER/$REPO" | grep -q '"full_name"'; then
    echo "== 仓库已存在，跳过创建"
else
    echo "== 创建仓库"
    api POST /user/repos "{\"name\":\"$REPO\",\"private\":$PRIVATE,\"description\":\"AdaptNote: local-first PDF/EPUB reader with annotations, sync, AI, portable Windows/Linux builds\"}" \
        | python3 -c 'import json,sys; d=json.load(sys.stdin); print("created:", d.get("full_name") or d)'
fi

echo "== 推送 $BRANCH"
# keep the token out of .git/config: use a credential helper fed from the environment
git config --local credential.helper '!f() { echo "username=x-access-token"; echo "password=$GH_TOKEN"; }; f'
git remote remove origin 2>/dev/null || true
git remote add origin "https://github.com/$OWNER/$REPO.git"
git push -u origin "$BRANCH" --force-with-lease=refs/heads/"$BRANCH": 2>/dev/null \
  || git push -u origin "$BRANCH"

echo "== 触发 workflow"
api POST "/repos/$OWNER/$REPO/actions/workflows/$WORKFLOW/dispatches" \
    "{\"ref\":\"$BRANCH\"}" >/dev/null
echo "   dispatched"

echo "== 等待运行出现"
RUN_ID=""
for _ in $(seq 1 30); do
    RUN_ID=$(api GET "/repos/$OWNER/$REPO/actions/workflows/$WORKFLOW/runs?per_page=1" \
             | python3 -c 'import json,sys; r=json.load(sys.stdin).get("workflow_runs") or []; print(r[0]["id"] if r else "")')
    [ -n "$RUN_ID" ] && break
    sleep 5
done
[ -n "$RUN_ID" ] || { echo "没有找到运行记录"; exit 1; }
echo "   run id: $RUN_ID"
echo "   https://github.com/$OWNER/$REPO/actions/runs/$RUN_ID"

echo "== 轮询状态"
for i in $(seq 1 180); do
    read -r STATUS CONCLUSION <<<"$(api GET "/repos/$OWNER/$REPO/actions/runs/$RUN_ID" \
        | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("status","?"), d.get("conclusion") or "-")')"
    printf "   [%3d] %s %s\n" "$i" "$STATUS" "$CONCLUSION"
    case "$STATUS" in
        completed) break ;;
    esac
    sleep 20
done

if [ "$CONCLUSION" != "success" ]; then
    echo "== 失败，抓取作业与日志"
    api GET "/repos/$OWNER/$REPO/actions/runs/$RUN_ID/jobs" \
      | python3 -c 'import json,sys
d=json.load(sys.stdin)
for j in d.get("jobs",[]):
    print("JOB", j["name"], j["conclusion"])
    for s in j.get("steps",[]):
        print("   ", s["conclusion"], s["name"])'
    mkdir -p "$OUT"
    curl -sSL -H "Authorization: Bearer $GH_TOKEN" \
         "https://api.github.com/repos/$OWNER/$REPO/actions/runs/$RUN_ID/logs" \
         -o "$OUT/logs.zip" || true
    [ -s "$OUT/logs.zip" ] && unzip -o -q "$OUT/logs.zip" -d "$OUT/logs" && echo "   日志在 $OUT/logs"
    exit 1
fi

echo "== 成功，下载产物"
mkdir -p "$OUT"
ART_URL=$(api GET "/repos/$OWNER/$REPO/actions/runs/$RUN_ID/artifacts" \
          | python3 -c 'import json,sys
d=json.load(sys.stdin)
a=d.get("artifacts") or []
print(a[0]["archive_download_url"] if a else "")')
[ -n "$ART_URL" ] || { echo "没有产物"; exit 1; }
curl -sSL -H "Authorization: Bearer $GH_TOKEN" "$ART_URL" -o "$OUT/artifact.zip"
unzip -o -q "$OUT/artifact.zip" -d "$OUT" && ls -lh "$OUT"/*.zip
echo "== 完成：$OUT"
