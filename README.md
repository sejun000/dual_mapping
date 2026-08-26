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

## v2.7 (canonical): WAF 분모 = 워크로드가 발행한 총 host write — RAM ≈ page4k의 10%, 동일 물리 공간(373블록, coarse OP 7% + fine 8블록 = 총 OP 9.28%), aging 8×, 1 MiB 쓰기 버퍼 (`results/final_ram10_wb`)
정책: fine 회수 = 비용 기반 fold(`U/d < v/(P−v)`), 쓰기 시점 = 선택적 RMW(`U/n < 1 + p·U/u`), 둘 다 튜닝 상수 없음. DFTL/μ-FTL 캐시 = proposed 실측 RAM(6.17 MiB).

**지표 정의(v2.7에서 변경).** `WAF = 총 NAND program / 워크로드가 발행한 총 host 페이지` — 쓰기 버퍼가 덮어쓰기로 흡수해 FTL까지 내려가지 않은 페이지도 분모에 포함한다. 버퍼에 도달한 페이지만 분모로 쓰면(구 정의) 버퍼가 흡수하는 WAF=1짜리 쓰기가 분모에서만 빠져 나가 **버퍼를 키울수록 WAF가 올라가는** 역설이 생긴다(NAND 총 쓰기는 줄어드는데도). JSON은 세 값을 함께 낸다: `WAF`(신규, 분모=발행), `WAF_ftl`(구, 분모=FTL 도달), `buffer_absorbed`(흡수 비율, proposed 기준 0.0~0.18).

| WAF | zipf 0 | 0.8 | 0.99 | 1.2 | RAM MiB |
|---|---|---|---|---|---|
| **proposed** | 7.96 | 7.52 | 5.08 | 2.57 | 6.17 |
| page4k | 6.55 | 6.70 | 4.76 | 2.63 | 64 |
| DFTL | 6.99 | 7.00 | 4.91 | 2.68 | 6.24 (+동기 map read/page 0.97/0.66/0.35/0.10) |
| IU+RMW | 11.93 | 12.19 | 8.55 | 4.24 | 4 |
| μ-FTL | 13.23 | 29.96 | 11.37 | 4.86 | 6.20 |
| FAST | 13.69 | 15.83 | 23.14 | 39.58 | 7.26 |
| LAST | 2204.74 | 1578.69 | 802.98 | 161.42 | — |
- proposed vs IU+RMW: -33 / -38 / -41 / -39 %. vs DFTL: +14 / +7 / +4 / -4 %. vs page4k(10× DRAM): +22 / +12 / +7 / -2 %.
- 버퍼 흡수율이 큰 skew 구간(z=1.2, 18.4%)에서 값이 가장 많이 내려간다(구 3.15 → 신 2.57). 정책 간 순서·상대차는 그대로.
- 크기 분포(`results/final_sizes`): proposed small-heavy 12.81 / 6.85, large-heavy 7.47 / 4.99 (zipf 0 / 0.99). 같은 조건 IU+RMW는 34.32 / 24.14 (small), 8.78 / 6.37 (large) — small-heavy에서 격차가 가장 크다(-63%).
- 4 KB 순차 쓰기(`results/seq`): 버퍼 없으면 proposed 13.47 / IU+RMW 17.04, 1 MiB 버퍼면 둘 다 1.00.
- ablation(fig9): 두 정책 다 끄면 6~48% 나빠짐(z=1.2 +6.3%, 균등 +30.9%, small-heavy z0 +47.9%). 상세는 SPEC.
- 그림: `results/fig/fig1..fig9` (PDF+PNG), 생성은 `python3 tools/plot.py`.

## v2.8: IU 크기 민감도 + 등(等)DRAM 비교 (`results/iu_sens`, fig10)
동일 물리 공간, 192 MiB 블록, aging 8×, 1 MiB 버퍼. U(=IU/4K)는 P=49152의 약수여야 하고 fine 비트맵이 32비트라 U ≤ 32(=128K)까지 지원한다.

**(1) 같은 IU에서** — 이 비교는 proposed 쪽에만 fine 맵 DRAM이 더 붙으므로 iso-DRAM이 아니다.

