// strandc.c -- the Strand compiler as an ordinary scalar C program.
// Same specification as boot/strand.py and src/compiler.strand; output must be byte-identical.
// It exists (1) as the honest baseline for "what did SIMD buy" and (2) as a third independent
// implementation for differential testing. Written the obvious way: a character loop, a hash table
// of names, a token array, sequential passes.   usage: strandc < prog.strand > a.out
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tables.h"

typedef struct { int start, end, line, first, last, sgi, opc, lit, str, isname, pidx, name, cls, cls0, local, slot, d0, code, tid, disp, dying; } Tok;
typedef struct { int off, len, last, slot, kw, defined; uint32_t h1, h2; int active, clast; } Name;

static unsigned char *text; static int T;
static Tok *tok; static int nt;
static Name *names; static int nn;
static int *htab; static int hmask;
static uint32_t *pool; static int np;
static int errmask;
#define ERR(e) (errmask |= 1 << (e))
static void die(void) { for (int e = 1; e < 16; e++) if (errmask >> e & 1) exit(e); }

static int intern(const unsigned char *s, int len) {
  uint32_t h = 2166136261u; for (int i = 0; i < len; i++) h = (h ^ s[i]) * 16777619u;
  for (uint32_t k = h & hmask;; k = (k + 1) & hmask) {
    int n = htab[k];
    if (n < 0) { htab[k] = nn; names[nn] = (Name){(int)(s - text), len, 0, 0, 0, 0, 0, 0, 0, 0}; return nn++; }
    if (names[n].len == len && !memcmp(text + names[n].off, s, len)) return n;
  }
}
static int by_last(const void *a, const void *b) { return names[*(const int *)a].last - names[*(const int *)b].last; }

