/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include "workbench/rendering/FrameNativeSurfacePayloadAdapter.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace workbench::rendering {
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
	std::vector<FrameNativeSurfaceRegistration> registrations;
	std::vector<std::shared_ptr<const FrameNativeSurfaceFrame>> frames;

	[[nodiscard]] FrameNativeSurfacePayloadSink Bind()
	{
		return {
			.registerSurface = [this](const FrameNativeSurfaceRegistration& registration) {
				++registerCount;
				registrations.push_back(registration);
				return true;
			},
			.updateSurface = [this](const FrameNativeSurfaceRegistration& registration) {
				++updateCount;
				registrations.push_back(registration);
				return true;
			},
			.closeSurface = [this](FrameSurfaceId, std::uint64_t) { ++closeCount; },
			.submitFrame = [this](std::shared_ptr<const FrameNativeSurfaceFrame> frame) {
				frames.push_back(std::move(frame));
				return true;
			},
		};
	}
};

FrameNativeSurfacePayloadTarget Target(const std::uint64_t lifetime)
{
	return {
		.surfaceId = 0x102,
		.surfaceLifetimeEpoch = lifetime,
		.deviceEpoch = 4,
		.displayEpoch = 7,
		.layoutEpoch = 9,
		.width = 8,
		.height = 6,
		.visible = true,
	};
}

TEST(FrameNativeSurfacePayloadAdapter, CapturesOnlyDirtyPixelsAsImmutableBgra)
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
	FrameNativeSurfacePayloadAdapter adapter;
	adapter.SetSink(sink.Bind());
	ASSERT_EQ(EFrameNativeSurfacePayloadStatus::Registered,
		adapter.Register(Target(1)).status);
	EXPECT_EQ(EFrameNativeSurfacePayloadStatus::Submitted,
		adapter.Submit(source.Dc(), RECT{ 2, 1, 5, 3 }).status);

	ASSERT_EQ(1u, sink.frames.size());
	const auto first = sink.frames.back();
	ASSERT_TRUE(first->IsValid());
	EXPECT_TRUE(first->compactDirtyPayload);
	EXPECT_EQ(8u, first->width);
	EXPECT_EQ(6u, first->height);
	EXPECT_EQ(2, first->dirtyRect.left);
	EXPECT_EQ(1, first->dirtyRect.top);
	EXPECT_EQ(5, first->dirtyRect.right);
	EXPECT_EQ(3, first->dirtyRect.bottom);
	EXPECT_EQ(3u * 2u * 4u, first->pixels->size());
	EXPECT_EQ(1u, first->surfaceLifetimeEpoch);
	EXPECT_EQ(4u, first->deviceEpoch);
	EXPECT_EQ(7u, first->displayEpoch);
	EXPECT_EQ(9u, first->layoutEpoch);
	EXPECT_EQ(1u, first->requestId);
	for (std::uint32_t y = 0; y < 2; ++y) {
		for (std::uint32_t x = 0; x < 3; ++x) {
			const auto* pixel = first->pixels->data() + (y * 3u + x) * 4u;
			EXPECT_EQ(static_cast<std::uint8_t>(x + 2 + 10), pixel[0]);
			EXPECT_EQ(static_cast<std::uint8_t>(y + 1 + 20), pixel[1]);
			EXPECT_EQ(static_cast<std::uint8_t>(x + 2 + y + 1 + 30), pixel[2]);
			EXPECT_EQ(0xff, pixel[3]);
		}
	}

	EXPECT_EQ(EFrameNativeSurfacePayloadStatus::NoDirtyPixels,
		adapter.Submit(source.Dc(), RECT{}).status);
	EXPECT_EQ(EFrameNativeSurfacePayloadStatus::Submitted,
		adapter.Submit(source.Dc(), RECT{ 0, 0, 1, 1 }).status);
	ASSERT_EQ(2u, sink.frames.size());
	EXPECT_EQ(4u, sink.frames.back()->pixels->size());
	EXPECT_EQ(2u, sink.frames.back()->requestId);
	EXPECT_EQ(1u, first->requestId);
	EXPECT_EQ(24u, first->pixels->size());
}

