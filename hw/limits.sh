#!/usr/bin/env bash
# Measure the machine limits the trace floors use; writes hw/limits.out.txt. Needs an idle machine and sudo.
#   hw/limits.sh [file-list]     file-list: paths whose cold read rate is measured (the build's own sources)
set -e
cd "$(dirname "$0")/.."
out=hw/limits.out.txt
gcc -O2 -march=native -pthread -o build/limits hw/limits.c
build/limits > $out
f=/home/exedev/targets/limits.big
dd if=/dev/urandom of=$f bs=1M count=2048 status=none
w=$( { /usr/bin/time -f %e dd if=$f of=$f.w bs=1M conv=fdatasync status=none; } 2>&1 )
echo "disk_write_bytes_per_s $(echo "2147483648 / $w" | bc)" >> $out
sync; echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
r=$( { /usr/bin/time -f %e dd if=$f of=/dev/null bs=1M status=none; } 2>&1 )
echo "disk_read_cold_bytes_per_s $(echo "2147483648 / $r" | bc)" >> $out
r=$( { /usr/bin/time -f %e dd if=$f of=/dev/null bs=1M status=none; } 2>&1 )
echo "pagecache_read_1t_bytes_per_s $(echo "2147483648 / $r" | bc)" >> $out
rm -f $f $f.w
if [ -n "$1" ]; then
  # many small files, cold, 8 readers: what "read inputs" of a real build looks like
  n=$(wc -l < "$1"); b=$(xargs -a "$1" stat -c %s | paste -sd+ | bc)
  sync; echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null
  s=$(date +%s.%N); xargs -a "$1" -P 8 -n 256 cat > /dev/null; e=$(date +%s.%N)
  echo "smallfiles_cold_8t_bytes_per_s $(echo "$b / ($e - $s)" | bc)" >> $out
  echo "smallfiles_cold_8t_files_per_s $(echo "$n / ($e - $s)" | bc)" >> $out
  s=$(date +%s.%N); xargs -a "$1" -P 8 -n 256 cat > /dev/null; e=$(date +%s.%N)
  echo "smallfiles_warm_8t_bytes_per_s $(echo "$b / ($e - $s)" | bc)" >> $out
  echo "smallfiles_warm_8t_files_per_s $(echo "$n / ($e - $s)" | bc)" >> $out
  echo "smallfiles_count $n" >> $out; echo "smallfiles_bytes $b" >> $out
fi
cat $out
