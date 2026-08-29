#include "pch.h"

#include <sakura/harnessbridge/HarnessBridgeSecurity.h>

#include <array>

namespace platform::harnessbridge {
namespace {

TEST(HarnessBridgeSecurity, NamesUseOnlyDistinctSafeHashPrefix)
{
	const std::wstring hash(64, L'a');
	const auto pipe = BuildHarnessPipeName(hash);
	const auto mapping = BuildHarnessEndpointMappingName(hash);
	EXPECT_TRUE(IsSafeHarnessPipeName(pipe));
	EXPECT_TRUE(IsSafeHarnessEndpointMappingName(mapping));
	EXPECT_FALSE(IsSafeHarnessPipeName(L"\\\\.\\pipe\\SakuraControl-" + hash));
	EXPECT_FALSE(IsSafeHarnessPipeName(pipe.substr(0, pipe.size() - 1)));
	EXPECT_TRUE(BuildHarnessPipeName(L"bad").empty());
}

TEST(HarnessBridgeSecurity, HashDoesNotExposeIdentityText)
{
	const std::array<std::uint8_t, 16> editor{ 1 };
	const auto hash = ComputeHarnessEndpointHash(L"profile-for-test", editor, 3);
	ASSERT_EQ(64u, hash.size());
	EXPECT_EQ(std::wstring::npos, hash.find(L"profile"));
}

} // namespace
} // namespace platform::harnessbridge
