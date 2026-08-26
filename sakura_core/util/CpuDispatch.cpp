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

struct DetectedCapabilities {
	CpuDispatch::Capabilities effective{};
	std::uint64_t rawFeatureBits{};
	std::uint64_t osExtendedStateBits{};
};

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

DetectedCapabilities DetectCapabilities() noexcept
{
	DetectedCapabilities detected{};
	int registers[4]{};
	const int maximumLeaf = GetMaximumCpuidLeaf();
	if (maximumLeaf < kCpuidLeafFeatures) {
		return detected;
	}

	Cpuid(registers, kCpuidLeafFeatures, kCpuidSubleafZero);
	const bool cpuAvx = (registers[2] & kCpuidEcxAvx) != 0;
	const bool osXsave = (registers[2] & kCpuidEcxOsXsave) != 0;
	if (cpuAvx) {
		detected.rawFeatureBits |= SakuraAbiBit(SakuraCpuFeature::Avx);
	}

	if (maximumLeaf >= kCpuidLeafExtendedFeatures) {
		Cpuid(registers, kCpuidLeafExtendedFeatures, kCpuidSubleafZero);
		if ((registers[1] & kCpuidEbxAvx2) != 0) {
			detected.rawFeatureBits |= SakuraAbiBit(SakuraCpuFeature::Avx2);
		}
		if ((registers[1] & kCpuidEbxAvx512F) != 0) {
			detected.rawFeatureBits |= SakuraAbiBit(SakuraCpuFeature::Avx512F);
		}
		if ((registers[1] & kCpuidEbxAvx512Bw) != 0) {
			detected.rawFeatureBits |= SakuraAbiBit(SakuraCpuFeature::Avx512Bw);
		}
	}

	std::uint64_t xcr0{};
	if (osXsave) {
		xcr0 = ReadXcr0();
		if ((xcr0 & (std::uint64_t{1} << 1)) != 0) {
			detected.osExtendedStateBits |= SakuraAbiBit(SakuraOsExtendedState::Xmm);
		}
		if ((xcr0 & (std::uint64_t{1} << 2)) != 0) {
			detected.osExtendedStateBits |= SakuraAbiBit(SakuraOsExtendedState::Ymm);
		}
		if ((xcr0 & (std::uint64_t{1} << 5)) != 0) {
			detected.osExtendedStateBits |= SakuraAbiBit(SakuraOsExtendedState::Opmask);
		}
		if ((xcr0 & (std::uint64_t{1} << 6)) != 0) {
			detected.osExtendedStateBits |= SakuraAbiBit(SakuraOsExtendedState::ZmmHi256);
		}
		if ((xcr0 & (std::uint64_t{1} << 7)) != 0) {
			detected.osExtendedStateBits |= SakuraAbiBit(SakuraOsExtendedState::Hi16Zmm);
		}
	}

	const bool avxState = (xcr0 & kXcr0AvxMask) == kXcr0AvxMask;
	detected.effective.avx = cpuAvx && osXsave && avxState;
	detected.effective.avx2 = detected.effective.avx
		&& (detected.rawFeatureBits & SakuraAbiBit(SakuraCpuFeature::Avx2)) != 0;
	const bool cpuAvx512 = (detected.rawFeatureBits &
		(SakuraAbiBit(SakuraCpuFeature::Avx512F)
			| SakuraAbiBit(SakuraCpuFeature::Avx512Bw)))
		== (SakuraAbiBit(SakuraCpuFeature::Avx512F)
			| SakuraAbiBit(SakuraCpuFeature::Avx512Bw));
	// AVX-512 is a process-wide tier: it selects both the UTF-16 path and the
	// C++ byte scanner, whose tail delegates to FindCrOrLfAvx2. Keep AVX2 in
	// this effective capability until that dependency is removed.
	detected.effective.avx512 = detected.effective.avx2
		&& cpuAvx512 && (xcr0 & kXcr0Avx512Mask) == kXcr0Avx512Mask;
	return detected;
}

const char* GetUtf16ImplementationName(CpuDispatch::Isa isa) noexcept
{
#if defined(SAKURA_UTF16_BACKEND_RUST)
	switch (isa) {
	case CpuDispatch::Isa::Avx512: return "rust-avx512bw-v2";
	case CpuDispatch::Isa::Avx2: return "rust-avx2-v2";
	default: return "rust-avx128-v2";
	}
#else
	switch (isa) {
	case CpuDispatch::Isa::Avx512: return "cpp-avx512bw";
	case CpuDispatch::Isa::Avx2: return "cpp-avx2";
	default: return "cpp-avx128";
	}
#endif
}

