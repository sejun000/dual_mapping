# DAC 2027 실행 계획 (2026-09-23 확정)

## 결정 사항
- **타깃: DAC 2027** — 공식 일정(https://dac.com/2027/program/research-manuscript-submissions): **초록 11/11 17:00 PST (KST 11/12 10:00), 본문 11/18 17:00 PST (KST 11/19 10:00)**, 통보 2027-03-08, 카메라레디 3/31. 6쪽+참고문헌 1쪽, 더블블라인드, rebuttal 없음. ATC'27(1월 마감설)과는 심사 기간이 겹쳐 택일 → DAC 확정(2026-09-23). 교수님 지시로 국문 저널 취소, FAST'28은 별도 주제로 — **이 논문은 DAC이 종착지, 내용 아낄 필요 없음**.
- **NVMeVirt는 DAC에도 포함** (read 성능 vs DFTL이 저DRAM 경쟁 구도의 마침표).
- 확률 추정: real trace가 버텨주면 55~70%, synthetic만이면 35~50%.

## 8주 플랜 (9/23 → ~11/19)

| 주차 | 작업 | 상태 |
|---|---|---|
| 1 | real trace 계측(크기·순차성·footprint·skew) + 6정책 리플레이(sim) / NVMeVirt 기동 | 계측 진행 중, ko 빌드 완료 |
| 2–4 | NVMeVirt에 ① DFTL 변형(CMT + translation-page read를 실제 NAND read로 발행) ② 우리 FTL 포팅(coarse맵+fine풀+정책스택+wbuf) / WAF sim↔NVMeVirt 교차검증 | |
| 5 | 성능 실험: 랜덤 read 지연(p50/p99) vs 워킹셋 크기, 혼합 R/W 처리량 | |
| 6–8 | 6쪽 집필 + 그림 v3.2 개편(두 동작점 + SepBIT/DOGI 라인) + 교수 리뷰 | |

## 논문 구도
- **WAF = 시뮬레이터** (전 베이스라인: page4k/DFTL/IU+RMW/FAST/μ-FTL/SepBIT/DOGI): 동일공간 + OP 7% + OP 상전이(fig11) + real trace.
- **성능 = NVMeVirt 트리오** (proposed / DFTL / page4k): 핵심 그림은 랜덤 read 지연 vs footprint — DFTL은 CMT 히트율 붕괴로 miss당 NAND read 1회 추가(~2× 지연), 우리는 맵 전체 인DRAM이라 평탄.
- "가상 디바이스" 방어: WAF는 카운팅 지표(타이밍 무관) + NVMeVirt는 FAST'23 출신·DOGI 아티팩트도 사용 + 지연은 실기기 프로파일 캘리브레이션 명시.
- 정직한 경계 공개: 균등(z0), pure-4K random(SPEC v3.3), OP≥15%(SepBIT 우위). 승패 지도가 3축(skew/크기/OP) 모두 측정돼 있음.

## NVMeVirt 셋업 절차 (재부팅 1회 필요 — 사용자 승인 후)
1. GRUB 백업: `sudo cp /etc/default/grub /etc/default/grub.bak`
2. `/etc/default/grub`의 `GRUB_CMDLINE_LINUX`에 추가: `memmap=96G\\\$256G` (지연 안정화 원하면 `isolcpus=<코어들>`도)
3. `sudo update-grub && sudo reboot`
4. 재부팅 후: `cd ~/nvmevirt && sudo insmod nvmev.ko memmap_start=256G memmap_size=96G cpus=<c1,c2>`
5. fio 스모크로 conv_ftl 기동 확인 (`/dev/nvmeXn1` 생성 확인)
- 커널 6.8.0-137, 헤더 OK, `nvmev.ko` 빌드 검증 완료 (~/nvmevirt, snu-csl/nvmevirt)
- RAM 503 GB → 96 GB 예약 부담 없음. sudo 무비번 확인됨.

## 트레이스 자산 (이 머신)
- `/home/sejun000/alibaba_dwpd1to2_4x.trace` (8.7 GB), `alibaba_dwpd2_5x.trace` (19.2 GB): 4–8K 쓰기 위주 → 우리 약점(4K-heavy) 시험대
- `/home/sejun000/ssdtrace_scaled_4x.trace` (4.2 GB): 대형 요청 혼합, 읽기 포함
- 형식: `dev,W|R,바이트오프셋,길이바이트,ts` CSV. ftlsim `trace=<file> trace_warmup=0.5`로 리플레이(쓰기만, LBA modulo 리매핑).
- 원칙: 크기 분포·공간 지역성 먼저 계측해 볼륨 선정 근거를 논문에 명시(체리피킹 방지). 4K-heavy 볼륨도 포함해 승패 지도로 제시.

## 재현성
- 빌드는 zig 고정(`./build.sh`; g++는 `CXX=g++`로만). canonical 재현 확인: prop_lo z0.99 = 4.3211.
