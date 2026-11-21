#!/usr/bin/env python3
"""
Two traps that have each broken a build before, caught in a second instead of
after a five-minute compile. Run by build.yml before arduino-cli; adds nothing
to the firmware.

1. Prototype order. Arduino copies a prototype of every function to just
   above the FIRST function in the .ino. A type (struct, enum, class, typedef,
   using) that a function signature mentions must therefore be defined above
   that first function, or the build fails with "'X' does not name a type"
   (ClockPillMode, PushNotificationParams, FwManifest, ToastKind did).
2. The text  extern "C"  anywhere in the .ino -- even inside a comment --
   makes the Arduino preprocessor wrap the wrong lines, and setup()/loop()
   lose C++ linkage. C linkage belongs in a .cpp file (see crash_note.cpp).

    python3 tools/lint_sketch.py sketch/Panel/Panel.ino
"""
import re, sys

TYPE_DEF = re.compile(
    r"^\s*(?:typedef\s+(?:struct|enum|union)?[^;{]*?\b(\w+)\s*;"     # typedef ... Name;
    r"|(?:typedef\s+)?(?:struct|class|union)\s+(\w+)"                # struct Name
    r"|enum\s+(?:class\s+|struct\s+)?(\w+)"                          # enum [class] Name
    r"|using\s+(\w+)\s*=)")                                          # using Name =
NOT_FUNCTION = {"if", "for", "while", "switch", "return", "sizeof", "catch", "else", "do"}


def strip(src):
    """Comments, strings and char literals blanked; newlines kept so line numbers hold."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i)); i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", src[i:j])); i = j
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            out.append(c + " " * (j - i - 1) + c); i = j + 1
        else:
            out.append(c); i += 1
    return "".join(out)


def top_level(code):
    """Yield (line, text, opens_brace) for each depth-0 statement or block header.
    line is where the text starts (its first non-blank character)."""
    depth, start, line = 0, 0, 1
    start_line = 1
    for k, ch in enumerate(code):
        if ch == "\n":
            line += 1
        if depth == 0 and start_line is None and not ch.isspace():
            start_line = line
        if ch == "{":
            if depth == 0:
                yield start_line or line, code[start:k], True
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                start, start_line = k + 1, None
        elif ch == ";" and depth == 0:
            yield start_line or line, code[start:k], False
            start, start_line = k + 1, None


def lint(path):
    raw = open(path, encoding="utf-8", errors="replace").read()
    problems = []

    for n, text in enumerate(raw.splitlines(), 1):
        if re.search(r'extern\s*"C"', text):
            problems.append(f"{path}:{n}: the text extern \"C\" must not appear in the .ino "
                            "(not even in a comment) -- move C-linkage code to a .cpp file")

    code = strip(raw)
    # Preprocessor lines (and their continuations) are not C++ statements.
    kept = []
    cont = False
    for text in code.split("\n"):
        is_pp = cont or text.lstrip().startswith("#")
        cont = is_pp and text.rstrip().endswith("\\")
        kept.append(" " * len(text) if is_pp else text)
    code = "\n".join(kept)

    types = {}          # name -> line defined
    functions = []      # (line, signature)
    prev_typedef = False
    for line, head, brace in top_level(code):
        h = " ".join(head.split())
        if not h:
            continue
        if prev_typedef and not brace and re.fullmatch(r"\w+", h):
            types.setdefault(h, line)                       # typedef struct {...} Name;
        prev_typedef = brace and h.startswith("typedef")
        m = TYPE_DEF.match(h)
        if m and "(" not in h:
            types.setdefault(next(g for g in m.groups() if g), line)
            continue
        if brace and "(" in h and "=" not in h.split("(")[0] and not h.startswith(("namespace", "extern", "template")):
            name = re.findall(r"(\w+)\s*\(", h)
            if name and name[0] not in NOT_FUNCTION:
                functions.append((line, h))

    if not functions:
        return problems
    first = functions[0][0]
    late = {t: l for t, l in types.items() if l > first}
    for line, sig in functions:
        for word in set(re.findall(r"\b\w+\b", sig)):
            if word in late:
                problems.append(
                    f"{path}:{line}: '{word}' is used in a function signature but defined on line "
                    f"{late[word]}, below the first function (line {first}). Arduino puts its "
                    f"prototypes above line {first}, so move the definition of '{word}' up there.")
    return problems


def main(paths):
    problems = [p for path in paths for p in lint(path)]
    ci = "GITHUB_ACTIONS" in __import__("os").environ
    for p in problems:
        print(("::error::" if ci else "error: ") + p)
    if not problems:
        print("sketch lint: prototype order and extern \"C\" -- clean")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:] or ["sketch/Panel/Panel.ino"]))
