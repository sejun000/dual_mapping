# Dual-Granularity (IU/4K) FTL — 연구 요약 (외부 리뷰용)

## 1. 문제
- 고용량 QLC SSD는 DRAM 제약 때문에 매핑 단위(IU, indirection unit)를 4 KiB → 16–64 KiB로 키우고 있다 (Solidigm 16K IU, Silicon Motion configurable IU 등).
- 호스트는 여전히 4 KiB 쓰기를 내므로, sub-IU 쓰기는 read-modify-write(RMW)로 IU 전체(64 KiB = 16페이지)를 다시 쓴다 → 쓰기 증폭(WAF) 급증 (우리 측정: 4K-heavy 워크로드에서 WAF 26–35).
- 동시에 현대 QLC의 (슈퍼)블록은 수백 MiB (4 TiB 드라이브에 ~12 GiB급 슈퍼블록) → 고전 하이브리드 FTL(log-block merge)은 merge 단위가 블록이라 성립하지 않음.

## 2. 제안 구조 (tiered dual mapping)
- **coarse 티어**: IU(64K) 단위 매핑 `coarse_map[IU] → chunk(16페이지 연속 물리 사본)`. 정렬된 IU 쓰기는 chunk로 기록. (NIU × 4B = 용량의 1/16K)
- **fine 티어**: 조각(fragment, 4K) 쓰기를 4K 단위로 기록하고 해시 `fine_map[IU] → {bitmap16, slot_ppn[d]}`로 관리. IU의 최신 상태 = chunk 나머지 + fine 조각.
- 물리 풀은 하나, 블록은 할당 시 fine/coarse로 태그. fine 블록 수는 **DRAM 예산**에서 역산한 quota(64 GiB에서 8블록 = 2.2%)로 제한. host용/GC용 active block을 티어별로 분리(총 4개).
- 연산:
  - **fold**(IU): fine 조각 d개 + chunk 나머지(16−d) → 새 chunk. 조각 전부 invalid.
  - **refold**: coarse GC가 살아있는 chunk를 옮길 때 조각을 흡수 — 추가 쓰기 0(어차피 16 쓰기).
  - **unfold**(옵션): fine-hot IU의 chunk 나머지를 fine으로 끌어와 chunk를 죽임(승격). 효과 ≤3%라 기본 off.
  - **chunk_drop**: host 조각 쓰기가 16페이지 전부 덮으면 chunk 자동 dead.
- GC: coarse GC = valid 최소 블록(조각에 덮인 페이지는 invalid로 셈), chunk별 refold / clean copy / dead skip. fine 회수 = valid 최소 fine 블록.
- **fine 회수 정책(무상수)**: victim(valid v, 블록 P페이지)의 각 IU(조각 d)에 대해 **fold iff 16/d < v/(P−v)** (접어서 얻는 슬롯당 쓰기 vs 복사 경로의 슬롯당 쓰기), 아니면 fine 내 복사. 압박(v)에 따라 d 문턱이 자동으로 움직임. 정책 튜닝 상수 0개.

