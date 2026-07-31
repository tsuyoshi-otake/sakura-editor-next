/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "util/CpuDispatch.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>

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

struct VirtualAllocationDeleter {
	void operator()(void* allocation) const noexcept
	{
		if (allocation != nullptr) {
			::VirtualFree(allocation, 0, MEM_RELEASE);
		}
	}
};
}

TEST(CpuDispatchTest, SelectsHighestSafeIsa)
{
	using CpuDispatch::Capabilities;
	using CpuDispatch::Isa;
	EXPECT_EQ(Isa::Avx, CpuDispatch::SelectBestIsa(Capabilities{}));
	EXPECT_EQ(Isa::Avx, CpuDispatch::SelectBestIsa(Capabilities{true, false, false}));
	EXPECT_EQ(Isa::Avx2, CpuDispatch::SelectBestIsa(Capabilities{true, true, false}));
	EXPECT_EQ(Isa::Avx512, CpuDispatch::SelectBestIsa(Capabilities{true, true, true}));
	EXPECT_EQ(Isa::Avx, CpuDispatch::SelectBestIsa(Capabilities{false, true, true}));
	EXPECT_EQ(Isa::Avx, CpuDispatch::SelectBestIsa(Capabilities{true, false, true}));
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

TEST(CpuDispatchTest, ProcessDispatchIsStable)
{
	const auto* first = &CpuDispatch::Initialize();
	const auto* second = &CpuDispatch::Get();
	EXPECT_EQ(first, second);
	ASSERT_NE(nullptr, first->findCrOrLf);
	EXPECT_EQ(3U, first->findCrOrLf("abc\rdef", 7));
}
