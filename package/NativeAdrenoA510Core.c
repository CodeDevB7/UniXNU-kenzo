/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoA510Core.h"
#include "NativeAdrenoA510HwInit.h"
#include "NativeAdrenoA510PM4.h"

#define A510_CP_EVENT_WRITE_0_IRQ (1u << 31)
#define A510_PERF_CP_ALWAYS_COUNT 0u
#define A510_PERF_SP_ALU_WORKING  8u
#define A510_RBBM_STATUS_BUSY_BIT (1u << 31)

static inline void gpu_w32(const A510CoreOps *ops, uint32_t reg, uint32_t val) {
    ops->gpu_write32(ops->ctx, reg, val);
}

static inline uint32_t gpu_r32(const A510CoreOps *ops, uint32_t reg) {
    return ops->gpu_read32(ops->ctx, reg);
}

static inline void gpu_w64(const A510CoreOps *ops, uint32_t reg_lo, uint64_t val) {
    gpu_w32(ops, reg_lo, (uint32_t)(val & 0xffffffffu));
    gpu_w32(ops, reg_lo + 1u, (uint32_t)(val >> 32));
}

static inline void gpu_rmw(const A510CoreOps *ops, uint32_t reg, uint32_t clear_mask, uint32_t set_mask) {
    uint32_t v = gpu_r32(ops, reg);
    v = (v & ~clear_mask) | set_mask;
    gpu_w32(ops, reg, v);
}

static uint32_t ring_free_dwords(const A510Ring *ring) {
    uint32_t used = (ring->wptr - ring->rptr_cached) & A510_RING_MASK;
    return (A510_RING_DWORDS - 1u) - used;
}

static inline void ring_push(A510Ring *ring, uint32_t word) {
    ring->cpu_va[ring->wptr] = word;
    ring->wptr = (ring->wptr + 1u) & A510_RING_MASK;
}

static void ring_flush(const A510CoreOps *ops, A510Ring *ring, bool emit_where_am_i) {
    if (emit_where_am_i && ring->rptr_shadow_iova != 0u) {
        ring_push(ring, a510_pm4_type7_header(A510_CP_WHERE_AM_I, 2u));
        ring_push(ring, (uint32_t)(ring->rptr_shadow_iova & 0xffffffffu));
        ring_push(ring, (uint32_t)(ring->rptr_shadow_iova >> 32));
    }
    if (ops->wmb) {
        ops->wmb(ops->ctx);
    }
    gpu_w32(ops, A510_CP_RB_WPTR, ring->wptr);
}

A510CoreStatus a510_ring_init(A510Ring *ring,
                              uint32_t *cpu_va,
                              uint64_t gpu_iova,
                              uint64_t rptr_shadow_iova,
                              uint64_t fence_iova) {
    if (!ring || !cpu_va || (gpu_iova & 0xfffu) != 0u) {
        return A510_CORE_ERR_INVALID;
    }
    ring->cpu_va = cpu_va;
    ring->gpu_iova = gpu_iova;
    ring->rptr_shadow_iova = rptr_shadow_iova;
    ring->fence_iova = fence_iova;
    ring->wptr = 0u;
    ring->rptr_cached = 0u;
    ring->next_seqno = 1u;
    return A510_CORE_OK;
}

A510CoreStatus a510_wait_idle(const A510CoreOps *ops, const A510Ring *ring, uint32_t max_iters) {
    if (!ops || !ops->gpu_read32 || !ring) {
        return A510_CORE_ERR_INVALID;
    }
    for (uint32_t i = 0; i < max_iters; ++i) {
        /* Delay first and read RBBM_STATUS BEFORE CP_RB_RPTR: on Adreno 510,
         * an AHB read of CP_RB_RPTR briefly wakes CP_BUSY/PFP_BUSY/ME_BUSY (0x800001C1)
         * on the immediately following RBBM_STATUS read! */
        if (ops->udelay) {
            ops->udelay(ops->ctx, 20u);
        }
        uint32_t status = gpu_r32(ops, A510_RBBM_STATUS);
        if ((status & A510_RBBM_STATUS_BUSY_BIT) == 0u) {
            uint32_t rptr = gpu_r32(ops, A510_CP_RB_RPTR);
            if (rptr == ring->wptr) {
                return A510_CORE_OK;
            }
        }
    }
    return A510_CORE_ERR_IDLE_TIMEOUT;
}

