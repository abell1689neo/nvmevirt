# NVMe Simple Copy (SCC) in NVMeVirt — 측정 결과

## 0. 개요

**디바이스 계층**: NVMeVirt에 **NVMe Simple Copy (opcode 0x19)** 구현 — 호스트가 LBA→LBA **디바이스 내부 복사** 명령을 발행(데이터가 호스트를 안 거침). `/proc/nvmev/copy_stat`에 `copy_submitted`·`copy_host_payload_avoided_bytes` 노출.

**응용 3곳** (FS·블록 두 계층, 시스템·유저 두 구동):

| | 계층 | 구동 | 측정 환경 |
|---|---|---|---|
| **1-1. f2fs GC** | FS | 시스템(백그라운드 GC) | 호스트 (베어메탈) |
| **1-2. f2fs copy_file_range** | FS | 유저 (cp·백업) | VM (config-B 커널) |
| **2. dm-kcopyd** | 블록 | 시스템(클론·스냅샷 CoW) | VM (config-B 커널) |

**공통 측정 원리**: 같은 작업을 **offload(SCC)** vs **baseline(호스트 경유)** 로 A/B. 지표 = 명령 수 · 호스트 트래픽 제거(`host_MB_avoided`) · 호스트 CPU · wall-clock · 무결성.

**에뮬레이터 한계(중요)**: 실제 data memcpy를 nvmev **워커 스레드**가 수행(디바이스 모델링). 따라서 **호스트 CPU 절감 수치는 하한선** — 실 HW면 PCIe DMA·메모리 대역폭까지 추가 절감.

---

## 1. f2fs (파일시스템 계층)

**공통 구현**: SCC를 f2fs에 통합. **run-coalescing** — 연속 valid 블록을 모아 한 copy 명령으로(상한 128블록 = 512KB = 디바이스 max_copy). on/off 토글(`gc_copy_offload`, `cfr_offload`).

### 1-1. GC — 시스템 구동 (백그라운드 가비지 컬렉션)

**환경**
- 호스트(베어메탈), 커널 6.9.0-next-20240516+, f2fs(패치).
- nvmev: SAMSUNG_970PRO 모델(conv_ftl), 4GB(memmap 10G\$4G), `nokaslr`. 디바이스 `/dev/nvme1n1`, f2fs `/mnt`.

**방법**
- victim 단편화: 각 2MB 세그먼트를 `valid_pct%`만 남기고 punch-hole → 완전-invalid 세그먼트 0개(GC가 valid 블록 마이그레이션 강제).
- `f2fs_io gc`로 GC 구동, drain까지 측정. offload vs baseline = `gc_copy_offload` 토글.

**결과**

(a) **디바이스 모델 신뢰성**: sustained WRITE ~2340 MB/s(NAND program 천장) < READ ~3520 MB/s(PCIe 천장) → 실제 970PRO와 일치, write backpressure 정상.

(b) **무결성**: GC 전후 md5 일치, copy_submitted>0.

(c) **고정-일량 GC 비용** (같은 일량, drain까지):
| valid% | mode | gc_time(s) | gc_cpu_sys(s) | reclaimed | copy_submitted | host_MB_avoided |
|---|---|---|---|---|---|---|
| 25 | offload | 1.63 | **0.75** | 774 | 768 | 805 |
| 25 | baseline | 1.48 | 1.02 | 773 | 0 | 0 |
| 50 | offload | 2.48 | **1.26** | 773 | 1536 | 1610 |
| 50 | baseline | 2.18 | 1.56 | 773 | 0 | 0 |
| 75 | offload | 3.52 | **1.78** | 773 | 2304 | 2415 |
| 75 | baseline | 2.65 | 2.17 | 773 | 0 | 0 |