int main(void) {
  size_t cap = 1 << 16, n = strlen(PRELUDE);
  text = malloc(cap); memcpy(text, PRELUDE, n);
  for (size_t r; (r = fread(text + n, 1, cap - n - 1, stdin)) > 0;) { n += r; if (n + 1 >= cap) text = realloc(text, cap *= 2); }
  text[n++] = '\n'; T = (int)n;
  tok = malloc(sizeof(Tok) * (T / 2 + 2)); pool = malloc(sizeof(uint32_t) * (T + 1));
  names = malloc(sizeof(Name) * (T / 2 + 2));
  for (hmask = 1024; hmask < T; hmask *= 2); htab = malloc(sizeof(int) * hmask); memset(htab, -1, sizeof(int) * hmask); hmask--;

  // ---- lex: lines, comments, strings, tokens
  int line = 0;
  for (int p = 0; p < T; line++) {
    int e = p; while (text[e] != '\n') e++;
    int c = p; while (c < e && text[c] != '#') c++;          // comment starts at the first '#'
    int quotes = 0; for (int i = p; i < c; i++) quotes += text[i] == '"';
    if (quotes & 1) ERR(1);
    int first = 1, i = p;
    while (i < c) {
      if (text[i] <= 32) { i++; continue; }
      int j = i, instr = 0;
      while (j < c && (instr || text[j] > 32)) {
        if (text[j] == '"') {
          if (!instr && j > i) ERR(1);                        // opening quote must start the token
          if (instr && j + 1 < c && text[j + 1] > 32) ERR(1); // closing quote must end it
          instr = !instr;
        }
        j++;
      }
      Tok *t = &tok[nt]; memset(t, 0, sizeof *t);
      t->start = i; t->end = j; t->line = line; t->first = first; first = 0;
      int c0 = text[i] > 127 ? 127 : text[i], c1 = j - i > 1 ? (text[i + 1] > 127 ? 127 : text[i + 1]) : 32;
      int sig = T_sig[c0] && j - i > 1 && T_opc[c1] >= 1 && T_opc[c1] <= 3;
      t->sgi = sig ? T_sig[c0] : 0; t->opc = sig ? T_opc[c1] : T_opc[c0];
      t->lit = t->opc == 3; t->str = t->opc == 4; t->isname = !t->lit && !t->str;
      int ns = i + sig, nl = j - ns;
      t->pidx = np;
      if (t->lit) {
        uint32_t v = 0; int ok = nl <= MAXDIGITS;
        for (int k = ns; k < j; k++) { if (text[k] < '0' || text[k] > '9') ok = 0; v = v * 10 + (text[k] - '0'); }
        if (!ok) ERR(2);
        pool[np++] = v;
      } else if (t->str) { for (int k = i + 1; k < j - 1; k++) pool[np++] = text[k]; }
      else { if (nl > MAXNAME) ERR(3); t->name = intern(text + ns, nl); names[t->name].last = nt; }
      nt++; i = j;
    }
    if (nt && tok[nt - 1].line == line) tok[nt - 1].last = 1;
    p = e + 1;
  }
  die();

  // ---- the scatter race of the language definition (boot/spec.py), simulated with plain loops.
  // Names with equal fingerprints form one class (found by sorting); classes race for table slots.
  {
    for (int i = 0; i < nn; i++) {
      uint32_t h1 = 0, h2 = 0;
      for (int k = 0; k < names[i].len; k++) {
        uint32_t x = text[names[i].off + k] + 256u * k, a;
        a = x * HA1; h1 += (a ^ (a >> HS1)) * HB1;
        a = x * HA2; h2 += (a ^ (a >> HS2)) * HB2;
      }
      names[i].h1 = h1; names[i].h2 = h2; names[i].active = 1; names[i].clast = names[i].last;
    }
    int collision = 0;                     // equal fingerprint, different name: O(n) via a second hash table
    int *ft = malloc(sizeof(int) * (hmask + 1)); memset(ft, -1, sizeof(int) * (hmask + 1));
    for (int i = 0; i < nn; i++) {
      uint32_t k = (names[i].h1 * 2654435761u ^ names[i].h2) & hmask;
      for (;; k = (k + 1) & hmask) {
        int j = ft[k];
        if (j < 0) { ft[k] = i; break; }
        if (names[j].h1 == names[i].h1 && names[j].h2 == names[i].h2) {   // merge i into class j
          collision = 1; names[i].active = 0; if (names[i].last > names[j].clast) names[j].clast = names[i].last; break;
        }
      }
    }
    uint32_t mask = nt; for (int sh = 1; sh <= 16; sh *= 2) mask |= mask >> sh; mask = 2 * mask + 1;
    int *owner = malloc(sizeof(int) * ((size_t)mask + 1)), left = 0;
    for (int r = 0; r < ROUNDS; r++) {
      memset(owner, -1, sizeof(int) * ((size_t)mask + 1));
#define KEY(i) (((names[i].h1 * RACE_U[r] + names[i].h2 * RACE_V[r]) >> 8) & mask)
      for (int i = 0; i < nn; i++) if (names[i].active) { uint32_t k = KEY(i); if (owner[k] < 0 || names[owner[k]].clast < names[i].clast) owner[k] = i; }
      left = 0;
      for (int i = 0; i < nn; i++) if (names[i].active) { if (owner[KEY(i)] == i) names[i].active = -1; else left++; }
      for (int i = 0; i < nn; i++) if (names[i].active < 0) names[i].active = 0;
    }
    if (left) ERR(4);
    if (collision) ERR(5);
    free(owner); free(ft);
  }
  die();

  // ---- names: keywords are the first K tokens; slots in order of last occurrence
  for (int k = 0; k < K; k++) names[tok[k].name].kw = k + 1;
  int *order = malloc(sizeof(int) * (nn + 1)), nslots = 0;
  for (int i = 0; i < nn; i++) order[i] = i;
  qsort(order, nn, sizeof(int), by_last);
  for (int i = 0; i < nn; i++) if (!names[order[i]].kw) names[order[i]].slot = ++nslots;

  // ---- classes
  for (int i = 0; i < nt; i++) {
    Tok *t = &tok[i]; int cls = T_clstab[t->sgi * 5 + t->opc];
    if (t->isname && names[t->name].kw) cls = t->sgi ? C_ERR : names[t->name].kw - 1;
    if (t->first) cls = T_leadof[cls];
    if (i < K) cls = C_NONE;
    if (cls == C_ERR) ERR(6);
    t->cls = cls; t->slot = t->isname ? names[t->name].slot : 0;
  }
  die();
  for (int i = 0; i < nt;) {               // data lines
    int j = i; while (!tok[j].last) j++;
    int isdata = tok[j].cls == C_DATA;
    for (int k = i; k <= j; k++) {
      Tok *t = &tok[k];
      if (isdata && k < j) { if ((t->sgi == 0 && t->lit) || t->cls == C_STR) t->cls = C_NONE; else ERR(7); }
      else if (t->cls == C_STR || (t->cls == C_DATA && k < j)) ERR(7);
    }
    i = j + 1;
  }
  die();
  for (int i = 0; i < nt;) {               // byte lines: a `bytes` lead switches its whole line to byte-lane classes
    int j = i; while (!tok[j].last) j++;
    if (tok[i].cls == C_BYTES) for (int k = i; k <= j; k++) tok[k].cls = T_byteof[tok[k].cls];
    i = j + 1;
  }
  // ---- chunk-local vectors: a ':x' whose name's last occurrence is on the tee's own line
  int *teetok = calloc(nslots + 1, sizeof(int));
  for (int i = 0; i < nt; i++) { tok[i].cls0 = tok[i].cls; if (T_istee[tok[i].cls] || T_isteec[tok[i].cls]) teetok[tok[i].slot] = i + 1; }
  for (int i = 0; i < nt; i++) {
    Tok *t = &tok[i]; int d = t->isname ? teetok[t->slot] - 1 : -1;
    t->local = d >= 0 && T_istee[tok[d].cls0] && tok[names[tok[d].name].last].line == tok[d].line && t->line == tok[d].line;
    if (t->local) t->cls = T_localof[t->cls];
    else if (d >= 0 && d != i && tok[d].line == t->line && t->cls == C_VEC) t->cls = C_VECT;   // re-read of a persistent tee
  }

  // ---- depth, stencils
  int depth = 0;
  for (int i = 0; i < nt; i++) {
    Tok *t = &tok[i]; t->d0 = depth; depth += T_delta[t->cls]; t->code = t->cls < NCODE;
    if (t->code && (t->d0 < 0 || t->d0 > 8 || (depth == 0) != t->last)) ERR(8);
  }
  die();
  for (int i = 0; i < nt; i++) { Tok *t = &tok[i]; t->tid = t->code ? t->cls * ND + t->d0 : 0; if (t->code && !T_slen[t->tid]) ERR(9); }
  die();
  for (int i = 0; i < nt;) {               // per-line checks
    int j = i; while (!tok[j].last) j++;
    long seen = 0;
    for (int k = i; k <= j; k++) if (tok[k].code && T_carry[tok[k].cls]) {
      long bit = 1L << ((T_carry[tok[k].cls] - 1) * 9 + tok[k].d0);
      if (seen & bit) ERR(10); seen |= bit;
    }
    if (tok[j].cls == C_SCATTER) for (int k = i; k <= j; k++) if (T_readsvec[tok[k].cls] && tok[k].slot == tok[j].slot) ERR(11);
    i = j + 1;
  }
  die();
  for (int i = 0; i < nt; i++) if (T_defines[tok[i].cls]) names[tok[i].name].defined = 1;
  for (int i = 0; i < nt; i++) if (T_uses[tok[i].cls] && tok[i].isname && !names[tok[i].name].defined) ERR(12);
  die();
  {                                        // tee rules
    int *nd = calloc(nslots + 1, sizeof(int));
    for (int i = 0; i < nt; i++) if (T_defines[tok[i].cls]) nd[tok[i].slot]++;
    for (int s = 1; s <= nslots; s++) if (teetok[s] && nd[s] != 1) ERR(13);
    for (int i = 0; i < nt; i++) {
      Tok *t = &tok[i]; int d = t->isname ? teetok[t->slot] - 1 : -1;
      if (d >= 0 && d != i && tok[d].line == t->line && !(T_istee[tok[d].cls0] && T_reread[t->cls0] && i > d)) ERR(13);
    }
  }
  die();
  {                                        // a name is a byte vector or a dword vector, never both
    unsigned char *vt = calloc(nslots + 1, 1);
    for (int i = 0; i < nt; i++) if (tok[i].isname && T_vtype[tok[i].cls]) vt[tok[i].slot] = T_vtype[tok[i].cls];
    for (int i = 0; i < nt; i++) if (tok[i].isname && T_vtype[tok[i].cls] && vt[tok[i].slot] != T_vtype[tok[i].cls]) ERR(14);
  }
  die();

  // ---- data image
  int datasize = SLOT * (nslots + 1) + 4 * np, W = SLOT / 4, stublen = sizeof T_stub;
  uint32_t vdata = VBASE + 112 + stublen;
  uint32_t *simg = calloc(W * (nslots + 1), 4); simg[0] = datasize; simg[2] = SLOT * (nslots + 1);
  for (int i = 0; i < nt; i++) {
    Tok *t = &tok[i];
    t->disp = t->lit ? SLOT * (nslots + 1) + 4 * t->pidx : SLOT * t->slot;
    t->dying = t->isname && t->slot > 0 && t->opc == 1 && i >= K && names[t->name].last == i && !t->local;
  }
  // ---- code: per line, prologues, bodies, releases
  unsigned char *code = malloc((size_t)nt * 512 + 16); size_t cl = 0;
#define EMIT(start, len, pos, val) do { memcpy(code + cl, T_sten + (start), (len)); if (pos) { uint32_t v_ = (val); memcpy(code + cl + (pos) - 1, &v_, 4); } cl += (len); } while (0)
  for (int i = 0; i < nt;) {
    int j = i; while (!tok[j].last) j++;
    if (tok[j].cls == C_DATA) { simg[W * tok[j].slot] = vdata + SLOT * (nslots + 1) + 4 * tok[i].pidx; simg[W * tok[j].slot + 2] = tok[j].pidx - tok[i].pidx; }
    for (int k = i; k <= j; k++) if (tok[k].code) EMIT(T_qstart[tok[k].tid], T_qlen[tok[k].tid], T_qpos[tok[k].tid], tok[k].disp + T_qadd[tok[k].tid]);
    for (int k = i; k <= j; k++) if (tok[k].code) EMIT(T_sstart[tok[k].tid], T_slen[tok[k].tid], T_ppos[tok[k].tid], tok[k].disp + T_padd[tok[k].tid]);
    for (int k = i; k <= j; k++) if (tok[k].dying) EMIT(RELS, RELL, RELP, SLOT * tok[k].slot);
    i = j + 1;
  }
  memcpy(code + cl, T_tail, sizeof T_tail); cl += sizeof T_tail;
  uint64_t fsize = 112 + stublen + datasize + cl;
  fwrite(T_ehdr, 1, sizeof T_ehdr, stdout); fwrite(&fsize, 8, 1, stdout); fwrite(&fsize, 8, 1, stdout);
  fwrite(T_stub, 1, stublen, stdout); fwrite(simg, 4, W * (nslots + 1), stdout); fwrite(pool, 4, np, stdout); fwrite(code, 1, cl, stdout);
  return 0;
}