A510CoreStatus a510_hw_init(const A510CoreOps *ops, A510Ring *ring, const A510HwInitConfig *cfg) {
    if (!ops || !ops->gpu_read32 || !ops->gpu_write32 || !ring || !ring->cpu_va || !cfg) {
        return A510_CORE_ERR_INVALID;
    }
    if (cfg->pm4_fw_iova == 0u || cfg->pfp_fw_iova == 0u || cfg->uche_trap_iova == 0u) {
        return A510_CORE_ERR_INVALID;
    }

    /* 0. Clear RBBM_SECVID_TRUST_CNTL = 0 at start of a5xx_start() (verified from stock Kenzo
     *    boot.img 0x409af0 & physical Kenzo Gate #3A G7 x0000) so CP PFP/ME boot in non-secure mode! */
    if (cfg->clear_secvid_trust_cntl) {
        gpu_w32(ops, A510_RBBM_SECVID_TRUST_CNTL, 0u);
    }

    /* 1. VBIF & RBBM baseline (verified against stock Kenzo boot.img 0x409af0..0x40a6ec) */
    gpu_w32(ops, A510_VBIF_ROUND_ROBIN_QOS_ARB, 0x00000003u);
    gpu_w32(ops, A510_RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffffu);
    gpu_w32(ops, A510_RBBM_AHB_CNTL0, 0x00000001u);
    gpu_w32(ops, A510_RBBM_INTERFACE_HANG_MASK_CNTL0 + 0u, 0xf0000000u);
    for (uint32_t m = 1u; m < 8u; ++m) {
        gpu_w32(ops, A510_RBBM_INTERFACE_HANG_MASK_CNTL0 + m, 0xffffffffu);
    }
    gpu_w32(ops, A510_RBBM_INTERFACE_HANG_INT_CNTL, 0x4003ffffu);
    gpu_w32(ops, A510_RBBM_PERFCTR_CNTL, 0x01u);
    gpu_w32(ops, A510_CP_PERFCTR_CP_SEL_0, A510_PERF_CP_ALWAYS_COUNT);
    gpu_w32(ops, A510_RBBM_PERFCTR_RBBM_SEL_0, 6u);
    gpu_w32(ops, A510_SP_PERFCTR_SP_SEL_0, A510_PERF_SP_ALU_WORKING);

    /* 2. UCHE cache ways, trap/write-thru base, and 256 KiB GMEM VA window */
    gpu_w32(ops, A510_UCHE_CACHE_WAYS, 0x02u);
    gpu_w64(ops, A510_UCHE_TRAP_BASE_LO, cfg->uche_trap_iova);
    gpu_w64(ops, A510_UCHE_WRITE_THRU_BASE_LO, cfg->uche_trap_iova);
    gpu_w64(ops, A510_UCHE_GMEM_RANGE_MIN_LO, A510_GMEM_BASE);
    gpu_w64(ops, A510_UCHE_GMEM_RANGE_MAX_LO, A510_GMEM_BASE + (uint64_t)A510_GMEM_BYTES - 1ull);

    /* 3. Adreno 510 specific CP thresholds, ECO controls, and HBB=14 */
    gpu_w32(ops, A510_CP_MEQ_THRESHOLDS, 0x20u);
    gpu_w32(ops, A510_CP_MERCIU_SIZE, 0x20u);
    gpu_w32(ops, A510_CP_ROQ_THRESHOLDS_2, 0x40000030u);
    gpu_w32(ops, A510_CP_ROQ_THRESHOLDS_1, 0x20100d0au);
    gpu_w32(ops, A510_PC_DBG_ECO_CNTL, (0x200u << 11) | (0x200u << 22));
    gpu_rmw(ops, A510_SP_DBG_ECO_CNTL, 0u, (1u << 25));
    gpu_w32(ops, A510_UCHE_MODE_CNTL, (1u << 29));
    gpu_w32(ops, A510_CP_CHICKEN_DBG, 0x02000000u);
    gpu_w32(ops, A510_RBBM_AHB_CNTL1, 0xa6ffffffu);
    gpu_rmw(ops, A510_RB_DBG_ECO_CNTL, (1u << 11), 0u);

    /* 4. Hardware clock gating (exact 56-register A510 table from stock Kenzo boot.img) */
    if (!cfg->skip_hwcg) {
        for (uint32_t i = 0; i < NATIVE_ADRENO_A510_HWCG_COUNT; ++i) {
            gpu_w32(ops, NativeAdrenoA510Hwcg[i][0], NativeAdrenoA510Hwcg[i][1]);
        }
        gpu_w32(ops, A510_RBBM_CLOCK_CNTL, 0xaaa8aa00u);
        gpu_w32(ops, A510_RBBM_ISDB_CNT, 0x00000182u);
    } else {
        gpu_w32(ops, A510_RBBM_CLOCK_CNTL, 0u);
        gpu_w32(ops, A510_RBBM_ISDB_CNT, 0u);
    }

    gpu_w32(ops, A510_RBBM_AHB_CNTL2, 0x0000003fu);
    gpu_w32(ops, A510_TPL1_MODE_CNTL, (A510_HIGHEST_BANK_BIT - 13u) << 7);

    /* 5. CP_PROTECT register windows (0..16; LAST_SPAN_INF_RANGE bit 2 cleared because slot 17 is unused) */
    if (!cfg->skip_cp_protect) {
        gpu_w32(ops, A510_CP_PROTECT_CNTL, 0x00000003u);
        for (uint32_t i = 0; i < NATIVE_ADRENO_A510_PROTECT_COUNT; ++i) {
            gpu_w32(ops, A510_CP_PROTECT + NativeAdrenoA510Protect[i][0], NativeAdrenoA510Protect[i][1]);
        }
    } else {
        gpu_w32(ops, A510_CP_PROTECT_CNTL, 0u);
    }

    /* 6. Disable TSB secure range & put all 12 A510 blocks into 64-bit addressing mode */
    gpu_w32(ops, A510_RBBM_SECVID_TSB_CNTL, 0u);
    gpu_w64(ops, A510_RBBM_SECVID_TSB_TRUSTED_BASE_LO, 0ull);
    gpu_w32(ops, A510_RBBM_SECVID_TSB_TRUSTED_SIZE, 0u);

    const uint32_t addr_mode_regs[] = {
        A510_CP_ADDR_MODE_CNTL,
        A510_VSC_ADDR_MODE_CNTL,
        A510_GRAS_ADDR_MODE_CNTL,
        A510_RB_ADDR_MODE_CNTL,
        A510_PC_ADDR_MODE_CNTL,
        A510_HLSQ_ADDR_MODE_CNTL,
        A510_VFD_ADDR_MODE_CNTL,
        A510_VPC_ADDR_MODE_CNTL,
        A510_UCHE_ADDR_MODE_CNTL,
        A510_SP_ADDR_MODE_CNTL,
        A510_TPL1_ADDR_MODE_CNTL,
        A510_RBBM_SECVID_TSB_ADDR_MODE_CNTL,
    };
    for (uint32_t i = 0; i < (sizeof(addr_mode_regs) / sizeof(addr_mode_regs[0])); ++i) {
        gpu_w32(ops, addr_mode_regs[i], 0x1u);
    }

    /* 7. Load CP microcode addresses & single ringbuffer (num_rings = 1) */
    gpu_w64(ops, A510_CP_ME_INSTR_BASE_LO, cfg->pm4_fw_iova);
    gpu_w64(ops, A510_CP_PFP_INSTR_BASE_LO, cfg->pfp_fw_iova);
    gpu_w64(ops, A510_CP_RB_BASE, ring->gpu_iova);
    gpu_w32(ops, A510_CP_RB_CNTL, A510_CP_RB_CNTL_DEFAULT);
    if (ring->rptr_shadow_iova != 0u) {
        gpu_w64(ops, A510_CP_RB_RPTR_ADDR, ring->rptr_shadow_iova);
    }

    /* 8. Enable RBBM interrupts & release CP PFP/ME from halt */
    gpu_w32(ops, A510_RBBM_INT_0_MASK, A510_RBBM_INT_0_MASK_DEFAULT);
    gpu_w32(ops, A510_CP_PFP_ME_CNTL, 0u);

    /* 9. Emit CP_ME_INIT verified bit-for-bit from stock Kenzo boot.img (0x40843c) for a530_pm4.fw (0x5ff066):
     *    count=7 (0x70c80007), arg0=0x0000000f, arg1=0x00000003 (multi-ctx), arg2=0x20000000 (err detect), arg3..6=0. */
    if (ring_free_dwords(ring) < 8u) {
        return A510_CORE_ERR_RING_FULL;
    }
    ring_push(ring, a510_pm4_type7_header(A510_CP_ME_INIT, 7u));
    ring_push(ring, 0x0000000fu);
    ring_push(ring, 0x00000003u); /* multiple HW contexts */
    ring_push(ring, 0x20000000u); /* error detection */
    ring_push(ring, 0x00000000u);
    ring_push(ring, 0x00000000u);
    ring_push(ring, 0x00000000u);
    ring_push(ring, 0x00000000u);
    ring_flush(ops, ring, false);

    A510CoreStatus st = a510_wait_idle(ops, ring, 5000u);
    if (st != A510_CORE_OK) {
        return st;
    }
    ring->rptr_cached = ring->wptr;
    return A510_CORE_OK;
}

