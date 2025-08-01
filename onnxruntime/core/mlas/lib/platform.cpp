/*++

Copyright (c) Microsoft Corporation. All rights reserved.

Licensed under the MIT License.

Module Name:

    platform.cpp

Abstract:

    This module implements logic to select the best configuration for the
    this platform.

--*/

#include "mlasi.h"

#if defined(USE_KLEIDIAI) && !defined(_MSC_VER)
#include "kleidiai/mlasi_kleidiai.h"
#endif

#include <thread>
#include <mutex>

#if defined(MLAS_TARGET_POWER)
#if defined(__linux__)
#include <sys/auxv.h>
#elif defined(_AIX)
#define POWER_10       0x40000
#define POWER_10_ANDUP (POWER_10)
#include <sys/systemcfg.h>
#define __power_10_andup() (_system_configuration.implementation & POWER_10_ANDUP)
#endif
#endif

#if defined(MLAS_TARGET_ARM64)
#if defined(_WIN32)

// N.B. Support building with downlevel versions of the Windows SDK.
#ifndef PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE
#define PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE 43
#endif

#if defined(BUILD_MLAS_NO_ONNXRUNTIME)
MLASCPUIDInfo::MLASCPUIDInfo()
{
    has_arm_neon_dot_ = (IsProcessorFeaturePresent(PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE) != 0);

    // raw hack! Need CPUIDInfo implementation for more precise detection
    has_fp16_ = has_arm_neon_dot_;
}
#endif

#elif defined(__linux__)

#include <sys/auxv.h>
#include <asm/hwcap.h>
// N.B. Support building with older versions of asm/hwcap.h that do not define
// this capability bit.
#ifndef HWCAP_ASIMDDP
#define HWCAP_ASIMDDP (1 << 20)
#endif

#ifndef HWCAP2_I8MM
#define HWCAP2_I8MM (1 << 13)
#endif

#ifndef HWCAP2_SVEI8MM
#define HWCAP2_SVEI8MM (1 << 9)
#endif

#ifndef HWCAP2_BF16
#define HWCAP2_BF16 (1 << 14)
#endif

#if defined(BUILD_MLAS_NO_ONNXRUNTIME)
MLASCPUIDInfo::MLASCPUIDInfo()
{
    has_arm_neon_dot_ = ((getauxval(AT_HWCAP) & HWCAP_ASIMDDP) != 0);

    // raw hack! Need CPUIDInfo implementation for more precise detection
    has_fp16_ = has_arm_neon_dot_;

    has_arm_neon_i8mm_ = ((getauxval(AT_HWCAP2) & HWCAP2_I8MM) != 0);
    has_arm_sve_i8mm_ = ((getauxval(AT_HWCAP2) & HWCAP2_SVEI8MM) != 0);

    has_arm_neon_bf16_ = ((getauxval(AT_HWCAP2) & HWCAP2_BF16) != 0);
}
#endif

#else

#if defined(BUILD_MLAS_NO_ONNXRUNTIME)
MLASCPUIDInfo::MLASCPUIDInfo() {}
#endif

#endif // Windows vs Linux vs Unknown
#else // not MLAS_TARGET_ARM64

#if defined(BUILD_MLAS_NO_ONNXRUNTIME)
MLASCPUIDInfo::MLASCPUIDInfo() {}
#endif

#endif // MLAS_TARGET_ARM64

#ifdef MLAS_TARGET_AMD64_IX86

//
// Stores a vector to build a conditional load/store mask for vmaskmovps.
//

MLAS_INTERNAL_DATA MLAS_DECLSPEC_ALIGN(const uint32_t MlasMaskMoveAvx[8], 32) = { 0, 1, 2, 3, 4, 5, 6, 7 };

//
// Stores a table of AVX vmaskmovps/vmaskmovpd load/store masks.
//