→ reclaimed 동일(공정 비교). **명령 128×↓, 호스트 트래픽 ~2.4GB/패스 제거, 호스트 GC CPU 18~26%↓.** 단 **동기 copy로 wall-clock +10~33%**(baseline은 비동기 writeback으로 I/O 겹침 → async offload로 해결 가능).

(d) **CPU-foreground** (GC 중 동시 CPU 앱을 같은 호스트 코어에서 경쟁 → 앱 throughput):
| valid% | offload kops/s | baseline | 향상 |
|---|---|---|---|
| 25 | **373.4** | 326.0 | **+14.5%** |
| 50 | **353.8** | 294.6 | **+20.1%** |
| 75 | **326.3** | 260.9 | **+25.1%** |

→ (c)의 GC 호스트 CPU 절감이 **동시 앱 속도 14~25%↑** 로 직결(한 인과).

### 1-2. copy_file_range — 유저 구동 (cp·백업)

**환경**
- VM(QEMU/KVM, config-B 커널: nvme/dm 빌트인). nvmev `/dev/nvme0n1`(1GB).
- f2fs **`-o noinline_data`** 마운트 (빈 파일은 inline이라 안 하면 copy_file_range가 inline 체크에 걸림).
- ※ VM 사용 이유: 수정 `f2fs.ko`가 호스트(config-A) 커널과 ABI 불일치 → VM(config-B)에서 안전 측정.

**방법**
- `cfr_test`(static, `copy_file_range()` 호출) — A/B: **offload**(`cfr_offload=1`, copy_file_range→SCC) vs **naive**(read/write 루프). 200MB 파일 복사.
- CPU = 호출 프로세스 user+sys(`getrusage`). 트래픽 독립검증 = `/proc/diskstats`.

**결과** (200MB 복사)
| mode | copy_submitted | host_MB_avoided | dev_read_MB | dev_write_MB | cpu_s | integrity |
|---|---|---|---|---|---|---|
| **offload** | **400** | **419** | **0** | 420¹ | **0.069** | OK |
| naive (r/w) | 0 | 0 | 209 | 210 | 0.290 | OK |

→ **트래픽 419MB 제거(=2×200MB) + 호스트 read 0 vs 209 + 호스트 CPU 4.2×↓**(0.069 vs 0.290s), 무손상.
- **★ copy_file_range는 호출 프로세스 컨텍스트라 호스트 CPU를 `getrusage`로 직접·깨끗이 측정** — naive의 `copy_to/from_user`(200MB×2 호스트 memcpy)를 offload가 통째로 제거.
- ¹ offload dev_write=420은 dst가 **디바이스 내부 SCC + f2fs 메타**로 써진 것(호스트 발행 아님) → 호스트 read·CPU가 0에 가까운 이유.
- 현 커널은 토글-off 시 VFS가 generic copy_file_range로 fallback 안 함 → baseline은 naive r/w.

---

## 2. dm-kcopyd (블록 계층)

**환경**
- VM(config-B 커널). nvmev `/dev/nvme0n1`(파티션 p1=원본/p2=대상·COW/p3=메타).
- ※ VM 사용 이유: 호스트 root가 dm(LVM) 위 → 버그난 dm 모듈이면 호스트 부팅 불가. VM에서 안전.

**구현**: **dm-kcopyd**(device-mapper 공용 copy 엔진 — snapshot/clone/cache가 공유)에 copy offload(~49줄) + 토글 `kcopy_offload`. f2fs(FS)와 **다른 블록 계층** 응용.

**방법**: 두 시나리오 — **dm-clone 하이드레이션**(볼륨 클론) + **dm-snapshot CoW 폭풍**(origin 쓰기 → 청크 첫쓰기마다 p1→p2 CoW). A/B = `kcopy_offload` 토글.

**결과**
| 시나리오 | mode | copy_submitted | host_MB_avoided | wall_s |
|---|---|---|---|---|
| **dm-clone** (384MB 클론) | offload | **768** | **805** | 1.71 |
| | baseline | 0 | 0 | 1.50 |
| **dm-snapshot CoW** (200MB 쓰기) | offload | **3200** | **419** | 1.69 |
| | baseline | 0 | 0 | 1.51 |

