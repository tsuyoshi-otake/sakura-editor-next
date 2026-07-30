/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"
#include "util/CpuDispatch.h"

#include <array>
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
	std::string data(257, 'x');
	constexpr std::array<std::size_t, 15> lengths{
		0, 1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 257
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

TEST(CpuDispatchTest, ProcessDispatchIsStable)
{
	const auto* first = &CpuDispatch::Initialize();
	const auto* second = &CpuDispatch::Get();
	EXPECT_EQ(first, second);
	ASSERT_NE(nullptr, first->findCrOrLf);
	EXPECT_EQ(3U, first->findCrOrLf("abc\rdef", 7));
}