MLAS_INTERNAL_DATA MLAS_DECLSPEC_ALIGN(const uint32_t MlasMaskMoveTableAvx[16], 32) = {
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

//
// Stores a table of AVX512 opmask register values.
//

MLAS_INTERNAL_DATA MLAS_DECLSPEC_ALIGN(const int16_t MlasOpmask16BitTableAvx512[16], 32) = {
    0x0000, 0x0001, 0x0003, 0x0007, 0x000F, 0x001F, 0x003F, 0x007F,
    0x00FF, 0x01FF, 0x03FF, 0x07FF, 0x0FFF, 0x1FFF, 0x3FFF, 0x7FFF,
};

//
// Reads the processor extended control register to determine platform
// capabilities.
//

#if !defined(_XCR_XFEATURE_ENABLED_MASK)
#define _XCR_XFEATURE_ENABLED_MASK 0
#endif

#if !defined(XFEATURE_MASK_XTILE)
#define XFEATURE_XTILECFG 17
#define XFEATURE_XTILEDATA 18
#define XFEATURE_MASK_XTILECFG (1 << XFEATURE_XTILECFG)
#define XFEATURE_MASK_XTILEDATA (1 << XFEATURE_XTILEDATA)
#define XFEATURE_MASK_XTILE (XFEATURE_MASK_XTILECFG | XFEATURE_MASK_XTILEDATA)
#endif

inline
uint64_t
MlasReadExtendedControlRegister(
    unsigned int ext_ctrl_reg
)
{
#if defined(_WIN32)
    return _xgetbv(ext_ctrl_reg);
#else
    uint32_t eax, edx;

    __asm__
    (
        "xgetbv"
        : "=a" (eax), "=d" (edx)
        : "c" (ext_ctrl_reg)
    );

    return ((uint64_t)edx << 32) | eax;
#endif
}

#if defined(__linux__)
#include <sys/syscall.h>
#endif

bool
MlasInitAMX()
{
#if defined(__linux__)

#define ARCH_GET_XCOMP_PERM 0x1022
#define ARCH_REQ_XCOMP_PERM 0x1023

    unsigned long bitmask = 0;
    long rc = syscall(SYS_arch_prctl, ARCH_REQ_XCOMP_PERM, XFEATURE_XTILEDATA);
    if (rc) {
        return false;
    }
    rc = syscall(SYS_arch_prctl, ARCH_GET_XCOMP_PERM, &bitmask);
    if (rc) {
        return false;
    }
    if (bitmask & XFEATURE_MASK_XTILE) {
        return true;
    }
    return false;
#else
    return true;
#endif
}

#endif // MLAS_TARGET_AMD64_IX86

#ifdef MLAS_TARGET_LARCH64

#if defined(__linux__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif
//
// Stores a vector to build a conditional load/store mask for vmaskmovps.
//

MLAS_INTERNAL_DATA MLAS_DECLSPEC_ALIGN(const uint32_t MlasMaskMoveLasx[8], 32) = { 0, 1, 2, 3, 4, 5, 6, 7 };

//
// Stores a table of AVX vmaskmovps/vmaskmovpd load/store masks.
//

MLAS_INTERNAL_DATA MLAS_DECLSPEC_ALIGN(const uint32_t MlasMaskMoveTableLasx[16], 32) = {
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
};

#endif
MLAS_PLATFORM::MLAS_PLATFORM(
    void
    )
/*++

Routine Description:

    This routine initializes the platform support for this library.

Arguments:

    None.

Return Value:

    None.

--*/
{

    this->ConvDepthwiseU8S8Kernel = MlasConvDepthwiseKernel<uint8_t, int8_t>;
    this->ConvDepthwiseU8U8Kernel = MlasConvDepthwiseKernel<uint8_t, uint8_t>;
    this->ConvDepthwiseS8S8Kernel = MlasConvDepthwiseKernel<int8_t, int8_t>;
    this->ConvDepthwiseS8U8Kernel = MlasConvDepthwiseKernel<int8_t, uint8_t>;
    this->CastF16ToF32Kernel = nullptr;
    this->CastF32ToF16Kernel = nullptr;

#if defined(MLAS_TARGET_AMD64_IX86)

    //
    // Default to the baseline SSE2 support.
    //

    this->GemmFloatKernel = MlasGemmFloatKernelSse;
    this->GemmU8S8Dispatch = &MlasGemmU8X8DispatchSse;
    this->GemmU8U8Dispatch = &MlasGemmU8X8DispatchSse;

#if defined(MLAS_TARGET_AMD64)

    this->TransposePackB16x4Routine = MlasSgemmTransposePackB16x4Sse;
    this->GemmDoubleKernel = MlasGemmDoubleKernelSse;
    this->ConvNchwFloatKernel = MlasConvNchwFloatKernelSse;
    this->ConvNchwcFloatKernel = MlasConvNchwcFloatKernelSse;
    this->ConvDepthwiseFloatKernel = MlasConvDepthwiseFloatKernelSse;
    this->ConvPointwiseFloatKernel = MlasConvPointwiseFloatKernelSse;
    this->PoolFloatKernel[MlasMaximumPooling] = MlasPoolMaximumFloatKernelSse;
    this->PoolFloatKernel[MlasAveragePoolingExcludePad] = MlasPoolAverageExcludePadFloatKernelSse;
    this->PoolFloatKernel[MlasAveragePoolingIncludePad] = MlasPoolAverageIncludePadFloatKernelSse;
    this->ComputeExpF32Kernel = MlasComputeExpF32Kernel;
    this->LogisticKernelRoutine = MlasLogisticKernel;
    this->TanhKernelRoutine = MlasTanhKernel;
    this->ErfKernelRoutine = MlasErfKernel;
    this->ComputeSumExpF32Kernel = MlasComputeSumExpF32Kernel;
    this->ComputeSoftmaxOutputF32Kernel = MlasComputeSoftmaxOutputF32Kernel;
    this->ComputeLogSoftmaxOutputF32Kernel = MlasComputeLogSoftmaxOutputF32Kernel;
    this->ReduceMaximumF32Kernel = MlasReduceMaximumF32Kernel;
    this->ReduceMinimumMaximumF32Kernel = MlasReduceMinimumMaximumF32Kernel;
    this->QLinearAddS8Kernel = MlasQLinearAddS8Kernel;
    this->QLinearAddU8Kernel = MlasQLinearAddU8Kernel;
    this->QuantizeLinearS8Kernel = MlasQuantizeLinearS8Kernel;
    this->QuantizeLinearU8Kernel = MlasQuantizeLinearU8Kernel;
    this->QuantizeLinearS16Kernel = MlasQuantizeLinearS16Kernel;
    this->QuantizeLinearU16Kernel = MlasQuantizeLinearU16Kernel;
    this->QuantizeLinearS4Kernel = MlasQuantizeLinearS4Kernel;
    this->QuantizeLinearU4Kernel = MlasQuantizeLinearU4Kernel;
    this->DequantizeLinearS8Kernel = MlasDequantizeLinearS8Kernel;
    this->DequantizeLinearU8Kernel = MlasDequantizeLinearU8Kernel;
#ifndef __APPLE__
#ifndef FORCE_GENERIC_ALGORITHMS
    this->CastF16ToF32Kernel = &MlasCastF16ToF32KernelSse;
#else  // FORCE_GENERIC_ALGORITHMS
    this->CastF16ToF32Kernel = nullptr;
#endif  // FORCE_GENERIC_ALGORITHMS
#endif  // __APPLE__

    this->NchwcBlockSize = 8;
    this->PreferredBufferAlignment = MLAS_DEFAULT_PREFERRED_BUFFER_ALIGNMENT;

    this->MaximumThreadCount = MLAS_MAXIMUM_THREAD_COUNT;

#endif

    unsigned Cpuid1[4];
#if defined(_WIN32)
    __cpuid((int*)Cpuid1, 1);
#else
    __cpuid(1, Cpuid1[0], Cpuid1[1], Cpuid1[2], Cpuid1[3]);
#endif

#if defined(_MSC_VER)

    //
    // Check if the processor supports SSE 4.1 instructions.
    //
#ifndef FORCE_GENERIC_ALGORITHMS
    if ((Cpuid1[2] & 0x80000) != 0) {
#else  // FORCE_GENERIC_ALGORITHMS
    if (false) {
#endif  // FORCE_GENERIC_ALGORITHMS
        this->GemmU8S8Dispatch = &MlasGemmU8S8DispatchSse41;
    }

#endif

    //
    // Check if the processor supports the AVX and OSXSAVE features.
    //

#ifndef FORCE_GENERIC_ALGORITHMS
    if ((Cpuid1[2] & 0x18000000) == 0x18000000) {
#else  // FORCE_GENERIC_ALGORITHMS
    if (false) {
#endif  // FORCE_GENERIC_ALGORITHMS

        //
        // Check if the operating system supports saving SSE and AVX states.
        //

        uint64_t xcr0 = MlasReadExtendedControlRegister(_XCR_XFEATURE_ENABLED_MASK);

        if ((xcr0 & 0x6) == 0x6) {

            this->GemmFloatKernel = MlasGemmFloatKernelAvx;

#if defined(MLAS_TARGET_AMD64)

            this->KernelM1Routine = MlasSgemmKernelM1Avx;
            this->KernelM1TransposeBRoutine = MlasSgemmKernelM1TransposeBAvx;
            this->TransposePackB16x4Routine = MlasSgemmTransposePackB16x4Avx;
            this->GemmDoubleKernel = MlasGemmDoubleKernelAvx;
            this->ConvNchwFloatKernel = MlasConvNchwFloatKernelAvx;
            this->ConvNchwcFloatKernel = MlasConvNchwcFloatKernelAvx;
            this->ConvDepthwiseFloatKernel = MlasConvDepthwiseFloatKernelAvx;
            this->ConvPointwiseFloatKernel = MlasConvPointwiseFloatKernelAvx;
            this->PoolFloatKernel[MlasMaximumPooling] = MlasPoolMaximumFloatKernelAvx;
            this->PoolFloatKernel[MlasAveragePoolingExcludePad] = MlasPoolAverageExcludePadFloatKernelAvx;
            this->PoolFloatKernel[MlasAveragePoolingIncludePad] = MlasPoolAverageIncludePadFloatKernelAvx;
            this->ComputeSoftmaxOutputF32Kernel = MlasComputeSoftmaxOutputF32KernelAvx;
            this->ComputeLogSoftmaxOutputF32Kernel = MlasComputeLogSoftmaxOutputF32KernelAvx;
            this->ReduceMaximumF32Kernel = MlasReduceMaximumF32KernelAvx;
            this->ReduceMinimumMaximumF32Kernel = MlasReduceMinimumMaximumF32KernelAvx;
            this->GemmU8U8Kernel = nullptr;

            //
            // Check if the processor supports AVX2/FMA3 features.
            //

            unsigned Cpuid7[4];
#if defined(_WIN32)
            __cpuidex((int*)Cpuid7, 7, 0);
#else
            __cpuid_count(7, 0, Cpuid7[0], Cpuid7[1], Cpuid7[2], Cpuid7[3]);
#endif

            if (((Cpuid1[2] & 0x1000) != 0) && ((Cpuid7[1] & 0x20) != 0)) {

                this->Avx2Supported_ = true;

                this->GemmU8S8Dispatch = &MlasGemmU8S8DispatchAvx2;
                this->GemmU8S8Kernel = MlasGemmU8S8KernelAvx2;
                this->GemvU8S8Kernel = MlasGemvU8S8KernelAvx2;
                this->GemmU8U8Dispatch = &MlasGemmU8U8DispatchAvx2;
                this->GemmU8U8Kernel = MlasGemmU8U8KernelAvx2;
                this->ConvSymU8S8Dispatch = &MlasConvSymDispatchAvx2;

                this->GemmFloatKernel = MlasGemmFloatKernelFma3;
                this->GemmDoubleKernel = MlasGemmDoubleKernelFma3;
                this->ConvNchwFloatKernel = MlasConvNchwFloatKernelFma3;
                this->ConvNchwcFloatKernel = MlasConvNchwcFloatKernelFma3;
                this->ConvDepthwiseFloatKernel = MlasConvDepthwiseFloatKernelFma3;
                this->ConvPointwiseFloatKernel = MlasConvPointwiseFloatKernelFma3;
                this->ComputeExpF32Kernel = MlasComputeExpF32KernelFma3;
                this->LogisticKernelRoutine = MlasComputeLogisticF32KernelFma3;
                this->TanhKernelRoutine = MlasComputeTanhF32KernelFma3;
                this->ErfKernelRoutine = MlasErfKernelFma3;
                this->QLinearAddS8Kernel = MlasQLinearAddS8KernelAvx2;
                this->QLinearAddU8Kernel = MlasQLinearAddU8KernelAvx2;
                this->ConvDepthwiseU8S8Kernel = MlasConvDepthwiseKernelAvx2<uint8_t, int8_t>;
                this->ConvDepthwiseU8U8Kernel = MlasConvDepthwiseKernelAvx2<uint8_t, uint8_t>;
                this->ConvDepthwiseS8S8Kernel = MlasConvDepthwiseKernelAvx2<int8_t, int8_t>;
                this->ConvDepthwiseS8U8Kernel = MlasConvDepthwiseKernelAvx2<int8_t, uint8_t>;
                this->ComputeSumExpF32Kernel = MlasComputeSumExpF32KernelFma3;
                this->QNBitGemmDispatch = &MlasSQNBitGemmDispatchAvx2;
                this->CastF16ToF32Kernel = &MlasCastF16ToF32KernelAvx2;
                this->CastF32ToF16Kernel = &MlasCastF32ToF16KernelAvx2;
                this->RopeDispatch = &MlasRopeDispatchAvx2;


                //
                // Check if the processor supports Hybrid core architecture.
                //

                if ((Cpuid7[3] & 0x8000) != 0) {
                    this->MaximumThreadCount = MLAS_MAXIMUM_THREAD_COUNT * 4;
                }

                //
                // Check if the processor supports AVXVNNI features.
                //

                unsigned Cpuid7_1[4];
#if defined(_WIN32)
                __cpuidex((int*)Cpuid7_1, 7, 1);
#else
                __cpuid_count(7, 1, Cpuid7_1[0], Cpuid7_1[1], Cpuid7_1[2], Cpuid7_1[3]);
#endif

                if ((Cpuid7_1[0] & 0x10) != 0) {

                    this->GemmU8U8Dispatch = &MlasGemmU8S8DispatchAvx2;
                    this->GemmU8S8Kernel = MlasGemmU8S8KernelAvxVnni;
                    this->GemvU8S8Kernel = MlasGemvU8S8KernelAvxVnni;
                    this->ConvSymU8S8Dispatch = &MlasConvSymDispatchAvxVnni;
                    this->QNBitGemmDispatch = &MlasSQNBitGemmDispatchAvx2vnni;
                }

#if !defined(ORT_MINIMAL_BUILD)

                //
                // Check if the processor supports AVX512F features and the
                // operating system supports saving AVX512F state.
                //

                if (((Cpuid7[1] & 0x10000) != 0) && ((xcr0 & 0xE0) == 0xE0)) {

                    this->GemmFloatKernel = MlasGemmFloatKernelAvx512F;
                    this->GemmDoubleKernel = MlasGemmDoubleKernelAvx512F;
                    this->ConvNchwFloatKernel = MlasConvNchwFloatKernelAvx512F;
                    this->ConvNchwcFloatKernel = MlasConvNchwcFloatKernelAvx512F;
                    this->ConvDepthwiseFloatKernel = MlasConvDepthwiseFloatKernelAvx512F;
                    this->ConvPointwiseFloatKernel = MlasConvPointwiseFloatKernelAvx512F;
                    this->PoolFloatKernel[MlasMaximumPooling] = MlasPoolMaximumFloatKernelAvx512F;
                    this->PoolFloatKernel[MlasAveragePoolingExcludePad] = MlasPoolAverageExcludePadFloatKernelAvx512F;
                    this->PoolFloatKernel[MlasAveragePoolingIncludePad] = MlasPoolAverageIncludePadFloatKernelAvx512F;
                    this->ComputeExpF32Kernel = MlasComputeExpF32KernelAvx512F;
                    this->ComputeSumExpF32Kernel = MlasComputeSumExpF32KernelAvx512F;
                    this->ReduceMaximumF32Kernel = MlasReduceMaximumF32KernelAvx512F;
                    this->QuantizeLinearS8Kernel = MlasQuantizeLinearS8KernelAvx512F;
                    this->QuantizeLinearU8Kernel = MlasQuantizeLinearU8KernelAvx512F;
                    this->NchwcBlockSize = 16;
                    this->PreferredBufferAlignment = 64;

                    //
                    // Check if the processor supports AVX512 core features
                    // (AVX512BW/AVX512DQ/AVX512VL).
                    //

                    if ((Cpuid7[1] & 0xC0020000) == 0xC0020000) {

                        this->Avx512Supported_ = true;

                        this->GemmU8S8Kernel = MlasGemmU8S8KernelAvx512Core;
                        this->GemvU8S8Kernel = MlasGemvU8S8KernelAvx512Core;
                        this->GemmU8U8Kernel = MlasGemmU8U8KernelAvx512Core;
                        this->ConvSymU8S8Dispatch = &MlasConvSymDispatchAvx512Core;
                        this->FpQ4GemmDispatch = &MlasFpQ4GemmDispatchAvx512;
                        this->QNBitGemmDispatch = &MlasSQNBitGemmDispatchAvx512;

                        //
                        // Check if the processor supports AVX512VNNI.
                        //

                        if ((Cpuid7[2] & 0x800) != 0) {

                            this->GemmU8U8Dispatch = &MlasGemmU8S8DispatchAvx2;
                            this->GemmU8S8Kernel = MlasGemmU8S8KernelAvx512Vnni;
                            this->GemvU8S8Kernel = MlasGemvU8S8KernelAvx512Vnni;
                            this->ConvSymU8S8Dispatch = &MlasConvSymDispatchAvx512Vnni;
                            this->Q8Q4GemmDispatch = &MlasQ8Q4GemmDispatchAvx512vnni;
                            this->QNBitGemmDispatch = &MlasSQNBitGemmDispatchAvx512vnni;
                        }
                    }
                }

                //
                // Check if the processor supports AVX-VNNI-INT8
                //
                if ((Cpuid7_1[3] & 0x10) != 0) {
                    this->GemmU8U8Dispatch = &MlasGemmU8U8DispatchAvx2Vnni;
                    this->GemmS8S8Dispatch = &MlasGemmS8S8DispatchAvx2Vnni;
                    this->GemmS8S8Kernel = MlasGemmS8S8KernelAvx2Vnni;
                    this->GemmS8U8Dispatch = &MlasGemmS8U8DispatchAvx2Vnni;
                    this->GemmS8U8Kernel = MlasGemmS8U8KernelAvx2Vnni;
                }

#ifndef __APPLE__
#if (defined(_MSC_VER) && (_MSC_VER >= 1933)) || (defined(__GNUC__) && (__GNUC__ >= 13))
                //
                // Check if the processor supports AVX NE CONVERT.
                //
                if ((Cpuid7_1[3] & (0b1 << 5)) != 0) {
                    this->CastF16ToF32Kernel = &MlasCastF16ToF32KernelAvx;
                }
#endif  // (defined(_MSC_VER) && (_MSC_VER >= 1933)) || (defined(__GNUC__) && (__GNUC__ >= 13))


                //
                // Check if the processor supports AMX-TILE and AMX-INT8
                // features.
                //
                if ((Cpuid7[3] & 0b1 << 24) != 0 &&
                    (Cpuid7[3] & 0b1 << 25) != 0 &&
                    (xcr0 & XFEATURE_MASK_XTILE) == XFEATURE_MASK_XTILE) {
                    if (MlasInitAMX()) {
                        this->GemmU8U8Dispatch = &MlasGemmU8S8DispatchAmx;
                        this->GemmU8S8Dispatch = &MlasGemmU8S8DispatchAmx;
                    }
                }
#endif // __APPLE__

#endif // ORT_MINIMAL_BUILD

            }

#endif // MLAS_TARGET_AMD64

        }
    }

#endif // MLAS_TARGET_AMD64_IX86

#if defined(MLAS_TARGET_ARM64)

    this->GemmU8U8Dispatch = &MlasGemmU8X8DispatchNeon;
    this->GemmU8S8Dispatch = &MlasGemmX8S8DispatchNeon;
    this->GemmS8S8Dispatch = &MlasGemmX8S8DispatchNeon;
    this->SymmQgemmDispatch = &MlasSymmQgemmS8DispatchNeon;
    this->ConvSymU8S8Dispatch = &MlasConvSymU8DispatchNeon;
    this->ConvSymS8S8Dispatch = &MlasConvSymS8DispatchNeon;
    this->RopeDispatch = &MlasRopeDispatchNeon;
    this->HGemmDispatch = &MlasHGemmDispatchNeon;
    this->SoftmaxDispatch = &MlasSoftmaxDispatchNeon;
    this->EltwiseDispatch = &MlasEltwiseDispatchNeon;

    //
    // Check if the processor supports ASIMD dot product instructions.
    //

    // Note:
    // Do NOT use ID_AA64ISAR0_EL1. It causes illegal instruction errors on Mac M1 and ARMv8-A chips
    // as well as failing on other ARM chips as it is an EL1 level register that requires extra
    // privileges to read.
    //
    // uint64_t isar0_el1;
    // asm("mrs %[reg], ID_AA64ISAR0_EL1\n" : [reg] "=r"(isar0_el1) : :);
    // const bool HasDotProductInstructions = ((isar0_el1 >> 44) & 0xfu) == 0x1u;

    const bool HasDotProductInstructions = MLAS_CPUIDINFO::GetCPUIDInfo().HasArmNeonDot();

    if (HasDotProductInstructions) {
        this->GemmU8U8Dispatch = &MlasGemmU8X8DispatchUdot;
        this->GemmU8S8Dispatch = &MlasGemmU8X8DispatchUdot;
        this->GemmS8S8Dispatch = &MlasGemmS8S8DispatchSdot;
        this->SymmQgemmDispatch = &MlasSymmQgemmS8DispatchSdot;
        this->ConvSymU8S8Dispatch = &MlasConvSymU8DispatchDot;
        this->ConvSymS8S8Dispatch = &MlasConvSymS8DispatchDot;
    }

    this->QNBitGemmDispatch = &GetMlasQNBitGemmDispatchNeon(HasDotProductInstructions);
#if defined(USE_KLEIDIAI) && !defined(_MSC_VER)
    if(MLAS_CPUIDINFO::GetCPUIDInfo().HasArm_SME()){
        this->MlasGemmBatchOverride = ArmKleidiAI::MlasGemmBatch;
        this->MlasGemmPackBSizeOverride = ArmKleidiAI::MlasGemmPackBSize;
        this->MlasGemmPackBOverride = ArmKleidiAI::MlasGemmPackB;
        this->MlasConvPrepareOverride = ArmKleidiAI::MlasConvPrepare;
        this->MlasConvOverride = ArmKleidiAI::MlasConv;
    }
#endif

#if defined(__linux__)
    //
    // Check if the processor supports ASIMD I8MM instructions.
    //
    if (MLAS_CPUIDINFO::GetCPUIDInfo().HasArmNeon_I8MM()) {
        this->GemmU8U8Dispatch = &MlasGemmU8X8DispatchUmmla;
        this->GemmU8S8Dispatch = &MlasGemmU8X8DispatchUmmla;
        this->GemmS8S8Dispatch = &MlasGemmS8S8DispatchSmmla;
    }
#endif

#if defined(MLAS_F16VEC_INTRINSICS_SUPPORTED)
    this->CastF16ToF32Kernel = &MlasCastF16ToF32KernelNeon;
    this->CastF32ToF16Kernel = &MlasCastF32ToF16KernelNeon;
#endif

#endif // MLAS_TARGET_ARM64
#if defined(MLAS_TARGET_POWER)
    this->GemmFloatKernel = MlasSgemmKernel;
    this->GemmDoubleKernel = MlasDgemmKernel;
    this->QuantizeLinearS8Kernel = MlasQuantizeLinearS8Kernel;
    this->QuantizeLinearU8Kernel = MlasQuantizeLinearU8Kernel;
    this->QuantizeLinearS16Kernel = MlasQuantizeLinearS16Kernel;
    this->QuantizeLinearU16Kernel = MlasQuantizeLinearU16Kernel;
    this->QuantizeLinearS4Kernel = MlasQuantizeLinearS4Kernel;
    this->QuantizeLinearU4Kernel = MlasQuantizeLinearU4Kernel;

#if defined(__linux__)
    unsigned long hwcap2 = getauxval(AT_HWCAP2);

    bool HasP9Instructions = hwcap2 & PPC_FEATURE2_ARCH_3_00;
#elif defined(_AIX)
    bool HasP9Instructions = __power_9_andup();
#elif defined(__FreeBSD__)
    unsigned long hwcap2;
    elf_aux_info(AT_HWCAP2, &hwcap2, sizeof(hwcap2));

    bool HasP9Instructions = hwcap2 & PPC_FEATURE2_ARCH_3_00;
#endif // __linux__
    if (HasP9Instructions) {
        this->QuantizeLinearS8Kernel = MlasQuantizeLinearS8KernelVSX;
        this->QuantizeLinearU8Kernel = MlasQuantizeLinearU8KernelVSX;
    }

#if defined(POWER10)
#if (defined(__GNUC__) && ((__GNUC__ > 10) || (__GNUC__== 10 && __GNUC_MINOR__ >= 2))) || \
    (defined(__clang__) && (__clang_major__ >= 12))
#if defined(__linux__) || defined(__FreeBSD__)
    bool HasP10Instructions = ((hwcap2 & PPC_FEATURE2_MMA) && (hwcap2 & PPC_FEATURE2_ARCH_3_1));
#elif defined(_AIX)
    bool HasP10Instructions = (__power_10_andup() && __power_mma_version() == MMA_V31);
#endif // __linux__
    if (HasP10Instructions) {
        this->GemmFloatKernel = MlasSgemmKernelPOWER10;
        this->GemmDoubleKernel = MlasDgemmKernelPOWER10;
        this->GemmU8X8Dispatch = &MlasGemm8X8DispatchPOWER10;
    }
#endif
#endif

#endif // MLAS_TARGET_POWER

#if defined(MLAS_TARGET_LARCH64)

    //
    // Default to the baseline LSX support.
    //

    int hwcap = getauxval(AT_HWCAP);
    bool cap_lasx = hwcap & HWCAP_LOONGARCH_LASX;
    bool cap_lsx = hwcap & HWCAP_LOONGARCH_LSX;

    if( cap_lasx ){
        this->GemmFloatKernel = MlasGemmFloatKernelLasx;
        this->GemmDoubleKernel = MlasGemmDoubleKernelLasx;
        this->ConvNchwFloatKernel = MlasConvNchwFloatKernelLasx;
        this->ConvNchwcFloatKernel = MlasConvNchwcFloatKernelLasx;
        this->ConvDepthwiseFloatKernel = MlasConvDepthwiseFloatKernelLasx;
        this->ConvPointwiseFloatKernel = MlasConvPointwiseFloatKernelLasx;
        this->PoolFloatKernel[MlasMaximumPooling] = MlasPoolMaximumFloatKernelLasx;
        this->PoolFloatKernel[MlasAveragePoolingExcludePad] = MlasPoolAverageExcludePadFloatKernelLasx;
        this->PoolFloatKernel[MlasAveragePoolingIncludePad] = MlasPoolAverageIncludePadFloatKernelLasx;
        this->ReduceMaximumF32Kernel = MlasReduceMaximumF32KernelLasx;
        this->ComputeSoftmaxOutputF32Kernel = MlasComputeSoftmaxOutputF32KernelLasx;
        this->ComputeLogSoftmaxOutputF32Kernel = MlasComputeLogSoftmaxOutputF32KernelLasx;
        this->TransposePackB16x4Routine = MlasSgemmTransposePackB16x4Lasx;

        this->GemmU8S8Dispatch = &MlasGemmU8X8DispatchLSX;
        this->GemmU8U8Dispatch = &MlasGemmU8X8DispatchLSX;
    }else if( cap_lsx ){
        this->GemmFloatKernel = MlasGemmFloatKernelLSX;
        this->GemmU8S8Dispatch = &MlasGemmU8X8DispatchLSX;
        this->GemmU8U8Dispatch = &MlasGemmU8X8DispatchLSX;
        this->TransposePackB16x4Routine = MlasSgemmTransposePackB16x4LSX;
        this->GemmDoubleKernel = MlasGemmDoubleKernelLSX;
        this->ConvNchwFloatKernel = MlasConvNchwFloatKernelLSX;
        this->ConvNchwcFloatKernel = MlasConvNchwcFloatKernelLSX;
        this->ConvDepthwiseFloatKernel = MlasConvDepthwiseFloatKernelLSX;
        this->ConvPointwiseFloatKernel = MlasConvPointwiseFloatKernelLSX;

        this->PoolFloatKernel[MlasMaximumPooling] = MlasPoolMaximumFloatKernelLSX;
        this->PoolFloatKernel[MlasAveragePoolingExcludePad] = MlasPoolAverageExcludePadFloatKernelLSX;
        this->PoolFloatKernel[MlasAveragePoolingIncludePad] = MlasPoolAverageIncludePadFloatKernelLSX;
        this->ReduceMaximumF32Kernel = MlasReduceMaximumF32Kernel;
        this->ComputeSoftmaxOutputF32Kernel = MlasComputeSoftmaxOutputF32Kernel;
        this->ComputeLogSoftmaxOutputF32Kernel = MlasComputeLogSoftmaxOutputF32Kernel;
    }else{
        this->ReduceMaximumF32Kernel = MlasReduceMaximumF32Kernel;
        this->ComputeSoftmaxOutputF32Kernel = MlasComputeSoftmaxOutputF32Kernel;
        this->ComputeLogSoftmaxOutputF32Kernel = MlasComputeLogSoftmaxOutputF32Kernel;
    }

    this->NchwcBlockSize = 8;
    // this->PreferredBufferAlignment = MLAS_DEFAULT_PREFERRED_BUFFER_ALIGNMENT;

    // this->MaximumThreadCount = MLAS_MAXIMUM_THREAD_COUNT;

#endif // MLAS_TARGET_LARCH64

}

size_t
MLASCALL
MlasGetPreferredBufferAlignment(
    void
    )
/*++

Routine Description:

    This routine returns the preferred byte alignment for buffers that are used
    with this library. Buffers that are not byte aligned to this value will
    function, but will not achieve best performance.

Arguments:

    None.

Return Value:

    Returns the preferred byte alignment for buffers.

--*/
{
#if defined(MLAS_TARGET_AMD64)
    return GetMlasPlatform().PreferredBufferAlignment;
#else
    return MLAS_DEFAULT_PREFERRED_BUFFER_ALIGNMENT;
#endif
}

#ifdef MLAS_TARGET_AMD64_IX86

bool
MLASCALL
MlasPlatformU8S8Overflow(
    void
    )
{
    const auto& p = GetMlasPlatform();
    return p.GemmU8U8Dispatch != p.GemmU8S8Dispatch;
}

#endif
thread_local size_t ThreadedBufSize = 0;
#ifdef _MSC_VER
thread_local std::unique_ptr<uint8_t, decltype(&_aligned_free)> ThreadedBufHolder(nullptr, &_aligned_free);
#else
thread_local std::unique_ptr<uint8_t, decltype(&free)> ThreadedBufHolder(nullptr, &free);
#endif
