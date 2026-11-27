/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_A510_HWINIT_H
#define NATIVE_ADRENO_A510_HWINIT_H
#include <stdint.h>
/* Adreno 510 hardware init tables verified bit-for-bit against stock Redmi Note 3
 * (kenzo / MSM8956) boot.img (0x409af0..0x40a738 & 0xd780b8) and Mesa a5xx.xml.
 * All offsets are DWORD register offsets inside kgsl_3d0_reg_memory (0x01c00000). */

#define A510_CHIP_ID_EXPECTED 0x05010000u
#define A510_GMEM_BASE        0x00100000ull
#define A510_GMEM_BYTES       0x00040000u  /* 256 KiB */
#define A510_HIGHEST_BANK_BIT 14u          /* HBB = 14 -> TPL1_MODE_CNTL (14-13)<<7 */
#define A510_NUM_SP_CORES     2u
#define A510_NUM_RINGS        1u           /* Single ring; A510 preemption disabled */

#define A510_RBBM_STATUS 0x004f5u
#define A510_RBBM_STATUS3 0x00530u
#define A510_RBBM_ISDB_CNT 0x00533u
#define A510_RBBM_ALWAYSON_COUNTER_LO 0x004d2u
#define A510_RBBM_ALWAYSON_COUNTER_HI 0x004d3u
#define A510_RBBM_INT_0_STATUS 0x004e1u
#define A510_RBBM_INT_0_MASK 0x00038u
#define A510_RBBM_INT_CLEAR_CMD 0x00037u
#define A510_RBBM_SW_RESET_CMD 0x00043u
#define A510_RBBM_AHB_CNTL0 0x00093u
#define A510_RBBM_AHB_CNTL1 0x00094u
#define A510_RBBM_AHB_CNTL2 0x00095u
#define A510_RBBM_INTERFACE_HANG_MASK_CNTL0 0x0009cu
#define A510_RBBM_PERFCTR_CNTL 0x00464u
#define A510_RBBM_PERFCTR_GPU_BUSY_MASKED 0x0046fu
#define A510_RBBM_PERFCTR_RBBM_SEL_0 0x0046bu
#define A510_RBBM_INTERFACE_HANG_INT_CNTL 0x0002fu
#define A510_RBBM_SECVID_TSB_CNTL 0x0f803u
#define A510_RBBM_SECVID_TSB_TRUSTED_BASE_LO 0x0f800u
#define A510_RBBM_SECVID_TSB_TRUSTED_BASE_HI 0x0f801u
#define A510_RBBM_SECVID_TSB_TRUSTED_SIZE 0x0f802u
#define A510_RBBM_SECVID_TRUST_CNTL 0x0f400u
#define A510_RBBM_SECVID_TSB_ADDR_MODE_CNTL 0x0f810u
#define A510_RBBM_CLOCK_CNTL 0x00067u
#define A510_CP_RB_BASE 0x00800u
#define A510_CP_RB_BASE_HI 0x00801u
#define A510_CP_RB_CNTL 0x00802u
#define A510_CP_RB_RPTR_ADDR 0x00804u
#define A510_CP_RB_RPTR_ADDR_HI 0x00805u
#define A510_CP_RB_RPTR 0x00806u
#define A510_CP_RB_WPTR 0x00807u
#define A510_CP_PFP_ME_CNTL 0x00832u
#define A510_CP_ME_INSTR_BASE_LO 0x00838u
#define A510_CP_ME_INSTR_BASE_HI 0x00839u
#define A510_CP_PFP_INSTR_BASE_LO 0x00835u
#define A510_CP_PFP_INSTR_BASE_HI 0x00836u
#define A510_CP_MEQ_THRESHOLDS 0x00825u
#define A510_CP_MERCIU_SIZE 0x00826u
#define A510_CP_ROQ_THRESHOLDS_1 0x0081fu
#define A510_CP_ROQ_THRESHOLDS_2 0x00820u
#define A510_CP_PROTECT_CNTL 0x008a0u
#define A510_CP_PROTECT 0x00880u
#define A510_CP_CHICKEN_DBG 0x00833u
#define A510_CP_HW_FAULT 0x00b1au
#define A510_CP_INTERRUPT_STATUS 0x00b18u
#define A510_CP_PERFCTR_CP_SEL_0 0x00bb0u
#define A510_CP_ADDR_MODE_CNTL 0x00860u
#define A510_UCHE_CACHE_WAYS 0x00e96u
#define A510_UCHE_TRAP_BASE 0x00e89u
#define A510_UCHE_TRAP_BASE_LO 0x00e89u
#define A510_UCHE_TRAP_BASE_HI 0x00e8au
#define A510_UCHE_WRITE_THRU_BASE 0x00e87u
#define A510_UCHE_WRITE_THRU_BASE_LO 0x00e87u
#define A510_UCHE_WRITE_THRU_BASE_HI 0x00e88u
#define A510_UCHE_GMEM_RANGE_MIN 0x00e8bu
#define A510_UCHE_GMEM_RANGE_MIN_LO 0x00e8bu
#define A510_UCHE_GMEM_RANGE_MIN_HI 0x00e8cu
#define A510_UCHE_GMEM_RANGE_MAX 0x00e8du
#define A510_UCHE_GMEM_RANGE_MAX_LO 0x00e8du
#define A510_UCHE_GMEM_RANGE_MAX_HI 0x00e8eu
#define A510_UCHE_MODE_CNTL 0x00e81u
#define A510_UCHE_ADDR_MODE_CNTL 0x00e80u
#define A510_VBIF_ROUND_ROBIN_QOS_ARB 0x03049u
#define A510_TPL1_MODE_CNTL 0x00f02u
#define A510_RB_DBG_ECO_CNTL 0x00cc4u
#define A510_PC_DBG_ECO_CNTL 0x00d00u
#define A510_SP_DBG_ECO_CNTL 0x00ec0u
#define A510_SP_PERFCTR_SP_SEL_0 0x00ed0u
#define A510_VSC_ADDR_MODE_CNTL 0x00bc1u
#define A510_GRAS_ADDR_MODE_CNTL 0x00c81u
#define A510_RB_ADDR_MODE_CNTL 0x00cc5u
#define A510_PC_ADDR_MODE_CNTL 0x00d01u
#define A510_HLSQ_ADDR_MODE_CNTL 0x00e05u
#define A510_VFD_ADDR_MODE_CNTL 0x00e41u
#define A510_VPC_ADDR_MODE_CNTL 0x00e61u
#define A510_SP_ADDR_MODE_CNTL 0x00ec1u
#define A510_TPL1_ADDR_MODE_CNTL 0x00f01u

