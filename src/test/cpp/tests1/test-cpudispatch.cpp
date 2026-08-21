/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "Utf16Phase8Benchmark.h"
#include "util/CpuDispatch.h"
#include "util/RustUtf16Scan.h"
#include "charset/CUtf8.h"
#include "charset/codechecker.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
std::size_t FindReference(const char* data, std::size_t length)
{
	std::size_t offset = 0;
	while (offset < length && data[offset] != '\r' && data[offset] != '\n') {
		++offset;
	}
	return offset;
}

std::size_t FindCrOrLfUtf16Reference(const wchar_t* data, std::size_t length)
{
	std::size_t offset = 0;
	while (offset < length && data[offset] != L'\r' && data[offset] != L'\n') {
		++offset;
	}
	return offset;
}

bool IsMarkdownInlineSpecialReference(wchar_t value)
{
	switch (value) {
	case L'\\':
	case L'`':
	case L'!':
	case L'[':
	case L'*':
	case L'_':
	case L'~':
	case L'<':
	case L'&':
	case L'$':
		return true;
	default:
		return false;
	}
}

std::size_t FindMarkdownInlineSpecialUtf16Reference(
	const wchar_t* data, std::size_t length)
{
	std::size_t offset = 0;
	while (offset < length && !IsMarkdownInlineSpecialReference(data[offset])) {
		++offset;
	}
	return offset;
}

std::size_t WidenAsciiToUtf16Reference(
	const char* source, std::size_t length, wchar_t* destination)
{
	std::size_t offset = 0;
	while (offset < length && static_cast<unsigned char>(source[offset]) < 0x80) {
		destination[offset] = static_cast<wchar_t>(
			static_cast<unsigned char>(source[offset]));
		++offset;
	}
	return offset;
}

std::size_t FindUtf16CharReference(
	const wchar_t* data, std::size_t length, wchar_t target)
{
	std::size_t offset = 0;
	while (offset < length && data[offset] != target) {
		++offset;
	}
	return offset;
}

constexpr std::array<CpuDispatch::Isa, 3> kImplementations{
	CpuDispatch::Isa::Avx,
	CpuDispatch::Isa::Avx2,
	CpuDispatch::Isa::Avx512,
};

constexpr std::array<wchar_t, 10> kMarkdownInlineSpecials{
	L'\\', L'`', L'!', L'[', L'*', L'_', L'~', L'<', L'&', L'$',
};

struct VirtualAllocationDeleter {
	void operator()(void* allocation) const noexcept
	{
		if (allocation != nullptr) {
			::VirtualFree(allocation, 0, MEM_RELEASE);
		}
	}
};

class CUtf8Access final : public CUtf8 {
public:
	static int DecodeForTest(const char* source, int sourceLength, wchar_t* destination, bool cesu8Mode)
	{
		return Utf8ToUni(source, sourceLength, destination, cesu8Mode);
	}

	static int DecodeCharacterForTest(const unsigned char* source, int sourceLength,
		unsigned short* destination, bool cesu8Mode)
	{
		return _Utf8ToUni_char(source, sourceLength, destination, cesu8Mode);
	}
};

// Literal replica of the per-character loop Utf8ToUni ran before the
// ASCII-prefix fast path was added; the fast path must be output-invisible
// against it for every input, including invalid sequences.
int Utf8ToUniPerCharacterReference(
	const char* pSrc, const int nSrcLen, wchar_t* pDst, bool bCESU8Mode)
{
	const unsigned char *pr, *pr_end;
	unsigned short *pw;
	int nclen;
	ECharSet echarset;

	if (nSrcLen < 1) {
		return 0;
	}
	pr = reinterpret_cast<const unsigned char*>(pSrc);
	pr_end = reinterpret_cast<const unsigned char*>(pSrc + nSrcLen);
	pw = reinterpret_cast<unsigned short*>(pDst);
	for (;;) {
		if (bCESU8Mode != true) {
			nclen = CheckUtf8Char(reinterpret_cast<const char*>(pr), pr_end - pr, &echarset, true, 0);
		} else {
			nclen = CheckCesu8Char(reinterpret_cast<const char*>(pr), pr_end - pr, &echarset, 0);
		}
		if (nclen < 1) {
			break;
		}
		if (echarset != CHARSET_BINARY) {
			pw += CUtf8Access::DecodeCharacterForTest(pr, nclen, pw, bCESU8Mode);
			pr += nclen;
		} else {
			if (nclen != 1) {
				nclen = 1;
			}
			pw += CUtf8Access::BinToText(pr, 1, pw);
			++pr;
		}
	}
	return int(pw - reinterpret_cast<unsigned short*>(pDst));
}
}

TEST(CpuDispatchTest, SelectsHighestSafeIsa)
{
	using CpuDispatch::Capabilities;
	using CpuDispatch::Isa;
	const std::array<std::pair<Capabilities, Isa>, 8> cases{{
		{{false, false, false}, Isa::Avx},
		{{false, false, true}, Isa::Avx},
		{{false, true, false}, Isa::Avx},
		{{false, true, true}, Isa::Avx},
		{{true, false, false}, Isa::Avx},
		{{true, false, true}, Isa::Avx},
		{{true, true, false}, Isa::Avx2},
		{{true, true, true}, Isa::Avx512},
	}};
	for (const auto& [capabilities, expected] : cases) {
		EXPECT_EQ(expected, CpuDispatch::SelectBestIsa(capabilities))
			<< "avx=" << capabilities.avx
			<< " avx2=" << capabilities.avx2
			<< " avx512=" << capabilities.avx512;
	}
}

TEST(CpuDispatchTest, Avx512TierRequiresAvx2Capability)
{
	using CpuDispatch::Capabilities;
	using CpuDispatch::Isa;

	// `avx512=true` represents AVX-512F/BW plus the required OS state, but the
	// global tier must still fall back when AVX2 is absent because the C++ byte
	// scanner's AVX-512 tail calls its AVX2 implementation.
	EXPECT_EQ(Isa::Avx, CpuDispatch::SelectBestIsa(Capabilities{true, false, true}));
	EXPECT_EQ(Isa::Avx512, CpuDispatch::SelectBestIsa(Capabilities{true, true, true}));
}

TEST(CpuDispatchTest, SupportedScannersMatchReferenceAtEveryBoundary)
{
	std::string data(705, 'x');
	constexpr std::array<std::size_t, 28> lengths{
		0, 1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 257,
		511, 512, 513, 527, 528, 529, 543, 544, 545, 575, 576, 577, 704
	};
	constexpr std::array<CpuDispatch::Isa, 3> implementations{
		CpuDispatch::Isa::Avx,
		CpuDispatch::Isa::Avx2,
		CpuDispatch::Isa::Avx512,
	};

	for (const auto isa : implementations) {
		const auto scanner = CpuDispatch::Testing::GetSupportedFindCrOrLf(isa);
		if (scanner == nullptr) {
			continue;
		}
		for (const auto length : lengths) {
			for (std::size_t position = 0; position <= length; ++position) {
				std::fill(data.begin(), data.end(), 'x');
				if (position < length) {
					data[position] = (position & 1) == 0 ? '\r' : '\n';
				}
				EXPECT_EQ(FindReference(data.data(), length), scanner(data.data(), length))
					<< "isa=" << CpuDispatch::GetIsaName(isa)
					<< " length=" << length << " position=" << position;
			}
		}
	}
}

TEST(CpuDispatchTest, SupportedScannersMatchReferenceAtEveryInputAlignment)
{
	constexpr std::size_t length = 704;
	alignas(64) std::array<char, length + 63> storage{};
	constexpr std::array<CpuDispatch::Isa, 3> implementations{
		CpuDispatch::Isa::Avx,
		CpuDispatch::Isa::Avx2,
		CpuDispatch::Isa::Avx512,
	};

	for (const auto isa : implementations) {
		const auto scanner = CpuDispatch::Testing::GetSupportedFindCrOrLf(isa);
		if (scanner == nullptr) {
			continue;
		}
		for (std::size_t alignmentOffset = 0; alignmentOffset < 64; ++alignmentOffset) {
			char* const data = storage.data() + alignmentOffset;
			for (std::size_t position = 0; position <= length; ++position) {
				std::fill(data, data + length, 'x');
				if (position < length) {
					data[position] = (position & 1) == 0 ? '\r' : '\n';
				}
				EXPECT_EQ(FindReference(data, length), scanner(data, length))
					<< "isa=" << CpuDispatch::GetIsaName(isa)
					<< " alignmentOffset=" << alignmentOffset
					<< " position=" << position;
			}
		}
	}
}

