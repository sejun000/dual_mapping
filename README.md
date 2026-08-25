# dual_mapping — Dual-Granularity FTL Simulator (v1)

스펙: [SPEC.md](SPEC.md). 구현: [ftlsim/ftlsim.cpp](ftlsim/ftlsim.cpp) (C++17 단일 파일, 의존성 없음).

## 빌드 / 실행
```bash
./build.sh                       # g++ 있으면 g++, 없으면 ~/tools 의 zig c++ 사용
./ftlsim/ftlsim mode=proposed capacity=64G zipf=0.99 out_ts=ts.csv   # JSON → stdout, 로그 → stderr
tools/run_matrix.sh results/kill # kill test 매트릭스 (zipf × mode × OP) 병렬 실행 + 요약표
python3 tools/summarize.py results/kill
```
64GB 1회 실행 ≈ 5초. 모든 파라미터는 `key=value` (capacity, P, U, op, fine_quota, T, mode, zipf, min_k/max_k, seed,
age_x, measure_x, seq_fill, fine_extra, gc_frontier, zipf_shuffle, ts_interval, out_ts, tag).

## 스펙 대비 구현 결정 (중요)
1. **fine quota는 OP 밖의 추가 공간** (`fine_extra=1` 기본). seq fill 후 모든 IU가 extent를 보유하므로 coarse 티어는
   항상 ≥ L 페이지를 점유해야 하는데, OP 7% 안에서 10%를 fine에 떼면 coarse < L → GC가 공간을 영원히 못 만듦(실측으로 확인,
   `fine_extra=0`이면 진단 메시지로 종료). 따라서 proposed 총 물리공간 = L·1.07 + quota ≈ 1.177L.
   공정 비교용으로 baseline은 OP 7%(`_op7`)와 동일 총공간 OP 17.7%(`_op18`) 두 가지로 돌림.
2. **d=U extent drop**: IU 16페이지 전부가 fine에 있으면 extent를 dead 처리(스펙 누락 케이스). hot IU의 coarse 공간 해제.
3. **GC frontier 분리** (`gc_frontier=1` 기본, 세 모드 공통): host 쓰기와 GC/fold 쓰기의 active block을 분리(fine·coarse 각각 → 4개).
   섞으면 GC 복사 cold 데이터가 frontier를 점령하는 자기강화 평형에 빠짐.
4. **zipf는 IU rank 단위**, rank==IU (hot 영역 주소상 연속, `zipf_shuffle=0` 기본). 셔플하면 큰 요청이 cold 이웃으로 넘쳐 스큐가 희석됨.
5. 요청 크기 4K–256K 2^n 로그균등. aging = seq fill 1× + 워크로드 2× → 통계 리셋 → 측정 1×.
6. 중첩 GC 안전성: fine GC 복사/fold 중 페이지 할당이 coarse GC를 중첩 유발해 같은 IU가 re-fold될 수 있어, 할당 *후* 상태를 재평가.

## 첫 결과 (64GB, U=16, P=1024(4 MiB 블록, 초기), quota 10%, seed 1)
| WAF | zipf 0 | 0.8 | 0.99 | 1.2 |
|---|---|---|---|---|
| proposed (1.177L) | 4.34 | 3.98 | 3.70 | 2.05 |
| page4k OP17.7% (동일 공간) | 3.33 | 3.60 | 3.76 | 1.88 |
| iu_rmw OP17.7% (동일 공간) | 6.01 | 6.50 | 6.82 | 4.50 |
| page4k OP7% | 6.99 | 7.32 | 7.08 | 3.85 |
| iu_rmw OP7% | 12.34 | 13.01 | 12.59 | 8.05 |

관찰:
- proposed는 동일 공간 iu_rmw 대비 31–46% 낮고, page4k와의 격차는 0–30% (zipf 0.99에서 page4k와 거의 같음).
- 스펙의 "균등에서 proposed ≈ iu_rmw ≈ 17" 예상과 다름: 요청 크기 mix에 정렬된 64K+가 섞여 iu_rmw도 16까지 안 가고, proposed는 fine 티어가 조각을 흡수해 훨씬 낮음.
- proposed의 쓰기 내역은 coarse GC clean copy가 40–55%로 지배적. 자발적 fold는 거의 없고(거의 전부 GC-시점 re-fold), re-fold d 분포는 1–2와 4, 8에 봉우리(요청 크기 2^n 효과).
- zipf 0.8/0.99는 greedy GC + OP 7%에서 효과가 작음(꼬리 트래픽이 cold 블록에 invalid를 흩뿌리는 stranding). α=1.2부터 차이 남. hot/cold 2구역 워크로드 추가 권장.

