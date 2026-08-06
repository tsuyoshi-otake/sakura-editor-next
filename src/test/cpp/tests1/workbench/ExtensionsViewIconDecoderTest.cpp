/*! @file
	@brief Tests for workbench::extension::DecodeExtensionIconBitmap (Issue #23 gap 1: extension icons)
*/
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/extension/ExtensionIconDecoder.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using workbench::extension::DecodedExtensionIcon;
using workbench::extension::DecodeExtensionIconBitmap;

//! Builds a minimal, valid, uncompressed 24bpp bottom-up BMP -- the one image
//! format WIC always decodes on a stock Windows install with no optional
//! codec pack, so this test has no environment dependency. Every pixel is the
//! same solid color; pixel content is irrelevant to what this decoder proves
//! (dimensions, bounds, and format conversion), only dimensions matter.
[[nodiscard]] std::vector<std::byte> BuildTestBmp(int width, int height)
{
	const int stride = ((width * 3 + 3) / 4) * 4;
	const std::uint32_t pixelBytes = static_cast<std::uint32_t>(stride) * static_cast<std::uint32_t>(height);
	const std::uint32_t pixelOffset = 14 + 40;
	const std::uint32_t fileSize = pixelOffset + pixelBytes;

	std::vector<std::byte> bytes(fileSize, std::byte{ 0 });
	const auto put16 = [&](std::size_t offset, std::uint16_t value) {
		bytes[offset] = static_cast<std::byte>(value & 0xFF);
		bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xFF);
	};
	const auto put32 = [&](std::size_t offset, std::uint32_t value) {
		for (int i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::byte>((value >> (i * 8)) & 0xFF);
	};

	// BITMAPFILEHEADER
	bytes[0] = std::byte{ 'B' };
	bytes[1] = std::byte{ 'M' };
	put32(2, fileSize);
	put32(6, 0); // reserved
	put32(10, pixelOffset);

	// BITMAPINFOHEADER
	put32(14, 40); // biSize
	put32(18, static_cast<std::uint32_t>(width));
	put32(22, static_cast<std::uint32_t>(height)); // positive == bottom-up
	put16(26, 1);  // biPlanes
	put16(28, 24); // biBitCount
	put32(30, 0);  // BI_RGB
	put32(34, pixelBytes);
	put32(38, 0);
	put32(42, 0);
	put32(46, 0);
	put32(50, 0);

	for (std::uint32_t i = pixelOffset; i < fileSize; i += 3) {
		if (i + 2 >= fileSize) break;
		bytes[i] = std::byte{ 0x40 };     // B
		bytes[i + 1] = std::byte{ 0x80 }; // G
		bytes[i + 2] = std::byte{ 0xC0 }; // R
	}
	return bytes;
}

class ScopedIcon final {
public:
	explicit ScopedIcon(DecodedExtensionIcon icon) : m_icon(icon) {}
	~ScopedIcon()
	{
		if (m_icon.bitmap != nullptr) ::DeleteObject(m_icon.bitmap);
	}
	ScopedIcon(const ScopedIcon&) = delete;
	ScopedIcon& operator=(const ScopedIcon&) = delete;
	[[nodiscard]] const DecodedExtensionIcon& Get() const noexcept { return m_icon; }

private:
	DecodedExtensionIcon m_icon;
};

//! WIC is a COM component, so a decode only succeeds on a thread that already
//! has an apartment. Production always does -- the single caller runs on the
//! OLE-initialized UI thread -- but a GoogleTest body does not, and the decoder
//! fails closed rather than throwing, so without this the successful-decode
//! tests would report "invalid icon" and read exactly like a decoder defect.
//! Only the tests that expect a *successful* decode need it; the rejection
//! tests below reject before ever reaching WIC.
class ExtensionsViewIconDecoderComTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		m_initialized = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
		ASSERT_TRUE(m_initialized);
	}
	void TearDown() override
	{
		if (m_initialized) ::CoUninitialize();
	}

private:
	bool m_initialized = false;
};

} // namespace

TEST_F(ExtensionsViewIconDecoderComTest, DecodesSmallBmpAtItsOwnDimensions)
{
	const std::vector<std::byte> bmp = BuildTestBmp(4, 4);
	const ScopedIcon icon(DecodeExtensionIconBitmap(bmp));
	EXPECT_TRUE(icon.Get().IsValid());
	EXPECT_EQ(icon.Get().width, 4);
	EXPECT_EQ(icon.Get().height, 4);
	EXPECT_NE(icon.Get().bitmap, nullptr);
}

TEST_F(ExtensionsViewIconDecoderComTest, DownscalesToTheRequestedEdgeBound)
{
	const std::vector<std::byte> bmp = BuildTestBmp(300, 150);
	const ScopedIcon icon(DecodeExtensionIconBitmap(bmp, 128));
	ASSERT_TRUE(icon.Get().IsValid());
	EXPECT_EQ(icon.Get().width, 128);
	EXPECT_EQ(icon.Get().height, 64); // 150 * 128 / 300, exact for this fixture
}

TEST(ExtensionsViewIconDecoder, RejectsEmptyInput)
{
	const std::vector<std::byte> empty;
	EXPECT_FALSE(DecodeExtensionIconBitmap(empty).IsValid());
}

TEST(ExtensionsViewIconDecoder, RejectsMalformedBytes)
{
	const std::vector<std::byte> garbage(64, std::byte{ 0xAB });
	EXPECT_FALSE(DecodeExtensionIconBitmap(garbage).IsValid());
}

TEST(ExtensionsViewIconDecoder, RejectsInputAboveTheSizeBound)
{
	// One byte over the 8 MiB bound; must be rejected by the size guard alone,
	// never handed to WIC. Deliberately larger than any legitimate marketplace
	// icon, matching the guard's own "hostile/broken response" comment.
	const std::vector<std::byte> oversized(8ULL * 1024ULL * 1024ULL + 1ULL, std::byte{ 0 });
	EXPECT_FALSE(DecodeExtensionIconBitmap(oversized).IsValid());
}

TEST(ExtensionsViewIconDecoder, RejectsNonPositiveMaxEdgePixels)
{
	const std::vector<std::byte> bmp = BuildTestBmp(4, 4);
	EXPECT_FALSE(DecodeExtensionIconBitmap(bmp, 0).IsValid());
	EXPECT_FALSE(DecodeExtensionIconBitmap(bmp, -1).IsValid());
}
