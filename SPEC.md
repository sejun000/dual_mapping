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