A510CoreStatus a510_ring_submit_smoke_write_ex(const A510CoreOps *ops,
                                               A510Ring *ring,
                                               uint64_t target_iova,
                                               uint32_t magic_word,
                                               bool emit_event_write,
                                               bool emit_event_irq,
                                               uint32_t *out_seqno) {
    if (!ops || !ops->gpu_write32 || !ring || !ring->cpu_va || target_iova == 0u || ring->fence_iova == 0u) {
        return A510_CORE_ERR_INVALID;
    }
    /* CP_MEM_WRITE (4) + CP_WAIT_FOR_IDLE (1) + CP_EVENT_WRITE (5) = 10 dwords */
    if (ring_free_dwords(ring) < 13u) {
        ring->rptr_cached = gpu_r32(ops, A510_CP_RB_RPTR);
        if (ring_free_dwords(ring) < 13u) {
            return A510_CORE_ERR_RING_FULL;
        }
    }

    uint32_t seqno = ring->next_seqno++;
    if (ring->next_seqno == 0u) ring->next_seqno = 1u;

    ring_push(ring, a510_pm4_type7_header(A510_CP_MEM_WRITE, 3u));
    ring_push(ring, (uint32_t)(target_iova & 0xffffffffu));
    ring_push(ring, (uint32_t)(target_iova >> 32));
    ring_push(ring, magic_word);

    ring_push(ring, a510_pm4_type7_header(A510_CP_WAIT_FOR_IDLE, 0u));

    if (emit_event_write) {
        uint32_t ev = (A510_EVENT_CACHE_FLUSH_TS & 0xffu) | (emit_event_irq ? A510_CP_EVENT_WRITE_0_IRQ : 0u);
        ring_push(ring, a510_pm4_type7_header(A510_CP_EVENT_WRITE, 4u));
        ring_push(ring, ev);
        ring_push(ring, (uint32_t)(ring->fence_iova & 0xffffffffu));
        ring_push(ring, (uint32_t)(ring->fence_iova >> 32));
        ring_push(ring, seqno);
    }

    ring_flush(ops, ring, false);
    if (out_seqno) {
        *out_seqno = seqno;
    }
    return A510_CORE_OK;
}