/* Exact 56-register A510 HWCG table verified bit-for-bit from stock Kenzo boot.img (0xd780b8) */
static const uint32_t NativeAdrenoA510Hwcg[][2] __attribute__((unused)) = {
    { 0x00068u, 0x02222222u }, /* RBBM_CLOCK_CNTL_SP0 */
    { 0x00069u, 0x02222222u }, /* RBBM_CLOCK_CNTL_SP1 */
    { 0x0006cu, 0x02222220u }, /* RBBM_CLOCK_CNTL2_SP0 */
    { 0x0006du, 0x02222220u }, /* RBBM_CLOCK_CNTL2_SP1 */
    { 0x00070u, 0x0000f3cfu }, /* RBBM_CLOCK_HYST_SP0 */
    { 0x00071u, 0x0000f3cfu }, /* RBBM_CLOCK_HYST_SP1 */
    { 0x00074u, 0x00000080u }, /* RBBM_CLOCK_DELAY_SP0 */
    { 0x00075u, 0x00000080u }, /* RBBM_CLOCK_DELAY_SP1 */
    { 0x0004au, 0x22222222u }, /* RBBM_CLOCK_CNTL_TP0 */
    { 0x0004bu, 0x22222222u }, /* RBBM_CLOCK_CNTL_TP1 */
    { 0x0004eu, 0x22222222u }, /* RBBM_CLOCK_CNTL2_TP0 */
    { 0x0004fu, 0x22222222u }, /* RBBM_CLOCK_CNTL2_TP1 */
    { 0x00052u, 0x00002222u }, /* RBBM_CLOCK_CNTL3_TP0 */
    { 0x00053u, 0x00002222u }, /* RBBM_CLOCK_CNTL3_TP1 */
    { 0x000b0u, 0x77777777u }, /* RBBM_CLOCK_HYST_TP0 */
    { 0x000b1u, 0x77777777u }, /* RBBM_CLOCK_HYST_TP1 */
    { 0x000b4u, 0x77777777u }, /* RBBM_CLOCK_HYST2_TP0 */
    { 0x000b5u, 0x77777777u }, /* RBBM_CLOCK_HYST2_TP1 */
    { 0x000b8u, 0x00007777u }, /* RBBM_CLOCK_HYST3_TP0 */
    { 0x000b9u, 0x00007777u }, /* RBBM_CLOCK_HYST3_TP1 */
    { 0x000a4u, 0x11111111u }, /* RBBM_CLOCK_DELAY_TP0 */
    { 0x000a5u, 0x11111111u }, /* RBBM_CLOCK_DELAY_TP1 */
    { 0x000a8u, 0x11111111u }, /* RBBM_CLOCK_DELAY2_TP0 */
    { 0x000a9u, 0x11111111u }, /* RBBM_CLOCK_DELAY2_TP1 */
    { 0x000acu, 0x00001111u }, /* RBBM_CLOCK_DELAY3_TP0 */
    { 0x000adu, 0x00001111u }, /* RBBM_CLOCK_DELAY3_TP1 */
    { 0x0005au, 0x22222222u }, /* RBBM_CLOCK_CNTL_UCHE */
    { 0x0005bu, 0x22222222u }, /* RBBM_CLOCK_CNTL2_UCHE */
    { 0x0005cu, 0x22222222u }, /* RBBM_CLOCK_CNTL3_UCHE */
    { 0x0005du, 0x00222222u }, /* RBBM_CLOCK_CNTL4_UCHE */
    { 0x0005eu, 0x00444444u }, /* RBBM_CLOCK_HYST_UCHE */
    { 0x0005fu, 0x00000002u }, /* RBBM_CLOCK_DELAY_UCHE */
    { 0x00078u, 0x22222222u }, /* RBBM_CLOCK_CNTL_RB0 */
    { 0x00079u, 0x22222222u }, /* RBBM_CLOCK_CNTL_RB1 */
    { 0x0007cu, 0x00222222u }, /* RBBM_CLOCK_CNTL2_RB0 */
    { 0x0007du, 0x00222222u }, /* RBBM_CLOCK_CNTL2_RB1 */
    { 0x00082u, 0x00022220u }, /* RBBM_CLOCK_CNTL_CCU0 */
    { 0x00083u, 0x00022220u }, /* RBBM_CLOCK_CNTL_CCU1 */
    { 0x0008au, 0x05522222u }, /* RBBM_CLOCK_CNTL_RAC */
    { 0x0008bu, 0x00505555u }, /* RBBM_CLOCK_CNTL2_RAC */
    { 0x00086u, 0x04040404u }, /* RBBM_CLOCK_HYST_RB_CCU0 */
    { 0x00087u, 0x04040404u }, /* RBBM_CLOCK_HYST_RB_CCU1 */
    { 0x00080u, 0x07444044u }, /* RBBM_CLOCK_HYST_RAC */
    { 0x0008cu, 0x00000002u }, /* RBBM_CLOCK_DELAY_RB_CCU_L1_0 */
    { 0x0008du, 0x00000002u }, /* RBBM_CLOCK_DELAY_RB_CCU_L1_1 */
    { 0x00081u, 0x00010011u }, /* RBBM_CLOCK_DELAY_RAC */
    { 0x00063u, 0x04222222u }, /* RBBM_CLOCK_CNTL_TSE_RAS_RBBM */
    { 0x00060u, 0x02222222u }, /* RBBM_CLOCK_MODE_GPC */
    { 0x00091u, 0x00002222u }, /* RBBM_CLOCK_MODE_VFD */
    { 0x00064u, 0x00000000u }, /* RBBM_CLOCK_HYST_TSE_RAS_RBBM */
    { 0x00062u, 0x04104004u }, /* RBBM_CLOCK_HYST_GPC */
    { 0x00090u, 0x00000000u }, /* RBBM_CLOCK_HYST_VFD */
    { 0x00066u, 0x00000000u }, /* RBBM_CLOCK_DELAY_HLSQ */
    { 0x00065u, 0x00004000u }, /* RBBM_CLOCK_DELAY_TSE_RAS_RBBM */
    { 0x00061u, 0x00000200u }, /* RBBM_CLOCK_DELAY_GPC */
    { 0x00092u, 0x00002222u }, /* RBBM_CLOCK_DELAY_VFD */
};
#define NATIVE_ADRENO_A510_HWCG_COUNT 56u

