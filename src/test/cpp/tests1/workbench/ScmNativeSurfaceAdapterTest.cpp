/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/scm/ScmNativeSurfaceAdapter.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace workbench::scm {
namespace {

class TestDib final {
public:
	TestDib(const std::uint32_t width, const std::uint32_t height)
		: m_width(width), m_height(height)
	{
		m_dc = ::CreateCompatibleDC(nullptr);
		if (m_dc == nullptr) return;
		BITMAPINFO info{};
		info.bmiHeader.biSize = sizeof(info.bmiHeader);
		info.bmiHeader.biWidth = static_cast<LONG>(width);
		info.bmiHeader.biHeight = -static_cast<LONG>(height);
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		m_bitmap = ::CreateDIBSection(m_dc, &info, DIB_RGB_COLORS,
			&m_bits, nullptr, 0);
		if (m_bitmap == nullptr) return;
		m_oldBitmap = ::SelectObject(m_dc, m_bitmap);
	}

	~TestDib() noexcept
	{
		if (m_dc != nullptr && m_oldBitmap != nullptr) {
			(void)::SelectObject(m_dc, m_oldBitmap);
		}
		if (m_bitmap != nullptr) ::DeleteObject(m_bitmap);
		if (m_dc != nullptr) ::DeleteDC(m_dc);
	}

	TestDib(const TestDib&) = delete;
	TestDib& operator=(const TestDib&) = delete;

	[[nodiscard]] HDC Dc() const noexcept { return m_dc; }
	[[nodiscard]] bool IsValid() const noexcept
	{
		return m_dc != nullptr && m_bitmap != nullptr && m_oldBitmap != nullptr
			&& m_oldBitmap != HGDI_ERROR && m_bits != nullptr;
	}
	[[nodiscard]] std::uint8_t* Pixels() const noexcept
	{
		return static_cast<std::uint8_t*>(m_bits);
	}
	[[nodiscard]] std::uint32_t Width() const noexcept { return m_width; }

private:
	std::uint32_t m_width = 0;
	std::uint32_t m_height = 0;
	HDC m_dc = nullptr;
	HBITMAP m_bitmap = nullptr;
	HGDIOBJ m_oldBitmap = nullptr;
	void* m_bits = nullptr;
};

struct FakeSink final {
	int registerCount = 0;
	int updateCount = 0;
	int closeCount = 0;
	std::vector<ScmNativeSurfaceRegistration> registrations;
	std::vector<std::shared_ptr<const ScmNativeSurfaceFrame>> frames;

	[[nodiscard]] ScmNativeSurfaceSink Bind()
	{
		return {
			.registerSurface = [this](const ScmNativeSurfaceRegistration& registration) {
				++registerCount;
				registrations.push_back(registration);
				return true;
			},
			.updateSurface = [this](const ScmNativeSurfaceRegistration& registration) {
				++updateCount;
				registrations.push_back(registration);
				return true;
			},
			.closeSurface = [this](rendering::FrameSurfaceId, std::uint64_t) {
				++closeCount;
			},
			.submitFrame = [this](std::shared_ptr<const ScmNativeSurfaceFrame> frame) {
				frames.push_back(std::move(frame));
				return true;
			},
		};
	}
};

ScmNativeSurfaceTarget Target(std::uint64_t lifetime)
{
	return {
		.surfaceId = 0x102,
		.surfaceLifetimeEpoch = lifetime,
		.deviceEpoch = 4,
		.layoutEpoch = 9,
		.visible = true,
	};
}

TEST(ScmNativeSurfaceAdapter, CapturesOnlyDirtyPixelsAsImmutableBgra)
{
	TestDib source(8, 6);
	ASSERT_TRUE(source.IsValid());
	for (std::uint32_t y = 0; y < 6; ++y) {
		for (std::uint32_t x = 0; x < 8; ++x) {
			auto* pixel = source.Pixels() + (y * source.Width() + x) * 4u;
			pixel[0] = static_cast<std::uint8_t>(x + 10);
			pixel[1] = static_cast<std::uint8_t>(y + 20);
			pixel[2] = static_cast<std::uint8_t>(x + y + 30);
			pixel[3] = 0;
		}
	}

	FakeSink sink;
	ScmNativeSurfacePayloadAdapter adapter;
	adapter.SetSink(sink.Bind());
	ASSERT_TRUE(adapter.SetTarget(Target(1)));
	adapter.CaptureAndSubmit(nullptr, source.Dc(), RECT{ 2, 1, 5, 3 }, 8, 6);

	ASSERT_EQ(1u, sink.frames.size());
	const auto& frame = *sink.frames.back();
	EXPECT_TRUE(frame.IsValid());
	EXPECT_EQ(8u, frame.width);
	EXPECT_EQ(6u, frame.height);
	EXPECT_EQ(2, frame.dirtyRect.left);
	EXPECT_EQ(1, frame.dirtyRect.top);
	EXPECT_EQ(5, frame.dirtyRect.right);
	EXPECT_EQ(3, frame.dirtyRect.bottom);
	EXPECT_EQ(3u, frame.payloadWidth);
	EXPECT_EQ(2u, frame.payloadHeight);
	EXPECT_EQ(3u * 2u * 4u, frame.pixels->size());
	EXPECT_EQ(1u, frame.surfaceLifetimeEpoch);
	EXPECT_EQ(4u, frame.deviceEpoch);
	EXPECT_EQ(9u, frame.layoutEpoch);
	EXPECT_EQ(1u, frame.requestId);
	for (std::uint32_t y = 0; y < frame.payloadHeight; ++y) {
		for (std::uint32_t x = 0; x < frame.payloadWidth; ++x) {
			const auto* pixel = frame.pixels->data() + (y * frame.payloadWidth + x) * 4u;
			EXPECT_EQ(static_cast<std::uint8_t>(x + 2 + 10), pixel[0]);
			EXPECT_EQ(static_cast<std::uint8_t>(y + 1 + 20), pixel[1]);
			EXPECT_EQ(static_cast<std::uint8_t>(x + 2 + y + 1 + 30), pixel[2]);
			EXPECT_EQ(0xff, pixel[3]);
		}
	}

	// A second dirty region replaces no pixels outside its own compact payload.
	adapter.CaptureAndSubmit(nullptr, source.Dc(), RECT{ 0, 0, 1, 1 }, 8, 6);
	ASSERT_EQ(2u, sink.frames.size());
	EXPECT_EQ(1u, sink.frames.back()->payloadWidth);
	EXPECT_EQ(1u, sink.frames.back()->payloadHeight);
	EXPECT_EQ(4u, sink.frames.back()->pixels->size());
	EXPECT_EQ(2u, sink.frames.back()->requestId);

	TestDib resized(9, 6);
	ASSERT_TRUE(resized.IsValid());
	adapter.CaptureAndSubmit(nullptr, resized.Dc(), RECT{ 1, 1, 3, 2 }, 9, 6);
	EXPECT_EQ(1, sink.updateCount);
	EXPECT_GT(sink.registrations.back().layoutEpoch, 9u);
}

TEST(ScmNativeSurfaceAdapter, ClosesAndRejectsRecycledLifetime)
{
	TestDib source(2, 2);
	ASSERT_TRUE(source.IsValid());
	FakeSink sink;
	ScmNativeSurfacePayloadAdapter adapter;
	adapter.SetSink(sink.Bind());
	ASSERT_TRUE(adapter.SetTarget(Target(7)));
	adapter.CaptureAndSubmit(nullptr, source.Dc(), RECT{ 0, 0, 2, 2 }, 2, 2);
	ASSERT_EQ(1u, sink.frames.size());

	ASSERT_TRUE(adapter.SetTarget(Target(8)));
	EXPECT_EQ(1, sink.closeCount);
	adapter.CaptureAndSubmit(nullptr, source.Dc(), RECT{ 0, 0, 1, 1 }, 2, 2);
	ASSERT_EQ(2u, sink.frames.size());
	EXPECT_EQ(8u, sink.frames.back()->surfaceLifetimeEpoch);
	EXPECT_EQ(2, sink.registerCount);

	EXPECT_FALSE(adapter.SetTarget(Target(7)));
	adapter.CaptureAndSubmit(nullptr, source.Dc(), RECT{ 0, 0, 1, 1 }, 2, 2);
	ASSERT_EQ(3u, sink.frames.size());
	EXPECT_EQ(8u, sink.frames.back()->surfaceLifetimeEpoch);

	adapter.ClearTarget();
	EXPECT_EQ(2, sink.closeCount);
	adapter.CaptureAndSubmit(nullptr, source.Dc(), RECT{ 0, 0, 1, 1 }, 2, 2);
	EXPECT_EQ(3u, sink.frames.size());
}

} // namespace
} // namespace workbench::scm