TEST(FrameNativeSurfacePayloadAdapter, CaptureAndPublishAreSeparateLatestWinsOperations)
{
	TestDib source(8, 6);
	ASSERT_TRUE(source.IsValid());
	for (std::uint32_t y = 0; y < 6; ++y) {
		for (std::uint32_t x = 0; x < 8; ++x) {
			auto* pixel = source.Pixels() + (y * source.Width() + x) * 4u;
			pixel[0] = static_cast<std::uint8_t>(x + 1);
			pixel[1] = static_cast<std::uint8_t>(y + 2);
			pixel[2] = 3;
			pixel[3] = 0;
		}
	}

	FakeSink sink;
	FrameNativeSurfacePayloadAdapter adapter;
	adapter.SetSink(sink.Bind());
	ASSERT_EQ(EFrameNativeSurfacePayloadStatus::Registered,
		adapter.Register(Target(11)).status);

	EXPECT_EQ(EFrameNativeSurfacePayloadStatus::Captured,
		adapter.CapturePending(source.Dc(), RECT{ 0, 0, 1, 1 }).status);
	EXPECT_TRUE(adapter.HasPending());
	EXPECT_TRUE(sink.frames.empty());
	EXPECT_EQ(EFrameNativeSurfacePayloadStatus::Captured,
		adapter.CapturePending(source.Dc(), RECT{ 4, 2, 6, 4 }).status);
	EXPECT_EQ(EFrameNativeSurfacePayloadStatus::Submitted,
		adapter.PublishPending().status);
	ASSERT_EQ(1u, sink.frames.size());
	EXPECT_EQ(4, sink.frames.back()->dirtyRect.left);
	EXPECT_EQ(2, sink.frames.back()->dirtyRect.top);
	EXPECT_EQ(6, sink.frames.back()->dirtyRect.right);
	EXPECT_EQ(4, sink.frames.back()->dirtyRect.bottom);
	EXPECT_EQ(2u, sink.frames.back()->requestId);
	EXPECT_FALSE(adapter.HasPending());
	EXPECT_EQ(EFrameNativeSurfacePayloadStatus::NoPending,
		adapter.PublishPending().status);

	EXPECT_EQ(EFrameNativeSurfacePayloadStatus::Captured,
		adapter.CapturePending(source.Dc(), RECT{ 0, 0, 1, 1 }).status);
	adapter.DiscardPending();
	EXPECT_FALSE(adapter.HasPending());
	EXPECT_EQ(EFrameNativeSurfacePayloadStatus::NoPending,
		adapter.PublishPending().status);
}

TEST(FrameNativeSurfacePayloadAdapter, FencesEpochsVisibilityAndLifetime)
{
	TestDib source(8, 6);
	ASSERT_TRUE(source.IsValid());
	FakeSink sink;
	FrameNativeSurfacePayloadAdapter adapter;
	adapter.SetSink(sink.Bind());
	ASSERT_EQ(EFrameNativeSurfacePayloadStatus::Registered,
		adapter.Register(Target(7)).status);

	auto hidden = Target(7);
	hidden.visible = false;
	EXPECT_EQ(EFrameNativeSurfacePayloadStatus::Hidden, adapter.Update(hidden).status);
	EXPECT_EQ(EFrameNativeSurfacePayloadStatus::Hidden,
		adapter.Submit(source.Dc(), RECT{ 0, 0, 1, 1 }).status);

	auto minimized = hidden;
	minimized.minimized = true;
	EXPECT_EQ(EFrameNativeSurfacePayloadStatus::Minimized, adapter.Update(minimized).status);
	EXPECT_EQ(EFrameNativeSurfacePayloadStatus::Minimized,
		adapter.Submit(source.Dc(), RECT{ 0, 0, 1, 1 }).status);

	auto advanced = Target(7);
	advanced.deviceEpoch = 5;
	advanced.displayEpoch = 8;
	advanced.layoutEpoch = 10;
	EXPECT_EQ(EFrameNativeSurfacePayloadStatus::Updated, adapter.Update(advanced).status);
	// Withdrawing the hidden target and restoring it at the advanced epochs are
	// distinct physical registration updates.
	EXPECT_EQ(2, sink.updateCount);
	EXPECT_EQ(EFrameNativeSurfacePayloadStatus::Submitted,
		adapter.Submit(source.Dc(), RECT{ 1, 1, 3, 2 }).status);
	ASSERT_EQ(1u, sink.frames.size());
	EXPECT_EQ(5u, sink.frames.back()->deviceEpoch);
	EXPECT_EQ(8u, sink.frames.back()->displayEpoch);
	EXPECT_EQ(10u, sink.frames.back()->layoutEpoch);

	auto stale = advanced;
	--stale.displayEpoch;
	EXPECT_EQ(EFrameNativeSurfacePayloadStatus::Stale, adapter.Update(stale).status);

	EXPECT_EQ(EFrameNativeSurfacePayloadStatus::ZeroSized,
		adapter.Update(FrameNativeSurfacePayloadTarget{
			.surfaceId = 0x102, .surfaceLifetimeEpoch = 7,
			.deviceEpoch = 5, .displayEpoch = 8, .layoutEpoch = 10,
			.width = 0, .height = 0, .visible = true }).status);
	EXPECT_FALSE(adapter.IsRegistered());
	EXPECT_EQ(1, sink.closeCount);

	EXPECT_EQ(EFrameNativeSurfacePayloadStatus::Stale,
		adapter.Register(Target(7)).status);
	EXPECT_EQ(EFrameNativeSurfacePayloadStatus::Registered,
		adapter.Register(Target(8)).status);
	EXPECT_EQ(EFrameNativeSurfacePayloadStatus::Closed, adapter.Close().status);
	EXPECT_EQ(EFrameNativeSurfacePayloadStatus::AlreadyClosed, adapter.Close().status);
}

} // namespace
} // namespace workbench::rendering
