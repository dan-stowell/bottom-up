// gen.c N_MiB > file : pseudo-text (words of lowercase letters, spaces, newlines)
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
int main(int c,char**v){ size_t n=(size_t)atoi(v[1])<<20; static uint8_t b[1<<16]; uint32_t s=12345; for(size_t i=0;i<n;i+=sizeof b){ for(size_t j=0;j<sizeof b;j++){ s=s*1664525+1013904223; uint32_t r=s>>24; b[j]= r<40?32: r<46?10: 97+(r%26);} fwrite(b,1,sizeof b,stdout);} return 0; }