TEST(CpuDispatchTest, Avx2ScannerReturnsEarliestMatchAcrossUnrolledVectors)
{
	constexpr std::size_t length = 704;
	alignas(64) std::array<char, length + 63> storage{};
	const auto scanner =
		CpuDispatch::Testing::GetSupportedFindCrOrLf(CpuDispatch::Isa::Avx2);
	if (scanner == nullptr) {
		GTEST_SKIP() << "AVX2 is unavailable on this machine";
	}

	for (std::size_t alignmentOffset = 0; alignmentOffset < 64; ++alignmentOffset) {
		char* const data = storage.data() + alignmentOffset;
		std::fill(data, data + length, 'x');
		data[543] = '\n';
		data[544] = '\r';
		data[575] = '\n';
		EXPECT_EQ(543U, scanner(data, length))
			<< "alignmentOffset=" << alignmentOffset;

		data[543] = 'x';
		EXPECT_EQ(544U, scanner(data, length))
			<< "alignmentOffset=" << alignmentOffset;
	}
}

TEST(CpuDispatchTest, SupportedScannersDoNotReadPastGuardPage)
{
	SYSTEM_INFO systemInfo{};
	::GetSystemInfo(&systemInfo);
	const std::size_t pageSize = systemInfo.dwPageSize;
	std::unique_ptr<void, VirtualAllocationDeleter> allocation{
		::VirtualAlloc(nullptr, pageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
	};
	ASSERT_NE(nullptr, allocation);
	DWORD previousProtection{};
	ASSERT_TRUE(::VirtualProtect(
		static_cast<char*>(allocation.get()) + pageSize,
		pageSize,
		PAGE_NOACCESS,
		&previousProtection));

	char* const pageEnd = static_cast<char*>(allocation.get()) + pageSize;
	constexpr std::array<CpuDispatch::Isa, 3> implementations{
		CpuDispatch::Isa::Avx,
		CpuDispatch::Isa::Avx2,
		CpuDispatch::Isa::Avx512,
	};
	for (const auto isa : implementations) {
		const auto scanner = CpuDispatch::Testing::GetSupportedFindCrOrLf(isa);
		if (scanner == nullptr) {
			continue;
		}
		for (std::size_t trailingBytes = 1; trailingBytes <= 64; ++trailingBytes) {
			const std::size_t length = 512 + trailingBytes;
			char* const data = pageEnd - length;
			std::fill(data, data + length, 'x');
			EXPECT_EQ(length, scanner(data, length))
				<< "isa=" << CpuDispatch::GetIsaName(isa)
				<< " trailingBytes=" << trailingBytes;
			if (trailingBytes > 32 && trailingBytes < 64) {
				const std::size_t prefixLength = trailingBytes - 32;
				const std::size_t prefixPosition = 512 + prefixLength - 1;
				data[prefixPosition] = '\r';
				EXPECT_EQ(prefixPosition, scanner(data, length))
					<< "isa=" << CpuDispatch::GetIsaName(isa)
					<< " trailingBytes=" << trailingBytes
					<< " prefixPosition=" << prefixPosition;
				data[prefixPosition] = 'x';
			}
			data[length - 1] = '\n';
			EXPECT_EQ(length - 1, scanner(data, length))
				<< "isa=" << CpuDispatch::GetIsaName(isa)
				<< " trailingBytes=" << trailingBytes;
		}
	}
}

TEST(CpuDispatchTest, Utf16ScannersMatchReferenceAtLengthsAndPositions)
{
	std::wstring data(705, L'x');
	constexpr std::array<std::size_t, 31> lengths{
		0, 1, 7, 8, 9, 15, 16, 17, 31, 32, 33, 62, 63, 64, 65, 66,
		95, 96, 97, 127, 128, 129, 255, 256, 257, 511, 512, 513, 639, 640, 704,
	};

	for (const auto isa : kImplementations) {
		const auto crOrLf = CpuDispatch::Testing::GetSupportedFindCrOrLfUtf16(isa);
		const auto markdownSpecial =
			CpuDispatch::Testing::GetSupportedFindMarkdownInlineSpecialUtf16(isa);
		if (crOrLf == nullptr || markdownSpecial == nullptr) {
			continue;
		}
		for (const auto length : lengths) {
			for (std::size_t position = 0; position <= length; ++position) {
				std::fill(data.begin(), data.end(), L'x');
				if (position < length) {
					data[position] = (position & 1) == 0 ? L'\r' : L'\n';
				}
				EXPECT_EQ(FindCrOrLfUtf16Reference(data.data(), length),
					crOrLf(data.data(), length))
					<< "isa=" << CpuDispatch::GetIsaName(isa)
					<< " length=" << length << " position=" << position;

				std::fill(data.begin(), data.end(), L'x');
				if (position < length) {
					data[position] = kMarkdownInlineSpecials[position % kMarkdownInlineSpecials.size()];
				}
				EXPECT_EQ(FindMarkdownInlineSpecialUtf16Reference(data.data(), length),
					markdownSpecial(data.data(), length))
					<< "isa=" << CpuDispatch::GetIsaName(isa)
					<< " length=" << length << " position=" << position;
			}
		}
	}
}

TEST(CpuDispatchTest, Utf16ScannersMatchReferenceAtEveryValidAlignment)
{
	constexpr std::size_t length = 704;
	alignas(64) std::array<wchar_t, length + 31> storage{};

	for (const auto isa : kImplementations) {
		const auto crOrLf = CpuDispatch::Testing::GetSupportedFindCrOrLfUtf16(isa);
		const auto markdownSpecial =
			CpuDispatch::Testing::GetSupportedFindMarkdownInlineSpecialUtf16(isa);
		if (crOrLf == nullptr || markdownSpecial == nullptr) {
			continue;
		}
		for (std::size_t alignmentOffset = 0; alignmentOffset < 32; ++alignmentOffset) {
			wchar_t* const data = storage.data() + alignmentOffset;
			for (std::size_t position = 0; position <= length; ++position) {
				std::fill(data, data + length, L'\u65e5');
				if (position < length) {
					data[position] = L'\n';
				}
				EXPECT_EQ(FindCrOrLfUtf16Reference(data, length), crOrLf(data, length))
					<< "isa=" << CpuDispatch::GetIsaName(isa)
					<< " alignmentOffset=" << alignmentOffset
					<< " position=" << position;

				std::fill(data, data + length, L'\u65e5');
				if (position < length) {
					data[position] = L'`';
				}
				EXPECT_EQ(FindMarkdownInlineSpecialUtf16Reference(data, length),
					markdownSpecial(data, length))
					<< "isa=" << CpuDispatch::GetIsaName(isa)
					<< " alignmentOffset=" << alignmentOffset
					<< " position=" << position;
			}
		}
	}
}

TEST(CpuDispatchTest, Utf16ScannersTreatNonAsciiAndSurrogatesAsOrdinaryCodeUnits)
{
	std::wstring data(96, L'\u65e5');
	data[17] = static_cast<wchar_t>(0xd83d);
	data[18] = static_cast<wchar_t>(0xde80);
	data[81] = L'$';
	data[89] = L'\r';

	for (const auto isa : kImplementations) {
		const auto crOrLf = CpuDispatch::Testing::GetSupportedFindCrOrLfUtf16(isa);
		const auto markdownSpecial =
			CpuDispatch::Testing::GetSupportedFindMarkdownInlineSpecialUtf16(isa);
		if (crOrLf == nullptr || markdownSpecial == nullptr) {
			continue;
		}
		EXPECT_EQ(89U, crOrLf(data.data(), data.size()))
			<< "isa=" << CpuDispatch::GetIsaName(isa);
		EXPECT_EQ(81U, markdownSpecial(data.data(), data.size()))
			<< "isa=" << CpuDispatch::GetIsaName(isa);
	}
}

TEST(CpuDispatchTest, Utf16ScannersDoNotReadPastGuardPage)
{
	SYSTEM_INFO systemInfo{};
	::GetSystemInfo(&systemInfo);
	const std::size_t pageSize = systemInfo.dwPageSize;
	std::unique_ptr<void, VirtualAllocationDeleter> allocation{
		::VirtualAlloc(nullptr, pageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
	};
	ASSERT_NE(nullptr, allocation);
	DWORD previousProtection{};
	ASSERT_TRUE(::VirtualProtect(
		static_cast<char*>(allocation.get()) + pageSize,
		pageSize,
		PAGE_NOACCESS,
		&previousProtection));

	wchar_t* const pageEnd = reinterpret_cast<wchar_t*>(
		static_cast<char*>(allocation.get()) + pageSize);
	for (const auto isa : kImplementations) {
		const auto crOrLf = CpuDispatch::Testing::GetSupportedFindCrOrLfUtf16(isa);
		const auto markdownSpecial =
			CpuDispatch::Testing::GetSupportedFindMarkdownInlineSpecialUtf16(isa);
		if (crOrLf == nullptr || markdownSpecial == nullptr) {
			continue;
		}
		for (std::size_t trailingUnits = 1; trailingUnits <= 32; ++trailingUnits) {
			const std::size_t length = 64 + trailingUnits;
			wchar_t* const data = pageEnd - length;
			std::fill(data, data + length, L'\u754c');
			EXPECT_EQ(length, crOrLf(data, length))
				<< "isa=" << CpuDispatch::GetIsaName(isa)
				<< " trailingUnits=" << trailingUnits;
			EXPECT_EQ(length, markdownSpecial(data, length))
				<< "isa=" << CpuDispatch::GetIsaName(isa)
				<< " trailingUnits=" << trailingUnits;

			data[length - 1] = L'\n';
			EXPECT_EQ(length - 1, crOrLf(data, length))
				<< "isa=" << CpuDispatch::GetIsaName(isa)
				<< " trailingUnits=" << trailingUnits;
			data[length - 1] = L'$';
			EXPECT_EQ(length - 1, markdownSpecial(data, length))
				<< "isa=" << CpuDispatch::GetIsaName(isa)
				<< " trailingUnits=" << trailingUnits;
		}
	}
}

TEST(CpuDispatchTest, ProcessDispatchIsStable)
{
	const auto* first = &CpuDispatch::Initialize();
	const auto* second = &CpuDispatch::Get();
	EXPECT_EQ(first, second);
	ASSERT_NE(nullptr, first->findCrOrLf);
	ASSERT_NE(nullptr, first->findCrOrLfUtf16);
	ASSERT_NE(nullptr, first->findMarkdownInlineSpecialUtf16);
	ASSERT_NE(nullptr, first->widenAsciiToUtf16);
	ASSERT_NE(nullptr, first->findUtf16Char);
	EXPECT_EQ(3U, first->findCrOrLf("abc\rdef", 7));
	EXPECT_EQ(3U, first->findCrOrLfUtf16(L"abc\rdef", 7));
	EXPECT_EQ(3U, first->findMarkdownInlineSpecialUtf16(L"abc`def", 7));
	EXPECT_EQ(3U, first->findUtf16Char(L"abcdef", 6, L'd'));

	std::array<wchar_t, 8> widened{};
	EXPECT_EQ(3U, first->widenAsciiToUtf16("abc\xe6\x97\xa5", 6, widened.data()));
	EXPECT_EQ(L'a', widened[0]);
	EXPECT_EQ(L'c', widened[2]);
}

TEST(CpuDispatchTest, Utf16ScannersHandleMixedMatchesAndEarliestResult)
{
	std::wstring data(96, L'x');
	for (const auto isa : kImplementations) {
		const auto crOrLf = CpuDispatch::Testing::GetSupportedFindCrOrLfUtf16(isa);
		const auto markdownSpecial =
			CpuDispatch::Testing::GetSupportedFindMarkdownInlineSpecialUtf16(isa);
		if (crOrLf == nullptr || markdownSpecial == nullptr) {
			continue;
		}
		const std::size_t vectorWidth = isa == CpuDispatch::Isa::Avx
			? 8
			: isa == CpuDispatch::Isa::Avx2 ? 16 : 32;

		// The pair straddles the vector boundary; the first of the two values
		// must win even though the second value is in the next vector.
		std::fill(data.begin(), data.end(), L'x');
		data[vectorWidth - 1] = L'\r';
		data[vectorWidth] = L'\n';
		EXPECT_EQ(vectorWidth - 1, crOrLf(data.data(), data.size()))
			<< "isa=" << CpuDispatch::GetIsaName(isa);

		// This pair is wholly inside one vector.
		std::fill(data.begin(), data.end(), L'x');
		data[vectorWidth / 2] = L'\r';
		data[vectorWidth / 2 + 1] = L'\n';
		EXPECT_EQ(vectorWidth / 2, crOrLf(data.data(), data.size()))
			<< "isa=" << CpuDispatch::GetIsaName(isa);

		// One bounded input contains every Markdown special kind. The first
		// one is the result regardless of later matches.
		std::fill(data.begin(), data.end(), L'x');
		constexpr std::size_t firstSpecial = 8;
		for (std::size_t index = 0; index < kMarkdownInlineSpecials.size(); ++index) {
			data[firstSpecial + index] = kMarkdownInlineSpecials[index];
		}
		EXPECT_EQ(firstSpecial, markdownSpecial(data.data(), data.size()))
			<< "isa=" << CpuDispatch::GetIsaName(isa);
	}
}

TEST(CpuDispatchTest, DispatchDiagnosticsExposePerOperationBackendAndThresholds)
{
	const auto& dispatch = CpuDispatch::Get();
#if defined(SAKURA_UTF16_BACKEND_RUST)
	EXPECT_STREQ("rust", dispatch.utf16Backend);
	EXPECT_STREQ("rust", dispatch.utf16BuildMode);
#else
	EXPECT_STREQ("cpp", dispatch.utf16Backend);
	EXPECT_STREQ("cpp", dispatch.utf16BuildMode);
#endif
	const auto expectImplementation = [&](const char* implementation) {
		ASSERT_NE(nullptr, implementation);
		const std::string value{implementation};
#if defined(SAKURA_UTF16_BACKEND_RUST)
		EXPECT_EQ(std::size_t{0}, value.find("rust-"));
#else
		EXPECT_EQ(std::size_t{0}, value.find("cpp-"));
#endif
		EXPECT_NE(std::string::npos, value.find(CpuDispatch::GetIsaName(dispatch.isa)));
	};
	expectImplementation(dispatch.utf16CrOrLfImplementation);
	expectImplementation(dispatch.utf16MarkdownImplementation);
	expectImplementation(dispatch.utf16FindCharImplementation);
	EXPECT_EQ(1U, dispatch.utf16AbiVersion);
	EXPECT_STREQ(CpuDispatch::GetIsaName(dispatch.isa),
		dispatch.isa == CpuDispatch::Isa::Avx
			? "avx128"
			: dispatch.isa == CpuDispatch::Isa::Avx2 ? "avx2" : "avx512bw");
	const auto policy = CpuDispatch::GetUtf16ScanPolicy(dispatch.isa);
	EXPECT_EQ(policy.crOrLfMinimumLength, dispatch.utf16ScanPolicy.crOrLfMinimumLength);
	EXPECT_EQ(policy.markdownInlineSpecialMinimumLength,
		dispatch.utf16ScanPolicy.markdownInlineSpecialMinimumLength);
	EXPECT_EQ(policy.findCharMinimumLength, dispatch.utf16ScanPolicy.findCharMinimumLength);
	std::printf(
		"UTF16_DISPATCH build_mode=%s backend=%s isa=%s crlf=%s markdown=%s "
		"find_char=%s thresholds=%zu,%zu,%zu abi=%u\n",
		dispatch.utf16BuildMode,
		dispatch.utf16Backend,
		CpuDispatch::GetIsaName(dispatch.isa),
		dispatch.utf16CrOrLfImplementation,
		dispatch.utf16MarkdownImplementation,
		dispatch.utf16FindCharImplementation,
		dispatch.utf16ScanPolicy.crOrLfMinimumLength,
		dispatch.utf16ScanPolicy.markdownInlineSpecialMinimumLength,
		dispatch.utf16ScanPolicy.findCharMinimumLength,
		dispatch.utf16AbiVersion);
	// These accessors resolve to the selected production backend.
	EXPECT_EQ(dispatch.findCrOrLfUtf16,
		CpuDispatch::Testing::GetSupportedFindCrOrLfUtf16(dispatch.isa));
	EXPECT_EQ(dispatch.findMarkdownInlineSpecialUtf16,
		CpuDispatch::Testing::GetSupportedFindMarkdownInlineSpecialUtf16(dispatch.isa));
	EXPECT_EQ(dispatch.findUtf16Char,
		CpuDispatch::Testing::GetSupportedFindUtf16Char(dispatch.isa));
}

// This test must run in a fresh tests1 process before any other CpuDispatch
// test that calls Get(); function-local static initialization cannot be reset
// after the process has initialized the dispatch table.
TEST(CpuDispatchTest, ConcurrentInitializationAndDiagnosticsAreStable)
{
	constexpr std::size_t threadCount = 64;
	std::array<const CpuDispatch::Dispatch*, threadCount> dispatches{};
	std::vector<std::thread> workers;
	workers.reserve(threadCount);
	for (std::size_t index = 0; index < threadCount; ++index) {
		workers.emplace_back([&dispatches, index] {
			dispatches[index] = &CpuDispatch::Get();
		});
	}
	for (auto& worker : workers) {
		worker.join();
	}
	for (const auto* dispatch : dispatches) {
		ASSERT_EQ(&CpuDispatch::Get(), dispatch);
		EXPECT_STREQ(dispatches[0]->utf16Backend, dispatch->utf16Backend);
		EXPECT_STREQ(dispatches[0]->utf16BuildMode, dispatch->utf16BuildMode);
		EXPECT_STREQ(dispatches[0]->utf16CrOrLfImplementation,
			dispatch->utf16CrOrLfImplementation);
		EXPECT_EQ(dispatches[0]->utf16AbiVersion, dispatch->utf16AbiVersion);
	}
	std::printf("UTF16_FRESH_INIT threads=%zu build_mode=%s backend=%s isa=%s abi=%u\n",
		threadCount,
		dispatches[0]->utf16BuildMode,
		dispatches[0]->utf16Backend,
		CpuDispatch::GetIsaName(dispatches[0]->isa),
		dispatches[0]->utf16AbiVersion);
}

#if defined(SAKURA_UTF16_BACKEND_RUST)
namespace
{
using RustUtf16ScanFunction = std::size_t (*)(
	const std::uint16_t*, std::size_t) noexcept;
using RustUtf16FindCharFunction = std::size_t (*)(
	const std::uint16_t*, std::size_t, std::uint16_t) noexcept;

struct RustUtf16Implementation {
	const char* name;
	RustUtf16ScanFunction crOrLf;
	RustUtf16ScanFunction markdownSpecial;
	RustUtf16FindCharFunction findChar;
};

std::array<RustUtf16Implementation, 3> GetRustUtf16Implementations()
{
	const auto& dispatch = CpuDispatch::Get();
	return {
		RustUtf16Implementation{
			"avx128",
			dispatch.capabilities.avx ? sakura_utf16_find_cr_or_lf_avx128_v1 : nullptr,
			dispatch.capabilities.avx
				? sakura_utf16_find_markdown_special_avx128_v1 : nullptr,
			dispatch.capabilities.avx ? sakura_utf16_find_char_avx128_v1 : nullptr,
		},
		RustUtf16Implementation{
			"avx2",
			dispatch.capabilities.avx2 ? sakura_utf16_find_cr_or_lf_avx2_v1 : nullptr,
			dispatch.capabilities.avx2
				? sakura_utf16_find_markdown_special_avx2_v1 : nullptr,
			dispatch.capabilities.avx2 ? sakura_utf16_find_char_avx2_v1 : nullptr,
		},
		RustUtf16Implementation{
			"avx512bw",
			dispatch.capabilities.avx512 ? sakura_utf16_find_cr_or_lf_avx512bw_v1 : nullptr,
			dispatch.capabilities.avx512
				? sakura_utf16_find_markdown_special_avx512bw_v1 : nullptr,
			dispatch.capabilities.avx512 ? sakura_utf16_find_char_avx512bw_v1 : nullptr,
		},
	};
}

std::size_t GetRustUtf16VectorWidth(const RustUtf16Implementation& implementation)
{
	return std::string_view{implementation.name} == "avx128"
		? 8
		: std::string_view{implementation.name} == "avx2" ? 16 : 32;
}
}

TEST(CpuDispatchTest, RustUtf16ScannersMatchReferenceAtEveryBoundary)
{
	const auto implementations = GetRustUtf16Implementations();
	for (const auto& implementation : implementations) {
		std::printf("RUST_ISA_EXECUTION %s=%s\n", implementation.name,
			implementation.crOrLf == nullptr ? "skipped" : "executed");
	}

	std::vector<std::uint16_t> data(193, static_cast<std::uint16_t>('x'));
	for (const auto& implementation : implementations) {
		if (implementation.crOrLf == nullptr) {
			continue;
		}
		for (std::size_t length = 0; length <= data.size(); ++length) {
			for (std::size_t position = 0; position <= length; ++position) {
				std::fill(data.begin(), data.end(), static_cast<std::uint16_t>('x'));
				if (position < length) {
					data[position] = static_cast<std::uint16_t>('\n');
				}
				EXPECT_EQ(position, implementation.crOrLf(data.data(), length))
					<< "isa=" << implementation.name << " length=" << length
					<< " position=" << position;

				std::fill(data.begin(), data.end(), static_cast<std::uint16_t>('x'));
				if (position < length) {
					data[position] = static_cast<std::uint16_t>('$');
				}
				EXPECT_EQ(position, implementation.markdownSpecial(data.data(), length))
					<< "isa=" << implementation.name << " length=" << length
					<< " position=" << position;
			}
		}

		const std::size_t vectorWidth = GetRustUtf16VectorWidth(implementation);
		std::fill(data.begin(), data.end(), static_cast<std::uint16_t>('x'));
		data[vectorWidth - 1] = static_cast<std::uint16_t>('\r');
		data[vectorWidth] = static_cast<std::uint16_t>('\n');
		EXPECT_EQ(vectorWidth - 1, implementation.crOrLf(data.data(), data.size()))
			<< "isa=" << implementation.name;

		std::fill(data.begin(), data.end(), static_cast<std::uint16_t>('x'));
		data[vectorWidth / 2] = static_cast<std::uint16_t>('\r');
		data[vectorWidth / 2 + 1] = static_cast<std::uint16_t>('\n');
		EXPECT_EQ(vectorWidth / 2, implementation.crOrLf(data.data(), data.size()))
			<< "isa=" << implementation.name;

		std::fill(data.begin(), data.end(), static_cast<std::uint16_t>('x'));
		constexpr std::size_t firstSpecial = 8;
		for (std::size_t index = 0; index < kMarkdownInlineSpecials.size(); ++index) {
			data[firstSpecial + index] = static_cast<std::uint16_t>(kMarkdownInlineSpecials[index]);
		}
		EXPECT_EQ(firstSpecial, implementation.markdownSpecial(data.data(), data.size()))
			<< "isa=" << implementation.name;

		std::fill(data.begin(), data.end(), static_cast<std::uint16_t>('x'));
		data[17] = static_cast<std::uint16_t>(0xd83d);
		data[18] = static_cast<std::uint16_t>(0xde00);
		EXPECT_EQ(17U, implementation.findChar(data.data(), data.size(), 0xd83d))
			<< "isa=" << implementation.name;
		EXPECT_EQ(data.size(), implementation.findChar(data.data(), data.size(), 0x2603))
			<< "isa=" << implementation.name;
	}

	std::uint64_t state = 0x2395'16f1ULL;
	for (std::size_t caseIndex = 0; caseIndex < 50'000; ++caseIndex) {
		state = state * 6'364'136'223'846'793'005ULL + 1;
		const std::size_t length = static_cast<std::size_t>(state % 4098);
		data.resize(length);
		for (auto& value : data) {
			state = (state << 17) | (state >> 47);
			state += 0x9e37'79b9'7f4a'7c15ULL;
			value = static_cast<std::uint16_t>(state);
		}
		state = (state << 23) | (state >> 41);
		const auto target = static_cast<std::uint16_t>(state);
		const auto wideData = reinterpret_cast<const wchar_t*>(data.data());
		const auto expectedCr = FindCrOrLfUtf16Reference(wideData, length);
		const auto expectedMarkdown = FindMarkdownInlineSpecialUtf16Reference(wideData, length);
		const auto expectedChar = FindUtf16CharReference(
			wideData, length, static_cast<wchar_t>(target));
		for (const auto& implementation : implementations) {
			if (implementation.crOrLf == nullptr) {
				continue;
			}
			EXPECT_EQ(expectedCr, implementation.crOrLf(data.data(), length));
			EXPECT_EQ(expectedMarkdown, implementation.markdownSpecial(data.data(), length));
			EXPECT_EQ(expectedChar, implementation.findChar(data.data(), length, target));
		}
	}
}

TEST(CpuDispatchTest, RustUtf16ScannersRejectInvalidFfiSpans)
{
	const auto implementations = GetRustUtf16Implementations();

	for (const auto& implementation : implementations) {
		if (implementation.crOrLf == nullptr) {
			continue;
		}
		alignas(std::uint16_t) std::array<std::uint16_t, 2> alignedStorage{};
		const auto* const misaligned = reinterpret_cast<const std::uint16_t*>(
			reinterpret_cast<const unsigned char*>(alignedStorage.data()) + 1);
		const std::array<std::uint16_t, 1> alignedDummy{};
		const auto oversized = (SIZE_MAX / sizeof(std::uint16_t)) + 1;

		for (const auto scan : {implementation.crOrLf, implementation.markdownSpecial}) {
			EXPECT_EQ(0U, scan(nullptr, 0));
			EXPECT_EQ(7U, scan(nullptr, 7));
			EXPECT_EQ(1U, scan(misaligned, 1));
			EXPECT_EQ(1U, scan(reinterpret_cast<const std::uint16_t*>(SIZE_MAX), 1));
			EXPECT_EQ(2U, scan(reinterpret_cast<const std::uint16_t*>(SIZE_MAX - 1), 2));
			EXPECT_EQ(SIZE_MAX, scan(nullptr, SIZE_MAX));
			EXPECT_EQ(SIZE_MAX, scan(alignedDummy.data(), SIZE_MAX));
			EXPECT_EQ(oversized, scan(alignedDummy.data(), oversized));
			EXPECT_EQ(0U, scan(reinterpret_cast<const std::uint16_t*>(2), 0));
		}
		for (const auto target : {wchar_t{0}, wchar_t{0xd800}}) {
			EXPECT_EQ(0U, implementation.findChar(nullptr, 0, target));
			EXPECT_EQ(7U, implementation.findChar(nullptr, 7, target));
			EXPECT_EQ(1U, implementation.findChar(misaligned, 1, target));
			EXPECT_EQ(1U, implementation.findChar(
				reinterpret_cast<const std::uint16_t*>(SIZE_MAX), 1, target));
			EXPECT_EQ(2U, implementation.findChar(
				reinterpret_cast<const std::uint16_t*>(SIZE_MAX - 1), 2, target));
			EXPECT_EQ(SIZE_MAX, implementation.findChar(nullptr, SIZE_MAX, target));
			EXPECT_EQ(SIZE_MAX, implementation.findChar(alignedDummy.data(), SIZE_MAX, target));
			EXPECT_EQ(oversized, implementation.findChar(alignedDummy.data(), oversized, target));
			EXPECT_EQ(0U, implementation.findChar(reinterpret_cast<const std::uint16_t*>(2), 0, target));
		}
	}
}

TEST(CpuDispatchTest, RustUtf16ScannersMatchEveryLegalAlignment)
{
	const auto implementations = GetRustUtf16Implementations();
	constexpr std::array<std::size_t, 13> lengths{
		1, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65,
	};
	constexpr std::size_t maxLength = 65;
	std::array<std::uint16_t, maxLength + 32> storage{};

	for (const auto& implementation : implementations) {
		if (implementation.crOrLf == nullptr) {
			std::printf("RUST_ALIGNMENT_EXECUTION %s=skipped\n", implementation.name);
			continue;
		}
		std::printf("RUST_ALIGNMENT_EXECUTION %s=executed\n", implementation.name);
		for (std::size_t byteOffset = 0; byteOffset < 64; byteOffset += 2) {
			auto* const data = reinterpret_cast<std::uint16_t*>(
				reinterpret_cast<unsigned char*>(storage.data()) + byteOffset);
			for (const auto length : lengths) {
				std::fill(data, data + length, static_cast<std::uint16_t>('x'));
				EXPECT_EQ(length, implementation.crOrLf(data, length));
				EXPECT_EQ(length, implementation.markdownSpecial(data, length));
				EXPECT_EQ(length, implementation.findChar(data, length, 0x2603));

				for (const auto position : {std::size_t{0}, length - 1}) {
					std::fill(data, data + length, static_cast<std::uint16_t>('x'));
					data[position] = static_cast<std::uint16_t>('\n');
					EXPECT_EQ(position, implementation.crOrLf(data, length))
						<< "isa=" << implementation.name
						<< " byteOffset=" << byteOffset << " length=" << length;

					std::fill(data, data + length, static_cast<std::uint16_t>('x'));
					data[position] = static_cast<std::uint16_t>('$');
					EXPECT_EQ(position, implementation.markdownSpecial(data, length))
						<< "isa=" << implementation.name
						<< " byteOffset=" << byteOffset << " length=" << length;

					std::fill(data, data + length, static_cast<std::uint16_t>('x'));
					data[position] = 0x2603;
					EXPECT_EQ(position, implementation.findChar(data, length, 0x2603))
						<< "isa=" << implementation.name
						<< " byteOffset=" << byteOffset << " length=" << length;
				}
			}
		}
	}
}

TEST(CpuDispatchTest, RustUtf16ScannersTouchBothGuardPagesAtEveryShortLength)
{
	const auto implementations = GetRustUtf16Implementations();
	SYSTEM_INFO systemInfo{};
	::GetSystemInfo(&systemInfo);
	const std::size_t pageSize = systemInfo.dwPageSize;
	std::unique_ptr<void, VirtualAllocationDeleter> allocation{
		::VirtualAlloc(nullptr, pageSize * 3, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
	};
	ASSERT_NE(nullptr, allocation);
	DWORD previousProtection{};
	ASSERT_TRUE(::VirtualProtect(
		allocation.get(), pageSize, PAGE_NOACCESS, &previousProtection));
	ASSERT_TRUE(::VirtualProtect(
		static_cast<char*>(allocation.get()) + pageSize * 2,
		pageSize, PAGE_NOACCESS, &previousProtection));

	std::uint16_t* const middle = reinterpret_cast<std::uint16_t*>(
		static_cast<char*>(allocation.get()) + pageSize);
	for (const auto& implementation : implementations) {
		if (implementation.crOrLf == nullptr) {
			std::printf("RUST_GUARD_EXECUTION %s=skipped\n", implementation.name);
			continue;
		}
		std::printf("RUST_GUARD_EXECUTION %s=executed\n", implementation.name);
		const std::size_t vectorWidth = GetRustUtf16VectorWidth(implementation);
		const std::size_t middleUnits = pageSize / sizeof(std::uint16_t);

		for (std::size_t length = 1; length <= 65; ++length) {
			for (const bool rightBoundary : {false, true}) {
				// The left span begins at the first readable code unit; the
				// right span ends at the first code unit of the guard page.
				std::uint16_t* const data = rightBoundary
					? middle + middleUnits - length
					: middle;
				ASSERT_TRUE(::VirtualProtect(
					middle, pageSize, PAGE_READWRITE, &previousProtection));
				std::fill(data, data + length, static_cast<std::uint16_t>('x'));
				ASSERT_TRUE(::VirtualProtect(
					middle, pageSize, PAGE_READONLY, &previousProtection));
				EXPECT_EQ(length, implementation.crOrLf(data, length));
				EXPECT_EQ(length, implementation.markdownSpecial(data, length));
				EXPECT_EQ(length, implementation.findChar(data, length, 0x2603));

				const auto clampPosition = [length](std::size_t position) {
					return std::min(position, length - 1);
				};
				const std::array<std::size_t, 8> positions{
					0,
					length - 1,
					clampPosition(vectorWidth - 1),
					clampPosition(vectorWidth),
					clampPosition(vectorWidth + 1),
					clampPosition(2 * vectorWidth - 1),
					clampPosition(2 * vectorWidth),
					clampPosition(2 * vectorWidth + 1),
				};
				for (const auto position : positions) {
					ASSERT_TRUE(::VirtualProtect(
						middle, pageSize, PAGE_READWRITE, &previousProtection));
					std::fill(data, data + length, static_cast<std::uint16_t>('x'));
					data[position] = static_cast<std::uint16_t>('\n');
					ASSERT_TRUE(::VirtualProtect(
						middle, pageSize, PAGE_READONLY, &previousProtection));
					EXPECT_EQ(position, implementation.crOrLf(data, length));

					ASSERT_TRUE(::VirtualProtect(
						middle, pageSize, PAGE_READWRITE, &previousProtection));
					std::fill(data, data + length, static_cast<std::uint16_t>('x'));
					data[position] = static_cast<std::uint16_t>('$');
					ASSERT_TRUE(::VirtualProtect(
						middle, pageSize, PAGE_READONLY, &previousProtection));
					EXPECT_EQ(position, implementation.markdownSpecial(data, length));

					ASSERT_TRUE(::VirtualProtect(
						middle, pageSize, PAGE_READWRITE, &previousProtection));
					std::fill(data, data + length, static_cast<std::uint16_t>('x'));
					data[position] = 0x2603;
					ASSERT_TRUE(::VirtualProtect(
						middle, pageSize, PAGE_READONLY, &previousProtection));
					EXPECT_EQ(position, implementation.findChar(data, length, 0x2603));
				}
			}
		}
	}
}
#endif

TEST(CpuDispatchTest, WidenAsciiMatchesReferenceAtLengthsAndStopPositions)
{
	constexpr std::array<std::size_t, 17> lengths{
		0, 1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 66, 127, 128, 129, 192, 256,
	};
	constexpr wchar_t kSentinel = static_cast<wchar_t>(0xCCCC);
	std::string source(320, 'x');
	std::vector<wchar_t> expected(320, kSentinel);
	std::vector<wchar_t> actual(320, kSentinel);

	for (const auto isa : kImplementations) {
		const auto widen = CpuDispatch::Testing::GetSupportedWidenAsciiToUtf16(isa);
		if (widen == nullptr) {
			continue;
		}
		for (const auto length : lengths) {
			// position == length means the whole input is ASCII.
			for (std::size_t position = 0; position <= length; ++position) {
				for (std::size_t index = 0; index < length; ++index) {
					// Cover the full ASCII byte range including NUL and 0x7f.
					source[index] = static_cast<char>((index * 37 + position) % 0x80);
				}
				if (position < length) {
					source[position] = static_cast<char>(0xE6);
				}
				std::fill(expected.begin(), expected.end(), kSentinel);
				std::fill(actual.begin(), actual.end(), kSentinel);
				const std::size_t expectedRun =
					WidenAsciiToUtf16Reference(source.data(), length, expected.data());
				EXPECT_EQ(expectedRun, widen(source.data(), length, actual.data()))
					<< "isa=" << CpuDispatch::GetIsaName(isa)
					<< " length=" << length << " position=" << position;
				EXPECT_EQ(expected, actual)
					<< "isa=" << CpuDispatch::GetIsaName(isa)
					<< " length=" << length << " position=" << position;
			}
		}
	}
}

TEST(CpuDispatchTest, WidenAsciiMatchesReferenceAtEveryInputAlignment)
{
	constexpr std::size_t length = 192;
	constexpr wchar_t kSentinel = static_cast<wchar_t>(0xCCCC);
	alignas(64) std::array<char, length + 63> storage{};
	std::vector<wchar_t> expected(length + 1, kSentinel);
	std::vector<wchar_t> actual(length + 1, kSentinel);

	for (const auto isa : kImplementations) {
		const auto widen = CpuDispatch::Testing::GetSupportedWidenAsciiToUtf16(isa);
		if (widen == nullptr) {
			continue;
		}
		for (std::size_t alignmentOffset = 0; alignmentOffset < 64; ++alignmentOffset) {
			char* const source = storage.data() + alignmentOffset;
			for (std::size_t position = 0; position <= length; ++position) {
				for (std::size_t index = 0; index < length; ++index) {
					source[index] = static_cast<char>('!' + (index + alignmentOffset) % 0x5e);
				}
				if (position < length) {
					source[position] = static_cast<char>(0x80);
				}
				std::fill(expected.begin(), expected.end(), kSentinel);
				std::fill(actual.begin(), actual.end(), kSentinel);
				const std::size_t expectedRun =
					WidenAsciiToUtf16Reference(source, length, expected.data());
				EXPECT_EQ(expectedRun, widen(source, length, actual.data()))
					<< "isa=" << CpuDispatch::GetIsaName(isa)
					<< " alignmentOffset=" << alignmentOffset
					<< " position=" << position;
				EXPECT_EQ(expected, actual)
					<< "isa=" << CpuDispatch::GetIsaName(isa)
					<< " alignmentOffset=" << alignmentOffset
					<< " position=" << position;
			}
		}
	}
}

TEST(CpuDispatchTest, WidenAsciiDoesNotReadPastInputGuardPage)
{
	SYSTEM_INFO systemInfo{};
	::GetSystemInfo(&systemInfo);
	const std::size_t pageSize = systemInfo.dwPageSize;
	std::unique_ptr<void, VirtualAllocationDeleter> allocation{
		::VirtualAlloc(nullptr, pageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
	};
	ASSERT_NE(nullptr, allocation);
	DWORD previousProtection{};
	ASSERT_TRUE(::VirtualProtect(
		static_cast<char*>(allocation.get()) + pageSize,
		pageSize,
		PAGE_NOACCESS,
		&previousProtection));

	char* const pageEnd = static_cast<char*>(allocation.get()) + pageSize;
	std::vector<wchar_t> destination(256);
	for (const auto isa : kImplementations) {
		const auto widen = CpuDispatch::Testing::GetSupportedWidenAsciiToUtf16(isa);
		if (widen == nullptr) {
			continue;
		}
		// Whole inputs of 1..130 bytes ending exactly at the guard page cover
		// the masked AVX-512 tail and the AVX/AVX2 scalar tail loops.
		for (std::size_t length = 1; length <= 130; ++length) {
			char* const source = pageEnd - length;
			std::fill(source, source + length, 'x');
			EXPECT_EQ(length, widen(source, length, destination.data()))
				<< "isa=" << CpuDispatch::GetIsaName(isa) << " length=" << length;

			source[length - 1] = static_cast<char>(0x80);
			EXPECT_EQ(length - 1, widen(source, length, destination.data()))
				<< "isa=" << CpuDispatch::GetIsaName(isa) << " length=" << length;
		}
	}
}

TEST(CpuDispatchTest, WidenAsciiDoesNotWritePastTheAsciiRun)
{
	SYSTEM_INFO systemInfo{};
	::GetSystemInfo(&systemInfo);
	const std::size_t pageSize = systemInfo.dwPageSize;
	std::unique_ptr<void, VirtualAllocationDeleter> allocation{
		::VirtualAlloc(nullptr, pageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
	};
	ASSERT_NE(nullptr, allocation);
	DWORD previousProtection{};
	ASSERT_TRUE(::VirtualProtect(
		static_cast<char*>(allocation.get()) + pageSize,
		pageSize,
		PAGE_NOACCESS,
		&previousProtection));

	wchar_t* const pageEnd = reinterpret_cast<wchar_t*>(
		static_cast<char*>(allocation.get()) + pageSize);
	std::string source(200, 'x');
	for (const auto isa : kImplementations) {
		const auto widen = CpuDispatch::Testing::GetSupportedWidenAsciiToUtf16(isa);
		if (widen == nullptr) {
			continue;
		}
		// The destination has room for exactly the ASCII run and ends at the
		// guard page: one unit written past the run faults immediately.
		for (std::size_t run = 1; run <= 130; ++run) {
			const std::size_t length = run + 8;
			std::fill(source.begin(), source.begin() + length, 'x');
			source[run] = static_cast<char>(0xE6);
			wchar_t* const destination = pageEnd - run;
			EXPECT_EQ(run, widen(source.data(), length, destination))
				<< "isa=" << CpuDispatch::GetIsaName(isa) << " run=" << run;
		}
	}
}

TEST(CpuDispatchTest, FindUtf16CharMatchesReferenceAtLengthsAndPositions)
{
	std::wstring data(705, L'\u65e5');
	constexpr std::array<std::size_t, 20> lengths{
		0, 1, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 66, 127, 128, 129, 256, 704,
	};

	for (const auto isa : kImplementations) {
		const auto findChar = CpuDispatch::Testing::GetSupportedFindUtf16Char(isa);
		if (findChar == nullptr) {
			continue;
		}
		for (const auto length : lengths) {
			for (std::size_t position = 0; position <= length; ++position) {
				std::fill(data.begin(), data.end(), L'\u65e5');
				if (position < length) {
					data[position] = L'q';
				}
				EXPECT_EQ(FindUtf16CharReference(data.data(), length, L'q'),
					findChar(data.data(), length, L'q'))
					<< "isa=" << CpuDispatch::GetIsaName(isa)
					<< " length=" << length << " position=" << position;
			}
			// Zeroed invalid lanes in the masked AVX-512 tail must not match a
			// NUL target.
			std::fill(data.begin(), data.end(), L'x');
			EXPECT_EQ(length, findChar(data.data(), length, L'\0'))
				<< "isa=" << CpuDispatch::GetIsaName(isa) << " length=" << length;
		}
	}
}

TEST(CpuDispatchTest, FindUtf16CharDoesNotReadPastGuardPage)
{
	SYSTEM_INFO systemInfo{};
	::GetSystemInfo(&systemInfo);
	const std::size_t pageSize = systemInfo.dwPageSize;
	std::unique_ptr<void, VirtualAllocationDeleter> allocation{
		::VirtualAlloc(nullptr, pageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
	};
	ASSERT_NE(nullptr, allocation);
	DWORD previousProtection{};
	ASSERT_TRUE(::VirtualProtect(
		static_cast<char*>(allocation.get()) + pageSize,
		pageSize,
		PAGE_NOACCESS,
		&previousProtection));

	wchar_t* const pageEnd = reinterpret_cast<wchar_t*>(
		static_cast<char*>(allocation.get()) + pageSize);
	for (const auto isa : kImplementations) {
		const auto findChar = CpuDispatch::Testing::GetSupportedFindUtf16Char(isa);
		if (findChar == nullptr) {
			continue;
		}
		for (std::size_t length = 1; length <= 70; ++length) {
			wchar_t* const data = pageEnd - length;
			std::fill(data, data + length, L'\u754c');
			EXPECT_EQ(length, findChar(data, length, L'q'))
				<< "isa=" << CpuDispatch::GetIsaName(isa) << " length=" << length;
			data[length - 1] = L'q';
			EXPECT_EQ(length - 1, findChar(data, length, L'q'))
				<< "isa=" << CpuDispatch::GetIsaName(isa) << " length=" << length;
		}
	}
}

TEST(CpuDispatchTest, Utf16ScanPolicyLocksPerIsaMinimumLengths)
{
	using CpuDispatch::GetUtf16ScanPolicy;
	using CpuDispatch::Isa;

	// AVX and AVX2 minimums equal their implementations' internal vector
	// widths; delegating below them reaches only the scalar fallback behind an
	// indirect call. The AVX-512 minimums are benchmark-derived break-even
	// points for the masked-load short path.
	const auto avx = GetUtf16ScanPolicy(Isa::Avx);
	EXPECT_EQ(8U, avx.crOrLfMinimumLength);
	EXPECT_EQ(8U, avx.markdownInlineSpecialMinimumLength);
	EXPECT_EQ(8U, avx.findCharMinimumLength);

	const auto avx2 = GetUtf16ScanPolicy(Isa::Avx2);
	EXPECT_EQ(16U, avx2.crOrLfMinimumLength);
	EXPECT_EQ(16U, avx2.markdownInlineSpecialMinimumLength);
	EXPECT_EQ(16U, avx2.findCharMinimumLength);

	const auto avx512 = GetUtf16ScanPolicy(Isa::Avx512);
	EXPECT_EQ(8U, avx512.crOrLfMinimumLength);
	EXPECT_EQ(6U, avx512.markdownInlineSpecialMinimumLength);
	EXPECT_EQ(16U, avx512.findCharMinimumLength);

	// The frozen process dispatch must carry the policy of its selected ISA.
	const auto& dispatch = CpuDispatch::Get();
	const auto selected = GetUtf16ScanPolicy(dispatch.isa);
	EXPECT_EQ(selected.crOrLfMinimumLength,
		dispatch.utf16ScanPolicy.crOrLfMinimumLength);
	EXPECT_EQ(selected.markdownInlineSpecialMinimumLength,
		dispatch.utf16ScanPolicy.markdownInlineSpecialMinimumLength);
	EXPECT_EQ(selected.findCharMinimumLength,
		dispatch.utf16ScanPolicy.findCharMinimumLength);
}

TEST(CpuDispatchTest, Utf8ConversionPolicyLocksPerIsaMinimumLengths)
{
	using CpuDispatch::GetUtf8ConversionPolicy;
	using CpuDispatch::Isa;

	// AVX and AVX2 minimums equal their widening kernels' input vector widths;
	// the AVX-512 minimum is the benchmark-derived break-even for the masked
	// short path.
	EXPECT_EQ(16U, GetUtf8ConversionPolicy(Isa::Avx).widenAsciiMinimumLength);
	EXPECT_EQ(32U, GetUtf8ConversionPolicy(Isa::Avx2).widenAsciiMinimumLength);
	EXPECT_EQ(16U, GetUtf8ConversionPolicy(Isa::Avx512).widenAsciiMinimumLength);

	const auto& dispatch = CpuDispatch::Get();
	EXPECT_EQ(GetUtf8ConversionPolicy(dispatch.isa).widenAsciiMinimumLength,
		dispatch.utf8ConversionPolicy.widenAsciiMinimumLength);
}

TEST(CpuDispatchTest, Utf16ScannersDoNotReadPastGuardPageForDirectShortInputs)
{
	SYSTEM_INFO systemInfo{};
	::GetSystemInfo(&systemInfo);
	const std::size_t pageSize = systemInfo.dwPageSize;
	std::unique_ptr<void, VirtualAllocationDeleter> allocation{
		::VirtualAlloc(nullptr, pageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
	};
	ASSERT_NE(nullptr, allocation);
	DWORD previousProtection{};
	ASSERT_TRUE(::VirtualProtect(
		static_cast<char*>(allocation.get()) + pageSize,
		pageSize,
		PAGE_NOACCESS,
		&previousProtection));

	wchar_t* const pageEnd = reinterpret_cast<wchar_t*>(
		static_cast<char*>(allocation.get()) + pageSize);
	for (const auto isa : kImplementations) {
		const auto crOrLf = CpuDispatch::Testing::GetSupportedFindCrOrLfUtf16(isa);
		const auto markdownSpecial =
			CpuDispatch::Testing::GetSupportedFindMarkdownInlineSpecialUtf16(isa);
		if (crOrLf == nullptr || markdownSpecial == nullptr) {
			continue;
		}
		// Whole inputs of 1..63 units ending exactly at the guard page: this is
		// the masked-load short path on AVX-512 (main vector plus 1..31 tail)
		// and the scalar fallback below vector width on AVX/AVX2.
		for (std::size_t length = 1; length <= 63; ++length) {
			wchar_t* const data = pageEnd - length;
			std::fill(data, data + length, L'\u754c');
			EXPECT_EQ(length, crOrLf(data, length))
				<< "isa=" << CpuDispatch::GetIsaName(isa)
				<< " length=" << length;
			EXPECT_EQ(length, markdownSpecial(data, length))
				<< "isa=" << CpuDispatch::GetIsaName(isa)
				<< " length=" << length;

			for (std::size_t position = 0; position < length; ++position) {
				data[position] = L'\n';
				EXPECT_EQ(position, crOrLf(data, length))
					<< "isa=" << CpuDispatch::GetIsaName(isa)
					<< " length=" << length << " position=" << position;
				data[position] = L'$';
				EXPECT_EQ(position, markdownSpecial(data, length))
					<< "isa=" << CpuDispatch::GetIsaName(isa)
					<< " length=" << length << " position=" << position;
				data[position] = L'\u754c';
			}
		}
	}
}

// Manual microbenchmark used to derive the Utf16ScanPolicy minimum lengths.
// Run explicitly with a Release tests1 build:
//   tests1.exe --gtest_also_run_disabled_tests \
//     --gtest_filter=CpuDispatchTest.DISABLED_Utf16ShortInputMicrobenchmark
TEST(CpuDispatchTest, DISABLED_Utf16ShortInputMicrobenchmark)
{
	if (Utf16Phase8Benchmark::TryRunFromEnvironment()) return;

	constexpr std::array<std::size_t, 10> lengths{1, 2, 4, 6, 8, 12, 16, 24, 32, 48};
	constexpr int iterations = 2'000'000;
	alignas(64) std::array<wchar_t, 64> buffer{};
	buffer.fill(L'x');

	LARGE_INTEGER frequency{};
	::QueryPerformanceFrequency(&frequency);
	const auto measure = [&](auto&& function, std::size_t length) {
		volatile std::size_t sink = 0;
		LARGE_INTEGER begin{};
		LARGE_INTEGER end{};
		::QueryPerformanceCounter(&begin);
		for (int i = 0; i < iterations; ++i) {
			sink = sink + function(buffer.data(), length);
		}
		::QueryPerformanceCounter(&end);
		return static_cast<double>(end.QuadPart - begin.QuadPart)
			* 1e9 / frequency.QuadPart / iterations;
	};

	for (const auto length : lengths) {
		printf("length=%2zu scalarCrOrLf=%7.2fns scalarSpecial=%7.2fns",
			length,
			measure(FindCrOrLfUtf16Reference, length),
			measure(FindMarkdownInlineSpecialUtf16Reference, length));
		for (const auto isa : kImplementations) {
			const auto crOrLf = CpuDispatch::Testing::GetSupportedFindCrOrLfUtf16(isa);
			const auto markdownSpecial =
				CpuDispatch::Testing::GetSupportedFindMarkdownInlineSpecialUtf16(isa);
			if (crOrLf == nullptr || markdownSpecial == nullptr) {
				continue;
			}
			printf(" %sCrOrLf=%7.2fns %sSpecial=%7.2fns",
				CpuDispatch::GetIsaName(isa), measure(crOrLf, length),
				CpuDispatch::GetIsaName(isa), measure(markdownSpecial, length));
		}
		printf("\n");
	}
}

TEST(CUtf8Test, Utf8ToUniAsciiFastPathIsOutputInvisible)
{
	std::vector<std::string> corpora;

	// Pure ASCII at vector-boundary lengths.
	for (const std::size_t length :
		{std::size_t{0}, std::size_t{1}, std::size_t{15}, std::size_t{16},
		 std::size_t{17}, std::size_t{31}, std::size_t{32}, std::size_t{33},
		 std::size_t{63}, std::size_t{64}, std::size_t{65}, std::size_t{127},
		 std::size_t{128}, std::size_t{129}, std::size_t{4096}}) {
		std::string ascii(length, 'x');
		for (std::size_t index = 0; index < length; ++index) {
			ascii[index] = static_cast<char>((index * 31) % 0x80);
		}
		corpora.push_back(std::move(ascii));
	}

	// ASCII broken by a multi-byte character at every position of the first
	// 130 bytes: exercises every fast-path handoff offset.
	for (std::size_t position = 0; position < 130; ++position) {
		std::string mixed(160, 'a');
		mixed.replace(position, 3, "\xE6\x97\xA5");
		corpora.push_back(std::move(mixed));
	}

	// Japanese-heavy text with short ASCII islands.
	std::string japanese;
	for (int i = 0; i < 64; ++i) {
		japanese += "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E abc 123 ";
	}
	corpora.push_back(std::move(japanese));

	// Astral characters as UTF-8 (4-byte) and CESU-8 (6-byte) sequences.
	corpora.push_back("A\xF0\x9F\x98\x80Z");
	corpora.push_back("A\xED\xA0\xBD\xED\xBA\x80Z");

	// Deterministic byte soup including invalid sequences and NUL bytes: the
	// fast path must stay invisible on the binary/protection branches too.
	std::mt19937 rng(20260808u);
	std::uniform_int_distribution<int> anyByte(0, 255);
	std::string soup(2048, '\0');
	for (auto& byte : soup) {
		byte = static_cast<char>(anyByte(rng));
	}
	corpora.push_back(std::move(soup));
	std::uniform_int_distribution<int> biased(0, 9);
	std::string biasedSoup(2048, '\0');
	for (auto& byte : biasedSoup) {
		byte = biased(rng) < 7
			? static_cast<char>(anyByte(rng) % 0x80)
			: static_cast<char>(anyByte(rng));
	}
	corpora.push_back(std::move(biasedSoup));

	constexpr wchar_t kSentinel = static_cast<wchar_t>(0xCCCC);
	for (std::size_t corpusIndex = 0; corpusIndex < corpora.size(); ++corpusIndex) {
		const std::string& source = corpora[corpusIndex];
		const int sourceLength = static_cast<int>(source.size());
		for (const bool cesu8Mode : {false, true}) {
			std::vector<wchar_t> expected(source.size() + 8, kSentinel);
			std::vector<wchar_t> actual(source.size() + 8, kSentinel);
			const int expectedLength = Utf8ToUniPerCharacterReference(
				source.data(), sourceLength, expected.data(), cesu8Mode);
			const int actualLength = CUtf8Access::DecodeForTest(
				source.data(), sourceLength, actual.data(), cesu8Mode);
			EXPECT_EQ(expectedLength, actualLength)
				<< "corpus=" << corpusIndex << " cesu8=" << cesu8Mode;
			EXPECT_EQ(expected, actual)
				<< "corpus=" << corpusIndex << " cesu8=" << cesu8Mode;
		}
	}
}

// Manual microbenchmark used to derive the Utf8ConversionPolicy minimum
// lengths. Run explicitly with a Release tests1 build:
//   tests1.exe --gtest_also_run_disabled_tests \
//     --gtest_filter=CpuDispatchTest.DISABLED_WidenAsciiMicrobenchmark
TEST(CpuDispatchTest, DISABLED_WidenAsciiMicrobenchmark)
{
	constexpr std::array<std::size_t, 13> lengths{
		1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 128, 512, 4096,
	};
	constexpr int iterations = 2'000'000;
	std::string source(4096, 'x');
	std::vector<wchar_t> destination(4096);

	LARGE_INTEGER frequency{};
	::QueryPerformanceFrequency(&frequency);
	const auto measure = [&](auto&& function, std::size_t length) {
		volatile std::size_t sink = 0;
		LARGE_INTEGER begin{};
		LARGE_INTEGER end{};
		::QueryPerformanceCounter(&begin);
		for (int i = 0; i < iterations; ++i) {
			sink = sink + function(source.data(), length, destination.data());
		}
		::QueryPerformanceCounter(&end);
		return static_cast<double>(end.QuadPart - begin.QuadPart)
			* 1e9 / frequency.QuadPart / iterations;
	};

	for (const auto length : lengths) {
		printf("length=%4zu scalarWiden=%8.2fns", length,
			measure(WidenAsciiToUtf16Reference, length));
		for (const auto isa : kImplementations) {
			const auto widen = CpuDispatch::Testing::GetSupportedWidenAsciiToUtf16(isa);
			if (widen == nullptr) {
				continue;
			}
			printf(" %sWiden=%8.2fns", CpuDispatch::GetIsaName(isa),
				measure(widen, length));
		}
		printf("\n");
	}
}

// Manual end-to-end conversion microbenchmark: the per-character reference
// loop against the production Utf8ToUni with the dispatched ASCII fast path.
//   tests1.exe --gtest_also_run_disabled_tests \
//     --gtest_filter=CUtf8Test.DISABLED_Utf8ToUniMicrobenchmark
TEST(CUtf8Test, DISABLED_Utf8ToUniMicrobenchmark)
{
	std::string pureAscii(4096, 'x');
	for (std::size_t index = 0; index < pureAscii.size(); ++index) {
		pureAscii[index] = static_cast<char>('!' + index % 0x5e);
	}
	std::string japaneseHeavy;
	for (int i = 0; i < 128; ++i) {
		japaneseHeavy += "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE3\x83\x86\xE3\x82\xAD\xE3\x82\xB9\xE3\x83\x88";
	}
	std::string mixedMarkdown;
	for (int i = 0; i < 64; ++i) {
		mixedMarkdown += "## \xE8\xA6\x8B\xE5\x87\xBA\xE3\x81\x97 heading `code span` **bold** \xE6\x9C\xAC\xE6\x96\x87 text\n";
	}
	const std::array<std::pair<const char*, const std::string*>, 3> corpora{{
		{"pureAscii", &pureAscii},
		{"japaneseHeavy", &japaneseHeavy},
		{"mixedMarkdown", &mixedMarkdown},
	}};

	constexpr int iterations = 200'000;
	LARGE_INTEGER frequency{};
	::QueryPerformanceFrequency(&frequency);
	const auto measure = [&](auto function, const std::string& source,
			std::vector<wchar_t>& destination) {
		volatile int sink = 0;
		LARGE_INTEGER begin{};
		LARGE_INTEGER end{};
		::QueryPerformanceCounter(&begin);
		for (int i = 0; i < iterations; ++i) {
			sink = sink + function(
				source.data(), static_cast<int>(source.size()), destination.data(), false);
		}
		::QueryPerformanceCounter(&end);
		return static_cast<double>(end.QuadPart - begin.QuadPart)
			* 1e9 / frequency.QuadPart / iterations;
	};

	for (const auto& [name, source] : corpora) {
		std::vector<wchar_t> destination(source->size() + 8);
		const double referenceNs =
			measure(Utf8ToUniPerCharacterReference, *source, destination);
		const double productionNs =
			measure(CUtf8Access::DecodeForTest, *source, destination);
		printf("corpus=%-14s bytes=%5zu perCharRef=%10.1fns production=%10.1fns speedup=%5.2fx\n",
			name, source->size(), referenceNs, productionNs,
			productionNs > 0.0 ? referenceNs / productionNs : 0.0);
	}
}

// Manual microbenchmark used to derive the find-char minimum lengths. The
// target is absent so every call scans the whole input (the worst case).
//   tests1.exe --gtest_also_run_disabled_tests \
//     --gtest_filter=CpuDispatchTest.DISABLED_FindUtf16CharMicrobenchmark
TEST(CpuDispatchTest, DISABLED_FindUtf16CharMicrobenchmark)
{
	constexpr std::array<std::size_t, 12> lengths{
		1, 2, 4, 6, 8, 12, 16, 24, 32, 48, 64, 128,
	};
	constexpr int iterations = 2'000'000;
	std::wstring data(128, L'\u65e5');

	LARGE_INTEGER frequency{};
	::QueryPerformanceFrequency(&frequency);
	const auto measure = [&](auto&& function, std::size_t length) {
		volatile std::size_t sink = 0;
		LARGE_INTEGER begin{};
		LARGE_INTEGER end{};
		::QueryPerformanceCounter(&begin);
		for (int i = 0; i < iterations; ++i) {
			sink = sink + function(data.data(), length, L'q');
		}
		::QueryPerformanceCounter(&end);
		return static_cast<double>(end.QuadPart - begin.QuadPart)
			* 1e9 / frequency.QuadPart / iterations;
	};

	for (const auto length : lengths) {
		printf("length=%3zu scalarFindChar=%7.2fns", length,
			measure(FindUtf16CharReference, length));
		for (const auto isa : kImplementations) {
			const auto findChar = CpuDispatch::Testing::GetSupportedFindUtf16Char(isa);
			if (findChar == nullptr) {
				continue;
			}
			printf(" %sFindChar=%7.2fns", CpuDispatch::GetIsaName(isa),
				measure(findChar, length));
		}
		printf("\n");
	}
}