## 3. 실험 설정
- 시뮬레이터: C++ 트레이스 구동, 64 GiB 논리, 4 KiB 페이지, 블록 192 MiB(4 TiB:12 GiB 비율), U=16. aging = seq fill 1× + 워크로드 8× → 통계 리셋 → 측정 1×. 매핑으로부터 valid 재계산하는 검증 통과.
- 워크로드: 요청 크기 4K–256K(2^n, 요청 수 균등; 페이지로는 조각 47%), 시작 IU zipf α∈{0, 0.8, 0.99, 1.2}(복원추출, hot 영역 주소 연속). small-heavy(4K 50%, 조각 81%) / large-heavy(조각 32%) 변형.
- 공정성: 모든 정책 **동일 물리 공간**(373블록, OP 9.28%) + **매핑 DRAM = page4k의 ~10%**(proposed 실측 6.06 MiB; DFTL/μ-FTL 캐시를 같은 값으로). RAM은 해시(엔트리 (키+값)/0.7) 가정으로 실측 상태에서 계산.
- baseline(같은 인프라에 포팅): page4k(greedy), DFTL(translation page 캐시, 동기 map read 카운트), IU+RMW(현행 QLC), FAST(TECS'07), LAST(OSR'08, 본문 대조), μ-FTL(EMSOFT'08, extent/μ-tree/파티션 update block 논문대로), 그리고 기각한 mixed 풀.
- host/GC active block 분리는 모든 정책에 동일 적용(μ-FTL만 논문대로 미분리).

## 4. 주요 결과 (WAF, zipf 0 / 0.8 / 0.99 / 1.2)
| | WAF | RAM |
|---|---|---|
| **proposed** | 8.53 / 7.67 / 5.16 / 2.76 | 6.06 MiB |
| page4k | 6.74 / 6.87 / 4.98 / 3.02 | 64 MiB |
| DFTL | 6.97 / 7.00 / 5.02 / 3.03 (+동기 map read 0.97 / 0.65 / 0.33 / 0.08 per host page) | 6.13 MiB |
| IU+RMW | 12.30 / 12.57 / 9.12 / 5.48 | 4 MiB |
| μ-FTL | 12.1 / 53 / 13 / 4.7 | 6.10 MiB |
| FAST | 13.7 / 15.9 / 24.5 / 44.1 | 7.27 MiB |
| LAST | 1519 / 1087 / 561 / 150 | — |
| Janus-FTL (DRAM 10% → fused≤34블록) | 2445 / — / 645 / 140 | 6.4 MiB |
| Janus-FTL (DRAM 무제한, u*=0.7 / 1.0) | 38.7 / — / 188 / 66.5  →  6.97 / — / 5.97 / 3.99 (u*=1 = page mapping, 64 MiB) | 64 MiB |
- vs IU+RMW −31/−39/−43/−50%; vs DFTL +22/+10/+3/−9%; vs page4k(10× DRAM) +27/+12/+4/−9%.
- small-heavy(zipf 0 / 0.99): proposed 13.4 / 7.2, DFTL 7.9 / 5.4 (+map 1.33 / 0.50 write, miss read ~1/page), IU+RMW 34.3 / 25.6.
- 쓰기 분해(proposed, zipf 0): host 1.0 + clean copy 6.1 + refold 0.8 + fold 0.4 + FineGC 0.3 → 추가 쓰기의 81%가 coarse GC clean copy (OP 7%의 greedy GC 비용). IU+RMW는 직접 RMW 0.83에 그로 인한 GC 10.5.
- 블록 크기 둔감: 4 MiB → 192 MiB 블록에서 proposed WAF 거의 동일(IU 단위 fold), FAST/LAST/μ-FTL은 붕괴.

## 5. ablation / 부정적 결과
- fine 회수 정책(기본 mix zipf 0 / small-heavy zipf 0): fold 금지 10.45 / —, FIFO-전부fold 9.90, greedy-복사만 10.38, 고정 d≥4 8.95 / 15.40, 고정 d≥8 8.36 / 17.49, **cost(무상수) 8.53 / 13.36**. hotness 타임스탬프 기반 cold 필터는 skew에서 1–2% 이득, 균등 small-heavy에서 12% 손해 → 미채택.
- unfold(GC 시점 승격): ±3%. 이유: 진짜 hot IU는 chunk_drop으로 이미 fine 상주, 기회가 coarse GC 시점뿐, fine 티어가 작음.
- host/GC frontier 분리 ablation(proposed): 합치면 +6 / +26 / +40%(zipf 0 / 0.99 / 1.2).
- mixed 풀(블록에 chunk+fragment 공존, RAM 예산으로 fine 맵 제한, 단일 GC): +4~29% 손해 → 기각. 조각과 chunk가 섞이면 저-valid 블록이 사라져 GC 효율 하락.
- 실효 비용 모델(평균/한계 배율로 fold 비용 추정): 전부 "fold 금지"로 수렴하거나 고정 문턱보다 못함 → 기각.
- fine quota를 RAM 10%→17.7%로 키우면 WAF 4.7(균등)까지 내려감(공간도 같이 늘어 confound).

## 6. 선행연구와 차이 (우리가 파악한 것)
- Janus-FTL(EMSOFT'10), CFTL(SIGMETRICS'10), TLC/SLC dual-mode FTL(MICPRO'16): page/block 매핑 영역 간 비용·패턴 기반 이동 — **coarse 단위가 블록**. 우리는 IU(64K). Janus는 본문대로 포팅해 비교: 192 MiB 블록에선 defusion이 어떤 빈도로든 손해라 u*→1(순수 page mapping, DRAM 64 MiB)에서만 성립, DRAM 10% 제약 시 WAF 수백~수천.
- FAST/LAST/BAST/Superblock: log-block merge — merge 단위 블록. 192 MiB 블록에서 붕괴(우리 실험).
- Subpage-aware SSD(TC'18, ICS'20): 큰 물리 페이지 내 서브페이지 매핑 + 핫니스 병합.
- μ-FTL: 가변 extent + μ-tree. DFTL/LeaFTL/LearnedFTL: 4K 매핑 유지 + 캐시/학습.
- SLC 캐시 하이브리드(ComboFTL 등): 매체 티어링, SLC folding 용어.
- 특허 US10409500(Intel/SK hynix), US10222984(EMC): 구역별/테이블별 정적 granularity, 동적 이동 없음.
- 주장하는 차별점: (1) IU-granular hybrid가 현대 슈퍼블록에서 유일하게 성립하는 설계점이라는 정량 증거, (2) fold의 coarse GC 통합(refold, 추가 쓰기 0) + 무상수 fold 정책, (3) 동일 DRAM에서 DFTL(동기 read)과의 정직한 trade-off 분석.

## 7. 알려진 약점
- 균등 워크로드에서 같은 DRAM의 DFTL보다 WAF 높음(+22%, small-heavy +70%). 반론은 DFTL의 동기 map read(latency) — 아직 시뮬 카운트뿐, 실측(NVMeVirt) 예정.
- 합성 워크로드만(MSR 등 실제 트레이스 미적용).
- 메커니즘 자체는 단순하고 계보가 뚜렷(Janus/CFTL/SLC-hybrid).
- LAST/μ-FTL 결과는 strawman처럼 보일 수 있음(거대 블록에서의 붕괴를 보이는 용도).

## 8. 물어보고 싶은 것
1. 위 차별점(6)이 DATE/MSST/SYSTOR급 리뷰어에게 novelty로 인정될지, 아니면 "Janus-FTL의 IU 단위 재현"으로 읽힐지.
2. 놓친 직접적 선행연구가 있는지(특히 "IU/large-page partial write를 디바이스 내 4K 티어로 흡수"하는 산업·학계 연구).
3. 균등 워크로드에서 DFTL에 지는 약점을 보완할 설계 아이디어(write buffer coalescing 외).
4. 이 결과로 어느 venue가 적절한지(DATE 6쪽 / MSST / SYSTOR / NVMSA / 국내 저널).