SakuraImplementationId GetUtf16ImplementationId(CpuDispatch::Isa isa) noexcept
{
#if defined(SAKURA_UTF16_BACKEND_RUST)
	switch (isa) {
	case CpuDispatch::Isa::Avx512: return SakuraImplementationId::RustAvx512Bw;
	case CpuDispatch::Isa::Avx2: return SakuraImplementationId::RustAvx2;
	default: return SakuraImplementationId::RustAvx128;
	}
#else
	switch (isa) {
	case CpuDispatch::Isa::Avx512: return SakuraImplementationId::CppAvx512Bw;
	case CpuDispatch::Isa::Avx2: return SakuraImplementationId::CppAvx2;
	default: return SakuraImplementationId::CppAvx128;
	}
#endif
}

SakuraOperationPolicyV1 MakeOperationPolicy(
	SakuraOperationId operationId, SakuraImplementationId implementationId,
	std::size_t minimumLength) noexcept
{
	SakuraOperationPolicyV1 policy{};
	policy.structSize = sizeof(policy);
	policy.abiVersion = SAKURA_NATIVE_ABI_VERSION_V1;
	policy.operationId = static_cast<std::uint32_t>(operationId);
	policy.implementationId = static_cast<std::uint32_t>(implementationId);
	policy.minimumLength = static_cast<std::uint64_t>(minimumLength);
	return policy;
}

CpuDispatch::FindUtf16Function SelectFindCrOrLfUtf16(CpuDispatch::Isa isa) noexcept
{
#if defined(SAKURA_UTF16_BACKEND_RUST)
	switch (isa) {
	case CpuDispatch::Isa::Avx512: return CpuDispatch::Internal::FindCrOrLfUtf16RustAvx512Bw;
	case CpuDispatch::Isa::Avx2: return CpuDispatch::Internal::FindCrOrLfUtf16RustAvx2;
	default: return CpuDispatch::Internal::FindCrOrLfUtf16RustAvx128;
	}
#else
	switch (isa) {
	case CpuDispatch::Isa::Avx512: return CpuDispatch::Internal::FindCrOrLfUtf16Avx512;
	case CpuDispatch::Isa::Avx2: return CpuDispatch::Internal::FindCrOrLfUtf16Avx2;
	default: return CpuDispatch::Internal::FindCrOrLfUtf16Avx;
	}
#endif
}

CpuDispatch::FindUtf16Function SelectFindMarkdownUtf16(CpuDispatch::Isa isa) noexcept
{
#if defined(SAKURA_UTF16_BACKEND_RUST)
	switch (isa) {
	case CpuDispatch::Isa::Avx512:
		return CpuDispatch::Internal::FindMarkdownInlineSpecialUtf16RustAvx512Bw;
	case CpuDispatch::Isa::Avx2:
		return CpuDispatch::Internal::FindMarkdownInlineSpecialUtf16RustAvx2;
	default:
		return CpuDispatch::Internal::FindMarkdownInlineSpecialUtf16RustAvx128;
	}
#else
	switch (isa) {
	case CpuDispatch::Isa::Avx512:
		return CpuDispatch::Internal::FindMarkdownInlineSpecialUtf16Avx512;
	case CpuDispatch::Isa::Avx2:
		return CpuDispatch::Internal::FindMarkdownInlineSpecialUtf16Avx2;
	default:
		return CpuDispatch::Internal::FindMarkdownInlineSpecialUtf16Avx;
	}
#endif
}

CpuDispatch::FindUtf16CharFunction SelectFindCharUtf16(CpuDispatch::Isa isa) noexcept
{
#if defined(SAKURA_UTF16_BACKEND_RUST)
	switch (isa) {
	case CpuDispatch::Isa::Avx512: return CpuDispatch::Internal::FindUtf16CharRustAvx512Bw;
	case CpuDispatch::Isa::Avx2: return CpuDispatch::Internal::FindUtf16CharRustAvx2;
	default: return CpuDispatch::Internal::FindUtf16CharRustAvx128;
	}
#else
	switch (isa) {
	case CpuDispatch::Isa::Avx512: return CpuDispatch::Internal::FindUtf16CharAvx512;
	case CpuDispatch::Isa::Avx2: return CpuDispatch::Internal::FindUtf16CharAvx2;
	default: return CpuDispatch::Internal::FindUtf16CharAvx;
	}
#endif
}

