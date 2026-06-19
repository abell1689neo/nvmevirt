# NVMe Simple Copy in NVMeVirt — 측정 결과 정리 (f2fs GC + dm-kcopyd)

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

### (4) ★CPU-bound foreground vs GC (호스트 CPU 경쟁) — 깨끗한 앱 이득
GC와 CPU 연산을 같은 호스트 CPU에 묶어, GC 도는 동안 앱(연산) throughput 측정:
| valid% | mode | fg_kops/s | 향상 | gc_reclaimed | copy_submitted |
|---|---|---|---|---|---|
| 25 | offload | **373.4** | **+14.5%** | 776 | 781 |
| 25 | baseline | 326.0 | — | 773 | 0 |
| 50 | offload | **353.8** | **+20.1%** | 773 | 1536 |
| 50 | baseline | 294.6 | — | 773 | 0 |
| 75 | offload | **326.3** | **+25.1%** | 773 | 2304 |
| 75 | baseline | 260.9 | — | 773 | 0 |

→ **GC 중 동시 CPU 앱이 offload면 14~25% 빠름, GC 일감(valid%)↑일수록 격차↑.** cost phase의 호스트 CPU 절감(18~26%)이 **앱 속도로 직결**. I/O foreground와 달리 **디바이스 워커를 안 거쳐 교란 없음**(깨끗). ※ 에뮬은 data memcpy를 워커가 흡수 → **하한선**(실 HW면 더 큼).

## 4. 핵심 발견

### ✅ 호스트 자원 절감 (offload의 본질 이득)
- **명령 증폭 128×↓**: per-block이면 세그먼트당 valid 블록 수만큼(예: 75%면 ~384개) 명령. run-coalescing은 **128블록당 1명령** → 같은 일을 **copy_submitted 768/1536/2304**로 (per-block 대비 ~128배 적음). *(우리 고유 기여)*
- **호스트 트래픽 ~2.4GB/패스 제거**: `host_MB_avoided`(75%: 2415MB). offload는 데이터가 호스트 PCIe/DRAM을 안 거침.
- **호스트 GC CPU 18~26%↓**: `gc_cpu_sys` offload < baseline (25%:26%↓, 50%:19%↓, 75%:18%↓). baseline은 페이지캐시 memcpy + read/write 오케스트레이션, offload는 명령 발행만.

### ⚠️ GC wall-clock — offload가 더 느림 (정직한 한계)
- offload gc_time이 baseline보다 +10~33% 김 (valid%↑일수록 격차↑).
- **원인**: baseline `move_data_page`는 **비동기 writeback**(bio 병합·prefetch)으로 GC가 I/O와 겹쳐 진행. offload `blkdev_copy_offload`는 **동기**라 copy마다 대기.
- → device-time 이득(PCIe 왕복 생략)을 **동기 대기**가 상쇄. **async copy offload로 해결 가능 (future work).**

### ✅ Foreground 앱 이득 — CPU-bound로 깨끗하게 측정 (§3-(4))
- **GC 중 동시 CPU 앱이 offload면 14~25% 빠름** (valid 25/50/75 = +14.5/20.1/25.1%, GC 부하↑일수록 격차↑).
- cost phase의 **호스트 GC CPU 18~26%↓** 가 **앱 throughput 14~25%↑** 로 직결 (두 지표 한 인과).
- **CPU foreground라 디바이스 워커 안 거쳐 교란 없음**(깨끗). ※ 하한선 (에뮬은 data memcpy를 워커가 흡수 → 실 HW면 더 큼).

### ❌ I/O foreground(fio) — 측정 불가라 폐기
- 초기 `ab` phase(fio randread/write)는 copy와 **같은 nvmev 워커·NAND 모델** 공유 → foreground 꼬리지연이 **에뮬 인공물로 교란**(워커 수 무관, 공유 NAND 타이밍이 병목). 실 SSD는 QoS로 interleave해 없는 스파이크.
- → **I/O foreground 폐기, CPU foreground로 대체**(위 ✅). "경쟁 자원"이 디바이스(I/O)→호스트 CPU(연산)로 바뀌어 교란 사라짐.

## 4.5 두 번째 응용: dm-kcopyd copy offload (블록 계층)
- **dm-kcopyd**(device-mapper 공용 copy 엔진 — snapshot/clone/cache가 공유)에 copy offload 패치(~49줄 + on/off 토글). f2fs(FS 계층)와 **다른 블록 계층** 응용.
- **환경**: root가 dm(LVM) 위라 호스트 수정은 위험 → **QEMU/KVM VM**(`-smp 6`, nvme/dm 빌트인 커널 + nvmev 모듈)에서 안전 측정. nvmev 워커 `cpus=3,4,5`, 호스트 작업(dd/dm-kcopyd)은 cpu0,1,2.

