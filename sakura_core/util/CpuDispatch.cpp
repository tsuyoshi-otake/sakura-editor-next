/*! @file @brief Runtime x64 ISA detection and immutable dispatch table. */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "util/CpuDispatch.h"
#include "util/CpuDispatchInternal.h"

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

namespace
{
constexpr int kCpuidLeafFeatures = 1;
constexpr int kCpuidLeafExtendedFeatures = 7;
constexpr int kCpuidSubleafZero = 0;
constexpr int kCpuidEcxOsXsave = 1 << 27;
constexpr int kCpuidEcxAvx = 1 << 28;
constexpr int kCpuidEbxAvx2 = 1 << 5;
constexpr int kCpuidEbxAvx512F = 1 << 16;
constexpr int kCpuidEbxAvx512Bw = 1 << 30;
constexpr std::uint64_t kXcr0AvxMask = (std::uint64_t{1} << 1) | (std::uint64_t{1} << 2);
constexpr std::uint64_t kXcr0Avx512Mask =
	kXcr0AvxMask | (std::uint64_t{1} << 5) | (std::uint64_t{1} << 6) | (std::uint64_t{1} << 7);

int GetMaximumCpuidLeaf() noexcept
{
#if defined(_MSC_VER)
	int registers[4]{};
	__cpuid(registers, 0);
	return registers[0];
#else
	return static_cast<int>(__get_cpuid_max(0, nullptr));
#endif
}

void Cpuid(int registers[4], int leaf, int subleaf) noexcept
{
#if defined(_MSC_VER)
	__cpuidex(registers, leaf, subleaf);
#else
	unsigned int eax{};
	unsigned int ebx{};
	unsigned int ecx{};
	unsigned int edx{};
	__cpuid_count(
		static_cast<unsigned int>(leaf),
		static_cast<unsigned int>(subleaf),
		eax, ebx, ecx, edx);
	registers[0] = static_cast<int>(eax);
	registers[1] = static_cast<int>(ebx);
	registers[2] = static_cast<int>(ecx);
	registers[3] = static_cast<int>(edx);
#endif
}

std::uint64_t ReadXcr0() noexcept
{
#if defined(_MSC_VER)
	return _xgetbv(0);
#else
	unsigned int eax{};
	unsigned int edx{};
	__asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(eax), "=d"(edx) : "c"(0));
	return (static_cast<std::uint64_t>(edx) << 32) | eax;
#endif
}

CpuDispatch::Capabilities DetectCapabilities() noexcept
{
	int registers[4]{};
	const int maximumLeaf = GetMaximumCpuidLeaf();
	if (maximumLeaf < kCpuidLeafFeatures) {
		return {};
	}

	Cpuid(registers, kCpuidLeafFeatures, kCpuidSubleafZero);
	const bool cpuAvx = (registers[2] & kCpuidEcxAvx) != 0;
	const bool osXsave = (registers[2] & kCpuidEcxOsXsave) != 0;
	if (!cpuAvx || !osXsave) {
		return {};
	}

	const std::uint64_t xcr0 = ReadXcr0();
	const bool avxState = (xcr0 & kXcr0AvxMask) == kXcr0AvxMask;
	if (!avxState) {
		return {};
	}

	CpuDispatch::Capabilities capabilities{true, false, false};
	if (maximumLeaf < kCpuidLeafExtendedFeatures) {
		return capabilities;
	}

	Cpuid(registers, kCpuidLeafExtendedFeatures, kCpuidSubleafZero);
	capabilities.avx2 = (registers[1] & kCpuidEbxAvx2) != 0;
	const bool cpuAvx512 =
		(registers[1] & kCpuidEbxAvx512F) != 0
		&& (registers[1] & kCpuidEbxAvx512Bw) != 0;
	capabilities.avx512 =
		capabilities.avx2 && cpuAvx512 && (xcr0 & kXcr0Avx512Mask) == kXcr0Avx512Mask;
	return capabilities;
}

