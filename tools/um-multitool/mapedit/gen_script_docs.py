#!/usr/bin/env python3
# Regenerates script_docs.hpp (the Script editor's tooltips) from docs/scripting.md:  python3 gen_script_docs.py
import os, re, sys

here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, '..', '..', '..', 'docs', 'scripting.md')
dst = os.path.join(here, 'script_docs.hpp')

row = re.compile(r'^\| \*\*([\w /]+)\*\*(?: \((.*?)\))?(?: → \w+)?(?: \(Command\))? \| (.*) \|$')
entries = []
for line in open(src, encoding='utf-8'):
    m = row.match(line.rstrip('\n'))
    if not m:
        continue
    names, params, text = m.group(1), m.group(2) or '', m.group(3).replace('`', '')
    for name in (n.strip() for n in names.split('/')):
        entries.append((name, params, text))

def q(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'

out = ['// SPDX-License-Identifier: GPL-3.0-or-later',
       '// Copyright (C) 2026 Kyr4l',
       "// Generated from docs/scripting.md (the script functions: parameter names and descriptions) by gen_script_docs.py - regenerate it if the doc changes.",
       '#pragma once', '', '#include <cstring>', '', 'namespace scriptdocs {',
       'struct Doc { const char* name; const char* params; const char* text; };',
       'inline const Doc* Docs(int& n) {', '    static const Doc kDocs[] = {']
for name, params, text in entries:
    out.append('        {%s, %s, %s},' % (q(name), q(params), q(text)))
out += ['    };', '    n = static_cast<int>(sizeof(kDocs) / sizeof(kDocs[0]));', '    return kDocs;', '}',
        'inline const Doc* Find(const char* name) {', '    int n = 0;', '    const Doc* d = Docs(n);',
        '    for (int i = 0; i < n; ++i) {',
        '        const char* a = d[i].name; const char* b = name;',
        '        while (*a && *b && (*a | 32) == (*b | 32)) { ++a; ++b; }',
        '        if (!*a && !*b) return &d[i];', '    }', '    return nullptr;', '}',
        '} // namespace scriptdocs', '']
open(dst, 'w', encoding='utf-8').write('\n'.join(out))
print(len(entries), 'entries')
