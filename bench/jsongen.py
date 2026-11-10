#!/usr/bin/env python3
"""jsongen.py -- a valid JSON document of about N bytes (records like an API would return, with
strings that contain quotes, backslashes, braces, colons and commas), and an independent statement
of what its structural characters are: the skeleton is rebuilt from the *parsed* document by walking
the tree, so it owes nothing to any hand-written scanner."""
import json, random
WORDS = 'the of and to in is that it was for on are as with his they at be this from have or by one had not but what all were when we there can an your which their said if do will each about how up out them then she many some so these would other into has more her two like him see time could no make than first been its who now people my made over did down only way find use may water long little very after words called just where most know'.split()
def text(r):
    n = r.randint(1, 14); out = []
    for _ in range(n):
        w = r.choice(WORDS); q = r.random()
        if q < 0.04: w = '"' + w + '"'
        elif q < 0.07: w = w + ','
        elif q < 0.09: w = '{' + w + '}'
        elif q < 0.11: w = 'C:\\' + w + '\\'
        elif q < 0.13: w = '[' + w + ']: '
        elif q < 0.14: w = w + '\n'
        elif q < 0.15: w = w + ' \u00e9\u4e2d'
        out.append(w)
    return ' '.join(out)
def record(r, depth=0):
    d = {'id': r.randrange(1 << 40), 'name': text(r), 'active': r.random() < 0.5, 'score': round(r.uniform(-1000, 1000), 3),
         'tags': [r.choice(WORDS) for _ in range(r.randint(0, 5))], 'note': None if r.random() < 0.2 else text(r)}
    if depth < 3 and r.random() < 0.4: d['children'] = [record(r, depth + 1) for _ in range(r.randint(0, 3))]
    if r.random() < 0.3: d['geo'] = {'lat': r.uniform(-90, 90), 'lon': r.uniform(-180, 180), 'path': [[r.randint(0, 99), r.randint(0, 99)] for _ in range(r.randint(0, 4))]}
    return d
def document(nbytes, seed=1):
    r = random.Random(seed); parts = []; size = 2
    while size < nbytes:
        s = json.dumps(record(r), ensure_ascii=r.random() < 0.5, indent=None if r.random() < 0.7 else 1); parts.append(s); size += len(s.encode()) + 2
    return ('[' + ',\n'.join(parts) + ']\n').encode()
def skeleton(doc):
    out = []
    def walk(v):
        if isinstance(v, dict):
            out.append('{')
            for i, (k, x) in enumerate(v.items()):
                if i: out.append(',')
                out.append(':'); walk(x)
            out.append('}')
        elif isinstance(v, list):
            out.append('[')
            for i, x in enumerate(v):
                if i: out.append(',')
                walk(x)
            out.append(']')
    walk(json.loads(doc)); return ''.join(out).encode()
if __name__ == '__main__':
    import sys; sys.stdout.buffer.write(document(int(sys.argv[1])))
