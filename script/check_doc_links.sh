#!/bin/bash
# Validate Markdown navigation and repository references.

set -euo pipefail
cd "$(dirname "$0")/.."

python3 - <<'PY'
from pathlib import Path
from urllib.parse import unquote
import re
import sys

ROOT = Path.cwd()
DOCS = [ROOT / "README.md", *sorted((ROOT / "docs").rglob("*.md"))]
errors = []


def github_slug(text):
    text = re.sub(r"<[^>]+>", "", text.strip().lower())
    text = re.sub(r"[^\w\s\u4e00-\u9fff-]", "", text)
    return re.sub(r"[\s-]+", "-", text).strip("-")


def anchors_for(path):
    counts = {}
    anchors = set()
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r"^#{1,6}\s+(.+?)\s*#*\s*$", line)
        if not match:
            continue
        base = github_slug(match.group(1))
        if not base:
            continue
        suffix = counts.get(base, 0)
        counts[base] = suffix + 1
        anchors.add(base if suffix == 0 else f"{base}-{suffix}")
    return anchors


anchor_cache = {}
make_targets = set()
for line in (ROOT / "Makefile").read_text(encoding="utf-8").splitlines():
    match = re.match(r"^([A-Za-z0-9_.-]+)\s*:(?!=)", line)
    if match:
        make_targets.add(match.group(1))

generated_refs = (
    "initrd_embed.h",
    "user_elf_embed.h",
    "asm_offsets_gas.h",
    "asm_offsets_nasm.inc",
)


def is_historical(path):
    name = path.name.lower()
    return (
        "review" in name
        or "fix" in name
        or name in {
            "known-issues.md",
            "arm64-port_zh.md",
            "arm64-port-todo.md",
            "m3a-el0-fix-summary.md",
            "vmm-review.md",
        }
        or "discussions" in path.parts
        or "plans" in path.parts
    )


def shell_fragments(text):
    fragments = []
    in_fence = False
    for line in text.splitlines():
        if line.strip().startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            fragments.append(line.strip())
    fragments.extend(match.group(1) for match in re.finditer(r"`([^`\n]+)`", text))
    return fragments


for doc in DOCS:
    text = doc.read_text(encoding="utf-8", errors="replace")
    rel_doc = doc.relative_to(ROOT)

    # Markdown links and local anchors.
    for match in re.finditer(r"(?<!!)\[[^\]]*\]\(([^)]+)\)", text):
        raw = match.group(1).strip().strip("<>")
        line = text.count("\n", 0, match.start()) + 1
        if not raw or re.match(r"^(https?|mailto):", raw):
            continue
        target_text, _, anchor = raw.partition("#")
        target = doc if not target_text else (doc.parent / unquote(target_text)).resolve()
        if not target.exists():
            errors.append(f"BROKEN LINK: {rel_doc}:{line} -> {raw}")
            continue
        if anchor and target.is_file() and target.suffix.lower() == ".md":
            anchors = anchor_cache.setdefault(target, anchors_for(target))
            if unquote(anchor).lower() not in anchors:
                errors.append(f"BROKEN ANCHOR: {rel_doc}:{line} -> {raw}")

    # Concrete repository paths in code spans. Globs and illustrative paths
    # are intentionally excluded.
    if not is_historical(rel_doc):
        for match in re.finditer(
            r"`((?:(?:kernel|module|user|script|conf|docs)/"
            r"[^`*<>{}:]+\.(?:c|h|S|asm|sh|py|md|conf)"
            r"|Makefile|kernel\.mk|README\.md))(?::\d+)?`",
            text,
        ):
            raw = match.group(1)
            line = text.count("\n", 0, match.start()) + 1
            path = ROOT / raw
            if path.exists() or raw.endswith(generated_refs):
                continue
            # ARM64 implementation files intentionally do not exist on the
            # x86_64 shared branch.
            if raw.startswith("kernel/arch/aarch64/"):
                continue
            source_line = text.splitlines()[line - 1].lower()
            if "linux" in source_line:
                continue
            if any(word in source_line for word in ("historical", "removed", "deleted", "已删除", "已移除")):
                continue
            errors.append(f"MISSING FILE: {rel_doc}:{line} -> {raw}")

    # Literal make targets in fenced or inline code.
    for fragment in shell_fragments(text):
        command = re.sub(r"^(?:[$#]\s*|sudo\s+)", "", fragment.strip())
        command = command.split("#", 1)[0].strip()
        if not command.startswith("make"):
            continue
        tokens = command.split()
        if not tokens or tokens[0] != "make":
            continue
        target = None
        for token in tokens[1:]:
            token = token.rstrip(";")
            if token in {"&&", "||", "\\", "|"}:
                break
            if token.startswith("-") or "=" in token:
                continue
            target = token
            break
        if target and target not in make_targets:
            errors.append(f"UNKNOWN MAKE TARGET: {rel_doc} -> {target}")

if errors:
    print("\n".join(errors))
    print(f"FAIL: {len(errors)} documentation reference error(s)")
    sys.exit(1)

print(f"PASS: checked {len(DOCS)} Markdown files, anchors, source paths and Make targets")
PY