| IU | 정책 | z=0 | 0.8 | 0.99 | 1.2 | small z=0.99 | RAM MiB |
|---|---|---|---|---|---|---|---|
| 16K | proposed | 7.47 | 7.23 | 4.95 | 2.71 | 4.98 | 17.6 |
| 16K | IU+RMW | 7.61 | 7.71 | 5.24 | 2.77 | 8.06 | 16.0 |
| 32K | proposed | 7.44 | 7.09 | **4.79** | 2.54 | 5.75 | 10.1 |
| 32K | IU+RMW | 9.04 | 9.21 | 6.34 | 3.17 | 13.30 | 8.0 |
| 64K | proposed | 7.96 | 7.52 | 5.08 | 2.57 | 6.85 | 5.9 |
| 64K | IU+RMW | 11.93 | 12.19 | 8.55 | 4.24 | 24.14 | 4.0 |
| 128K | proposed | 9.43 | 8.66 | 5.75 | 2.81 | 8.28 | 3.7 |
| 128K | IU+RMW | 17.63 | 18.11 | 13.15 | 6.74 | 46.73 | 2.0 |

**(2) 같은 매핑 DRAM에서 (공정 비교).** IU+RMW를 U ∈ {4,6,8,12,16,24,32}(16K~128K)로 촘촘히 돌려 DRAM 축을 채우고, proposed의 실측 RAM과 같은 지점의 IU+RMW를 log–log 보간으로 읽었다(α=0.99).

| proposed IU | 실측 RAM | proposed WAF | 같은 RAM의 IU+RMW | 차이 | small-heavy 차이 |
|---|---|---|---|---|---|
| 16K | 17.6 MiB | 4.95 | 5.12 | −3% | −28% |
| 32K | 10.1 MiB | 4.79 | 5.90 | −19% | −43% |
| 64K | 5.9 MiB | 5.08 | 7.17 | **−29%** | **−54%** |
| 128K | 3.7 MiB | 5.75 | 8.93 | −36% | −61% |

같은 IU 기준(−5/−25/−41/−56%)보다는 작지만 **DRAM을 맞춰도 이득의 방향과 추세는 그대로**고, DRAM을 줄일수록(IU를 키울수록) 이득이 커진다. 절대 기준점: proposed 64K는 5.9 MiB로 WAF 5.08 — IU+RMW가 같은 WAF를 내려면 16K, 즉 16 MiB가 필요하므로 **2.7× 적은 매핑 DRAM**이다. proposed 32K(10.1 MiB, 4.79)는 page4k(64 MiB, 4.76)와 사실상 동률.

**(3) fine 티어를 키워 4K 맵 DRAM을 더 쓰면?** (fig10 오른쪽) 전체 물리 공간을 373블록으로 고정하고 fine 블록만 4→8→16으로 늘린 결과(α=0.99):

| fine 블록 | 32K IU | 64K IU |
|---|---|---|
| 4 (0.8 GiB) | 9.0 MiB, WAF 4.97 | 4.9 MiB, WAF 5.55 |
| 8 (1.5 GiB) | 10.1 MiB, **4.79** | 5.9 MiB, 5.08 |
| 16 (3.0 GiB) | 10.0 MiB, 5.89 | 7.0 MiB, **5.05** |

**중복 저장은 없다.** fragment를 쓰면 그 IU 청크의 해당 페이지가 즉시 무효화되고(`bvalid--`), 16페이지가 전부 fragment가 되면 청크 자체가 죽는다(`extent_drop`). 검증 루틴도 LPN당 물리 페이지 정확히 1개만 valid로 센다. 즉 fine 티어는 coarse 원본 위에 얹히는 사본이 아니다.

**그런데도 fine을 키우면 나빠지는 진짜 이유는 활용률이다.** fine 티어는 *고정 예약*인데 채워지는 양은 워크로드가 만들어내는 fragment 워킹셋이 정한다. 32K IU에서 16블록을 주면 활용률이 64.9% → **34.9%**로 떨어져(=5블록어치 공간이 그냥 놀고) coarse OP만 깎여 WAF가 4.79 → 5.89가 된다. 64K IU는 fragment가 많아 활용률이 유지되므로(69.5% → 65.4%) 거의 평평하다.

