#!/bin/sh
# run.sh FILE -- whole-process throughput of the three sweet-spot programs: scalar C, gcc -O3, Strand
S=$1; SZ=$(stat -c %s $S); N=${2:-5}
for k in "1 nl" "2 words" "3 upper"; do set -- $k; for b in base$1 basev$1 st_$2; do r=$(taskset -c 3 ./bench $N $S ./$b); w=$(echo "$r" | sed 's/wall min \([0-9]*\).*/\1/'); echo "$2 $b: $(python3 -c "print('%6.2f GB/s' % ($SZ/$w/1e3))")  $(echo "$r" | sed 's/.*median usr/usr/')"; done; done
