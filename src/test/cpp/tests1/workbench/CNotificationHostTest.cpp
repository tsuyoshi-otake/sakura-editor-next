/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/notification/CNotificationHost.h"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace workbench::notification {

struct CNotificationHostTestPeer final {
	struct LayoutSnapshot final {
		std::uint64_t id{};
		RECT bounds{};
		RECT message{};
		RECT detail{};
		RECT source{};
		RECT close{};
		std::vector<RECT> actions;
	};

	[[nodiscard]] static std::vector<LayoutSnapshot> Rebuild(
		CNotificationHost& host, HDC dc, int width, UINT dpi)
	{
		host.RebuildLayout(dc, width, dpi);
		std::vector<LayoutSnapshot> snapshots;
		snapshots.reserve(host.m_layouts.size());
		for (const auto& layout : host.m_layouts) {
			snapshots.push_back({
				.id = layout.notification.id,
				.bounds = layout.bounds,
				.message = layout.message,
				.detail = layout.detail,
				.source = layout.source,
				.close = layout.close,
				.actions = layout.actions,
			});
		}
		return snapshots;
	}

	static void ResolveAt(CNotificationHost& host, POINT point) noexcept
	{
		host.ResolveAt(point);
	}

	[[nodiscard]] static std::size_t ToastCount(const CNotificationHost& host) noexcept
	{
		return host.m_toasts.size();
	}

	[[nodiscard]] static ULONGLONG Deadline(const CNotificationHost& host, std::size_t index) noexcept
	{
		return index < host.m_toasts.size() ? host.m_toasts[index].deadline : 0;
	}

	static void AdvanceTimer(CNotificationHost& host, ULONGLONG now, bool pause) noexcept
	{
		host.AdvanceTimer(now, pause);
	}
};

} // namespace workbench::notification

namespace {

class ScopedMemoryDc final {
public:
	ScopedMemoryDc() noexcept : m_dc(::CreateCompatibleDC(nullptr)) {}
	~ScopedMemoryDc() noexcept
	{
		if (m_dc != nullptr) ::DeleteDC(m_dc);
	}
	ScopedMemoryDc(const ScopedMemoryDc&) = delete;
	ScopedMemoryDc& operator=(const ScopedMemoryDc&) = delete;