CpuDispatch::Dispatch CreateDispatch() noexcept
{
	LARGE_INTEGER begin{};
	LARGE_INTEGER end{};
	::QueryPerformanceCounter(&begin);

	CpuDispatch::Dispatch dispatch{};
	const auto detected = DetectCapabilities();
	dispatch.capabilities = detected.effective;
	dispatch.isa = CpuDispatch::SelectBestIsa(dispatch.capabilities);
	dispatch.utf16CrOrLfIsa = CpuDispatch::SelectUtf16OperationIsa(
		SakuraOperationId::FindCrOrLfUtf16, dispatch.capabilities);
	dispatch.utf16MarkdownIsa = CpuDispatch::SelectUtf16OperationIsa(
		SakuraOperationId::FindMarkdownSpecialUtf16, dispatch.capabilities);
	dispatch.utf16FindCharIsa = CpuDispatch::SelectUtf16OperationIsa(
		SakuraOperationId::FindCharUtf16, dispatch.capabilities);
	const auto crOrLfPolicy = CpuDispatch::GetUtf16ScanPolicy(dispatch.utf16CrOrLfIsa);
	const auto markdownPolicy = CpuDispatch::GetUtf16ScanPolicy(dispatch.utf16MarkdownIsa);
	const auto findCharPolicy = CpuDispatch::GetUtf16ScanPolicy(dispatch.utf16FindCharIsa);
	dispatch.utf16ScanPolicy = {
		crOrLfPolicy.crOrLfMinimumLength,
		markdownPolicy.markdownInlineSpecialMinimumLength,
		findCharPolicy.findCharMinimumLength,
	};
	dispatch.utf8ConversionPolicy = CpuDispatch::GetUtf8ConversionPolicy(dispatch.isa);
	dispatch.utf16CrOrLfImplementation = GetUtf16ImplementationName(dispatch.utf16CrOrLfIsa);
	dispatch.utf16MarkdownImplementation = GetUtf16ImplementationName(dispatch.utf16MarkdownIsa);
	dispatch.utf16FindCharImplementation = GetUtf16ImplementationName(dispatch.utf16FindCharIsa);
	dispatch.nativeCapabilities.structSize = sizeof(dispatch.nativeCapabilities);
	dispatch.nativeCapabilities.abiVersion = SAKURA_NATIVE_ABI_VERSION_V1;
	dispatch.nativeCapabilities.rawFeatureBits = detected.rawFeatureBits;
	dispatch.nativeCapabilities.osExtendedStateBits = detected.osExtendedStateBits;
	dispatch.nativeOperationPolicies = {
		MakeOperationPolicy(SakuraOperationId::FindCrOrLfUtf16,
			GetUtf16ImplementationId(dispatch.utf16CrOrLfIsa),
			dispatch.utf16ScanPolicy.crOrLfMinimumLength),
		MakeOperationPolicy(SakuraOperationId::FindMarkdownSpecialUtf16,
			GetUtf16ImplementationId(dispatch.utf16MarkdownIsa),
			dispatch.utf16ScanPolicy.markdownInlineSpecialMinimumLength),
		MakeOperationPolicy(SakuraOperationId::FindCharUtf16,
			GetUtf16ImplementationId(dispatch.utf16FindCharIsa),
			dispatch.utf16ScanPolicy.findCharMinimumLength),
	};
#if defined(SAKURA_UTF16_BACKEND_RUST)
	dispatch.utf16Backend = "rust";
	dispatch.utf16BuildMode = "rust";
#else
	dispatch.utf16Backend = "cpp";
	dispatch.utf16BuildMode = "cpp";
#endif
#if defined(SAKURA_UTF16_BACKEND_RUST) || defined(SAKURA_UTF16_RUST_CANDIDATE)
	dispatch.nativeCandidateLinked = true;
	dispatch.nativeInitializationStatus = sakura_native_initialize_v1(
		&dispatch.nativeCapabilities, dispatch.nativeOperationPolicies.data(),
		static_cast<std::uint64_t>(dispatch.nativeOperationPolicies.size()));
#endif
	dispatch.findCrOrLfUtf16 = SelectFindCrOrLfUtf16(dispatch.utf16CrOrLfIsa);
	dispatch.findMarkdownInlineSpecialUtf16 =
		SelectFindMarkdownUtf16(dispatch.utf16MarkdownIsa);
	dispatch.findUtf16Char = SelectFindCharUtf16(dispatch.utf16FindCharIsa);
	// Rust is selected only when the build explicitly defines
	// SAKURA_UTF16_BACKEND_RUST. The default production path remains the C++
	// implementation; a Rust-linked test build can still expose both providers
	// through the explicit Testing accessors below.
	switch (dispatch.isa) {
	case CpuDispatch::Isa::Avx512:
		dispatch.findCrOrLf = CpuDispatch::Internal::FindCrOrLfAvx512;
		dispatch.widenAsciiToUtf16 = CpuDispatch::Internal::WidenAsciiToUtf16Avx512;
		break;
	case CpuDispatch::Isa::Avx2:
		dispatch.findCrOrLf = CpuDispatch::Internal::FindCrOrLfAvx2;
		dispatch.widenAsciiToUtf16 = CpuDispatch::Internal::WidenAsciiToUtf16Avx2;
		break;
	default:
		dispatch.findCrOrLf = CpuDispatch::Internal::FindCrOrLfAvx;
		dispatch.widenAsciiToUtf16 = CpuDispatch::Internal::WidenAsciiToUtf16Avx;
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

Isa SelectUtf16OperationIsa(
	SakuraOperationId operationId, const Capabilities& capabilities) noexcept
{
	// Each operation owns an independent ISA ceiling. They currently all use
	// the highest audited tier, but keeping the policy slots separate lets an
	// adoption decision lower one cell without changing the other operations or
	// the legacy byte/widening tier.
	Isa maximumIsa = Isa::Avx;
	switch (operationId) {
	case SakuraOperationId::FindCrOrLfUtf16:
		maximumIsa = Isa::Avx512;
		break;
	case SakuraOperationId::FindMarkdownSpecialUtf16:
		maximumIsa = Isa::Avx512;
		break;
	case SakuraOperationId::FindCharUtf16:
		maximumIsa = Isa::Avx512;
		break;
	}
	if (maximumIsa == Isa::Avx512
		&& capabilities.avx && capabilities.avx2 && capabilities.avx512) {
		return Isa::Avx512;
	}
	if (maximumIsa != Isa::Avx && capabilities.avx && capabilities.avx2) {
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
	// The find-char minimums follow the same shape: vector width for AVX/AVX2.
	// For AVX-512 the masked find-char call costs a flat ~2.0-2.5ns while the
	// caller-local scalar loop still wins at 8 units (~1.7ns) and roughly ties
	// at 12, so its break-even is 16 units (three runs of the disabled
	// CpuDispatchTest microbenchmark, x64 Release, 2026-08-08, Ryzen 7 9700X /
	// Zen 5).
	switch (isa) {
	case Isa::Avx512:
		return Utf16ScanPolicy{8, 6, 16};
	case Isa::Avx2:
		return Utf16ScanPolicy{16, 16, 16};
	default:
		return Utf16ScanPolicy{8, 8, 8};
	}
}

Utf8ConversionPolicy GetUtf8ConversionPolicy(Isa isa) noexcept
{
	// The AVX and AVX2 minimums equal their widening kernels' input vector
	// widths (16 and 32 bytes); below that the kernel would only run its own
	// scalar prefix loop behind an indirect call. The AVX-512 kernel handles
	// any length with masked loads and stores, so its minimum is the
	// benchmark-derived break-even against a caller-local scalar widening loop:
	// the masked call costs a flat ~2.2ns, the scalar loop still wins at 8
	// bytes (~1.6ns) and ties at 12, so the kernel takes over at 16 (three runs
	// of the disabled CpuDispatchTest microbenchmark, x64 Release, 2026-08-08,
	// Ryzen 7 9700X / Zen 5).
	switch (isa) {
	case Isa::Avx512:
		return Utf8ConversionPolicy{16};
	case Isa::Avx2:
		return Utf8ConversionPolicy{32};
	default:
		return Utf8ConversionPolicy{16};
	}
}

const char* GetIsaName(Isa isa) noexcept
{
	switch (isa) {
	case Isa::Avx512: return "avx512bw";
	case Isa::Avx2: return "avx2";
	default: return "avx128";
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

FindUtf16Function GetSupportedFindCrOrLfUtf16Cpp(Isa isa) noexcept
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

FindUtf16Function GetSupportedFindCrOrLfUtf16Rust(Isa isa) noexcept
{
	const auto& dispatch = Get();
#if defined(SAKURA_UTF16_BACKEND_RUST) || defined(SAKURA_UTF16_RUST_CANDIDATE)
	switch (isa) {
	case Isa::Avx512:
		return dispatch.capabilities.avx512 ? Internal::FindCrOrLfUtf16RustAvx512Bw : nullptr;
	case Isa::Avx2:
		return dispatch.capabilities.avx2 ? Internal::FindCrOrLfUtf16RustAvx2 : nullptr;
	default:
		return dispatch.capabilities.avx ? Internal::FindCrOrLfUtf16RustAvx128 : nullptr;
	}
#else
	(void)isa;
	return nullptr;
#endif
}

FindUtf16Function GetSupportedFindCrOrLfUtf16(Isa isa) noexcept
{
#if defined(SAKURA_UTF16_BACKEND_RUST)
	return GetSupportedFindCrOrLfUtf16Rust(isa);
#else
	return GetSupportedFindCrOrLfUtf16Cpp(isa);
#endif
}

FindUtf16Function GetSupportedFindMarkdownInlineSpecialUtf16Cpp(Isa isa) noexcept
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

FindUtf16Function GetSupportedFindMarkdownInlineSpecialUtf16Rust(Isa isa) noexcept
{
	const auto& dispatch = Get();
#if defined(SAKURA_UTF16_BACKEND_RUST) || defined(SAKURA_UTF16_RUST_CANDIDATE)
	switch (isa) {
	case Isa::Avx512:
		return dispatch.capabilities.avx512
			? Internal::FindMarkdownInlineSpecialUtf16RustAvx512Bw
			: nullptr;
	case Isa::Avx2:
		return dispatch.capabilities.avx2
			? Internal::FindMarkdownInlineSpecialUtf16RustAvx2
			: nullptr;
	default:
		return dispatch.capabilities.avx
			? Internal::FindMarkdownInlineSpecialUtf16RustAvx128
			: nullptr;
	}
#else
	(void)isa;
	return nullptr;
#endif
}

FindUtf16Function GetSupportedFindMarkdownInlineSpecialUtf16(Isa isa) noexcept
{
#if defined(SAKURA_UTF16_BACKEND_RUST)
	return GetSupportedFindMarkdownInlineSpecialUtf16Rust(isa);
#else
	return GetSupportedFindMarkdownInlineSpecialUtf16Cpp(isa);
#endif
}

WidenAsciiToUtf16Function GetSupportedWidenAsciiToUtf16(Isa isa) noexcept
{
	const auto& dispatch = Get();
	switch (isa) {
	case Isa::Avx512:
		return dispatch.capabilities.avx512 ? Internal::WidenAsciiToUtf16Avx512 : nullptr;
	case Isa::Avx2:
		return dispatch.capabilities.avx2 ? Internal::WidenAsciiToUtf16Avx2 : nullptr;
	default:
		return dispatch.capabilities.avx ? Internal::WidenAsciiToUtf16Avx : nullptr;
	}
}

FindUtf16CharFunction GetSupportedFindUtf16CharCpp(Isa isa) noexcept
{
	const auto& dispatch = Get();
	switch (isa) {
	case Isa::Avx512:
		return dispatch.capabilities.avx512 ? Internal::FindUtf16CharAvx512 : nullptr;
	case Isa::Avx2:
		return dispatch.capabilities.avx2 ? Internal::FindUtf16CharAvx2 : nullptr;
	default:
		return dispatch.capabilities.avx ? Internal::FindUtf16CharAvx : nullptr;
	}
}

FindUtf16CharFunction GetSupportedFindUtf16CharRust(Isa isa) noexcept
{
	const auto& dispatch = Get();
#if defined(SAKURA_UTF16_BACKEND_RUST) || defined(SAKURA_UTF16_RUST_CANDIDATE)
	switch (isa) {
	case Isa::Avx512:
		return dispatch.capabilities.avx512 ? Internal::FindUtf16CharRustAvx512Bw : nullptr;
	case Isa::Avx2:
		return dispatch.capabilities.avx2 ? Internal::FindUtf16CharRustAvx2 : nullptr;
	default:
		return dispatch.capabilities.avx ? Internal::FindUtf16CharRustAvx128 : nullptr;
	}
#else
	(void)isa;
	return nullptr;
#endif
}

FindUtf16CharFunction GetSupportedFindUtf16Char(Isa isa) noexcept
{
#if defined(SAKURA_UTF16_BACKEND_RUST)
	return GetSupportedFindUtf16CharRust(isa);
#else
	return GetSupportedFindUtf16CharCpp(isa);
#endif
}
}
}