→ 두 시나리오 모두 **클론/CoW 복사를 SCC로 → 호스트 트래픽 제거**(805MB, 419MB). 메인라인 어느 dm도 안 한 첫 작동. **wall-clock +12~14%**(동기 copy, f2fs와 동일 한계).

**호스트 CPU는 측정 못 함 (구조적)**: dm-kcopyd는 copy를 **비동기 kthread(workqueue)** 에서 수행 → 호출 프로세스와 분리 → f2fs GC(ioctl 프로세스)·copy_file_range(syscall 프로세스)처럼 **프로세스 CPU로 귀속 불가**. 게다가 dm 호스트 작업은 bio 오케스트레이션뿐(데이터는 DMA) → 절감 자체가 작음. **dm의 깨끗한 지표 = 트래픽·명령 수.** (cpufg식 시도는 §4.)

---

## 3. 종합

> NVMe Simple Copy를 NVMeVirt에 구현, **세 응용**(FS·블록 두 계층, 시스템·유저 두 구동)에 적용:
> - **f2fs GC**(시스템): 명령 128×↓, 트래픽 ~2.4GB/패스 제거, GC CPU 18~26%↓, 동시 앱 14~25%↑. (동기로 wall +10~33%.)
> - **f2fs copy_file_range**(유저): 200MB 복사에 트래픽 419MB 제거 + 호스트 CPU 4.2×↓. **프로세스 컨텍스트라 CPU 이득 깨끗이 실증**.
> - **dm-kcopyd**(블록): 클론 805MB·CoW 419MB 트래픽 제거. (CPU는 비동기 kthread라 측정 구조적 불가 → 트래픽이 지표.)
>
> 협업자의 정적 census를 **성능 실현 지표(명령·CPU·트래픽·앱 throughput)** 로 확장하고 breadth 확보.

## 4. 한계 & Future Work
- **async copy offload**: 동기 대기 제거 → wall-clock도 baseline 이김(현 최대 약점 해소).
- **scatter-gather multi-range**: 흩어진 블록도 1명령(현재는 연속 run만).
- 호스트 CPU·앱 이득은 에뮬에서 **과소평가**(memcpy를 워커가 흡수) → 실 HW에선 더 큼.
- **f2fs copy_file_range**: 덮어쓰기·inline 자동변환·generic(splice) baseline.
- **dm-kcopyd CPU-foreground**: `kcopyd` workqueue를 한 코어에 핀(cpumask) + 동시 CPU 앱으로 cpufg 시도 가능 — 단 dm 호스트 CPU가 작아 신호 약할 전망.

## 5. 재현
```bash
# 1-1. f2fs GC (호스트)
sudo bash tests/scc_gc_bench.sh model    # 디바이스/backpressure
sudo bash tests/scc_gc_bench.sh integ    # 무결성
sudo bash tests/scc_gc_bench.sh cost     # 고정-일량 GC 비용 (시간/CPU/명령/트래픽)
sudo bash tests/scc_gc_bench.sh cpufg    # CPU-foreground (동시 앱 throughput)

# 1-2. f2fs copy_file_range (VM, config-B 커널)
#   crc32_generic.ko → lz4_compress.ko → lz4hc_compress.ko → f2fs.ko 적재,
#   /sys/block/nvme0n1/queue/copy_max_bytes 활성화, mount -o noinline_data 후:
sudo bash cfr_bench_vm.sh 200            # offload(SCC) vs naive(r/w)

# 2. dm-kcopyd (VM)
sudo bash dm_copy_bench.sh               # dm-clone 하이드레이션 + dm-snapshot CoW
```
출력: `scc_cost_*.csv`, `scc_cpufg_*.csv` (호스트).