## 공간 / 매핑 메모리 (논리 64 GiB)
- 물리: 세 모드 모두 19,283 블록 = 75.3 GiB (OP 17.69%; baseline은 `op=0.17695`로 블록 수를 맞춤).
- 매핑 DRAM (PPN 4B, P2L 역맵은 공통이라 제외): page4k 64 MiB / iu_rmw 4 MiB /
  proposed = coarse 4 MiB + fine map. fine map 실측(정상상태): zipf 0 → 201,880 IU·1,405,936 페이지(d≈7.0),
  zipf 0.99 → 160,834·1,118,350, zipf 1.2 → 118,825·775,330.
  compact 레이아웃(키 4B+bitmap 2B+d×4B)이면 6.5 MiB → 총 ≈10.5 MiB, 최악(quota 가득, d≈1.7) ≈17 MiB.
  고정 16슬롯 배열(72B/IU)은 최악 72 MiB로 page4k를 넘으므로 compact 필수. → page4k 대비 4~6× 절감.

## v1.1: 192 MiB 블록 + FAST/LAST baseline (`results/kill_p192`)
블록 크기를 4 TiB:12 GiB 비율로 맞춤(`P=0` 자동 → 64 GiB에서 49,152 페이지). `mode=fast|last` 추가(설계는 SPEC.md v1.1 절).
| WAF (동일 물리 75.3 GiB) | zipf 0 | 0.8 | 0.99 | 1.2 |
|---|---|---|---|---|
| proposed | 4.66 | 4.30 | 3.43 | 1.71 |
| page4k | 3.67 | 3.94 | 3.41 | 1.52 |
| iu_rmw | 6.71 | 7.20 | 6.09 | 3.34 |
| FAST | 7.59 | 9.69 | 18.3 | 38.4 |
| LAST | 1489 | 970 | 484 | 118 |
- FAST는 균등에서 1/OP에 수렴(블록 크기 무관)하지만 skew에서 hot LB 반복 merge로 악화; LAST는 블록연관 순차 로그가 거대 블록에서 thrashing.
- proposed는 4 MiB → 192 MiB로 가도 WAF 거의 동일(IU 단위 fold라 블록 크기 둔감) — 논문 포인트.

## v1.2: DFTL / μ-FTL 포팅 + 실측 RAM (`results/kill_p192`, 7개 정책)
| WAF (동일 물리) | zipf 0 | 0.8 | 0.99 | 1.2 | RAM MiB |
|---|---|---|---|---|---|
| proposed | 4.66 | 4.30 | 3.43 | 1.71 | 11.7 |
| page4k | 3.67 | 3.94 | 3.41 | 1.52 | 64 |
| DFTL(캐시=11.7) | 3.86 | 4.06 | 3.47 | 1.53 | 11.7 |
| μ-FTL(캐시=11.7) | 6.86 | 9.40 | 6.67 | 2.83 | 11.7 |
| iu_rmw | 6.71 | 7.20 | 6.09 | 3.34 | 4 |
| FAST | 7.59 | 9.70 | 18.3 | 38.7 | 14.6 |
| LAST | 1489 | 970 | 484 | 118 | — |
- DFTL은 같은 RAM에서 WAF가 proposed보다 낮지만 host 페이지당 매핑 miss read 0.94(균등)~0.06(zipf 1.2)가 붙음(proposed 0). RAM 산정 규칙은 SPEC v1.2 절.

