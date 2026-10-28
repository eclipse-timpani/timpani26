/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

/**
 * @file timpani_rt.h
 * @brief TIMPANI L1(TT) 및 L2(CBS) 워크로드 통합 FFI API 명세
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================
 * 1. 공통 제어 API (Common APIs)
 * ========================================== */

/**
 * @brief TIMPANI RT 라이브러리 초기화 (앱 기동 시 1회 호출)
 * @return 0: 성공, 음수: 에러 코드
 */
int timpani_rt_init(void);


/* ==========================================
 * 2. L1 타임 트리거드(TT) 제어 API
 * ========================================== */

/**
 * @brief 현재 스레드를 지정된 TT Task 슬롯과 매핑
 * @param workload_id 워크로드 식별자
 * @param task_id 태스크 식별자
 * @return 0: 성공, 음수: 에러 코드
 */
int timpani_tt_register(const char* workload_id, const char* task_id);

/**
 * @brief 다음 타임 슬롯 주기 시작 시점까지 블로킹 대기 (동기화)
 * @details 내부적으로 sigwait(시그널 방식) 또는 timerfd의 복잡한 시스템 콜을 은닉
 * @return 0: 성공, 음수: 에러 코드 (데드라인 미스 등)
 */
int timpani_tt_wait_period(void);


/* ==========================================
 * 3. L2 산발적/이벤트 구동(CBS) 제어 API
 * ========================================== */

/**
 * @brief 이벤트 고유 계약 등록 (디스패치 모니터링 계약 수립)
 * @details workload/task 정보와 서비스 상세 정보를 연접 해싱하여 event_attestation_map에 등록함.
 *          service_id, instance_id, event_id 값은 내부적으로 접두사 없는 10진수 문자열로 정규화 해싱됨.
 * @param workload_id 대상 CBS 워크로드 ID
 * @param task_id 대상 CBS 태스크 ID
 * @param service_id SOME/IP 서비스 식별 ID
 * @param instance_id SOME/IP 인스턴스 식별 ID
 * @param event_id SOME/IP 이벤트/메서드 식별 ID
 * @param mit_us 최소 이벤트 유입 주기 (마이크로초)
 * @return 0: 성공, 음수: 에러 코드
 */
int timpani_evt_register(
    const char* workload_id,
    const char* task_id,
    const char* service_id,
    const char* instance_id,
    const char* event_id,
    uint32_t mit_us
);

/**
 * @brief 디스패치 큐 진입 시점 t0 도착 증명 마킹
 * @details 미들웨어 Shim 또는 애플리케이션 Enqueue 지점에서 호출하여 
 *          해당 이벤트의 arrival_ns를 갱신하고 Contract 검증을 실행함.
 * @param service_id SOME/IP 서비스 식별 ID
 * @param instance_id SOME/IP 인스턴스 식별 ID
 * @param event_id SOME/IP 이벤트/메서드 식별 ID
 * @param seq 메시지 순번
 * @return 0: 성공, 음수: 에러 코드
 */
int timpani_evt_mark_arrival(
    const char* service_id,
    const char* instance_id,
    const char* event_id,
    uint32_t seq
);

/**
 * @brief 현재 동작 중인 스레드를 CBS 예산 관리 대상으로 바인딩
 * @param workload_id 워크로드 식별자
 * @param task_id 태스크 식별자
 * @return 0: 성공, 음수: 에러 코드
 */
int timpani_cbs_bind_thread(const char* workload_id, const char* task_id);

/**
 * @brief 스레드의 CBS 바인딩 해제 (CFS 스케줄링 복귀)
 * @details 호출한 스레드의 TID(Thread ID)에 매핑된 cbs_tid_map 엔트리를 삭제하여,
 *          이후 OS에 의해 동일 TID가 재사용(TID Recycling)되더라도 엉뚱한 CBS에 매핑되는 예외를 원천 예방함.
 * @return 0: 성공, 음수: 에러 코드
 */
int timpani_cbs_unbind_thread(void);

/**
 * @brief [Legacy] 이벤트 소스 소켓 등록 (SOCKET_COOKIE fast-path UDP 전용 최적화 경로)
 * @details socket_fd로부터 getsockopt(..., SO_COOKIE)를 수행하여 cbs_event_source_map에 매핑함.
 */
int timpani_cbs_register_socket(
    int socket_fd, 
    const char* workload_id, 
    const char* task_id, 
    uint32_t mit_us
);

#ifdef __cplusplus
}
#endif
