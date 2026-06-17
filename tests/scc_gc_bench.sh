#!/bin/bash
# scc_gc_bench.sh — NVMe Simple Copy GC offload 재현 측정 스위트
# 실행:  sudo bash scc_gc_bench.sh [all|model|integ|ab]
set -u

#=================== CONFIG (논문용으로 여기만 조정) ===================
DEV=/dev/nvme1n1
MNT=/mnt
FSNAME=nvme1n1
PARAM=/sys/module/f2fs/parameters/gc_copy_offload
CSTAT=/proc/nvmev/copy_stat
GCSYS=/sys/fs/f2fs/$FSNAME

FILL_MB=1536            # GC 대상(victim) 파일 크기
FG_MB=512              # foreground 파일 크기
RUNTIME=15             # case당 fio 측정 시간(초)
QD=16

VALID_PCTS=(25 50 75)              # victim 세그먼트의 valid 비율 (단편화 정도)
WORKLOADS=(randread randwrite)    # foreground 워크로드
COPY_MODES=(1 0)                  # 1=copy offload ON, 0=baseline
#====================================================================

STAMP=$(date +%Y%m%d_%H%M%S); OUT=scc_bench_$STAMP.csv; OUT2=scc_cost_$STAMP.csv
[ "$(id -u)" -eq 0 ] || { echo "root로 실행: sudo bash $0"; exit 1; }
[ -e "$PARAM" ] || { echo "패치 f2fs 미로드 ($PARAM 없음)"; exit 1; }
for t in fio f2fs_io python3 fallocate mkfs.f2fs; do
  command -v "$t" >/dev/null || { echo "$t 필요"; exit 1; }
done
TIMEBIN=$(command -v /usr/bin/time || echo "")   # 없으면 GC CPU는 NA

sval(){ grep -w "$1" "$CSTAT" | awk '{print $2}'; }

# 모든 세그먼트를 valid_pct%만 남기고 구멍 → 완전invalid 세그먼트 0개 (GC가 valid 마이그레이션 강제)
fragment(){
  local vp=$1 seg=2097152 hole sz off
  hole=$(( seg*(100-vp)/100 ))
  rm -f "$MNT/bigfile"
  dd if=/dev/zero of="$MNT/bigfile" bs=1M count=$FILL_MB conv=fsync status=none; sync #통째로 가득 채움
  sz=$(stat -c %s "$MNT/bigfile")
  for ((off=0; off<sz; off+=seg)); do
    fallocate --punch-hole --keep-size --offset=$off --length=$hole "$MNT/bigfile" #2mb segment마다 구멍 뚫기(invalid)
  done
  sync; echo 3 > /proc/sys/vm/drop_caches
}

# fio json -> "iops mean_us p99_us" 
parse(){ python3 -c "import json,sys; j=json.load(open(sys.argv[1]))['jobs'][0]; f=lambda s:(s['iops'], s['clat_ns'].get('mean',0), s['clat_ns'].get('percentile',{}).get('99.000000',0)); ri,rm,r9=f(j['read']); wi,wm,w9=f(j['write']); print('%.0f %.1f %.1f'%(ri+wi, max(rm,wm)/1000.0, max(r9,w9)/1000.0))" "$1"; }

# fio json에서 대역폭(MB/s) 추출
bw_of(){ python3 -c "import json,sys; print(int(json.load(open(sys.argv[1]))['jobs'][0][sys.argv[2]]['bw_bytes'])//1000000)" "$1" "$2"; }

#=================== PHASE 1: 디바이스 모델 / backpressure 검증 ===================
phase_model(){
  echo "### PHASE 1: device model / backpressure 검증 (raw device) ###"
  umount "$MNT" 2>/dev/null
  for rw in write read; do
    fio --name=$rw --filename="$DEV" --rw=$rw --bs=256k --ioengine=libaio \
        --iodepth=$QD --direct=1 --size=3G --runtime=15 --time_based \
        --output-format=json --output=/tmp/m_$rw.json >/dev/null 2>&1
    echo "  $rw BW = $(bw_of /tmp/m_$rw.json $rw) MB/s"
  done
  echo "  판정: WRITE<READ (WRITE~NAND율, READ~PCIe) -> backpressure 작동"
  mkfs.f2fs -f "$DEV" >/dev/null 2>&1; mount -t f2fs "$DEV" "$MNT"
}

#=================== PHASE 2: GC copy 데이터 무결성 ===================
phase_integ(){
  echo "### PHASE 2: GC copy 데이터 무결성 ###"
  mountpoint -q "$MNT" || mount -t f2fs "$DEV" "$MNT"
  echo 1 > "$PARAM"
  dd if=/dev/urandom of="$MNT/bigfile" bs=1M count=$FILL_MB status=none; sync #random한 내용 넣기
  local seg=2097152 sz off; sz=$(stat -c %s "$MNT/bigfile")
  for ((off=0; off<sz; off+=seg)); do
    fallocate --punch-hole --keep-size --offset=$off --length=1048576 "$MNT/bigfile" #단편화 진행
  done
  sync; echo 3 > /proc/sys/vm/drop_caches
  local b a s; b=$(md5sum "$MNT/bigfile"|awk '{print $1}') #GC전 hash값
  echo 0 > "$CSTAT"
  for i in $(seq 300); do f2fs_io gc 1 "$MNT" >/dev/null 2>&1; done #gc 강제
  echo 3 > /proc/sys/vm/drop_caches
  a=$(md5sum "$MNT/bigfile"|awk '{print $1}'); s=$(sval copy_submitted) #GC후 hash값
  if [ "$b" = "$a" ] && [ "${s:-0}" -gt 0 ]; then #비교
    echo "  PASS: copy_submitted=$s, md5 일치 -> copy로 옮긴 데이터 무손상"
  else
    echo "  FAIL: before=$b after=$a copy_submitted=$s"
  fi
}