**제약을 풀면?**(`results/unfold_big`) 청크의 남은 (U−d)페이지까지 fine으로 옮기고 청크를 버리는 `unfold`(coarse GC 중 수행, 구현돼 있으나 기본 off)를 "공간 있으면 항상"으로 켜고 같은 스윕을 돌리면:

| IU | fine 블록 | unfold off | unfold always |
|---|---|---|---|
| 32K | 8 | 4.789 (util 64.9%, 10.05 MiB) | 4.788 (64.0%, 10.43 MiB) |
| 32K | 16 | 5.891 (34.9%, 10.00 MiB) | **4.926** (63.2%, 11.88 MiB) |
| 64K | 8 | 5.083 (69.5%, 5.87 MiB) | 5.067 (70.8%, 6.22 MiB) |
| 64K | 16 | 5.053 (65.4%, 7.02 MiB) | 5.030 (64.5%, 7.38 MiB) |
| 32K/64K | 32~64 | GC 진행 불가(coarse OP 고갈) | 동일하게 진행 불가 |

- unfold는 활용률 문제를 실제로 고친다(34.9% → 63.2%, WAF 5.89 → 4.93) — 사용자가 지적한 제약이 맞았다.
- 하지만 **여전히 fine 8블록(4.79)을 못 이긴다.** DRAM은 1.8 MiB 더 쓰는데 WAF는 나빠진다. fine 티어도 자기 OP가 필요한 page-mapped 로그 구조라, coarse(192 MiB 블록 + greedy GC)에서 공간을 빼앗아 옮기는 것 자체가 이득이 아니다.
- 32블록 이상은 unfold를 켜도 죽는다. unfold는 *이미 fragment가 있는* IU만 fine으로 옮길 수 있어서, fine 티어가 흡수할 수 있는 양은 결국 워크로드가 정한다. 예약만 키우면 coarse OP가 고갈된다(free=2, fine=28/32, coarse=343).

**후속 3(v2.9): 개선 정책 — skew 시 page4k를 역전.** "쿼터를 키우면 좋아져야 한다"는 지적을 따라 세 가지 유출을 막았다: `fine_full=1`(상주 IU의 호스트 쓰기는 fine에 유지), `fine_policy=cost_cold`(hot은 fold하지 않음), `gc_select=cost`(coarse/fine victim을 v/(P−v)로 비교해 싼 쪽 회수). 32K IU + 쿼터 16에서 z0.8/0.99/1.2에 page4k를 −4.5/−5.4/−26%로 이기고(large-heavy −13%), DRAM은 page4k의 ~1/6(10–11 MB). 균등에서는 분리할 hot이 없어 page4k 열세(7.92 vs 6.55). 전부 기본 off 플래그로 기존 동작 보존. 상세는 SPEC v2.9.

**후속 2: 과잉 쿼터 사망 제거.** fine 쿼터를 공간이 감당 못 하게 줘도 이제 죽지 않는다: fine 성장 가드에 최소 coarse OP 바닥(상주의 ~3%)을 두고, dead fine 블록 즉시 회수 + 강제 fold 백스톱 + 예약 인출을 추가했다. 과잉 쿼터는 가드가 정하는 평형(32K→22블록 9.57, 64K→26블록 8.77, 128K→31블록 8.48)으로 수렴하고, canonical과 가드 미발동 구성은 비트 동일. 상세는 SPEC v2.8 보론 3.

**후속: 소유권 제약 자체를 제거한 실험(`unfold=any`).** coarse GC가 만난 fragment 없는 IU까지 통째로 fine으로 보낼 수 있게 구현해 coarse 풀이 전체 LBA를 소유할 필요를 없앴다(fine 상주 최대 156K IU ≈ 4.8 GiB 확인). 그러나 WAF는 모든 IU 크기·쿼터에서 plain 대비 단조 악화(예: 128K/24블록 5.27 → 5.92)였고 쿼터 48+에서는 여전히 사망. 원인: GC가 축출하는 데이터는 cold라서 page-mapped 풀에 두는 게 손해이고, cost-based fold가 그것부터 도로 접어 핑퐁이 생기며(최대 host 쓰기의 18.6%), fine 풀 평형 활용률이 ~65%라 자체 OP 요구를 넘는 순 OP 이득이 없다. 상세는 SPEC v2.8 보론 2.