## v2.2 (최종): RAM ≈ page4k의 10%, 동일 물리 공간(373블록, OP 9.28%), aging 8×, **정책 상수 0개** (`results/final_ram10`)
quota 8블록(2.2%). fine 정리 정책 `cost`: victim 블록(valid v)의 IU별로 **fold iff 16/d < v/(P−v)** (접어서 얻는 슬롯당 쓰기 vs 복사 경로의 슬롯당 쓰기). unfold는 옵션(off; 켜면 ≤1.5%). DFTL/μ-FTL 캐시 = proposed 실측 RAM.
| WAF | zipf 0 | 0.8 | 0.99 | 1.2 | RAM MiB |
|---|---|---|---|---|---|
| **proposed** | 8.53 | 7.67 | 5.16 | 2.76 | 6.06 (9.5%) |
| page4k | 6.74 | 6.87 | 4.98 | 3.02 | 64 |
| DFTL | 6.97 | 7.00 | 5.02 | 3.03 | 6.13 (+동기 map read/page 0.97/0.66/0.33/0.08) |
| IU+RMW | 12.30 | 12.57 | 9.12 | 5.48 | 4 |
| μ-FTL | 12.09 | 53.34 | 13.13 | 4.70 | 6.10 |
| FAST | 13.69 | 15.92 | 24.54 | 44.11 | 7.27 |
| LAST | 1518.59 | 1087.28 | 560.60 | 149.64 | — |
- proposed vs IU+RMW: -31 / -39 / -43 / -50 %. vs DFTL: +22 / +10 / +3 / -9 %. vs page4k(10× DRAM): +27 / +12 / +4 / -9 %.
- small-heavy(4K 50%…): proposed 13.36 / 7.15 (zipf 0 / 0.99), DFTL 7.86 / 5.38, IU+RMW 35.4 / 26.3.
- 정책 비교(기본 mix zipf 0 / small-heavy zipf 0): 고정 d≥4: 8.95 / 15.40, d≥8: 8.36 / 17.49, **cost: 8.53 / 13.36** — 압박(v)에 따라 d 문턱이 자동으로 움직여 양쪽을 모두 커버.
- aging: seq fill 1× + 워크로드 8× → 리셋 → 측정 1×. μ-FTL은 논문대로 GC 복사를 host와 같은 update block에 써서 장기 aging에서 악화(의도적 유지).

## Janus-FTL baseline (`results/janus`, 64 GiB, 192 MiB 블록, 동일 공간)
| zipf | DRAM 10% (fused≤34), u*=0.7 | 무제한 u*=0.7 | u*=0.9 | u*=1.0 (= page mapping, 64 MiB) | page4k | proposed |
|---|---|---|---|---|---|---|
| 0 | 2,445 | 38.7 | 58.5 | 6.97 | 6.74 | 8.53 |
| 0.99 | 645 | 188 | 48.9 | 5.97 | 4.98 | 5.16 |
| 1.2 | 140 | 66.5 | 13.5 | 3.99 | 3.02 | 2.76 |
거대 블록에선 defusion(블록 단위 demote)이 항상 손해 → Janus는 page-mapping 끝점에서만 성립(DRAM 64 MiB). 상세 SPEC 참조.

## v2.6 (canonical): 두 정책 모두 적용 — RAM ≈ page4k의 10%, 동일 물리 공간(373블록, coarse OP 7% + fine 8블록 = 총 OP 9.28%), aging 8×, 1 MiB 쓰기 버퍼
정책: fine 회수 = 비용 기반 fold(`U/d < v/(P−v)`), 쓰기 시점 = 선택적 RMW(`U/n < 1 + p·U/u`), 둘 다 튜닝 상수 없음. DFTL/μ-FTL 캐시 = proposed 실측 RAM(6.17 MiB).
| WAF | zipf 0 | 0.8 | 0.99 | 1.2 | RAM MiB |
|---|---|---|---|---|---|
| **proposed** | 7.96 | 7.56 | 5.37 | 3.15 | 6.17 |
| page4k | 6.55 | 6.73 | 5.03 | 3.23 | 64 |
| DFTL | 6.99 | 7.03 | 5.18 | 3.29 | 6.24 (+동기 map read/page 0.97/0.66/0.35/0.10) |
| IU+RMW | 11.93 | 12.25 | 9.03 | 5.20 | 4 |
| μ-FTL | 13.23 | 30.10 | 12.01 | 5.95 | 6.20 |
| FAST | 13.69 | 15.91 | 24.44 | 48.50 | 7.26 |
| LAST | 2204.77 | 1585.89 | 847.92 | 197.82 | — |
- proposed vs IU+RMW: -33 / -38 / -41 / -39 %. vs DFTL: +14 / +7 / +4 / -4 %. vs page4k(10× DRAM): +22 / +12 / +7 / -2 %.
- 크기 분포: small-heavy 12.81 / 7.49, large-heavy 7.47 / 5.21 (zipf 0 / 0.99).
- ablation(fig9): 두 정책 다 끄면 6~32% 나빠짐. 상세는 SPEC.
- 그림: `results/fig/fig1..fig9` (PDF+PNG), 생성은 `python3 tools/plot.py`.