CpuDispatch::Dispatch CreateDispatch() noexcept
{
	LARGE_INTEGER begin{};
	LARGE_INTEGER end{};
	::QueryPerformanceCounter(&begin);

	CpuDispatch::Dispatch dispatch{};
	dispatch.capabilities = DetectCapabilities();
	dispatch.isa = CpuDispatch::SelectBestIsa(dispatch.capabilities);
	dispatch.utf16ScanPolicy = CpuDispatch::GetUtf16ScanPolicy(dispatch.isa);
	switch (dispatch.isa) {
	case CpuDispatch::Isa::Avx512:
		dispatch.findCrOrLf = CpuDispatch::Internal::FindCrOrLfAvx512;
		dispatch.findCrOrLfUtf16 = CpuDispatch::Internal::FindCrOrLfUtf16Avx512;
		dispatch.findMarkdownInlineSpecialUtf16 =
			CpuDispatch::Internal::FindMarkdownInlineSpecialUtf16Avx512;
		break;
	case CpuDispatch::Isa::Avx2:
		dispatch.findCrOrLf = CpuDispatch::Internal::FindCrOrLfAvx2;
		dispatch.findCrOrLfUtf16 = CpuDispatch::Internal::FindCrOrLfUtf16Avx2;
		dispatch.findMarkdownInlineSpecialUtf16 =
			CpuDispatch::Internal::FindMarkdownInlineSpecialUtf16Avx2;
		break;
	default:
		dispatch.findCrOrLf = CpuDispatch::Internal::FindCrOrLfAvx;
		dispatch.findCrOrLfUtf16 = CpuDispatch::Internal::FindCrOrLfUtf16Avx;
		dispatch.findMarkdownInlineSpecialUtf16 =
			CpuDispatch::Internal::FindMarkdownInlineSpecialUtf16Avx;
		break;
	}

	::QueryPerformanceCounter(&end);
	dispatch.initializationTicks = end.QuadPart - begin.QuadPart;
	return dispatch;
}
}

namespace CpuDispatch
{
Isa SelectBestIsa(const Capabilities& capabilities) noexcept
{
	if (capabilities.avx && capabilities.avx2 && capabilities.avx512) {
		return Isa::Avx512;
	}
	if (capabilities.avx && capabilities.avx2) {
		return Isa::Avx2;
	}
	return Isa::Avx;
}

Utf16ScanPolicy GetUtf16ScanPolicy(Isa isa) noexcept
{
	// The AVX and AVX2 minimums equal their implementations' internal vector
	// widths (8 and 16 UTF-16 units): below that the implementation would run
	// its own scalar fallback behind an indirect call. The AVX-512
	// implementations handle any length with masked loads, so their minimums
	// are purely benchmark-derived break-even points against a caller-local
	// scalar loop: the masked scan costs a flat ~2ns (CR/LF) / ~4ns (inline
	// specials) per call regardless of length, crossing the scalar loop at
	// about 8 and 6 units. Median of three runs of the disabled CpuDispatchTest
	// microbenchmark, x64 Release, 2026-08-08, Ryzen 7 9700X (Zen 5).
	switch (isa) {
	case Isa::Avx512:
		return Utf16ScanPolicy{8, 6};
	case Isa::Avx2:
		return Utf16ScanPolicy{16, 16};
	default:
		return Utf16ScanPolicy{8, 8};
	}
}

const char* GetIsaName(Isa isa) noexcept
{
	switch (isa) {
	case Isa::Avx512: return "avx512";
	case Isa::Avx2: return "avx2";
	default: return "avx";
	}
}

const Dispatch& Get() noexcept
{
	static const Dispatch dispatch = CreateDispatch();
	return dispatch;
}

const Dispatch& Initialize() noexcept
{
	return Get();
}

namespace Testing
{
FindCrOrLfFunction GetSupportedFindCrOrLf(Isa isa) noexcept
{
	const auto& dispatch = Get();
	switch (isa) {
	case Isa::Avx512:
		return dispatch.capabilities.avx512 ? Internal::FindCrOrLfAvx512 : nullptr;
	case Isa::Avx2:
		return dispatch.capabilities.avx2 ? Internal::FindCrOrLfAvx2 : nullptr;
	default:
		return dispatch.capabilities.avx ? Internal::FindCrOrLfAvx : nullptr;
	}
}

FindUtf16Function GetSupportedFindCrOrLfUtf16(Isa isa) noexcept
{
	const auto& dispatch = Get();
	switch (isa) {
	case Isa::Avx512:
		return dispatch.capabilities.avx512 ? Internal::FindCrOrLfUtf16Avx512 : nullptr;
	case Isa::Avx2:
		return dispatch.capabilities.avx2 ? Internal::FindCrOrLfUtf16Avx2 : nullptr;
	default:
		return dispatch.capabilities.avx ? Internal::FindCrOrLfUtf16Avx : nullptr;
	}
}

FindUtf16Function GetSupportedFindMarkdownInlineSpecialUtf16(Isa isa) noexcept
{
	const auto& dispatch = Get();
	switch (isa) {
	case Isa::Avx512:
		return dispatch.capabilities.avx512
			? Internal::FindMarkdownInlineSpecialUtf16Avx512
			: nullptr;
	case Isa::Avx2:
		return dispatch.capabilities.avx2
			? Internal::FindMarkdownInlineSpecialUtf16Avx2
			: nullptr;
	default:
		return dispatch.capabilities.avx
			? Internal::FindMarkdownInlineSpecialUtf16Avx
			: nullptr;
	}
}
}
}
