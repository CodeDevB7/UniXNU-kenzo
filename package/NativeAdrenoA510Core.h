/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_A510_CORE_H
#define NATIVE_ADRENO_A510_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ringbuffer size: 32 KiB = 8192 dwords (matches Linux MSM_GPU_RINGBUFFER_SZ). */
#define A510_RING_BYTES   32768u
#define A510_RING_DWORDS  (A510_RING_BYTES / 4u)
#define A510_RING_MASK    (A510_RING_DWORDS - 1u)

/* Default CP_RB_CNTL matching Linux MSM_GPU_RB_CNTL_DEFAULT (log2(32768/8) = 12) | NO_UPDATE */
#define A510_CP_RB_CNTL_DEFAULT (12u | (1u << 27))

/* RBBM_INT_0_MASK matching Linux A5XX_INT_MASK (without GPMU voltage droop, A510 has no GPMU) */
#define A510_RBBM_INT_0_MASK_DEFAULT \
    ((1u << 0)  | /* RBBM_GPU_IDLE */ \
     (1u << 1)  | /* RBBM_AHB_ERROR */ \
     (1u << 4)  | /* RBBM_ATB_BUS_OVERFLOW */ \
     (1u << 6)  | /* CP_HW_ERROR */ \
     (1u << 15) | /* CP_AHB_ERROR */ \
     (1u << 20) | /* CP_CACHE_FLUSH_TS */ \
     (1u << 22) | /* UCHE_OOB_ACCESS */ \
     (1u << 23))  /* UCHE_TRAP_INTR */

typedef enum {
    A510_CORE_OK = 0,
    A510_CORE_ERR_INVALID = -1,
    A510_CORE_ERR_RING_FULL = -2,
    A510_CORE_ERR_IDLE_TIMEOUT = -3,
} A510CoreStatus;

typedef struct {
    void *ctx;
    uint32_t (*gpu_read32)(void *ctx, uint32_t dword_offset);
    void (*gpu_write32)(void *ctx, uint32_t dword_offset, uint32_t value);
    void (*wmb)(void *ctx);    /* write memory barrier before CP_RB_WPTR write */
    void (*udelay)(void *ctx, uint32_t usec);
} A510CoreOps;

typedef struct {
    uint32_t *cpu_va;
    uint64_t gpu_iova;
    uint64_t rptr_shadow_iova;
    uint64_t fence_iova;
    uint32_t wptr;             /* next dword index [0..A510_RING_DWORDS-1] */
    uint32_t rptr_cached;      /* last observed RPTR */
    uint32_t next_seqno;       /* monotonic nonzero fence sequence */
} A510Ring;

typedef struct {
    uint64_t pm4_fw_iova;      /* a530_pm4.fw in GPU VA */
    uint64_t pfp_fw_iova;      /* a530_pfp.fw in GPU VA */
    uint64_t uche_trap_iova;   /* A510_UCHE_TRAP_BASE_IOVA */
    /* Hardware Gate #2: Linux a5xx_hw_init() writes RBBM_SECVID_TRUST_CNTL = 0 on A510
     * (no ZAP shader). Kept opt-in so physical bring-up can test EL1 access safely. */
    bool clear_secvid_trust_cntl;
} A510HwInitConfig;

typedef struct {
    uint64_t iova;
    uint32_t dwords;
} A510IbDesc;

/* Initialize ring state over a zeroed 32 KiB buffer. */
A510CoreStatus a510_ring_init(A510Ring *ring,
                              uint32_t *cpu_va,
                              uint64_t gpu_iova,
                              uint64_t rptr_shadow_iova,
                              uint64_t fence_iova);

/* Program Adreno 510 hardware registers following Linux a5xx_hw_init(), emit CP_ME_INIT
 * (with A510 workaround 0x00000001), flush WPTR, and wait for CP idle. */
A510CoreStatus a510_hw_init(const A510CoreOps *ops, A510Ring *ring, const A510HwInitConfig *cfg);

/* Emit a self-contained hardware smoke test: CP_MEM_WRITE(target_iova, magic_word) +
 * CP_EVENT_WRITE(CACHE_FLUSH_TS | IRQ, fence_iova, seqno) + CP_WHERE_AM_I. */
A510CoreStatus a510_ring_submit_smoke_write(const A510CoreOps *ops,
                                            A510Ring *ring,
                                            uint64_t target_iova,
                                            uint32_t magic_word,
                                            uint32_t *out_seqno);

/* Emit a client IB batch (1..128 IBs) followed by CACHE_FLUSH_TS fence & WHERE_AM_I. */
A510CoreStatus a510_ring_submit_ibs(const A510CoreOps *ops,
                                    A510Ring *ring,
                                    const A510IbDesc *ibs,
                                    size_t ib_count,
                                    uint32_t *out_seqno);

/* Poll CP_RB_RPTR == ring->wptr && (RBBM_STATUS & 0x80000000) == 0. */
A510CoreStatus a510_wait_idle(const A510CoreOps *ops, const A510Ring *ring, uint32_t max_iters);

#ifdef __cplusplus
}
#endif

#endif