/* Exact A5xx CP_PROTECT register encoding verified bit-for-bit from stock Kenzo boot.img (0x40a018..0x40a548):
 *   [30:29] = 0x3 (RW protect = 0x60000000)
 *   [28:24] = log2(len_dwords)
 *   [17:0]  = dword_offset << 2 (byte offset) */
#define A510_PROTECT_RW(off, log2_len) (0x60000000u | (((uint32_t)(log2_len) & 0x1fu) << 24) | (((uint32_t)(off) << 2) & 0x3ffffu))
static const uint32_t NativeAdrenoA510Protect[][2] __attribute__((unused)) = {
    {  0u, 0x62000010u }, /* RW off=0x00004 len=4   */
    {  1u, 0x63000020u }, /* RW off=0x00008 len=8   */
    {  2u, 0x64000040u }, /* RW off=0x00010 len=16  */
    {  3u, 0x65000080u }, /* RW off=0x00020 len=32  */
    {  4u, 0x66000100u }, /* RW off=0x00040 len=64  */
    {  5u, 0x66000200u }, /* RW off=0x00080 len=64  */
    {  6u, 0x6403e000u }, /* RW off=0x0f800 len=16  (SECVID_TSB) */
    {  7u, 0x6103d000u }, /* RW off=0x0f400 len=2   (SECVID_TRUST) */
    {  8u, 0x66002000u }, /* RW off=0x00800 len=64  (CP_RB) */
    {  9u, 0x63002100u }, /* RW off=0x00840 len=8   */
    { 10u, 0x65002200u }, /* RW off=0x00880 len=32  (CP_PROTECT) */
    { 11u, 0x60002a80u }, /* RW off=0x00aa0 len=1   */
    { 12u, 0x60003300u }, /* RW off=0x00cc0 len=1   */
    { 13u, 0x610033c0u }, /* RW off=0x00cf0 len=2   */
    { 14u, 0x630039a0u }, /* RW off=0x00e68 len=8   */
    { 15u, 0x640039c0u }, /* RW off=0x00e70 len=16  */
    { 16u, 0x64003a00u }, /* RW off=0x00e80 len=16  (UCHE) */
};
#define NATIVE_ADRENO_A510_PROTECT_COUNT 17u
#endif
