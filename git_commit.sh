#!/bin/bash
# Claude Fable - 2026-11-24: git runs on the Mac through the build loop.
# Trigger with: echo git_commit > trigger. First run creates the repo; later runs commit using the message in git_msg.txt.
source "$(dirname "$0")/_common.sh"
exec > git_out.txt 2>&1
[ -d .git ] || git init -b main
git config user.name  >/dev/null || git config user.name  "Braden Bowdish"
git config user.email >/dev/null || git config user.email "braden@localhost"
if [ -f git_squash ]; then   # one-time: restart history as a single commit (flag file is removed by the next line)
  mv git_squash git_squash.done; git update-ref -d HEAD; git rm -r -q -f --cached . >/dev/null 2>&1
fi
git add -A
echo "--- staged summary"; git diff --cached --stat | tail -3
echo "--- largest staged files"; git diff --cached --name-only | while read -r f; do [ -f "$f" ] && echo "$(stat -f%z "$f") $f"; done | sort -rn | head -12
if [ -s git_msg.txt ]; then git commit -q -F git_msg.txt && : > git_msg.txt; else echo "no git_msg.txt: staged only, not committed"; fi
[ -f git_squash.done ] && { git reflog expire --expire=now --all; git gc -q --prune=now; }
echo "--- log"; git log --oneline | head -5
echo "--- status"; git status --short | head -20
stage "Idle" 0