	[[nodiscard]] HDC Get() const noexcept { return m_dc; }

private:
	HDC m_dc{};
};

[[nodiscard]] bool HasArea(const RECT& rect) noexcept
{
	return rect.right > rect.left && rect.bottom > rect.top;
}

[[nodiscard]] bool Contains(const RECT& outer, const RECT& inner) noexcept
{
	return inner.left >= outer.left && inner.top >= outer.top &&
		inner.right <= outer.right && inner.bottom <= outer.bottom;
}

[[nodiscard]] POINT Center(const RECT& rect) noexcept
{
	return { (rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2 };
}

[[nodiscard]] SExtensionNotification Notification(std::uint64_t id)
{
	return {
		.id = id,
		.extensionId = L"odangoo.otak-usage",
		.generation = 1,
		.severity = EExtensionNotificationSeverity::Warning,
		.state = EExtensionNotificationState::Pending,
		.message = L"A notification message that wraps when the toast is narrow.",
		.detail = L"Additional notification detail.",
		.actions = { L"Open Settings", L"Do Not Show Again Today" },
	};
}

TEST(CNotificationHost, StacksEveryInteractiveAndPaintedRectangleInsideItsToast)
{
	ScopedMemoryDc dc;
	ASSERT_NE(nullptr, dc.Get());

	for (const UINT dpi : std::array<UINT, 4>{ 96, 120, 144, 192 }) {
		SCOPED_TRACE(dpi);
		workbench::notification::CNotificationHost host;
		host.SetNotifications({ Notification(1), Notification(2), Notification(3) });
		const int width = ::MulDiv(450, static_cast<int>(dpi), 96);
		const auto layouts = workbench::notification::CNotificationHostTestPeer::Rebuild(
			host, dc.Get(), width, dpi);

		ASSERT_EQ(3U, layouts.size());
		EXPECT_EQ(3U, layouts[0].id);
		EXPECT_EQ(2U, layouts[1].id);
		EXPECT_EQ(1U, layouts[2].id);
		EXPECT_EQ(0, layouts.front().bounds.top);
		const int gap = ::MulDiv(8, static_cast<int>(dpi), 96);
		const int padding = ::MulDiv(12, static_cast<int>(dpi), 96);
		for (std::size_t index = 0; index < layouts.size(); ++index) {
			const auto& layout = layouts[index];
			if (index != 0) {
				EXPECT_EQ(layouts[index - 1].bounds.bottom + gap, layout.bounds.top);
			}
			EXPECT_TRUE(Contains(layout.bounds, layout.message));
			EXPECT_TRUE(Contains(layout.bounds, layout.detail));
			EXPECT_TRUE(Contains(layout.bounds, layout.source));
			EXPECT_TRUE(Contains(layout.bounds, layout.close));
			ASSERT_EQ(2U, layout.actions.size());
			for (const auto& action : layout.actions) {
				EXPECT_TRUE(Contains(layout.bounds, action));
			}
			EXPECT_EQ(layout.actions.back().bottom + padding, layout.bounds.bottom);
		}
	}
}

TEST(CNotificationHost, KeepsAbsentOptionalRowsEmptyAndWrapsNarrowActionsWithinTheCard)
{
	ScopedMemoryDc dc;
	ASSERT_NE(nullptr, dc.Get());
	workbench::notification::CNotificationHost host;
	auto plain = Notification(1);
	plain.extensionId.clear();
	plain.detail.clear();
	plain.actions.clear();
	auto narrow = Notification(2);
	narrow.actions = {
		L"A deliberately long first notification action",
		L"A deliberately long second notification action",
		L"A deliberately long third notification action",
		L"A deliberately long fourth notification action",
		L"This fifth action is intentionally not presented",
	};
	host.SetNotifications({ std::move(plain), std::move(narrow) });
	const auto layouts = workbench::notification::CNotificationHostTestPeer::Rebuild(
		host, dc.Get(), 220, 96);

	ASSERT_EQ(2U, layouts.size());
	const auto& actionLayout = layouts[0];
	ASSERT_EQ(4U, actionLayout.actions.size());
	for (std::size_t index = 0; index < actionLayout.actions.size(); ++index) {
		EXPECT_TRUE(Contains(actionLayout.bounds, actionLayout.actions[index]));
		if (index != 0) EXPECT_GT(actionLayout.actions[index].top, actionLayout.actions[index - 1].top);
	}
	const auto& plainLayout = layouts[1];
	EXPECT_FALSE(HasArea(plainLayout.detail));
	EXPECT_FALSE(HasArea(plainLayout.source));
	EXPECT_TRUE(plainLayout.actions.empty());
	EXPECT_TRUE(Contains(plainLayout.bounds, plainLayout.message));
	EXPECT_TRUE(Contains(plainLayout.bounds, plainLayout.close));
	EXPECT_EQ(plainLayout.message.bottom + 12, plainLayout.bounds.bottom);
}

TEST(CNotificationHost, ResolvesControlsInTheSecondToastFromTheirDisplayedCoordinates)
{
	ScopedMemoryDc dc;
	ASSERT_NE(nullptr, dc.Get());

	{
		workbench::notification::CNotificationHost host;
		std::uint64_t resolvedId{};
		std::optional<std::size_t> selectedAction;
		host.SetResolveCallback([&](std::uint64_t id, std::optional<std::size_t> selected) {
			resolvedId = id;
			selectedAction = selected;
		});
		host.SetNotifications({ Notification(1), Notification(2) });
		const auto layouts = workbench::notification::CNotificationHostTestPeer::Rebuild(
			host, dc.Get(), 450, 96);
		ASSERT_EQ(2U, layouts.size());
		ASSERT_EQ(2U, layouts[1].actions.size());

		workbench::notification::CNotificationHostTestPeer::ResolveAt(
			host, Center(layouts[1].actions[1]));

		EXPECT_EQ(1U, resolvedId);
		ASSERT_TRUE(selectedAction.has_value());
		EXPECT_EQ(1U, *selectedAction);
		EXPECT_EQ(1U, workbench::notification::CNotificationHostTestPeer::ToastCount(host));
	}

	{
		workbench::notification::CNotificationHost host;
		std::uint64_t resolvedId{};
		std::optional<std::size_t> selectedAction = 0;
		host.SetResolveCallback([&](std::uint64_t id, std::optional<std::size_t> selected) {
			resolvedId = id;
			selectedAction = selected;
		});
		host.SetNotifications({ Notification(1), Notification(2) });
		const auto layouts = workbench::notification::CNotificationHostTestPeer::Rebuild(
			host, dc.Get(), 450, 96);
		ASSERT_EQ(2U, layouts.size());

		workbench::notification::CNotificationHostTestPeer::ResolveAt(
			host, Center(layouts[1].close));

		EXPECT_EQ(1U, resolvedId);
		EXPECT_FALSE(selectedAction.has_value());
		EXPECT_EQ(1U, workbench::notification::CNotificationHostTestPeer::ToastCount(host));
	}
}

TEST(CNotificationHost, KeepsActionPromptsVisibleUntilTheUserMakesAnExplicitChoice)
{
	workbench::notification::CNotificationHost host;
	auto prompt = Notification(1);
	prompt.severity = EExtensionNotificationSeverity::Warning;
	host.SetNotifications({ std::move(prompt) });

	EXPECT_EQ(1U, workbench::notification::CNotificationHostTestPeer::ToastCount(host));
	EXPECT_EQ(0U, workbench::notification::CNotificationHostTestPeer::Deadline(host, 0));
	workbench::notification::CNotificationHostTestPeer::AdvanceTimer(host, 100, true);
	EXPECT_EQ(0U, workbench::notification::CNotificationHostTestPeer::Deadline(host, 0));
	workbench::notification::CNotificationHostTestPeer::AdvanceTimer(host, UINT64_MAX, false);
	EXPECT_EQ(1U, workbench::notification::CNotificationHostTestPeer::ToastCount(host));
	EXPECT_EQ(1U, host.Status().pendingCount);
}

TEST(CNotificationHost, TimeoutRetractsOnlyTheToastAndNeverResolvesTheNotification)
{
	workbench::notification::CNotificationHost host;
	auto informational = Notification(1);
	informational.severity = EExtensionNotificationSeverity::Information;
	informational.actions.clear();
	std::size_t resolveCalls{};
	host.SetResolveCallback([&](std::uint64_t, std::optional<std::size_t>) { ++resolveCalls; });
	host.SetNotifications({ std::move(informational) });
	const auto deadline = workbench::notification::CNotificationHostTestPeer::Deadline(host, 0);
	ASSERT_NE(0U, deadline);

	workbench::notification::CNotificationHostTestPeer::AdvanceTimer(host, deadline, false);

	EXPECT_EQ(0U, resolveCalls);
	EXPECT_EQ(0U, workbench::notification::CNotificationHostTestPeer::ToastCount(host));
	EXPECT_EQ(1U, host.Status().pendingCount);
}

} // namespace