**결론.** 4K 맵 DRAM은 "중복 저장 때문에" 못 늘리는 게 아니라, **fine 티어의 유효 크기가 fragment 워킹셋으로 정해지기 때문**이다. 그 이상은 놀고 있는 공간이고, 제약(unfold)을 풀어 활용률을 되살려도 WAF 이득으로는 바뀌지 않는다. 이는 fine 티어를 고정 크기(≈2% 공간)로 두는 설계 근거이자, 앞서 기각한 RAM-CAP/mixed-pool 변형과 같은 결론이다.

## v3.0 (canonical): cost-benefit victim + 나이 캐스케이드 승격 — 두 동작점 (`results/final_v3`)
사용자 승인(2026-08-26)으로 `cgc_vict=cb`(coarse victim = (P−v)/v·√age, age는 기존 블록 시퀀스 재사용)와 `coarse_class=2`(GC 생존자 나이 캐스케이드, 블록당 1 B)를 canonical에 편입. 추가 메타데이터는 매핑 엔트리당 0, 블록(192 MiB)당 1 B(+기존 8 B 시퀀스 재사용) = 총 ~3 KB. 캐스케이드 frontier가 예약을 고갈시키지 않도록 free≤1일 때 공용 GC frontier로 우회하는 fallback 포함(T=2 유지, OP 비용 0).

**두 동작점.**
- **proposed-6M** (`prop_lo`): 64K IU, fine 8블록(추가), 기본 정책 + cb + cc2. RAM 5.6–6.2 MB.
- **proposed-12M** (`prop_hi`): 32K IU, 쿼터 16(OP 분할), fine_full + cost_cold + gc_select=cost + cb + cc2. RAM 9.8–15.9 MB.

| WAF | proposed-6M | proposed-12M | page4k (64 MB) | SepBIT (64 MB) | DOGI (~100 MB) | 구 canonical (v2.7) |
|---|---|---|---|---|---|---|
| z=0 | 8.09 | 7.88 | **6.55** | 8.68 | 7.81 | 7.96 |
| z=0.8 | 6.73 | **6.09** | 6.70 | 6.92 | 5.90 | 7.52 |
| z=0.99 | 4.32 | **4.06** | 4.76 | 4.27 | 3.78 | 5.08 |
| z=1.2 | 1.91 | **1.85** | 2.63 | 1.80 | 1.45 | 2.57 |
| small z0.99 | 6.12 | 4.37 | 4.74 | **4.22** | 4.00 | 7.49 |
| large z0.99 | 4.21 | **3.87** | 4.79 | 4.28 | 3.64 | 5.21 |
| small z0 | 12.91 | 9.12 | **6.55** | – | – | 12.81 |
| large z0 | 7.62 | 7.64 | **6.53** | – | – | 7.47 |
| 4K seq (1 MiB 버퍼) | 1.00 | 1.00 | 1.00 | – | – | 1.00 |

- **proposed-6M**: 구 canonical 대비 z0.99 −15%(5.08→4.32), z1.2 −26%. **6 MB로 64 MB page4k를 skew 전 구간에서 역전**(z0.99 −9%), SepBIT(64 MB)와 1% 차.
- **proposed-12M**: SepBIT를 z0/0.8/0.99/1.2에서 이기고(단 small-heavy는 SepBIT 우세), DOGI 대비 +3%(z0.8)~+27%(z1.2), z0.99 +7% — DRAM은 1/9.
- 한계(정직): 균등+small-heavy 조합이 최약점(6M: 12.9 vs page4k 6.6). 분리할 hot이 없고 fragment만 많은 경우로, 페이지 맵 자체의 체급 차이가 드러나는 지점. skew가 있으면 small에서도 12M이 4.37로 경쟁력.
