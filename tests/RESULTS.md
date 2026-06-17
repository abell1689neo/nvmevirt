# NVMe Simple Copy — f2fs GC Offload 측정 결과 정리

## 1. 구현 요약 (우리 스택)

| 계층 | 내용 |
|---|---|
| **디바이스** | NVMeVirt에 NVMe Simple Copy(0x19) 지원 (SCC) — 호스트가 발행하는 LBA→LBA 내부 복사 |
| **호스트 I3** | `copy_file_range` → `blkdev_copy_offload` → 0x19 경로 검증 |
| **f2fs GC I4** | `gc.c`에 `gc_copy_offload` 토글 + GC가 valid 블록 이주 시 copy offload 사용 (`move_data_block_range`) |
| **최적화** | **run-coalescing**: 연속 valid 블록을 모아 한 copy 명령으로 (per-block → 배치) |
| **버그 수정** | conv_ftl write-buffer EIO(backpressure), GC self-deadlock(노드 페이지 락) |

## 2. 측정 환경

- 디바이스: NVMeVirt (SAMSUNG_970PRO 모델, conv_ftl), 4GB(memmap 10G$4G), `nokaslr`
- 커널: 6.9.0-next-20240516+ , f2fs(패치)
- nvmev: `cpus=...` (dispatcher + io_worker)
- victim 단편화: 각 2MB 세그먼트를 `valid_pct%`만 남기고 punch-hole → 완전-invalid 세그먼트 0개(GC가 valid 마이그레이션 강제)
- run-coalescing 상한: `GC_COPY_RUN_MAX=128` (128×4KB=512KB = 디바이스 max_copy)

## 3. 지표 결과

### (1) 디바이스 모델 / backpressure 검증 — 플랫폼 신뢰성
| | BW |
|---|---|
| sustained WRITE | ~2340 MB/s (NAND program 천장) |
| sustained READ | ~3520 MB/s (PCIe 천장) |

WRITE < READ 이고 WRITE가 실제 970 PRO sustained write(~2.3GB/s)와 일치 → **현실적 디바이스 + write-cache backpressure 정상 동작.**

### (2) 데이터 무결성 — correctness
- GC 전후 `md5` 일치, `copy_submitted=592` (>0) → **배치 copy로 옮긴 데이터 무손상.**

### (3) 고정-일량 GC 비용 (foreground 없음, drain까지)
| valid% | mode | gc_time(s) | gc_cpu_sys(s) | gc_reclaimed | copy_submitted | host_MB_avoided |
|---|---|---|---|---|---|---|
| 25 | offload | 1.63 | **0.75** | 774 | 768 | 805 |
| 25 | baseline | 1.48 | 1.02 | 773 | 0 | 0 |
| 50 | offload | 2.48 | **1.26** | 773 | 1536 | 1610 |
| 50 | baseline | 2.18 | 1.56 | 773 | 0 | 0 |
| 75 | offload | 3.52 | **1.78** | 773 | 2304 | 2415 |
| 75 | baseline | 2.65 | 2.17 | 773 | 0 | 0 |

`gc_reclaimed`이 ON/OFF 동일(~773) → **같은 GC 일량 = 공정 비교.**

## 4. 핵심 발견

### ✅ 호스트 자원 절감 (offload의 본질 이득)
- **명령 증폭 128×↓**: per-block이면 세그먼트당 valid 블록 수만큼(예: 75%면 ~384개) 명령. run-coalescing은 **128블록당 1명령** → 같은 일을 **copy_submitted 768/1536/2304**로 (per-block 대비 ~128배 적음). *(우리 고유 기여)*
- **호스트 트래픽 ~2.4GB/패스 제거**: `host_MB_avoided`(75%: 2415MB). offload는 데이터가 호스트 PCIe/DRAM을 안 거침.
- **호스트 GC CPU 18~26%↓**: `gc_cpu_sys` offload < baseline (25%:26%↓, 50%:19%↓, 75%:18%↓). baseline은 페이지캐시 memcpy + read/write 오케스트레이션, offload는 명령 발행만.

### ⚠️ GC wall-clock — offload가 더 느림 (정직한 한계)
- offload gc_time이 baseline보다 +10~33% 김 (valid%↑일수록 격차↑).
- **원인**: baseline `move_data_page`는 **비동기 writeback**(bio 병합·prefetch)으로 GC가 I/O와 겹쳐 진행. offload `blkdev_copy_offload`는 **동기**라 copy마다 대기.
- → device-time 이득(PCIe 왕복 생략)을 **동기 대기**가 상쇄. **async copy offload로 해결 가능 (future work).**

### ❌ Foreground I/O 간섭 — 측정 불가 (에뮬 한계)
- `fg_iops`/`fg_p99`는 신뢰 불가 → **폐기.**
- **원인**: NVMeVirt는 배치 copy 명령을 **원자적**으로 처리(QoS 없는 per-LUN FIFO). 큰 copy가 LUN을 통째 점유 → foreground 읽기 꼬리지연 폭발. 워커 스레드 수와 무관(병목이 공유 NAND 타이밍 모델).
- 실제 SSD 컨트롤러는 copy 도중 호스트 I/O를 interleave → 이 스파이크 없음. **에뮬이 못 모델하는 부분.**

## 5. 종합 메시지

> **NVMe Simple Copy를 f2fs GC에 통합. naive per-block은 비실용적(세그먼트당 수백 명령 + self-deadlock). Run-coalescing으로 실용화·안전화:**
> - 명령 **128×↓**, 호스트 트래픽 **~2.4GB/패스 제거**, 호스트 GC CPU **18~26%↓**, 데이터 무손상.
> - 단, **동기 copy로 GC wall-clock은 +10~33%** (async offload로 해결 가능).
>
> 협업자의 정적 census("host-link 32B vs 2N")를 **성능 실현 지표(명령·CPU·트래픽)로 확장**한 것이 본 작업의 기여.

## 6. 한계 & Future Work
- **async copy offload**: 동기 대기 제거 → GC wall-clock도 baseline 이김 (현 최대 약점 해소).
- **scatter-gather multi-range**: 흩어진 valid 블록까지 1명령으로 (현재는 연속 run만).
- **foreground 간섭의 충실한 측정**: 에뮬에 copy-vs-host-I/O interleaving(QoS) 모델 필요.
- 호스트 CPU 절감은 에뮬에서 **과소평가**됨(데이터 memcpy를 워커가 흡수) → 실제 HW에선 PCIe DMA·메모리대역폭 비용까지 더해져 더 큼.

## 7. 재현 방법
```bash
sudo bash tests/scc_gc_bench.sh model    # (1) 디바이스/backpressure
sudo bash tests/scc_gc_bench.sh integ    # (2) 무결성
sudo bash tests/scc_gc_bench.sh cost     # (3) 고정-일량 GC 비용 (시간/CPU/명령/트래픽)
#  tests/cfr_bench.c : copy_file_range 청크 스윕 마이크로벤치 (배치 효과 원리)
```
출력: `scc_cost_*.csv` (cost), `scc_bench_*.csv` (ab, 폐기).
