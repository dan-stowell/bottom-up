#!/usr/bin/env python3
"""Emit build/tables.h for the scalar C baseline from build/stencils.json and boot/spec.py."""
import json, os, sys
R = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
sys.path.insert(0, R + '/boot')
from spec import *
T = json.load(open(R + '/build/stencils.json'))
def arr(ty, name, v): return 'static const %s %s[] = {%s};\n' % (ty, name, ','.join(map(str, v)))
with open(R + '/build/tables.h', 'w') as f:
    for n in ('sten', 'tail', 'stub', 'ehdr'): f.write(arr('unsigned char', 'T_' + n, T[n]))
    for n in ('sstart', 'slen', 'ppos', 'padd', 'qstart', 'qlen', 'qpos', 'qadd'): f.write(arr('int', 'T_' + n, T[n]))
    f.write(arr('signed char', 'T_delta', DELTA)); f.write(arr('unsigned char', 'T_leadof', LEADOF)); f.write(arr('unsigned char', 'T_localof', LOCALOF)); f.write(arr('unsigned char', 'T_byteof', BYTEOF)); f.write(arr('unsigned char', 'T_istee', ISTEE)); f.write(arr('unsigned char', 'T_reread', REREAD)); f.write(arr('unsigned char', 'T_clstab', CLSTAB))
    f.write(arr('unsigned char', 'T_opc', [opclass(c) for c in range(128)])); f.write(arr('unsigned char', 'T_sig', [sigil(c) for c in range(128)]))
    for n, v in (('defines', DEFINES), ('uses', USES), ('readsvec', READSVEC), ('carry', CARRY), ('isteec', ISTEEC), ('vtype', VTYPE), ('isblead', ISBLEAD), ('isscat', ISSCAT), ('tvof', TVOF)): f.write(arr('unsigned char', 'T_' + n, v))
    f.write(arr('unsigned', 'RACE_U', RACE_U)); f.write(arr('unsigned', 'RACE_V', RACE_V))
    f.write('static const char *const KWS[] = {%s};\n' % ','.join('"%s"' % k.replace('\\', '\\\\').replace('"', '\\"') for k in KEYWORDS))
    for n, v in (('K', K), ('NCODE', NCODE), ('C_DATA', DATA), ('C_STR', STR), ('C_NONE', NONE), ('C_ERR', ERR), ('C_SCATTER', SCATTER), ('C_SLICES', SLICES), ('C_TEE', TEE), ('C_TEEC', TEEC), ('C_VECT', VECT), ('C_BYTES', KW['bytes']), ('C_VEC', VEC), ('C_LEN', LEN), ('SLOT', SLOT), ('HA1', HA1), ('HB1', HB1), ('HS1', HS1), ('HA2', HA2), ('HB2', HB2), ('HS2', HS2), ('ROUNDS', ROUNDS),
                 ('ND', ND), ('VBASE', VBASE), ('MAXNAME', MAXNAME), ('MAXDIGITS', MAXDIGITS), ('RELS', T['rels']), ('RELL', T['rell']), ('RELP', T['relp'])):
        f.write('#define %s %d\n' % (n, v))