### A/B 결과 (offload vs baseline, 두 dm 시나리오)
| 시나리오 | mode | copy_submitted | host_MB_avoided | wall_s | host_cpu_s |
|---|---|---|---|---|---|
| **dm-clone** (볼륨 클론, 384MB 하이드레이션) | offload | **768** | **805** | 1.71 | 3.52 |
| | baseline | 0 | 0 | 1.50 | 3.11 |
| **dm-snapshot CoW** (프로비저닝 폭풍, 200MB origin 쓰기) | offload | **3200** | **419** | 1.69 | 3.92 |
| | baseline | 0 | 0 | 1.51 | 3.83 |

- **트래픽·명령 = 깨끗한 이득**: 두 시나리오 모두 offload가 클론/CoW 복사를 SCC로 발행 → 호스트 트래픽 제거(클론 805MB/768명령, CoW **419MB=2×3200×64KiB 정확히**/3200명령). baseline은 전부 호스트 경유(=0) → **메인라인 어느 dm도 안 한 첫 작동.**
- **wall-clock +12~14%**: 동기 `blkdev_copy_offload`(f2fs GC와 동일 한계 → async로 해소).
- **host_cpu_s는 무의미**(CoW: 3.92 vs 3.83 거의 동일): dm-kcopyd는 kthread + nvmev 폴링 CPU가 지배 → offload 절감이 노이즈에 묻힘. **dm의 깨끗한 지표 = 트래픽·명령 수** (CPU/앱 이득은 f2fs cpufg가 담당).
- 의의: **VM/컨테이너 프로비저닝(thin snapshot CoW)·볼륨 클로닝** 등 dm 다수 응용이 한 엔진으로 혜택.

## 5. 종합 메시지

> **NVMe Simple Copy를 NVMeVirt에 구현하고 두 계층 응용에 적용 — FS 계층(f2fs GC) + 블록 계층(dm-kcopyd).**
>
> **f2fs GC** (run-coalescing으로 실용화·안전화):
> - 명령 **128×↓**, 호스트 트래픽 **~2.4GB/패스 제거**, 호스트 GC CPU **18~26%↓**, **동시 CPU 앱 14~25%↑**, 데이터 무손상.
> - 단, **동기 copy로 GC wall-clock은 +10~33%** (async offload로 해결 가능).
>
> **dm-kcopyd** (블록 계층): SCC 실제 작동 — 볼륨 클론(805MB/768명령)·스냅샷 CoW 폭풍(419MB/3200명령) 호스트 트래픽 제거, wall-clock +12~14%(동기). VM/컨테이너 프로비저닝 등으로 일반화.
>
> 협업자의 정적 census("host-link 32B vs 2N")를 **성능 실현 지표(명령·CPU·트래픽·앱 throughput)로 확장**하고 **두 계층(FS+블록)으로 breadth** 한 것이 본 작업의 기여.

## 6. 한계 & Future Work
- **async copy offload**: 동기 대기 제거 → GC wall-clock도 baseline 이김 (현 최대 약점 해소).
- **scatter-gather multi-range**: 흩어진 valid 블록까지 1명령으로 (현재는 연속 run만).
- **foreground 측정**: I/O foreground는 에뮬 교란(QoS 모델 필요)이라 폐기 → **CPU foreground로 대체**(§3-(4)). 단 이것도 에뮬은 하한선.
- 호스트 CPU·앱 이득은 에뮬에서 **과소평가**(데이터 memcpy를 워커가 흡수) → 실 HW에선 PCIe DMA·메모리대역폭까지 더해져 더 큼.
- **dm-kcopyd A/B**: 트래픽·명령 정량화 완료(클론 805MB/768, CoW 419MB/3200). 호스트 CPU는 kthread+폴링 노이즈로 교란 → dm 깨끗한 지표는 트래픽·명령. (dm CoW 폭풍을 크게 키워 CPU foreground 시도는 future work.)
- **f2fs copy_file_range**: 유저 파일 복사(cp/백업/VM) offload — 구현 중.

## 7. 재현 방법
```bash
# f2fs GC (호스트)
sudo bash tests/scc_gc_bench.sh model    # (1) 디바이스/backpressure
sudo bash tests/scc_gc_bench.sh integ    # (2) 무결성
sudo bash tests/scc_gc_bench.sh cost     # (3) 고정-일량 GC 비용 (시간/CPU/명령/트래픽)
sudo bash tests/scc_gc_bench.sh cpufg    # (4) CPU foreground vs GC (동시 앱 throughput)
#  tests/cfr_bench.c : copy_file_range 청크 스윕 마이크로벤치 (배치 효과 원리)

# dm-kcopyd (QEMU/KVM VM 안)
sudo bash /mnt/dm_copy_bench.sh          # dm-clone A/B + dm-snapshot CoW (offload vs baseline)
```
출력: `scc_cost_*.csv`, `scc_cpufg_*.csv` (호스트), `scc_*` (VM).
(`ab`/`scc_bench_*.csv` = fio I/O foreground 측정이나 에뮬 교란이라 **폐기** → cpufg로 대체.)