#=================== PHASE 3: GC offload A/B 스윕 ===================
phase_ab(){
  echo "### PHASE 3: GC copy-offload A/B 스윕 -> $OUT ###"
  mountpoint -q "$MNT" || mount -t f2fs "$DEV" "$MNT"
  echo "workload,valid_pct,copy,fg_iops,fg_mean_us,fg_p99_us,gc_reclaimed,copy_submitted,host_MB_avoided,gc_cpu_sys_s" | tee "$OUT"
  for wl in "${WORKLOADS[@]}"; do
   for vp in "${VALID_PCTS[@]}"; do #단편화 정도
    for cm in "${COPY_MODES[@]}"; do #COPY ON/OFF
      echo $cm > "$PARAM"
      fragment $vp
      #foreground용 파일 생성
      dd if=/dev/zero of="$MNT/fgfile" bs=1M count=$FG_MB conv=fsync status=none; sync
      echo 0 > "$CSTAT"; r0=$(cat "$GCSYS/gc_reclaimed_segments")
      touch /tmp/gcrun
      GCLOOP='while [ -f /tmp/gcrun ]; do f2fs_io gc 1 '"$MNT"' >/dev/null 2>&1; done'
      if [ -n "$TIMEBIN" ]; then
        { "$TIMEBIN" -o /tmp/gct -f "%S" bash -c "$GCLOOP"; } & gp=$!
      else
        echo NA > /tmp/gct; bash -c "$GCLOOP" & gp=$!
      fi
      #foreground: GC간섭 하 앱 IO 측정 
      fio --name=fg --filename="$MNT/fgfile" --rw=$wl --bs=4k --ioengine=libaio \
          --iodepth=$QD --direct=1 --size=${FG_MB}M --runtime=$RUNTIME --time_based \
          --output-format=json --output=/tmp/fg.json >/dev/null 2>&1
      rm -f /tmp/gcrun; wait $gp 2>/dev/null
      read iops mean p99 < <(parse /tmp/fg.json)
      recl=$(( $(cat "$GCSYS/gc_reclaimed_segments") - r0 ))
      av=$(( $(sval copy_host_payload_avoided_bytes)/1000000 )); sub=$(sval copy_submitted)
      echo "$wl,$vp,$cm,$iops,$mean,$p99,$recl,${sub:-0},$av,$(cat /tmp/gct)" | tee -a "$OUT"

    done
   done
  done
  echo "### 완료 -> $OUT (논문 표/그래프용) ###"
}

#=================== PHASE 4: 고정-일량 GC 비용 (foreground 없음) ===================
# 단편화 1회를 "다 회수할 때까지만" GC -> 그 wall-time/CPU 측정 (스핀 없음).
# offload는 PCIe 왕복(데이터 호스트 경유)을 생략하므로 GC가 더 빨리 끝나야 함.
phase_cost(){
  echo "### PHASE 4: 고정-일량 GC 비용 (drain까지 시간/CPU) -> $OUT2 ###"
  mountpoint -q "$MNT" || mount -t f2fs "$DEV" "$MNT"
  echo "valid_pct,copy,gc_time_s,gc_cpu_sys_s,gc_reclaimed,copy_submitted,host_MB_avoided" | tee "$OUT2"
  # 3회 연속 진행 없으면(=단편화 소진) 종료. 무한루프 방지 위해 3000회 상한.
  local DRAIN='p=-1;n=0;i=0;while [ $i -lt 3000 ];do i=$((i+1));f2fs_io gc 1 '"$MNT"' >/dev/null 2>&1;c=$(cat '"$GCSYS"'/gc_reclaimed_segments);if [ "$c" = "$p" ];then n=$((n+1)); [ $n -ge 3 ] && break;else n=0;fi;p=$c;done;true'
  for vp in "${VALID_PCTS[@]}"; do
    for cm in "${COPY_MODES[@]}"; do
      echo $cm > "$PARAM"
      fragment $vp
      echo 0 > "$CSTAT"; local r0; r0=$(cat "$GCSYS/gc_reclaimed_segments")
      local gtime gcpu
      if [ -n "$TIMEBIN" ]; then
        "$TIMEBIN" -o /tmp/gct -f "%e %S" bash -c "$DRAIN"   # %e=wall, %S=system CPU
        read gtime gcpu < /tmp/gct
      else
        local s e; s=$(date +%s.%N); bash -c "$DRAIN"; e=$(date +%s.%N)
        gtime=$(awk "BEGIN{printf \"%.2f\", $e-$s}"); gcpu=NA
      fi
      local recl av sub
      recl=$(( $(cat "$GCSYS/gc_reclaimed_segments") - r0 ))
      av=$(( $(sval copy_host_payload_avoided_bytes)/1000000 )); sub=$(sval copy_submitted)
      echo "$vp,$cm,$gtime,$gcpu,$recl,${sub:-0},$av" | tee -a "$OUT2"
    done
  done
  echo "### 완료 -> $OUT2 ###"
}

case "${1:-all}" in
  model) phase_model;;
  integ) phase_integ;;
  ab)    phase_ab;;
  cost)  phase_cost;;
  all)   phase_model; phase_integ; phase_ab; phase_cost;;
  *) echo "사용법: sudo bash $0 [all|model|integ|ab|cost]";;
esac
