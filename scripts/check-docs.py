#!/usr/bin/env python3
"""Checks that every relative link in the documentation points at a file and heading that exist."""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
LINK = re.compile(r"(?<!\!)\[[^\]]*\]\(([^)\s]+)\)")
HEADING = re.compile(r"^#{1,6}\s+(.*)$")
FENCE = re.compile(r"^\s*```")


def documents():
    yield from ROOT.glob("*.md")
    yield from (ROOT / "docs").rglob("*.md")


def without_code_blocks(path):
    inside = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if FENCE.match(line):
            inside = not inside
        elif not inside:
            yield line


def anchor_for(heading):
    """Follows GitHub's rule: lower case, no punctuation, and spaces become hyphens."""
    text = re.sub(r"`", "", heading.strip()).lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def anchors_in(path):
    return {anchor_for(match.group(1)) for line in without_code_blocks(path)
            if (match := HEADING.match(line))}


def main():
    problems = []
    for document in documents():
        for line in without_code_blocks(document):
            # Inline code can look like a link.
            for target in LINK.findall(re.sub(r"`[^`]*`", "", line)):
                if re.match(r"[a-z]+:", target):
                    continue
                file_part, _, anchor = target.partition("#")
                resolved = (document.parent / file_part).resolve() if file_part else document
                name = document.relative_to(ROOT).as_posix()
                if not resolved.exists():
                    problems.append(f"{name}: {target}: no such file")
                elif anchor and resolved.suffix == ".md" and anchor not in anchors_in(resolved):
                    problems.append(f"{name}: {target}: no such heading")

    for problem in problems:
        print(problem)
    print(f"Checked {sum(1 for _ in documents())} documents, and found {len(problems)} problems.")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
