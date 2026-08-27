# Dual-Granularity FTL Simulator Spec (v1)

## 목표
- IU 기반 coarse 매핑 + 4K fine 티어 FTL의 WAF를 트레이스 구동으로 실측
- baseline(4K page-map, IU+RMW) 대비 비교, fold 정책 ablation의 기반 마련
- 목표 학회: DATE 2027 (abstract 9/13, full 9/20)

## Config 파라미터
- `CAPACITY`: 디바이스 논리 용량 (기본 64GB, 시뮬레이션 규모용 축소값)
- `PAGE_SIZE`: 4KB 고정
- `PAGES_PER_BLOCK` (P): 블록당 페이지 수 (기본 1024 = 4MB 블록)
- `IU_PAGES` (U): IU당 페이지 수 (기본 16 = 64KB; 4 = 16KB도 실험)
- `OP_RATIO`: over-provisioning (기본 7%)
- `FINE_QUOTA`: fine 티어 최대 블록 수 (기본 전체의 10%; 사이징 곡선의 x축)
- `FREE_THRESHOLD` (T): 전역 free 블록 하한 (기본 2)
- `MODE`: `proposed` | `page4k` | `iu_rmw`
- 워크로드: `zipf_alpha`, 요청 크기 분포, `uniform` 플래그, `seed`
- aging: 측정 전 용량 2배 선행 쓰기 (통계 리셋 후 본 측정)

## 상태 (자료구조)
- `coarse_map[IU_id]` → extent 시작 PPN (16칸 연속) 또는 NULL
- `fine_map`: hash< IU_id → { bitmap16, slot_ppn[16] } >
  - d = popcount(bitmap), fold 시 조각 열거·coarse GC stale 판정에 사용
- 블록 메타: { 태그(fine/coarse/free), valid 카운트(fine=페이지 수, coarse=유효 바이트), P2L 역맵(슬롯→IU_id,offset) }
- 공유 free 블록 리스트 (물리 풀은 하나)
- frontier 2개: fine active block / coarse active block (write pointer 각각)

## 풀 정책
- free 리스트는 공유, 블록은 할당 시점에 frontier 태그
- fine 티어 점유 블록 수는 `FINE_QUOTA`로 정적 상한
- fine이 QUOTA 도달 상태에서 새 블록 필요 → fine 회수 발동
- 전역 free ≤ T → coarse GC 발동
- 데드락 방지: 예약 블록 T개 상시 확보

## Write 경로
- 요청 (lba, len)을 3분할: head 조각 | IU 정렬된 64K × k | tail 조각
- 정렬부 → coarse frontier에 16페이지 extent로 append
  - 구 extent invalid 처리
  - 해당 IU의 fine 조각 전부 invalid (bitmap 클리어, fine 블록 valid 감소) ← 누락 주의
- 조각부 → fine frontier에 4K append
  - 이전 fine 사본 있으면 invalid, bitmap 비트 set, slot 갱신
- (v1은 write buffer 없음. coalescing은 v2 옵션)

## 회수 프리미티브 3개
- **FineGC(블록)**: valid 4K들을 fine frontier로 복사 후 erase
  - 비용 = v 쓰기, 순회수 = P−v 슬롯, 슬롯당 비용 = v/(P−v)
- **Fold(IU)**: fine d개 + 구 extent (16−d)개 read → coarse frontier에 16페이지 extent write
  - bitmap 클리어, 구 extent invalid, fine 슬롯들 invalid
  - 주의: coarse 공간 16페이지 소비 / fine 블록을 직접 비우진 않음(invalid만 생성)
- **CoarseGC(블록)**: victim의 살아있는 extent마다
  - dirty(fine 조각 있음) → re-Fold: fine 최신본과 합쳐 이동 (공짜 fold, d≥1이면 무조건 이득)
  - clean → 16페이지 그대로 복사
  - 완전 사망 extent(전부 superseded) → 스킵
  - 완료 후 erase

## 트리거 & victim 정책 (v1)
- fine 압박 시: victim = valid 최소 fine 블록. 두 옵션 비용 비교 후 싼 쪽 실행
  - FineGC: 슬롯당 비용 v/(P−v)
  - 블록 통짜 Fold: 블록 내 valid 페이지들의 서로 다른 IU 수 = k → 비용 16k/P
  - (v2: 블록 내 IU별로 d 큰 것 Fold / d 작은 것 복사 혼합, d-greedy + 핫 IU 보호)
- coarse 압박 시: victim = 유효 바이트 최소 블록
  - 유효 바이트 산정 시 fine에 superseded된 4K는 invalid로 계산
  - (→ re-fold 이득 큰 블록이 자연히 먼저 뽑힘)

## Baseline 모드 (같은 인프라 공유)
- `page4k`: 전 영역 4K page-map + greedy GC (WAF 하한 참조점)
- `iu_rmw`: IU 단위 매핑만 존재, sub-IU 쓰기는 read-modify-write로 16페이지 재작성 (현행 QLC 관행)

## 측정 지표
- WAF = 총 NAND program 페이지 / 호스트 쓰기 페이지
- 쓰기 원인별 분해: host / FineGC / Fold / CoarseGC(clean copy vs re-fold)
- fold 시점 d 히스토그램 (자발적 fold vs GC-시점 re-fold 구분)
- fine 티어 점유율 f 시계열
- 블록 erase 수 (P/E 추정), read amplification (RMW/fold read)

## 실험 계획 (kill test)
- zipf α ∈ {0(균등), 0.8, 0.99, 1.2} × MODE 3종 → WAF 비교
- 검증 포인트: 균등 랜덤에서 proposed ≈ iu_rmw (17 vs 16) 수렴 확인
- skew에서 proposed의 이득이 d 분포로 설명되는지 확인
- 이후: FINE_QUOTA 스윕(사이징 곡선), IU=16K vs 64K, MSR 트레이스

## v2 백로그
- IU 단위 fold 결정 (d-greedy), 핫 IU 보호(빈도 기반 상주)
- write buffer coalescing (WAF-1 직행 경로)
- frontier 통합(mixed) vs 분리 ablation
- MSR Cambridge 트레이스 파서, FINE_QUOTA 동적 조정

---

# 추가 (v1.1): 블록 크기 · 고전 하이브리드 baseline (FAST / LAST)

## 블록 크기
- 실제 고용량 QLC의 (슈퍼)블록은 용량에 비례해 거대함: 4 TiB 디바이스 ≈ 12 GiB. 같은 비율(3/1024)을 적용해
  64 GiB 시뮬레이션에서는 **192 MiB 블록 = 49,152 페이지** (`P=0` 자동; 논리 블록 341개, 총 ~393 블록).
- 이 크기에서 block-level hybrid의 full merge 1회 = 192 MiB 복사 → 고전 방식이 왜 쓸 수 없는지를 정량으로 보임.

## 고전 log-buffer 하이브리드 FTL 공통 구조 (`mode=fast|last`)
- 데이터 블록: **블록 단위 매핑** `dmap[LB] → 물리 블록` (LB = P 페이지 논리 블록)
- 로그 블록: 페이지 단위 매핑 `lmap[lpn] → 로그 내 ppn` (최신본이 로그에 있을 때만)
- 풀: NB = NLB(데이터) + nlog(로그) + T(merge 대상용 예약). 데이터 블록은 merge로만 교체됨 (별도 GC 없음)
- merge 3종: **switch**(로그 블록이 LB 전체를 순서대로 보유 → 매핑만 교체, 복사 0) / **partial**(순서대로 썼지만 미완 → 나머지 복사 후 switch) /
  **full**(새 블록에 LB 전 페이지 최신본 복사(P 쓰기) + 구 데이터블록 erase; 로그 victim의 연관 LB마다 수행)
- 우리 제안과의 대응: fold ≈ full merge인데 단위가 IU(16페이지) vs LB(49,152페이지). 우리는 coarse 티어에도 GC가 있고(extent 단위 복사/re-fold), 고전 방식은 데이터 블록 GC가 merge에 내장됨.

## FAST (Lee et al., TECS 2007)
- 로그 = 순차 로그 블록(SW) 1개 + 랜덤 로그 블록(RW, **완전 연관**) nlog−1개
- 쓰기: 페이지 오프셋 0 → SW 새로 시작(기존 SW는 switch/partial/full로 정리); SW의 다음 기대 오프셋이면 SW; 그 외 RW에 append
- RW 회수: victim = **가장 오래된 블록(FIFO)**, 연관 LB 전부 full merge 후 erase. 이 과정에서 다른 RW 블록의 같은 LB 페이지도 무효화 → 대부분 dead가 되어 공짜 회수
- 관찰: 균등 랜덤에서 1회 회수가 사실상 전체 디바이스 재작성 → WAF ≈ L / 로그용량 ≈ 1/OP, **블록 크기에 둔감**. skew에선 hot LB가 반복 merge되어 악화

## LAST (Lee et al., SIGOPS OSR 2008) — 논문 본문 확인 기준
- 로그 버퍼 = **순차 로그 버퍼**(블록 연관, BAST식, `last_seq_frac`=25%) + **랜덤 로그 버퍼**(완전 연관) → 랜덤은 hot/cold 파티션
- locality detector: 요청 크기 > `last_seq_thresh`면 순차 로그(논문 4 KB@2 KB 페이지; 본 환경 스윕 결과 16 KB(4페이지) 최적 → 기본)
- hot 판정: 이전 사본과의 update interval(랜덤 로그 쓰기 수 기준 페이지 거리) < k (`last_k`, 기본 = hot 파티션 용량). 한 번 hot이면 evict까지 hot
- 순차 로그 회수: 전부 valid·순서대로인 블록 switch, 없으면 LRU → partial(순서대로) / full
- 랜덤 회수 2단계: hot에 dead 블록 있으면 그것(erase만) → 없으면 cold에서 **연관도 N_a 최소(merge cost 최소)** full merge
- 빈 블록 재배정(dead-hot victim → cold, 아니면 → hot)은 균등 워크로드에서 hot이 최대로 드리프트해 더 나빠짐 → `last_fixed_caps=1`(고정, 기본)이 LAST에 유리해 채택. k·파티션 적응(3.4.2의 U_hot/N_d 규칙)과 시간 기반 hot 퇴출은 v1에서 생략
- 관찰: 블록연관 순차 로그가 192 MiB 블록에서 thrashing → WAF 수백~천

## 결과 (64 GiB, 192 MiB 블록, 동일 물리 75.3 GiB, seed 1)
| WAF | zipf 0 | 0.8 | 0.99 | 1.2 |
|---|---|---|---|---|
| proposed | 4.66 | 4.30 | 3.43 | 1.71 |
| page4k | 3.67 | 3.94 | 3.41 | 1.52 |
| iu_rmw | 6.71 | 7.20 | 6.09 | 3.34 |
| FAST | 7.59 | 9.69 | 18.3 | 38.4 |
| LAST | 1489 | 970 | 484 | 118 |

## 정책별 매핑 메모리 (논리 64 GiB, 192 MiB 블록, PPN 4B 기준; P2L 역맵/블록 메타는 공통이라 제외)
| 정책 | 구성 | 크기 | 비고 |
|---|---|---|---|
| page4k | L × 4B = 16.8M × 4B | **64 MiB** | 용량 비례 (1 TB → 1 GiB) |
| iu_rmw | NIU × 4B = 1.05M × 4B | **4 MiB** | |
| proposed | coarse 4 MiB + fine(compact: IU키 4B + bitmap 2B + d×4B) | **≈10.5 MiB** (측정 fine IU 20만·조각 140만), 최악 ≈17 MiB | quota 10% 기준; quota 스윕이 DRAM축 |
| BAST | 블록맵 341×4B + 로그블록별 페이지맵 nlog×P×2B | ≈5.7 MiB | 블록 연관이라 offset 2B |
| FAST | 블록맵 + 로그 페이지맵 nlog×P×4B(=OP 영역 전체를 4K 단위) + 역방향 탐색용 해시(lpn→로그위치, ~8B/엔트리) | **≈11–23 MiB** | 로그 용량(OP 17.7%)에 비례: 0.177×64 MiB |
| LAST | FAST + 로그 페이지별 쓰기시각(hot 판정, 4B) + 블록별 연관도(log2 P bit) | **≈14–34 MiB** | |
→ proposed는 FAST/LAST와 **같은 DRAM급(10~20 MiB)** 이면서 WAF 4.66 vs 7.6(FAST, 균등)/18~38(FAST, skew)/수백(LAST).