A510CoreStatus a510_ring_submit_smoke_write(const A510CoreOps *ops,
                                            A510Ring *ring,
                                            uint64_t target_iova,
                                            uint32_t magic_word,
                                            uint32_t *out_seqno) {
    return a510_ring_submit_smoke_write_ex(ops, ring, target_iova, magic_word, true, true, out_seqno);
}

A510CoreStatus a510_ring_submit_ibs_ex(const A510CoreOps *ops,
                                       A510Ring *ring,
                                       const A510IbDesc *ibs,
                                       size_t ib_count,
                                       bool emit_event_irq,
                                       uint32_t *out_seqno) {
    if (!ops || !ops->gpu_write32 || !ring || !ring->cpu_va || !ibs || ib_count == 0u || ib_count > 128u ||
        ring->fence_iova == 0u) {
        return A510_CORE_ERR_INVALID;
    }
    for (size_t i = 0; i < ib_count; ++i) {
        if (ibs[i].iova == 0u || (ibs[i].iova & 3u) != 0u || ibs[i].dwords == 0u) {
            return A510_CORE_ERR_INVALID;
        }
    }

    uint32_t needed = (uint32_t)(ib_count * 4u + 1u + 5u + 3u);
    if (ring_free_dwords(ring) < needed) {
        ring->rptr_cached = gpu_r32(ops, A510_CP_RB_RPTR);
        if (ring_free_dwords(ring) < needed) {
            return A510_CORE_ERR_RING_FULL;
        }
    }

    uint32_t seqno = ring->next_seqno++;
    if (ring->next_seqno == 0u) ring->next_seqno = 1u;

    for (size_t i = 0; i < ib_count; ++i) {
        ring_push(ring, a510_pm4_type7_header(A510_CP_INDIRECT_BUFFER_PFE, 3u));
        ring_push(ring, (uint32_t)(ibs[i].iova & 0xffffffffu));
        ring_push(ring, (uint32_t)(ibs[i].iova >> 32));
        ring_push(ring, ibs[i].dwords);
    }

    ring_push(ring, a510_pm4_type7_header(A510_CP_WAIT_FOR_IDLE, 0u));

    {
        uint32_t ev = (A510_EVENT_CACHE_FLUSH_TS & 0xffu) | (emit_event_irq ? A510_CP_EVENT_WRITE_0_IRQ : 0u);
        ring_push(ring, a510_pm4_type7_header(A510_CP_EVENT_WRITE, 4u));
        ring_push(ring, ev);
        ring_push(ring, (uint32_t)(ring->fence_iova & 0xffffffffu));
        ring_push(ring, (uint32_t)(ring->fence_iova >> 32));
        ring_push(ring, seqno);
    }

    ring_flush(ops, ring, false);
    if (out_seqno) {
        *out_seqno = seqno;
    }
    return A510_CORE_OK;
}

A510CoreStatus a510_ring_submit_ibs(const A510CoreOps *ops,
                                    A510Ring *ring,
                                    const A510IbDesc *ibs,
                                    size_t ib_count,
                                    uint32_t *out_seqno) {
    return a510_ring_submit_ibs_ex(ops, ring, ibs, ib_count, true, out_seqno);
}