## 직접적 선행연구 (반드시 인용·차별화)
- **Janus-FTL** (Kwon et al., EMSOFT 2010): 페이지매핑 영역과 블록매핑 영역 사이의 스펙트럼, 비용 모델로 데이터를 두 영역 간 이동 — "coarse↔fine 선택을 비용으로" 한다는 점에서 가장 가까움. 차이: coarse 단위가 블록(여기선 192 MiB), IU 단위 extent 아님; GC-시점 re-fold 없음; QLC IU 맥락 아님.
- **CFTL** (Park, Debnath, Du, SIGMETRICS 2010 / TOS): 접근 패턴에 따라 블록별로 page↔block 매핑 전환 + 매핑 캐시. 차이 동일.
- **FAST/LAST/BAST/Superblock/KAST**: log-block 하이브리드. merge 단위 = 블록.
- **Subpage-aware SSD** (Kang·Lee·Kim TC'18; ICS'20): 물리 페이지 내 4K 서브페이지 매핑 + 핫니스 기반 병합.
- **μ-FTL** (Lee et al. 2008): 가변 크기 extent 매핑(μ-tree).
- 산업: Solidigm 16K IU, Silicon Motion configurable IU, CSAL(EuroSys'24), 다중 granularity FTL 특허들.

---

# 추가 (v1.2): DFTL / μ-FTL 포팅, 실측 기반 RAM 산정

## RAM 산정 규칙 (모든 모드가 JSON `mem`으로 출력)
- PPN 4B. 페이지 단위 매핑이 필요한 곳(fine 조각, 로그 페이지)은 논문 구현과 무관하게 **해시(open addressing, load factor 0.7)** 로 관리한다고 가정: 엔트리 = (키+값)/0.7.
- **실측 종료 상태**(fine IU 수·조각 수, 로그 페이지 수 등)로 계산. 이론 범위 아님.
- page4k: L×4B. iu_rmw: NIU×4B. proposed: NIU×4B + hash(fine IU, 키4+bitmap2+슬롯오프셋4) + 조각×4B.
  FAST: NLB×4B + hash(로그 페이지, 8B). LAST: + 페이지별 write-seq 4B(hot 판정) + 블록별 연관도 1B.
  DFTL: hash(CMT 엔트리, 8B) + GTD(L/1024×4B). μ-FTL: μ-tree 캐시 + 비트맵 캐시 + 블록별 invalid 카운터 1B.
- DFTL/μ-FTL의 캐시 크기는 **proposed의 실측 RAM(12.2 MB)** 으로 맞춤 (`tools/run_matrix.sh`가 자동).

## DFTL (Gupta et al., ASPLOS'09; CFTL의 쓰기 경로와 동일) — `mode=dftl` (`cftl` alias)
- 쓰기 경로는 순수 페이지 매핑(= page4k). 전체 페이지맵(tier-2, 4K 페이지당 1024 엔트리)은 플래시 상주, RAM에는 CPMT(LRU 캐시)와 tier-1(GTD).
- DFTL식 모델: miss → translation page 1 read; dirty 엔트리 evict → 같은 translation page의 dirty 엔트리 batch flush(1 read + 1 write, `prog.map`).
  GC 이동 페이지는 캐시에 있으면 in-place dirty, 없으면 translation page 단위로 batch write.
- cold 데이터용 블록맵 캐시(CBMT)는 읽기 가속 전용이라 WAF에 무관 → 생략.

## μ-FTL (Lee·Jung·Kang·Kim, EMSOFT'08) — `mode=mu` (논문 §4 기준)
- extent `<l,p,n>` (최대 128페이지, 블록 경계 불가) 매핑을 μ-tree(플래시 상주 B+tree 변종, 경로 전체를 한 페이지에 기록)에 저장, RAM은 고정 크기 캐시.
- 쓰기: 파티션(`mu_parts`=8, 논리공간 균등 분할)별 update block에 append → 구 extent 삭제/분할/절단(Case 0–3) → 새 extent 삽입(논리·물리 연속이면 merge).
- GC: free ≤ 2면 victim = invalid 최다 블록(update block 제외), valid run을 **파티션의 update block으로 복사**(host와 혼합, 논문대로), free ≥ 4까지 반복.
- 무효 비트맵: μ-tree 내 엔트리 + 해시 비트맵 캐시(32 KB)로 버퍼링, 캐시 가득 차면 flush(블록 그룹당 leaf 1페이지 dirty).
- 모델링: 매핑 정확성은 메모리 내 leaf 구조(leaf당 256 extent)로 유지하고, μ-tree I/O는 "touch한 leaf" 기준 LRU 페이지 캐시(miss → read, dirty evict → write)로 계산.

## 결과 (64 GiB, 192 MiB 블록, 동일 물리 75.3 GiB, seed 1)
| WAF | zipf 0 | 0.8 | 0.99 | 1.2 | RAM(실측, MiB) |
|---|---|---|---|---|---|
| proposed | 4.66 | 4.30 | 3.43 | 1.71 | 11.7 |
| page4k | 3.67 | 3.94 | 3.41 | 1.52 | 64.0 |
| DFTL (캐시 11.7 MiB) | 3.86 | 4.06 | 3.47 | 1.53 | 11.7 |
| μ-FTL (캐시 11.7 MiB) | 6.86 | 9.40 | 6.67 | 2.83 | 11.7 |
| iu_rmw | 6.71 | 7.20 | 6.09 | 3.34 | 4.0 |
| FAST | 7.59 | 9.70 | 18.3 | 38.7 | 14.6 |
| LAST | 1489 | 970 | 484 | 118 | 0.2* |
(*LAST는 로그가 거의 비어 있는 상태라 실측 RAM이 작게 나옴)

DFTL의 숨은 비용 — 매핑 miss read (host 페이지당): zipf 0: **0.94 read**, 0.8: 0.58, 0.99: 0.27, 1.2: 0.06. proposed는 0 (매핑 전부 DRAM 상주, 결정적 변환).

## host / GC 쓰기 분리 (hot/cold separation) 적용 현황
- proposed, page4k, iu_rmw, DFTL: host frontier와 GC frontier 분리(`gc_frontier=1` 기본; proposed는 fine·coarse 각각 → active block 4개. 자발적 fold도 coarse GC frontier에 기록).
- FAST/LAST: 구조상 분리(merge 결과는 새 data block, host는 log block).
- μ-FTL: **논문대로 미분리** — GC 복사가 파티션 update block에 host와 함께 기록됨. 의도적으로 유지. 근거(원문):
  - §4.3.1: "Valid pages in the victim block are copied into free pages in the associated update block. If there is not enough free space in the update block, μ-FTL allocates a reserved block as a new update block for the partition and continues to copy valid pages."
  - §4.2.3: "μ-FTL writes requested data D to the update block allocated to the partition corresponding to the logical page number L."
  - §4.4: "An update block is allocated for each partition to prevent data in different partitions from being mixed in the same physical block." (분리 단위 = 논리 파티션, host/GC 아님)
- ablation (proposed, 64 GiB, 분리→합침): zipf 0: 4.76→5.03, 0.99: 3.43→4.33, 1.2: 1.71→2.38.

## fold 정책 실험 기록 (v1.3, 삭제된 시도 포함)
- `fine_policy=block`(기본): victim 블록 통짜로 FineGC vs Fold를 쓰기/순이득 슬롯 비용으로 비교. Fold 이득에 "다른 fine 블록에서 같이 풀리는 조각(extra)" 포함.
- `fine_policy=greedy fold_d=8`: victim 내 IU별로 d≥8이면 fold, 아니면 fine 내 복사. quota ≤4%에서만 이득(2%: 10.9→8.8, 4%: 7.5→6.9 @zipf0), 6% 이상에선 손해(10%: 4.7→5.2).
- 시도 후 삭제: 양 티어 배율을 런타임 추정해 실효 비용으로 비교하는 모델 2종(평균 배율 1+A_tier, 한계 비용 P/(P−victim valid)). 둘 다 "fold 금지"로 수렴해 큰 quota에선 block과 동일, 작은 quota에선 고정 임계 greedy보다 못함 → 평균/한계 배율이 fold의 한계 비용을 과대평가(fold는 old chunk를 동시에 invalid시켜 coarse 순소비가 작음)하는 것으로 추정. 메커니즘적으로 자명한 결과라 폐기.
- 결론: quota 10%에서는 자발적 fold 없이 coarse GC의 re-fold(추가 쓰기 0)에 맡기는 것이 최적. fold는 fine 압박 시 비상 밸브.

## 용어 (v1.4, 확정)
- **IU**: 논리 단위(64 KB = 16 페이지, coarse 매핑 엔트리 1개). **chunk**: IU의 coarse 티어 물리 사본(16페이지 연속). **fragment**: fine 티어의 4K 조각.
- 연산: **fold** = fragment d개 + chunk 나머지 → 새 chunk (fine 압박 시 eviction; `fine_policy=block|greedy`, `fold_d`).
  **refold** = coarse GC가 chunk를 옮기면서 수행하는 fold(추가 쓰기 0). **unfold** = chunk의 남은 (16−d)페이지를 fragment로 옮기고 chunk를 dead 처리 → IU가 fine에 상주(`unfold=hot`, `unfold_tau`).
  **clean copy** = fragment 없는 chunk의 GC 이동. **dead chunk** = 이미 교체된 chunk, GC에서 skip.
- 정책 설명 시 캐시 관점 용어 병기: fine 티어 = hot IU 캐시, unfold = promote(admission), fold = demote(eviction).
- 논문/그림 표기 ↔ JSON 키: **Fine-tier GC** = `finegc`(fine 블록 회수 시 조각 복사), **Merge with fine-tier GC** = `fold`(fine 회수 중 자발적 merge), **Merge with coarse-tier GC** = `cgc_refold`(coarse GC가 chunk를 옮기며 조각 흡수, 추가 쓰기 0), **Coarse-tier GC** = `cgc_clean`(조각 없는 chunk 이동).
- JSON 키: prog.{fold, cgc_refold, unfold, cgc_clean, finegc}, events.{fold, refold, unfold, blockfold, forced_fold}, d_hist_{fold, refold, unfold}.
- merge/split은 쓰지 않음(log-block merge, μ-FTL extent split과 충돌). fold는 SLC folding과 같은 계보임을 related work에서 인정.

## unfold 결과 (64 GiB, coarse GC 시 hot IU만, τ = fine 용량 배수)
| zipf | off | τ=0.25 | 0.5 | 1 | 2 |
|---|---|---|---|---|---|
| 0 | 4.76 | 4.66 | 4.68 | 4.70 | 4.69 |
| 0.8 | 4.39 | 4.25 | 4.28 | 4.29 | 4.34 |
| 0.99 | 3.43 | 3.39 | 3.37 | 3.37 | 3.32 |
| 1.2 | 1.71 | 1.55 | 1.52 | 1.51 | 1.54 |
RAM +4B/fine IU (타임스탬프) → 12.2 MiB. 기회가 "coarse GC victim에 걸린 hot chunk"뿐이라 효과 2–12%.
다음: (1) eviction = coldest-first fold, (2) admission = host 쓰기 시점 unfold, (3) 압박 하 admission 허용(cold fold → hot unfold 교환).

## fine 티어 eviction 정책 (v1.5, 기본값 확정)
`fine_policy=cold`: fine 블록 victim(valid 최소)을 정리할 때
1. **압박 게이트**: victim의 v/P ≥ `fold_gate`(0.6)일 때만 fold 허용. 아니면 전부 fine 내 복사 (여유 있으면 접지 않음 — coarse GC의 refold에 맡김).
2. **coldest-first**: 허용 시 IU별로 `fine_clock − flast ≥ fold_tau(0.5) × fine 용량` 이고 d ≥ `fold_d`(4)이면 fold, 아니면 복사. (hot IU는 접지 않음)
3. 진행 불가(복사로 순공간 0) 시 강제 fold 가드.
RAM: fine IU당 타임스탬프 4B(unfold와 공유).

결과 (64 GiB, WAF; 괄호는 block / greedy d8):
| quota | zipf 0 | 0.99 | 1.2 |
|---|---|---|---|
| 2% | 9.29 (10.94 / 8.79) | 5.42 (5.78 / 5.75) | 2.61 (2.80 / 3.08) |
| 4% | 7.15 (7.50 / 6.94) | 4.50 (4.61 / 5.15) | 2.11 (2.20 / 2.93) |
| 10% | 4.68 (4.76 / 5.24) | 3.43 (3.43 / 4.48) | 1.71 (1.71 / 2.34) |
단일 설정으로 전 구간 최선 대비 ≤6% (유일 갭: 균등+2%에서 greedy가 5% 우위). 왜 둘 다 필요한가: greedy(압박 무시)는 10%에서 +12~31% 손해, cold 필터 없는 fold는 skew에서 hot IU를 접어 손해, 게이트 없는 cold는 10%에서 +5~8% 손해.

---

# v2.0 고정 설정 (2026-08-22 확정)
- **DRAM 예산 = page4k의 10%** (64 GiB에서 6.4 MiB): coarse map 4 MiB + fine map 2.4 MiB → `fine_quota=0.022` = **8블록**(192 MiB) — 균등 워크로드 실측 6.40 MiB. 고정, sensitivity 안 함.
- 동일 물리 공간 비교: proposed = OP 7% + 8블록 → baseline **OP 9.28%** (`op=0.09277`), DFTL/μ-FTL 캐시 = proposed 실측 RAM.
- 정책 기본값: `fine_policy=cold`(게이트 0.6, τ 0.5, d≥4), `unfold=hot`(fine-hot = fine 1바퀴 안에 **요청 단위** 조각 쓰기 ≥2회, τ=1), `gc_frontier=1`.
- aging 기본 8× (`age_x=8`): zipf 1.2가 2×에서 미수렴(전 모드 WAF 10–19% 상승)이었음. 최종 표는 README v2.0 절.
- 용어: **fine-hot / fine-cold** (단순 write-hot이 아니라 "sub-IU 쓰기가 fine 티어 한 바퀴 안에 반복되는가"). 엔트리당 타임스탬프 4B + 요청 카운터 1B (RAM 산정 포함).

## 결과 (64 GiB, 192 MiB 블록, 동일 공간, RAM 10%) — `results/final_ram10`
| WAF | zipf 0 | 0.8 | 0.99 | 1.2 | RAM |
|---|---|---|---|---|---|
| **proposed** | 9.02 | 7.74 | 5.18 | 2.46 | 6.40 MiB |
| page4k | 6.73 | 6.87 | 4.99 | 2.48 | 64 MiB |
| DFTL (캐시 6.4) | 7.12 | 7.09 | 5.09 | 2.51 | 6.46 MiB |
| IU+RMW | 12.30 | 12.55 | 9.13 | 5.37 | 4 MiB |
| μ-FTL (캐시 6.4) | 11.05 | 17.69 | 10.79 | 4.76 | 6.43 MiB |
| FAST | 13.74 | 15.71 | 24.21 | 43.63 | 6.96 MiB |
| LAST | 1521 | 1089 | 561 | 150 | — |
- proposed vs IU+RMW: −27 / −38 / −43 / −54 %. vs DFTL: +27 / +9 / +2 / −2 % (단, DFTL 동기 map read 0.97 / 0.65 / 0.33 / 0.08 per host page, proposed 0).
- unfold/fold 이벤트(측정 구간): zipf 0: unfold 18k, fold 571k; 1.2: unfold 9.9k, fold 25k.

## ablation: fold × unfold (quota 2% 및 10%, cold/hot 판정 포함 여부)
| quota | zipf | fold 없음 | +unfold(k1) | cold-gated fold | cold fold + unfold(k2) |
|---|---|---|---|---|---|
| 2% | 0 | 11.08 | 12.21 | 9.29 | 9.32 |
| 2% | 1.2 | 2.91 | 3.13 | 2.61 | 2.58 |
| 10% | 0 | 5.09 | 5.98 | 4.68 | 4.74 |
| 10% | 1.2 | 1.71 | 1.50 | 1.71 | 1.60 |
fold는 균등(cold 조각 은퇴)에, unfold는 skew(fine-hot 승격)에 필요하며 hot/cold 판정 없이는 반대 워크로드에서 손해. fine-hot 판정은 페이지 단위 카운트면 균등에서 오판(+8~17%) → 요청 단위 k≥2로 수정.

## ablation: fine 티어 eviction 정책 × unfold (v2.0 고정 설정: quota 8블록, 동일 공간)
단순 정책: A `fifo_fold`(가장 오래된 fine 블록 전부 fold), B `greedy_copy`(valid 최소 블록 복사만), C `alt`(A/B 1:1). `no-fold` = fold 금지(강제 가드만).
| zipf | no-fold | A | A+unf | B | B+unf | C | C+unf | cold | cold+unf |
|---|---|---|---|---|---|---|---|---|---|
| 0 | 10.45 | 9.90 | 9.90 | 10.38 | 10.52 | 9.84 | 9.96 | **8.90** | 9.02 |
| 0.8 | 8.80 | 8.77 | 8.77 | 8.84 | 8.82 | 8.50 | 8.46 | **7.72** | 7.74 |
| 0.99 | 5.88 | 6.55 | 6.55 | 5.79 | 5.90 | 6.33 | 6.12 | 5.27 | **5.18** |
| 1.2 | 2.92 | 3.85 | 3.85 | 2.89 | 2.89 | 3.47 | 3.46 | 2.53 | **2.46** |
- 블록 단위 정책(A/B/C)은 어느 것도 양쪽 워크로드를 못 잡음(A는 skew에서 hot을 접어 no-fold보다 나쁨, B는 균등에서 cold 조각을 못 은퇴). IU 단위 fine-hot/cold 판단(cold)이 전 구간 −9~−15%.
- unfold는 이 예산(8블록)에선 ±3%로 중립 — fine 티어가 작아 승격 공간이 없음. 주 기여는 eviction 정책; unfold는 GC 통합 승격 옵션으로 기술.

## v2.1 정책 상수 축소 (확정)
5개 상수(게이트 0.6, τ_fold 0.5, d≥4, τ_unfold 1, k≥2) → **3개**: τ = fine 티어 **반 바퀴**(공유 관측 창), fold = 창 내 조각 쓰기 0회 ∧ **d ≥ 8(= chunk 절반)**, unfold = 창 내 요청 ≥ 2회. 게이트 제거.
one-at-a-time 감도(고정 설정): d 문턱이 핵심(d≥1: +6~12%, d≥8: 균등 −6%), 게이트는 d≥8이면 불필요, τ_fold 0.25~0.5 둔감(1은 균등 +19%).
결과(WAF, zipf 0/0.8/0.99/1.2): 5상수 9.02/7.75/5.16/2.61 → 3상수 **8.36/7.55/5.11/2.65**.


## v2.2 최종 정책: 무상수 per-IU 비용식 (확정)
- fine victim(valid v)의 각 valid 페이지가 속한 IU(조각 d개)에 대해 **fold iff U/d < v/(P−v)** (⇔ d·v > U·(P−v)); 아니면 fine 내 복사.
  v1의 블록 단위 비용식을 IU 단위로 옮긴 것. fine 여유(v 작음)면 큰 d만 접고, 포화(v→P)면 작은 d도 접음 → d 문턱·게이트·τ가 전부 불필요.
- unfold 기본 off (켜면 ≤1.5%; τ/k 상수 필요). 타임스탬프·카운터도 제거돼 RAM 6.06 MiB.
- 결과(WAF): 기본 mix 8.53 / 7.67 / 5.16 / 2.76, small-heavy 13.36 / 7.15 — 고정 d 정책들의 양쪽 최선을 한 식으로 커버.
- 남은 상수는 구조 파라미터(U=16, P, quota←DRAM 예산, T=2)뿐.

## 시도 후 기각: mixed 풀 (`mode=mixed`, 2026-08-23)
한 블록에 chunk와 fragment 공존, 블록 태그 없음, fine 맵은 RAM 예산(2.4 MiB)으로 직접 제한(초과 시 d 최대 IU부터 fold), 단일 greedy GC(live chunk → refold/clean, fragment → U/d < v/(P−v)면 fold 아니면 복사), frontier host/GC 2개.
결과(WAF, 동일 공간·RAM): 기본 mix 8.85 / 8.91 / 6.25 / 3.42 vs tiered 8.53 / 7.67 / 5.16 / 2.76 (+4~24%), small-heavy 0.99: 9.24 vs 7.15 (+29%), small-heavy 균등은 예약 블록 고갈(미수정).
원인: fragment(수명 짧음)와 chunk(cold)가 같은 블록에 섞여 저-valid 블록이 사라짐 → 모든 GC가 ~84% valid 블록에서 fragment까지 복사(clean copy 비중 0.71→0.77). tiered의 이득은 fragment만 모인 fine 블록을 valid 29–71%에서 싸게 비우는 데서 나옴. 예산 eviction은 기본 mix에서 0회 발동(GC 비용식만으로 RAM이 예산 이하).
→ tiered 유지. mixed는 ablation/related-work 반박용으로만 보존.

## Janus-FTL baseline (`mode=janus`, Kwon et al. EMSOFT'10, 본문 §3–4 기준) — 2026-08-23
- BMA(블록맵 데이터 블록) + PMA(페이지맵: OP 블록 + fused 블록). **Fusion**: 논리 블록의 첫 sub-block 갱신 시 그 물리 블록이 그대로 PMA에 편입(invalid 공간을 OP로 재활용, 비용 0). **Defusion**: LRU 논리 블록을 P페이지 복사해 BMA로 복귀. PMA GC = greedy min-valid. 빈 블록 ≥2 유지.
- 정책: 새 active 블록 필요 시 PMA 사용률 > u\*(`janus_u`, 기본 0.7)이면 defusion, 아니면 GC. 논문은 u\*를 비용 모델로 찾음(128 KB 블록 기준).
- DRAM 제약: fused 블록당 P×4B 페이지맵 → 10% 예산이면 fused ≤ 34블록(`janus_cap`, 0=예산, −1=무제한). 초과 시 즉시 LRU defusion.
- 결과(64 GiB, 192 MiB 블록, 동일 공간; zipf 0 / 0.99 / 1.2):
  - cap 34 (RAM 10%): **2,445 / 645 / 140** (요청 대부분이 fusion→즉시 192 MiB defusion)
  - cap 없음 u\*=0.7: 38.7 / 188 / 66.5; u\*=0.9: 58.5 / 48.9 / 13.5
  - u\*=1.0 (defusion 없음 = 순수 page mapping, RAM 64 MiB): 6.97 / 5.97 / 3.99 (page4k 6.74 / 4.98 / 3.02)
- 해석: 거대 블록에선 defusion(블록 단위 demote)이 어떤 빈도로든 손해 → Janus 스펙트럼의 최적점이 page-mapping 끝으로 퇴화. "coarse 단위 = 블록"인 모든 하이브리드의 공통 한계이며, IU 단위 chunk(fold 16페이지)가 필요한 직접 근거. cap 실행은 aging 1× + 측정 0.1×(defusion당 49k 복사로 시뮬 비용 큼), 나머지는 기본 설정.


## v2.3: 디바이스 쓰기 버퍼 (2026-08-23)
- 순차 4K/16K 쓰기에서 proposed WAF 1.99(조각 16개가 차면 chunk_drop 후 결국 fold 16 쓰기), IU+RMW 17.0/6.0, page4k 1.0 — 구조적 약점 확인.
- 해결: 모든 정책 공통의 IU 단위 coalescing 쓰기 버퍼(`wbuf`, 기본 1 MiB = 256페이지). IU별로 페이지를 모아 16이 차면 정렬 IU 쓰기, 버퍼 초과 시 LRU IU를 조각 run으로 내림. 버퍼 내 덮어쓰기는 흡수.
- 결과: 순차 4K 전 정책 1.00; 랜덤 mix ±3%(1.2에서 전 정책 WAF 소폭 상승 — 버퍼가 최상위 hot 덮어쓰기를 흡수해 FTL 도달 트래픽의 skew가 감소). canonical 표는 README v2.3.
- 후속 아이디어(미구현): **switch fold** — IU 16 조각이 fine 블록에 물리적으로 연속·순서대로 있으면 복사 없이 매핑만 chunk로 전환(FAST switch merge 유사). 버퍼 도입 후 기회는 다중 순차 스트림이 버퍼를 넘칠 때로 제한 → 기회 측정 후 구현 여부 결정.

## 시도 후 기각: RAM-cap fine 티어 (`fine_cap=ram`, 2026-08-23)
블록 태그는 유지하되 fine 블록 수 제한을 없애고 fine 맵 RAM 예산(2.4 MiB)만 제한, 공간 압박 시 두 티어를 합쳐 valid 최소 블록을 victim으로.
결과: zipf 0.8/0.99/1.2에서 +23/+26/+21% 손해, fine 블록이 2개(frontier)로 수렴, 예산 fold 0회, 균등은 예약 블록 고갈.
원인: 갓 찬 fine 블록은 조각이 빨리 죽어 항상 valid 최소 → 전역 greedy GC가 즉시 회수 → 조각이 덮어써질 체류 시간을 못 받음. **블록 quota는 RAM 대리변수일 뿐 아니라 fine 블록을 greedy GC로부터 보호하는 최소 체류 시간 장치**. 이를 GC에 맡기려면 age-aware cost-benefit GC(추가 정책/상수)가 필요 → 고정 quota 유지.

## 그림 생성 (v2.4, 2026-08-24)
`tools/plot.py` (matplotlib 3.6.3, STIXGeneral = Times 호환 serif, 본문 폰트 19–20pt). 그림당 파일 하나, PDF + PNG 동시 출력.
```bash
python3 tools/plot.py [outdir]   # 기본 results/fig
```
- matplotlib은 시스템에 없어 `~/tools/pyenv`(Debian 패키지 추출본)에 두고, 스크립트가 PYTHONPATH/LD_LIBRARY_PATH/MATPLOTLIBRC를 세팅해 자기 자신을 re-exec.
- 그림 목록: fig5/fig5b는 정책별 쓰기 분해를 **동일한 구성 요소**로 그림(Host / RMW / Coarse-tier GC / Merge with coarse-tier GC / Merge with fine-tier GC / Fine-tier GC / Map write). page4k·DFTL은 전 영역을 4 K로 매핑하므로 그들의 GC(`pgc`)는 **Fine-tier GC**로 집계. fig5b는 y축을 세 패널이 공유(비교 가능). 크기 분포 3종(default / small-heavy / large-heavy) × 정책 × zipf(0, 0.99). default 패널만 1 MiB 버퍼 설정(canonical), 나머지 두 패널은 버퍼 없음. fig4b = 크기 분포별(small/large-heavy × zipf 0/0.99) proposed 쓰기 분해. fig1 WAF vs zipf(6정책, 로그; LAST는 스케일 압축 때문에 그림에서 제외, 표에는 유지), fig1b 동일 데이터 주요 4정책(선형, headline), fig2 요청 크기 분포별, fig3 매핑 DRAM, fig4 proposed 쓰기 분해, fig5 정책별 쓰기 분해(host/RMW/GC/map), fig6 순차 4K(버퍼 유무), fig7 DFTL 동기 map read.

## 선행연구 조사 및 비교표 (fig8, v2.5)
Proposed가 전부 만족하는 6개 feature로 분해. 각 행은 최소 하나씩 X.

| feature | 정의 (왜 필요한가) |
|---|---|
| **Sub-block coarse unit** | coarse 매핑 엔트리가 erase block보다 작다(IU 64 KB = 16페이지) → merge 1회 비용이 블록 크기가 아니라 16페이지. 192 MiB 슈퍼블록에서 블록 단위 hybrid가 붕괴하는 원인 |
| **Coexisting 4 KB map** | **같은 논리 범위**가 4 KB와 coarse 단위로 **동시에** 매핑되고, 4 K 사본이 coarse 단위의 일부를 supersede. (모드 전환·영역 분리·매체 티어링은 해당 안 됨) |
| **No special media/host** | page re-programming(NOP≥2), SLC 티어, 호스트 힌트/인터페이스 변경 없이 QLC에 그대로 적용 |
| **Cost-based GC vs. merge** | "4 K 영역을 그대로 회수" vs "coarse로 merge"를 쓰기 비용으로 결정(고정 규칙·핫니스 분류가 아님) |
| **Merge in coarse GC** | coarse 영역에 자체 GC가 있고, 그 재배치에 **티어 간 merge**를 얹어 추가 program 0으로 처리(refold). coarse GC가 없으면(로그블록 하이브리드) 또는 merge가 별도 연산이면 X |
| **In-DRAM map** | 매핑 전체가 DRAM 상주 → I/O 경로에 동기 map read 없음 |

| 연구 | sub | dual | plain | cost | gc-merge | dram | 근거 / 주된 차이 |
|---|---|---|---|---|---|---|---|
| BAST(2002) / FAST(TECS'07) / LAST(OSR'08) | X | O | O | X | X | O | 로그 블록 = 페이지 매핑, 데이터 블록 = 블록 매핑. 회수는 merge뿐(로그 내 GC 없음), 데이터 블록 GC 없음 |
| Superblock FTL (EMSOFT'06, TECS'10) | X | O | O | X | X | O | coarse 단위가 superblock(블록 여러 개 = 더 큼), superblock 내부만 페이지 매핑 |
| Janus-FTL (EMSOFT'10) | X | O | O | **O** | X | O | PMA GC vs defusion을 사용률 u* 임계로 선택(가장 가까운 선행). coarse = 블록, BMA에 GC 없음, 결정이 전역·블록 단위 |
| CFTL (SIGMETRICS'10) | X | X | O | X | X | X | 블록별 page↔block **모드 전환**(동시 공존 아님). 블록맵은 연속 PPN 읽기 가속용이고 데이터는 항상 페이지맵이 정본. tier-2는 플래시 상주 |
| DFTL (ASPLOS'09) / LeaFTL / LearnedFTL | X | X | O | X | X | X | 단일 4 K granularity + 캐시/학습으로 DRAM 절감 → 동기 map read 발생 |
| μ-FTL (EMSOFT'08) | O | X | O | X | X | X | **단일** 가변-granularity 맵(extent 1–128페이지)이라 티어 공존 아님. extent 병합은 논리·물리 연속일 때 기회적으로만(비용 판단 없음), μ-tree 플래시 상주 |
| Subpage-aware SSD (TC'18, ICS'20) / Minipage-FTL (NAS'18) | O | O | O | X | X | O | 큰 물리 페이지 내 4 K 서브페이지 매핑, 병합 기준은 갱신 빈도(핫니스) 클러스터링 |
| MGA-FTL (**DATE'17**) | O | O | X | X | X | O | 2-level(page 16 K + subpage 4 K) 매핑으로 DRAM 제어. **page re-programming(NOP) 필요** → MLC/TLC/QLC는 LLH 등 별도 기법. subpage merging 기준은 조각 수·**read count**(읽기 성능)로 쓰기 비용이 아님 |
| SLC cache, 디바이스 내부 (ComboFTL'10, warm-block'18) | X | X | X | X | X | O | **매체** 티어링: 두 티어 모두 4 K 매핑이고 coarse 맵이 없음(DRAM 절감 목적 아님), SLC 영역 필요 |
| CSAL (EuroSys'24, 호스트측) | X | X | X | X | X | X | 아티팩트 기준 검증: **4 KB 블록/매핑**, L2P DRAM 상한 **2048 MB**(전체 테이블보다 작은 캐시), OP 18%, 빠른 드라이브를 FIFO 로그 버퍼로 쓰고 compaction 때 유효 로그를 QLC에 큰 순차 쓰기로 이동, L2P는 한 시점에 한 위치만 지시. **미검증**: band/chunk 크기, 2-level L2P의 구조, hot/cold 분리 granularity (EuroSys 원문 접근 불가) |
| Mixed-IU + FDP (Silicon Motion OCP'24; US 10409500, US 10222984) | O | X | X | X | X | O | 4 K IU 영역과 16 K IU 영역을 **정적·배타적**으로 분리(한 LBA는 한쪽만) → 동시 공존 아님. 호스트가 배치 결정, granularity 간 이동 없음 |
| IU + RMW (현행 QLC) | O | X | O | X | X | O | coarse 매핑만, sub-IU 쓰기는 RMW |
| **Proposed** | **O** | **O** | **O** | **O** | **O** | **O** | |

주의: μ-FTL의 extent 병합은 삽입 시 논리·물리 연속일 때만 일어나는 기회적 연산이라 cost/gc-merge = X. Subpage-aware·Minipage·MGA-FTL은 *물리* 페이지 크기(16 K) 문제이고 우리는 *매핑* granularity(IU) 문제 — 동기는 다르지만 구조가 가까워 반드시 인용. 표 수정은 `tools/plot.py`의 `FEATS`/`RW`. fig8b는 4행 압축판(FAST/Janus/μ-FTL/Proposed, 5열).

## MGA-FTL (DATE'17) 원문 상세 — 가장 가까운 선행이므로 별도 정리
Feng et al., "Mapping Granularity Adaptive FTL Based on Flash Page Re-programming", DATE 2017 (HUST).
- **구조**: 1st = PMT(물리 페이지 단위 8/16 KB), 2nd = SPMT(4 KB 서브페이지). SPMT는 페이지보다 작은 요청이 올 때만 할당 → DRAM 절감.
- **서브페이지 쓰기 방법**: 같은 물리 페이지의 **빈 서브페이지에 부분 프로그램**하고 나머지는 0xFF 마스크로 채움(page_padding). 즉 한 물리 페이지를 여러 번 프로그램.
  → **NOP(Number Of Partial programs) ≥ 2 필요.** 데이터시트상 SLC 4–8, **MLC는 1**. 논문도 MLC/TLC는 LLH 기법이 필요하다고 명시하고, 검증은 Samsung **SLC** 칩으로 수행. QLC(NOP=1 + 다단계 프로그램 순서 제약)에는 사실상 적용 불가. **매체 티어링(SLC+QLC)이 아니라 단일 매체의 부분 프로그램 능력 의존.**
- **할당 정책**: Exclusive/Shared Allocation을 free page 비율 임계 AT(0.25–0.35)로 전환.
- **merge 트리거**: 논리 페이지가 흩어진 물리 페이지 수(PPN Count) 상한을 **Read Count**로 계단식 조절(0→4, ≤2→3, ≤5→2, >5→1). 목적은 **읽기 성능/조각 감소**이고 쓰기 비용 모델이 아니며, 논문도 "merging은 필수 연산이 아니다"라고 기술. GC는 일반 페이지 단위(PV 상태 추가).
- **결과**: 16 KB 페이지에서 WAF −30.8%, erase −40%, 평균 응답시간 −53~72%(Synth-A). DRAM은 page-level 대비 **+16.5%**, 순수 subpage 매핑 대비 1/3. baseline = page-level(PM), sector-log hybrid. 규모는 블록 128페이지.
- **우리와의 차이**: coarse 단위가 물리 페이지 vs **IU(매핑 단위, 물리 페이지와 독립)**; fine 배치가 같은 페이지 부분 프로그램 vs **별도 fine 블록 log-append**; 매체 요구 NOP≥2 vs **없음**; merge 기준 read count vs **쓰기 비용식**; GC 통합 없음 vs **refold(추가 쓰기 0)**; DRAM이 결과 보고(+16.5%) vs **예산을 입력으로 고정(page4k의 10%)**; 블록 128페이지 vs **49,152페이지(192 MiB)**.
- 절대 DRAM 비교(1 TB): MGA-FTL의 coarse는 16 KB → 64 M 엔트리 × 4 B = 256 MB(+16.5%). 우리 coarse는 64 KB IU → 16 M × 4 B = 64 MB + fine ≈ 40 MB ≈ **104 MB**.

### 물리 페이지가 커져도 부분 프로그램이 필요 없는 이유 (discussion용)
문제의 층위가 다르다: MGA-FTL 계열은 **프로그램 단위(물리 페이지)** 가 커진 것이 원인이라 조각을 같은 물리 페이지에 나중에 끼워 넣어야 하고(→ NOP 필요), 우리는 **매핑 단위(IU)** 가 커진 것이 원인이라 조각을 아무 빈 페이지에나 정상 프로그램하면 된다.
16 KB 물리 페이지 + 64 KB IU가 동시에 오는 경우에도, fine frontier는 append-only이므로 **서로 다른 IU의 조각 4개를 모아 16 KB 페이지 하나를 꽉 채워** 프로그램하면 되고 부분 프로그램이 필요 없다(조각별 매핑이라 한 물리 페이지에 여러 IU의 조각이 섞여도 무관). 반면 coarse 티어는 chunk가 물리적으로 연속이어야 매핑 엔트리 1개를 유지하므로 fold가 16페이지를 함께 쓴다 — 이것이 fold 비용 16의 근거.

## fig5c: FAST / μ-FTL 쓰기 분해
- **Full merge**(FAST/LAST): victim 로그 블록의 유효 페이지가 속한 **논리 블록마다** 빈 블록을 잡아 그 논리 블록의 전 페이지(P=49,152) 최신본을 모아 쓰고 구 데이터 블록을 erase. 로그 블록 하나 회수에 연관 논리 블록 수만큼 반복.
- **Partial merge**: 로그 블록이 오프셋 0부터 **순서대로** 쓰였지만 다 안 찬 경우, 데이터 블록의 **남은 (P−written) 페이지를 복사**해 채운 뒤 switch. 거대 블록에서는 "남은 페이지"가 수만 개라 전혀 싸지 않음 → skew가 커져 순차 로그 블록이 일찍 쫓겨날수록 폭증(FAST α=1.2에서 36.9, small-heavy α=0.99에서 **56.4**).
- **Switch merge**: 로그 블록이 P페이지를 순서대로 다 채운 경우 매핑만 교체(복사 0). 우리 실험에서는 거의 발생하지 않음.
- μ-FTL은 merge 개념이 없고 전부 **GC relocation**(`pgc`) + μ-tree **map write**. small-heavy에서 extent 수가 폭증해 map write가 3.2–8.0까지 커짐(기본 mix에서는 0.03–0.72).
- LAST는 full merge 197–2203이라 그림에서 제외.

## v2.6: 쓰기 시점 선택적 RMW (eager merge, `eager_u`)
부분 IU 호스트 쓰기(예: 48 KB = 12페이지)를 조각으로 흘리는 대신, **기존 fine 조각과 합쳐 그 자리에서 64 KB chunk 하나로 완성**하는 선택지. 즉 "쌀 때만 하는 RMW"이고, 그 RMW가 동시에 merge 역할을 해서 fine 엔트리를 비운다.
- **비용 모델(v2, 기본 `eager_u=-1`, 상수 없음)** — 호스트 페이지당으로 비교:
  - eager: chunk 16페이지를 n개의 호스트 페이지를 위해 씀 → **U/n**
  - fine: 조각 1페이지 + 나중에 fold될 확률 p × (fold 16페이지 ÷ 조각 u개) → **1 + p·U/u**
  - p(조각이 실제로 fold로 은퇴하는 비율)는 런타임 측정: `frag_folded / (frag_folded + frag_freed)`. 조각 대부분은 coarse GC의 refold나 호스트 덮어쓰기로 **공짜로** 사라지므로 p ≈ 0.15–0.23으로 낮게 나오고, 그 결과 문턱이 n ≳ 9로 자동 형성된다.
- **기각한 v1 모델**: "eager 추가 비용 (U−n) vs fine 슬롯 점유 비용 u·v/(P−v)". fine 티어가 포화되면 v/(P−v)가 커져 **모든 4 K 쓰기를 RMW로 돌려** small-heavy에서 WAF 13.4 → **45.9**(IU+RMW 34.3보다 나쁨). 교훈: 순간 압박 신호는 쓰기 시점 결정에 부적합하고, fine 티어의 출구 비용(fold 16/d, 그나마 대부분 공짜)을 반영해야 한다.
- 결과(WAF, off → cost-v2): 기본 mix 8.53→**7.96**(z0), 7.65→7.56(0.8), 5.40→**5.37**(0.99), 3.14→3.15(1.2); small-heavy 13.36→**12.81**(z0), 7.15→7.49(0.99, +5% 손해); large-heavy 7.59→7.47. 고정 문턱(u>10 또는 n>8)은 z0에서 7.91–7.92로 살짝 더 좋지만 skew에서 +3~11% 손해.
- 채워 넣는 (U−n) 페이지는 `prog.rmw`로 계상되므로 쓰기 분해 그림에 RMW 성분으로 나타난다(호스트 페이지당 0.01–0.05).

## fig9: 2×2 ablation (쓰기 시점 선택적 RMW × fine 회수 fold)
`results/ablation`. neither = `eager_u=0 fold_d=99 fine_policy=greedy`, nofold = 쓰기시점만, nowrite = fold만, full = 기본 설정.
| 워크로드 | neither | write-time only | fold only | both | both vs neither |
|---|---|---|---|---|---|
| default α=0 | 10.41 | 8.80 | 8.53 | **7.96** | −23.6% |
| default α=0.8 | 8.84 | 8.03 | 7.65 | **7.56** | −14.5% |
| default α=0.99 | 6.06 | 5.76 | 5.40 | **5.37** | −11.4% |
| default α=1.2 | 3.35 | 3.31 | **3.14** | 3.15 | −5.9% |
| small-heavy α=0 | 18.94 | 16.76 | 13.39 | **12.81** | −32.4% |
| small-heavy α=0.99 | 9.61 | 8.85 | 7.63 | **7.49** | −22.0% |
| large-heavy α=0 | 8.24 | 7.73 | 7.58 | **7.47** | −9.4% |
- 두 기법은 **상보적**이며 겹치지 않는다: 둘 다 켜면 항상 각각보다 좋거나 같다(α=1.2만 +0.3% 예외).
- fold 쪽이 기여가 크고(특히 조각이 많은 workload), 쓰기 시점 규칙은 그 위에 −1~7%를 더한다.
- 둘 다 끄면(= 조각을 fine에 넣고 coarse GC의 refold로만 정리) WAF가 6~32% 나빠진다 → refold만으로는 부족하다는 근거.

## 그림 색 인코딩 (v2.7)
쓰기 분해 그림에서 **색 = 어느 티어의 작업인가, 해칭(`///`) = merge 여부**로 이중 인코딩한다.
- Host `#2a78d6`(파랑), RMW `#4a3aa7`(보라), **coarse 티어 `#eb6834`(주황)** — Coarse-tier GC(단색) / Merge with coarse-tier GC(해칭), **fine 티어 `#1baf7a`(청록)** — Fine-tier GC(단색) / Merge with fine-tier GC(해칭), Map write `#8a5a00`(갈색).
- fig5c(legacy)도 동일 규칙: Full merge(주황 단색) / Partial merge(주황 해칭) / GC relocation(청록) / Map write(갈색).
- 이전에는 7개 색을 다 다르게 썼는데 주황과 노랑, 두 보라가 구분이 안 됐다. 색 5개로 줄이고 dataviz 검증 스크립트(`validate_palette.py`)로 통과 확인: lightness band / chroma floor / CVD 분리 / normal-vision floor 전부 PASS(청록의 대비 2.74로 WARN 하나 — 막대 위 값 라벨과 표가 보조 인코딩 역할).

## v2.7 — WAF 분모 정의 변경 (총 host write 기준)

**변경 전:** `WAF = 총 NAND program / FTL에 도달한 host 페이지`. 1 MiB IU-coalescing 쓰기 버퍼가 덮어쓰기로 흡수한 페이지는 분모에서 빠져 있었다.

**문제:** 버퍼가 흡수하는 쓰기는 (만약 내려갔다면) WAF≈1로 처리됐을 쓰기다. 이걸 분모에서만 제거하면 남은 쓰기들의 평균 증폭이 올라가서, **버퍼를 키울수록 NAND 총 쓰기는 줄어드는데 WAF는 커지는** 역설이 생긴다. proposed z=0.99 실측: 버퍼 1 MiB에서 흡수 5.3%, NAND 총 쓰기 86.5 M → 79.4 M(-8%)인데 구 지표 WAF는 5.15 → 5.93으로 상승.

**변경 후:** `WAF = 총 NAND program / 워크로드가 발행한 총 host 페이지`(버퍼 흡수분 포함). aging의 순차 채우기도 동일하게 버퍼를 통과시켜 분모에 들어간다.

구현 (`ftlsim/ftlsim.cpp`):
- `Stats::host_submitted` 추가 — `Runner::wb_write()` 진입 시 무조건 누적. `host_pages`는 기존대로 FTL 도달분.
- `seq_fill()`도 `wb_write()`를 경유(이전에는 `host_write()` 직접 호출).
- 세 곳의 `report()`가 함께 출력: `host_pages`(=발행분, 분모), `host_pages_ftl`, `buffer_absorbed = 1 − ftl/발행`, `WAF`(신규), `WAF_ftl`(구 정의, 회귀 비교용).
- `wbuf=0`이면 두 값이 동일하므로 버퍼 없는 결과는 영향 없음.

**영향:** 모든 정책에 동일하게 적용되므로 순서·상대 비교는 불변. 흡수율이 큰 skew 구간에서 절대값이 내려간다(proposed z=1.2: 흡수 18.4%, WAF 3.15 → 2.57). `results/final_ram10_wb`, `final_sizes`, `seq`, `ablation` 전부 이 정의로 재생성했고 fig1~fig9도 재생성했다(plot.py는 `host_pages`로 나누므로 코드 변경 불필요).

## v2.8 — IU 크기 민감도와 등DRAM 비교 (`results/iu_sens`)

**설정.** `U ∈ {4,6,8,12,16,24,32}` 페이지 = 16K~128K IU. U는 P(=49152)의 약수여야 하며, fine 비트맵이 `u32`라 U ≤ 32이다(U=48은 `U > 32 unsupported (bitmap)`로 종료). proposed와 IU+RMW를 동일 물리 공간에서, canonical과 같은 조건(64 GiB, 192 MiB 블록, aging 8×, 1 MiB 버퍼)으로 비교했다.

**왜 같은 IU 비교만으로는 부족한가.** proposed의 매핑 RAM = coarse 맵(용량/IU × 4 B) + fine 해시 인덱스 + fine 슬롯이다. 같은 IU에서 proposed는 IU+RMW보다 항상 RAM을 더 쓴다(64K: 5.9 vs 4.0 MiB). 따라서 표의 같은 행 비교는 iso-DRAM이 아니다. 공정한 비교는 **DRAM 축을 맞추는 것**이고, IU+RMW의 RAM은 IU로만 결정되므로 U를 촘촘히(6, 12, 24 포함) 돌려 곡선을 채운 뒤 proposed의 실측 RAM 지점에서 log–log 보간해 읽었다.

**등DRAM 결과(α=0.99).** proposed 16K/32K/64K/128K = −3 / −19 / −29 / −36 % (small-heavy: −28 / −43 / −54 / −61 %). 같은 IU 기준(−5 / −25 / −41 / −56 %)보다 작지만 추세는 동일하며, **DRAM이 적을수록(IU가 클수록) 이득이 커진다** — IU를 키워 매핑 DRAM을 줄이려는 실제 동기와 같은 방향이다. 16K 부근에서는 두 정책이 수렴한다: 4 KB~16 KB 사이엔 부분 쓰기가 거의 없어 dual granularity가 흡수할 여지가 없다.

**fine 티어 크기 스윕(전체 공간 고정).** `fine_extra=0 op=0.09278`로 NB=373을 고정한 채 fine 블록을 4/8/16으로 바꿨다. fine 맵의 DRAM은 fine 티어의 *물리 크기*에 묶여 있으므로(블록당 슬롯 P×4 B = 192 KB), 4K 맵 DRAM을 키우는 유일한 방법은 OP를 fine 쪽으로 옮기는 것이다. 전체 OP가 32블록(373−341)뿐이라:
- 32K IU: 4.97 → **4.79** → 5.89 (fine 4/8/16). 16블록에서는 coarse OP가 반토막 나 WAF가 악화.
- 64K IU: 5.55 → 5.08 → **5.05**. coarse 맵이 작아 여유가 있어 거의 평평.

즉 proposed에 "IU+RMW 16K와 같은 16 MiB"를 4K 맵으로 주는 구성은 동일 공간에서 성립하지 않으며, 부분적으로 시도해도 손해다. 이것이 fine 티어를 **고정 크기(≈2% 공간, 슬롯 RAM ≤1 MiB)**로 두는 설계 근거를 한 번 더 확인해 준다(RAM-CAP·mixed-pool 기각 사유와 같은 맥락).

**그림.** `fig10_iu_sensitivity` — (a) IU 크기별 WAF, (b) 매핑 DRAM 대 WAF의 등DRAM Pareto(점 라벨=IU, 점선=page4k), (c) 전체 공간 고정 하의 fine 티어 크기 스윕.

### v2.8 보론 — "coarse 티어가 모든 데이터의 원본을 가질 필요가 있나?" (`results/unfold_big`)

**먼저 사실 확인: 중복 저장은 없다.** `fine_write()`에서 새 fragment가 생기면 해당 IU 청크의 그 페이지를 즉시 무효화하고(`bvalid[blk_of(coarse_map[iu])]--`), U개가 모두 fragment가 되면 `coarse_map[iu] = NONE`으로 청크를 통째로 버린다(`extent_drop`). `verify()`도 LPN당 정확히 물리 페이지 1개만 valid로 센다. 따라서 "coarse에 원본, fine에 사본"이 아니라 **페이지 단위로 소유권이 이동**한다.

**남아 있던 제약**은 하나다: 호스트가 건드리지 않은 (U−d)페이지는 청크에 남는다 → 그 IU는 청크를 계속 붙들고 있다. 이걸 푸는 것이 `unfold`(coarse GC 중 나머지 (U−d)페이지를 fine으로 옮기고 청크를 버림)이며, 이미 구현돼 있으나 튜닝 상수(`unfold_tau`, `unfold_k`) 때문에 기본 off다. 쓰기 비용만 보면 unfold는 refold보다 항상 싸다(U−d < U). 대신 fine 엔트리 (U−d)개와 fine 공간을 더 쓴다.

**실험.** 전체 공간 373블록 고정(`fine_extra=0 op=0.09278`), fine 블록 8/16/32/48/64, `unfold=hot unfold_tau=1e9 unfold_k=1`(= 공간 있으면 항상 unfold), α=0.99.

| IU | fine 블록 | unfold off (WAF / 활용률 / RAM) | unfold always |
|---|---|---|---|
| 32K | 8 | 4.789 / 64.9% / 10.05 MiB | 4.788 / 64.0% / 10.43 MiB |
| 32K | 16 | 5.891 / 34.9% / 10.00 MiB | **4.926** / 63.2% / 11.88 MiB |
| 64K | 8 | 5.083 / 69.5% / 5.87 MiB | 5.067 / 70.8% / 6.22 MiB |
| 64K | 16 | 5.053 / 65.4% / 7.02 MiB | 5.030 / 64.5% / 7.38 MiB |
| — | 32~64 | `GC makes no progress` | 동일 |

**해석.**
1. fine을 키울 때 나빠지는 원인은 중복이 아니라 **활용률**이다. fine 티어는 고정 예약인데 채워지는 양은 fragment 워킹셋이 정한다. 32K IU에 16블록을 주면 활용률이 34.9%로 떨어지고 그만큼 coarse OP가 사라진다.
2. unfold는 그 문제를 실제로 고친다(34.9% → 63.2%, 5.89 → 4.93). 제약을 푸는 방향 자체는 옳다.
3. 그러나 **fine 8블록(4.79)을 넘지 못한다.** fine 티어도 자기 OP가 필요한 page-mapped 로그 구조라, 192 MiB 블록 + greedy GC로 이미 효율적인 coarse 티어에서 공간을 빼앗아 오는 것이 순이득이 아니다. DRAM만 1.8 MiB 더 든다.
4. 32블록 이상은 unfold를 켜도 죽는다: unfold는 *이미 fragment가 있는* IU만 옮길 수 있으므로 fine이 흡수 가능한 총량은 워크로드가 정한다. 예약만 키우면 coarse가 굶는다(종료 상태 free=2, fine=28/32, coarse=343).

**결론.** 4K 맵 DRAM을 늘려 WAF를 사는 경로는 이 구조에 존재하지 않는다. fine 티어의 유효 크기는 fragment 워킹셋이 정하고, 그 이상은 노는 공간이다. 이는 고정 크기 fine 티어(≈2% 공간, 슬롯 RAM ≤1 MiB) 설계의 근거이며 RAM-CAP·mixed-pool 기각과 같은 결론이다. 남은 미탐색 방향은 *fragment가 없는 IU까지* 통째로 fine(=page-mapped)으로 승격시키는 것인데, 이는 사실상 hybrid page/coarse 파티션(Janus-FTL 계열)이 되고 DRAM이 제한된 상황에서는 page4k로 수렴한다.

### v2.8 보론 2 — coarse 풀이 전체 LBA를 소유할 필요를 실제로 제거 (`unfold=any`, `results/unfold_big/*_any.json`)

**구현.** 이전 보론의 unfold는 *fragment가 이미 있는* IU만 fine으로 보낼 수 있어 coarse 풀은 여전히 사실상 전체 LBA의 잔여 페이지를 소유해야 했다(그래서 fine 쿼터 32블록 이상에서 over-commit으로 사망). 이번에 그 제약을 완전히 제거했다:
- coarse GC가 만난 **fragment 없는(clean) IU도** `fine_get_or_create` 후 통째로 unfold 가능 (`unfold=any`). GC 시점 이동이므로 쓰기 비용은 refold와 동일한 U페이지 — 이동 자체는 공짜다.
- `can_unfold`의 예약 검사 `free > T`를 `free >= T`로 완화. GC는 정확히 free==T에서 돌므로 기존 조건으로는 carved 쿼터에서 unfold가 한 번도 발동하지 못했다(victim erase가 블록 하나를 돌려주므로 안전).

이로써 fine 풀이 IU를 온전히 소유할 수 있다(측정: 32K IU, 쿼터 32에서 fine 상주 156K IU ≈ 4.8 GiB). 중복 저장은 원래 없었고, 이제 잔여-페이지 소유 제약도 없다.

**결과(64 GiB, z=0.99, 전체 공간 373블록 고정, 같은 쿼터의 plain(off)과 비교).**

| IU | fine 블록 | off | unfold=hot | unfold=any |
|---|---|---|---|---|
| 128K | 8 | **5.751** | 5.888 | 6.547 |
| 128K | 16 | **5.492** | 5.685 | 5.758 |
| 128K | 24 | **5.274** | 5.615 | 5.922 |
| 128K | 32 | 사망 | 사망 | 6.109 |
| 64K | 16 | **5.053** | 5.030 | 5.211 |
| 64K | 24 | 7.222(u35%) | **5.248** | 5.482 |
| 64K | 32 | 사망 | 사망 | 5.827 |
| 32K | 16 | 5.891(u35%) | **4.926** | 5.199 |
| 32K | 24/32 | 사망 | 사망 | 5.544 / 5.974 |
| 모든 IU | 48~64 | 사망 | 사망 | **여전히 사망** |

**해석 — 제약이 병목이 아니었다.**
1. **모든 지점에서 plain(off)이나 hot보다 나쁘다.** WAF는 fine 풀 크기에 대해 단조 악화된다.
2. **GC 주도 축출은 cold 데이터를 옮긴다.** coarse GC victim에 남은 clean IU는 정의상 오래 안 쓰인 데이터다. cold 데이터는 coarse 티어가 가장 잘 저장하는 대상(IU당 맵 엔트리 1개, refold는 GC에 공짜로 편승)이고, page-mapped 풀의 장점은 hot 부분 덮어쓰기에만 있는데 그것은 이미 fragment 경로가 잡고 있다.
3. **핑퐁 비용.** 옮겨진 cold IU는 fine 회수의 cost-based fold에 가장 먼저 걸려(d=U → U/d=1) 도로 접힌다. 128K/쿼터32에서 unfold 트래픽이 host 쓰기의 18.6%, fold 횟수 2.8×.
4. **평형 활용률 ~65%.** fine 풀도 자기 OP가 필요해서 쿼터의 65%까지만 차고, 그 이상 밀어넣으면 fold-back이 상쇄한다. 그래서 쿼터 48+에서는 unfold=any로도 coarse가 굶는다(잔여 coarse OP 4%).
5. **진짜 헤드룸은 fragment 워킹셋이 정한다.** 128K IU는 워킹셋이 커서 plain으로도 쿼터 8→24에서 5.75→5.27로 좋아진다 — 제약 해제가 아니라 쿼터 사이징으로 이미 얻는 이득이다.

**결론.** "coarse가 모든 데이터를 소유해야 한다"는 제약은 이제 구현상 존재하지 않지만, 그것을 활용해 fine 풀을 fragment 워킹셋 너머로 키우는 것은 어느 IU 크기에서도 순손실이다. 병목은 소유권 제약이 아니라 (a) 축출 가능한 데이터가 cold라는 사실과 (b) fine 풀의 자체 OP 요구다. 기본값은 `unfold=off` 유지, `unfold=any`는 음성 결과로 보존한다.

### v2.8 보론 3 — 과잉 fine 쿼터에서의 liveness (사망 제거)

**문제.** fine 쿼터를 공간이 감당 못 할 만큼 크게 주면(예: 128K IU, 총 OP 32.6블록에서 쿼터 32+) coarse 티어가 over-commit되어 `GC makes no progress`로 죽었다. 쿼터는 *상한*일 뿐인데, fine 풀이 일단 자라고 나면(성장은 그 시점의 valid 기준으로 허용됨) fragment가 나중에 무효화되며 coarse OP가 0으로 수렴해도 아무도 fine 공간을 돌려주지 않았다.

**해결(4중 escape, 전부 liveness 가드 — 정책 아님).**
1. **성장 가드 + 최소 OP 바닥**: fine 블록 할당 시 `(NB − fine − 1 − T)·P ≥ resident + max(P, resident/32)`를 요구(resident = L − fine_valid). coarse OP가 상주 데이터의 ~3% 아래로 내려가는 성장은 쿼터와 무관하게 거부하고 먼저 fine을 회수한다. `/32`는 GC가 풀블록 복사로 퇴화하는 지점을 피하는 liveness 상수(기존 4/64와 같은 부류).
2. **dead 블록 즉시 회수**: 할당 stall 시 valid=0인 fine 블록(현재 회수 중인 victim 제외)을 복사 0으로 erase. 큰 쿼터에선 쿼터 압박이 없어 dead 블록이 방치되던 것을 수거.
3. **강제 fold 백스톱**: victim이 완전 valid이거나 stall > 56이면 fine victim 블록을 강제 fold해 coarse로 돌려준다. `!in_fine_reclaim`/`!in_shrink` 가드로 재진입 금지(재진입 시 같은 블록 이중 회수로 `greedy_reclaim left valid=16` 크래시가 났었다).
4. **예약 인출**: fine 회수가 진행 중일 때 그 회수의 coarse 쓰기는 free > T를 기다리지 않고 예약분에서 꺼낸다(회수 완료가 블록을 돌려줌).

이 과정에서 겪은 함정: (a) 백스톱을 stall 1회부터 쏘면 정상적 net-0 GC 라운드에도 개입해 대량 fold가 쓰기시점 p 통계를 오염(p→1) → "전부 RMW, fine 미사용" 흡수 상태(WAF 5.79 고착). (b) 문턱만 높이면(56) 진짜 over-commit에서 매번 56회의 풀블록 헛복사 후에야 개입해 WAF 46.7. (c) 공간 부등식·full-valid victim 같은 판별식은 경계(slack 1~2블록, victim 99.x%)에서 발화하지 않았다. 결국 **성장 시점에 바닥을 두는 것**(1번)이 유일하게 안정적이었고, 이후 백스톱은 사실상 잠잔다(shrink 0~3회).

**결과(64 GiB, z=0.99, 총 공간 373블록 고정, 최종 바이너리로 전 행 통일).**

| IU | 쿼터(블록) | WAF | fine 평형 | 활용률 | 가드 발동 |
|---|---|---|---|---|---|
| 32K | 8 / 16 | 4.789 / 5.891 | 8 / 16 | 65 / 35% | 0 |
| 32K | 24~64 | 9.565 (동일) | 22 | 15% | capped 75 |
| 64K | 8 / 16 / 24 | 5.083 / 5.053 / 7.222 | 8 / 16 / 24 | 70 / 65 / 35% | 0 |
| 64K | 32~64 | 8.768 (동일) | 26 | 24% | capped 157 |
| 128K | 8 / 16 / 24 | 5.751 / 5.492 / 5.274 | 8 / 16 / 24 | 73 / 67 / 64% | 0 |
| 128K | 32~64 | 8.477 (동일) | 31 | 39% | capped 282 |

- **사망 0.** 과잉 쿼터는 가드가 결정하는 평형 크기로 수렴하고 그 이상 쿼터는 무의미(행 동일).
- **회귀 0.** canonical(z=0.99) WAF 5.0833 비트 동일, 가드·백스톱 발동 0회. 가드가 안 발동한 모든 행은 이전 값과 일치.
- 과잉 쿼터의 열화(8.5~9.6)는 "공간을 그렇게 나누면 원래 나쁘다"의 정직한 반영이다 — fine 워킹셋 이상의 쿼터는 활용률만 깎는다(보론 1·2와 같은 결론). 죽는 대신 측정 가능해졌다는 것이 이 변경의 목적.

## v2.9 — "쿼터를 키우면 좋아져야 한다": hot/cold 분리형 개선 정책 (`fine_full` + `cost_cold` + `gc_select=cost`)

**출발점(사용자 지적).** 4K 매핑 공간(fine 쿼터)을 늘리면 극한에서 page4k가 되므로 WAF는 page4k(동일 공간 4.761)로 단조 수렴해야 하는데, 실측은 역행했다(64K: 5.05 → 7.2 → 8.8). 근본 제약이 아니라 최적화 실패다.

**진단(3중 유출).** 쓰기 소스 분해로 확인한 병목은 fine이 아니라 **coarse GC clean copy의 폭발**(3.28 → 6.89/host)이었다:
1. **hot 데이터가 fine에 못 머문다.** fine 상주 IU도 full-IU 덮어쓰기가 오면 무조건 coarse에 재청크되며 fragment가 전멸한다. 유입은 partial write뿐, 유출은 상시 → 풀을 키워도 채울 트래픽이 없다(활용률 35%).
2. **회수가 hot부터 접는다.** cost 규칙(U/d vs v/(P−v))에서 d=U인 상주 IU가 최저 비용 victim이라 hot이 먼저 coarse로 쫓겨난다.
3. **fine이 슬랙을 반납하지 않는다.** fine 회수는 쿼터 도달 시에만 돌므로, 활용률 낮은 fine 풀의 invalid 공간이 통째로 잠긴 채 coarse OP만 4%로 고갈된다. (p-통계 쌍안정 가설은 eager_u=0 대조 실험으로 기각.)

**수정(각각 플래그, 기본 off — 기존 동작 보존).**
- `fine_full=1`: fine 상주 IU에 대한 호스트 쓰기는 full-IU든 partial이든 (쿼터 여유 시) fine에 유지. d=U가 되면 청크 자동 drop → IU가 온전히 page-mapped.
- `fine_policy=cost_cold`: 회수 시 fold는 "비용상 유리 **그리고** cold"일 때만 → hot 상주 IU 보호.
- `gc_select=cost`: free 블록이 필요할 때 coarse victim과 fine victim의 해방-블록당 복사비 v/(P−v)를 비교해 싼 쪽을 회수(동일 화폐, 상수 0, Janus u*의 동적 판). 활용률 낮은 fine 블록이 쿼터 미달이어도 자연히 반납된다.

**결과(64 GiB, 총 공간 373블록 동일, 32K IU + 쿼터 16 = "32K-gs").**

| 워크로드 | 32K-gs (RAM MB) | page4k (64 MB) | 기존 canonical 64K (6.2 MB) |
|---|---|---|---|
| z=0 | 7.918 (10.2) | **6.546** | 7.955 |
| z=0.8 | **6.398** (10.4) | 6.697 | 7.524 |
| z=0.99 | **4.506** (11.1) | 4.761 | 5.083 |
| z=1.2 | **1.935** (10.9) | 2.632 | 2.572 |
| small z0.99 | 5.076 (13.6) | **4.738** | 7.494 |
| large z0.99 | **4.152** (10.7) | 4.793 | 5.208 |

- **skew가 있으면 page4k를 이긴다**(z0.8 −4.5%, z0.99 −5.4%, z1.2 −26%, large −13%) — DRAM은 page4k의 ~1/6(10–11 MB ≈ 페이지맵의 16%). 쿼터도 이제 제대로 동작한다: 32K에서 8→16이 4.687→4.506으로 개선, 24는 4.742로 완만히 꺾임(붕괴 없음).
- **이기는 메커니즘 = 구조적 hot/cold 분리.** hot IU는 fine 풀에 page-mapped로 살고 cold는 coarse 청크로 산다. page4k는 hot/cold가 같은 블록에 섞여 GC가 cold valid를 반복 복사하지만, 이 구조는 풀 자체가 분리라서 coarse victim이 거의 안 나온다(z1.2에서 격차 최대인 이유).
- 한계: 균등(z=0)에서는 분리할 hot이 없어 page4k에 진다(7.92 vs 6.55). 기존 canonical(7.955)과는 비슷.
- 64K-gs도 z0.8/1.2에서 canonical을 이기지만(7.33/2.47) 폭이 작다 — 32K IU가 이 정책 세트의 최적 동작점.

**미결.** canonical을 이 정책으로 교체할지(그러면 전체 스윕·ablation·그림 재생성 필요), 32K를 새 기본 IU로 삼을지는 결정 대기. 균등 워크로드 열세를 줄이는 방향(잔존 skew 추정으로 fine_full 게이팅 등)도 후보.

## v2.9.1 — SepBIT 베이스라인 (FAST'22, `mode=sepbit`, `results/sepbit`)

**왜.** v2.9의 page4k 역전은 "구조적 hot/cold 분리" 효과다. 그렇다면 최신 분리 기법을 얹은 page-mapped FTL이 정당한 대조군이다 — 리뷰어가 반드시 요구할 비교. SepBIT(Wang et al., FAST'22)를 공개 레퍼런스(github fallfish/sepbit)에서 그대로 포팅했다.

**포팅 내용(레퍼런스 충실).**
- 6클래스 배치: user write는 직전 수명(now−마지막 user write 시각) < ℓ이면 class 0, 아니면 1. GC copy는 victim 세그먼트가 class 0이면 2, 아니면 페이지 나이로 <4ℓ→3, <16ℓ→4, ≥16ℓ→5. 클래스당 open 세그먼트 1개(총 6 frontier).
- 시간 = 누적 user write 페이지 수(GC append는 tick 없음), 타임스탬프는 user write에만 갱신 — 레퍼런스의 Metadata/globalTimestamp semantics 그대로.
- ℓ = class-0 세그먼트가 GC로 수거될 때의 나이를 16개마다 평균(초기 ∞).
- **victim 선택 = cost-benefit** `(P−v)/v × √age` 최대 — 레퍼런스 기본값(Config: selection="CostBenefit"). greedy로 하면 SepBIT이 page4k보다 나빠진다(8G z0.99: greedy 5.50 vs CB 4.66) — 포팅 시 주의점.
- 예약 T=8: GC가 클래스 2~5 네 frontier로 흩어 쓰므로 T=2로는 free 고갈.
- RAM = page4k와 동일한 전체 4K 페이지 맵(64 MiB) + 세그먼트당 메타(무시 가능). 즉 **DRAM 풍부 조건의 SOTA 분리 베이스라인**.

**결과(64 GiB, 동일 물리 공간 op=9.278%, 1 MiB 버퍼).**

| 워크로드 | SepBIT (128 MB) | page4k (64 MB) | 32K-gs (11 MB) |
|---|---|---|---|
| z=0 | 8.684 | **6.546** | 7.918 |
| z=0.8 | 6.918 | 6.697 | **6.398** |
| z=0.99 | **4.266** | 4.761 | 4.506 |
| z=1.2 | **1.795** | 2.632 | 1.935 |
| small z0.99 | **4.215** | 4.738 | 5.076 |
| large z0.99 | 4.275 | 4.793 | **4.152** |

**해석 — 주장의 재구성.**
1. SepBIT은 고skew·small-heavy에서 32K-gs보다 좋다(z0.99 −5%, z1.2 −7%, small −17%). 단 **64 MB 페이지 맵 전체가 전제**다.
2. 32K-gs는 z0.8과 large-heavy에서 SepBIT을 이기고(−7.5%, −2.9%), 나머지에서도 ±7% 이내다.
3. 따라서 논문 주장은 "page-mapped를 이긴다"가 아니라: **"SOTA 분리 기법(SepBIT)의 ±7% 성능을 매핑 DRAM 1/6로 달성하며, 중간 skew·대형 I/O에서는 오히려 이긴다"**. Pareto로 보면 (64 MB, 4.27)과 (11 MB, 4.51)은 비지배 관계고, page4k(64 MB, 4.76)는 우리에게 양축 모두 지배당한다.
4. SepBIT도 균등에서는 page4k보다 나쁘다(8.68 vs 6.55) — 분리 기법 공통의 한계로, 우리 균등 열세(7.92)와 같은 성격이며 오히려 우리가 덜 나쁘다.
5. 향후: SepBIT의 나이-클래스 아이디어를 우리 fine 풀/coarse GC frontier에 접목하면 고skew 격차(−5~−7%)를 닫을 여지가 있다.

## v2.9.2 — DOGI 베이스라인 (FAST'26, ML 포함 아티팩트 포팅, `third_party/dogi_port`, `results/dogi`)

**출처와 포팅.** DOGI(Kim et al., FAST'26)는 SepBIT 이후의 SOTA 배치 기법으로, 공식 아티팩트(github dgist-datalab/DOGI, SepBIT 프로토타입 기반)를 `third_party/dogi_port`로 가져와 ZNS 장치 없이 돌게 최소 패치했다:
- storage adapter를 no-op **NullAdapter**로(세그먼트 메타데이터는 전부 인메모리라 WAF에 페이로드 불필요), ZenFS 어댑터 제거, `zbd reset` 제거.
- 크기 파라미터를 env로: `DOGI_LOGICAL_BYTES`(64 GiB), `DOGI_OP`(0.09278 → GpThreshold=OP/(1+OP)=0.0849), 세그먼트 = **49152블록 = 192 MiB**(ftlsim과 동일). 물리 세그먼트 373개 = ftlsim의 NB=373과 정확히 일치.
- 측정 구간 경계에서 raw 카운터를 찍는 `DOGI_MARK_PAGES`(버퍼 드레인 후 기록).
- 빌드: zig c++, cblas는 naive sgemm 스텁(추론 수학은 동일), zconf.h 스텁, sstream include 추가.
- **ML 파이프라인 온전 가동**: 학습 트리거(2×장치 쓰기) → 수집 → `../venv/bin/python3 model_trainer.py`(PyTorch CPU, ~/venv에 설치) → MLP 리로드 → Markov-chain 그룹 최적화(K, BIR 동적 적용) → APPLY_ML=1. 워크로드당 2~4회 재학습 확인.

**입력.** ftlsim에 `out_trace=` 내보내기 추가: **post-buffer** 호스트 스트림(1 MiB IU-coalescing 버퍼 통과 후)을 4K 페이지당 한 줄("0 1 lpn 4096")로 덤프 — DOGI의 시간 단위(4K당 1틱)와 일치, 우리 FTL이 본 것과 같은 트래픽. WAF 분모는 우리 규약(측정 구간 제출 페이지, export JSON의 host_pages)로 환산.

**결과(64 GiB, 동일 물리 공간 OP 9.278%, 192 MiB 세그먼트).**

| 워크로드 | DOGI (64 MB+) | SepBIT (128 MB) | page4k (64 MB) | 32K-gs (11 MB) | canonical (6.2 MB) |
|---|---|---|---|---|---|
| z=0 | 7.809* | 8.684 | **6.546** | 7.918 | 7.955 |
| z=0.8 | **5.896** | 6.918 | 6.697 | 6.398 | 7.524 |
| z=0.99 | **3.779** | 4.266 | 4.761 | 4.506 | 5.083 |
| z=1.2 | **1.445** | 1.795 | 2.632 | 1.935 | 2.572 |
| small z0.99 | **4.001** | 4.215 | 4.738 | 5.076 | 7.494 |
| large z0.99 | **3.635** | 4.275 | 4.793 | 4.152 | 5.208 |

*z=0에서는 학습 데이터 수집이 끝나지 않아 ML 미적용(휴리스틱만) — 균등에서는 분리 기법 모두 page4k에 열세.

**해석.**
1. **DOGI는 실제 SOTA다**: skew 전 구간에서 SepBIT 대비 −5~−20%, 우리 32K-gs 대비 −8~−25%.
2. 단 DOGI의 DRAM은 page 맵 64 MB에 **더해** per-4K 메타데이터가 크다: HotIntervalTracker 1 B/4K(16 MB) + FreqFeatures freqBits 1 B/4K(16 MB) + chunkFreq + FIFO/FrozenFilter + MLP/학습 버퍼 ≈ **총 ~100 MB급**. 32K-gs(11 MB)의 9배가 넘는다.
3. 우리 주장(등DRAM Pareto)은 유지된다: (100 MB, 3.78) vs (11 MB, 4.51)은 비지배 관계. 다만 "SOTA의 ±7%"는 SepBIT 기준이었고 DOGI 기준으로는 **+16~34% 뒤진다** — 논문에서는 "1/9 DRAM으로 DOGI 대비 +16~34%, page4k(동일 64 MB급) 대비는 우세" 구도로 서술해야 정직하다.
4. DOGI의 이득 원천(무효화 시점 예측 + 동적 그룹)은 우리 구조와 직교한다 — fine 풀 배치/coarse GC frontier에 DOGI식 나이-그룹·예측을 접목하는 것이 자연스러운 후속 최적화.

## v2.10 — coarse 티어의 cost-benefit victim + 나이 캐스케이드 (`cgc_vict=cb`, `coarse_class=N`)

**동기.** DOGI와의 잔여 격차의 본체는 coarse GC clean copy였다. 시도 순서와 결과:
1. **나이 캐스케이드 단독**(coarse GC 생존자를 세대별 frontier로 분리, 블록당 1바이트): 거의 무효(z0.8 −2.6%가 최대). 원인 진단 — victim이 greedy(최소 valid)라서 분리해 놓은 cold 블록에 "죽음이 쌓일 시간"을 주는 메커니즘이 없음.
2. **cost-benefit victim**(`(P−v)/v × √age` 최대, Rosenblum; age = alloc 시퀀스 차): 큰 효과. SepBIT 포팅 때 greedy→CB가 5.50→4.66을 만든 것과 같은 원리.
3. **CB + 캐스케이드 조합**: CB의 √age 항이 캐스케이드가 만든 나이-균질 블록에서 의미를 갖게 되어 추가 이득.

**결과(64 GiB, z별, 동일 물리 공간).**

| 구성 | RAM | z=0 | z=0.8 | z=0.99 | z=1.2 | small | large |
|---|---|---|---|---|---|---|---|
| canonical+cb+cc2 (64K, 기본 정책) | **5.9–6.1 MB** | 8.10 | **6.66** | **4.32** | **1.90** | – | – |
| 32K-gs+cb+cc2 (v2.9 정책 세트) | **11.8 MB** | 7.91 | **5.99** | **4.02** | **1.78** | **4.40** | **3.87** |
| page4k | 64 MB | 6.55 | 6.70 | 4.76 | 2.63 | 4.74 | 4.79 |
| SepBIT | 64 MB | 8.68 | 6.92 | 4.27 | 1.80 | 4.22 | 4.28 |
| DOGI | ~100 MB | 7.81 | 5.90 | 3.78 | 1.45 | 4.00 | 3.64 |

- **저메모리 지점이 변모했다**: canonical 구성 그대로 victim/캐스케이드만 바꿔 5.08→4.32(−15%). **6 MB로 page4k(64 MB)를 skew 전 구간에서 이기고**(z0.8/0.99/1.2에서 −0.5/−9/−28%), SepBIT(64 MB)와 1% 차. 균등은 +1.8% 소폭 악화(√age가 균등에서 무의미).
- 11.8 MB 지점(32K-gs+cb+cc2)은 SepBIT를 전 skew에서 이기고 DOGI와 +1.3%(z0.8)~+23%(z1.2), z0.99 +6.5%.
- 추가 메타데이터: age는 기존 bseq 재사용(0), 캐스케이드는 블록당 1바이트(373 B) — DRAM 스케일링 주장 불변.
- RAM 모델 수정: cost_cold의 flast(4 B/fine 상주 IU)가 미계상이던 버그 수정(32K-gs 11.12→11.76 MB).

**미결(사용자 결정 대기).** cb+cc2를 새 canonical로 승격하면(±두 지점 모두) 전체 결과/그림 재생성이 필요하다. fine 풀 내부의 SepBIT식 클래스 배치(flast 재사용, 추가 메타 0)는 남은 미개척 최적화.

## v3.0 — cb+cc2 canonical 승격 (`results/final_v3`)
v2.10의 두 knob을 canonical로 편입(사용자 승인). 최종 수치는 README v3.0 표 참조. 구현 보완: 캐스케이드 frontier의 신규 블록 할당이 GC 예약(free≤1)을 고갈시키려 하면 공용 GC frontier(또는 여유 있는 클래스 frontier)로 우회하는 fallback — T를 올리는 방식(3+cc)은 실효 OP를 3블록(~9%) 깎아 전면 회귀(z0.99 4.02→4.46)했기에 기각. 메타데이터: 매핑 엔트리당 0 B, 블록당 1 B(클래스) + 기존 8 B 시퀀스(age) 재사용. 4K 순차 회귀(버퍼 1 MiB) 두 동작점 모두 WAF=1.000. 그림 갱신은 보류 중(두 동작점+SepBIT/DOGI 라인 반영 필요).

## v3.0.1 — 총 OP 7% 고정 비교 (`results/op7`)

동일 공간 기준을 총 OP 9.278%에서 **7%**로 조이면(총 365블록; proposed는 fine을 OP에서 분할해 총량 정확히 7%):

| WAF | z=0 | z=0.8 | z=0.99 | z=1.2 |
|---|---|---|---|---|
| proposed-6M | 10.91 | 8.89 | 5.60 | **2.28** |
| proposed-12M | 10.62 | **8.03** | **5.47** | 2.34 |
| page4k (64 MB) | **8.75** | 8.53 | 5.99 | 3.09 |
| DFTL | 9.36 | 8.93 | 6.18 | 3.14 |
| SepBIT (128 MB) | 13.55 | 10.56 | 6.37 | 2.43 |
| IU+RMW | 15.94 | 15.57 | 10.69 | 5.07 |
| μ-FTL | 17.30 | 55.66 | 33.19 | 12.40 |
| FAST | 18.56 | 20.58 | 27.48 | 42.55 |
| LAST | 2217.6 | 1674.4 | 868.5 | 209.3 |

추가: **DOGI(ML, ~100 MB) 전 구간** — z0 10.395 / z0.8 9.566 / z0.99 5.898 / z1.2 1.701. 9.278% 대비 악화 +33 / **+62** / +56 / +18% (ML 정상, 재학습 2~4회; z0은 미학습). 낮은 OP 붕괴는 중간 skew에서 최대이고 SepBIT(+49%)보다 심하다.

**관찰.**
1. **결론이 OP에 강건하다**: skew에서 proposed가 여전히 page4k를 이긴다(z0.99 −9%, z1.2 −26%).
2. **분리 기법일수록 낮은 OP에 취약하다**: 9.278%→7%의 악화율이 page4k +26%, proposed +30~35%인데 **SepBIT는 +49%**(z0.8에서 6.92→10.56). 그룹별로 OP를 쪼개 쓰는 구조라 OP가 조이면 그룹마다 굶는다. proposed는 gc_select=cost/성장 가드가 fine-coarse 간 공간을 동적으로 재배분해 상대적으로 잘 버틴다 — z0.8에서 SepBIT 대비 **−24%**로 격차가 9.278%때(−12%)보다 벌어진다.
3. proposed-6M이 z1.2에서 12M을 역전(2.28 vs 2.34): 총 OP 23.9블록 중 16블록을 fine에 분할하는 12M 구성은 조인 OP에서 무리 — OP가 작을수록 작은 fine 풀이 유리.
4. μ-FTL은 7%에서 폭주(z0.8 55.7), FAST/LAST는 여전히 논외.
5. **OP 7%에서 proposed가 DOGI를 z0.8과 z0.99에서 역전한다**(z0.8: 12M 8.03 vs DOGI 9.57 = **−16%**; z0.99: 5.47/5.60 vs 5.90). 예측-기반 다그룹 분리는 그룹마다 유휴 슬랙(expired 대기 공간)을 요구해 OP가 조이면 무너지는 반면, 우리 구조는 두 풀 사이 공간을 비용 기반으로 동적 재배분한다. 단 z1.2(극단 skew, hot set 극소)에서는 DOGI가 1.70으로 우위 유지, z0은 page4k 우위. **정확한 문장: "상용 OP(7%)의 중간~높은 skew(0.8~0.99)에서 1/9~1/17 DRAM으로 SOTA를 이긴다"** — 헤드라인 후보.

## v3.1 — OP 상전이 그림 (`results/opsweep`): 저OP 우위는 체계적이다

"왜 하필 7%냐"에 답하기 위해 OP 4~20%를 스윕했다(z=0.8, 0.99; proposed는 fine 풀을 OP에서 분할해 총량 일치).

**z=0.99**

| OP | proposed-6M | proposed-12M | page4k (64 MB) | SepBIT (128 MB) |
|---|---|---|---|---|
| 4% | 사망 | 사망 | 10.65 | 사망 |
| 5% | 8.45 | **7.17** | 8.03 | 13.45 |
| 7% | 5.60 | **5.47** | 5.99 | 6.37 |
| 10% | 4.16 | **3.87** | 4.57 | 3.96 |
| 15% | 3.17 | 2.89 | 3.53 | **2.69** |
| 20% | 2.73 | 2.39 | 3.04 | **2.22** |

**z=0.8**

| OP | proposed-6M | proposed-12M | page4k (64 MB) | SepBIT (128 MB) |
|---|---|---|---|---|
| 4% | 사망 | 사망 | 15.76 | 사망 |
| 5% | 13.97 | 12.16 | **11.82** | 사망 |
| 7% | 8.89 | **8.03** | 8.52 | 10.56 |
| 10% | 6.37 | **5.72** | 6.37 | 6.41 |
| 15% | 4.73 | 4.16 | 4.48 | **4.08** |
| 20% | 3.96 | 3.41 | 3.55 | **3.14** |

**핵심 관찰.**
1. **SepBIT과의 크로스오버가 OP ≈ 12%에 있다.** 그 아래에서는 proposed가, 위에서는 SepBIT이 이긴다. 7%가 체리피킹이 아니라 **저OP 영역 전체가 우리 구간**이라는 뜻이다. 상용 SSD의 OP(GB/GiB 차이에서 오는 ~7%)가 정확히 이 구간에 있다.
2. **page4k는 z=0.99 전 OP 구간에서, z=0.8은 OP≥7%에서 진다.** proposed-12M 기준.
3. **다그룹 분리는 OP를 open segment에 묶어 놓는다.** SepBIT은 5%(z0.8)·4%(전부)에서 `GC makes no progress`로 사망하는데, 원인은 6개 그룹의 open segment(6블록) + GC 팬아웃용 예약(T=8) = 14블록이 총 OP 16.7블록의 84%를 점유하기 때문이다. 우리는 예약 T=2로 충분해(캐스케이드는 여유 없으면 공용 frontier로 우회) 같은 5%에서 생존한다. **"분리 기법은 분리할 공간부터 필요하다"**는 것이 저OP 붕괴의 메커니즘.
4. **우리 쪽 실현 가능 하한은 OP 5%.** 4%에서는 `no fine victim`으로 사망 — 총 OP 12.7블록에서 fine 8블록을 떼면 coarse OP가 1.4%(12M은 음수)라 애초에 성립하지 않는 구성이다. 정직하게 한계로 명시한다.

### v3.1 보론 — 매핑 DRAM 재계산: 왜 예측 기반 기법은 페이지당 DRAM을 더 쓰는가

**원인은 예측의 단위가 4K라는 것.** DOGI/SepBIT은 "이 4K 블록이 언제 무효화될지"를 블록별로 추정한다. 추정하려면 그 블록의 과거를 기억해야 하고, 어느 블록이 중요한지 미리 알 수 없으므로 **장치 전체를 추적**해야 한다. 64 GiB 기준 정보량(구현 상수가 아닌 필요 최소량):

| 항목 | DOGI | SepBIT | proposed-12M |
|---|---|---|---|
| L2P 페이지 맵 (4 B/4K) | 64.0 MB | 64.0 MB | — |
| coarse 맵 (4 B/IU = 0.5 B/4K) | — | — | 8.0 MB |
| 재작성 간격 `intervals_` (1 B/4K) | 16.0 MB | — | — |
| 최근성 비트맵 `freqBits_` (1 B/4K) | 17.5 MB | — | — |
| 청크 빈도 (8 B/2 MiB) | 0.3 MB | — | — |
| FrozenFilter (1 bit/4K) | 2.0 MB | — | — |
| last-write 타임스탬프 (4 B/4K) | — | 64.0 MB | — |
| fine 맵 + 해시 인덱스 + `flast` (**상주분만**) | — | — | 3.5 MB |
| MLP 가중치 (6-32-32) | ~0 MB | — | — |
| **합계** | **99.8 MB** | **128.0 MB** | **11.5 MB** |

**세 가지 시사점.**
1. **DRAM을 먹는 건 ML이 아니다.** MLP는 6→32→32라 수 KB다. 비용은 **ML에 먹일 피처를 per-LBA로 모으는 데** 있다. "ML을 빼면 가벼워진다"는 반론은 성립하지 않는다.
2. **SepBIT 표기를 64 MB → 128 MB로 정정한다.** 이전 표는 페이지 맵만 세고 GC 나이 판정용 per-LPN 타임스탬프(레퍼런스 `Metadata`, 8 B; 최소 4 B)를 누락했다. ftlsim의 RAM 모델도 수정했다(`mode=sepbit`). 우리에게 불리한 방향의 오류였으므로 정정하면 격차가 커진다.
3. **우리 비용은 O(hot 워킹셋)이다.** coarse 맵은 IU당 1엔트리라 4K당 0.5 B로 압축되고, per-LBA 추적은 아예 없다 — hotness 1차 신호가 "fine 풀 상주 여부" 자체이기 때문에 비상주 영역(대부분)은 추적할 이유가 없다. `flast`(4 B)도 상주 IU에만 붙는다. 용량이 커질수록 격차가 벌어진다: 4 TiB로 스케일하면 DOGI ~6.2 GB / SepBIT ~8 GB vs proposed ~0.6 GB.
