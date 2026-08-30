/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/panel/CBottomPanelTool.h"
#include "workbench/viewcontainer/CViewContainerPages.h"
#include "workbench/viewcontainer/ViewContainerPagePool.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace workbench::viewcontainer {

struct ViewContainerPagePoolTestPeer final {
	static void FailNextStateStaging(ViewContainerPagePool& pool) noexcept
	{
		pool.FailNextStateStagingForTest();
	}
};

namespace {

constexpr std::string_view kContainerId = "sample.container";
constexpr ViewContainerFocusToken kFocusToken{ 41 };

enum class FakeStep : std::uint8_t {
	CaptureFocus,
	Detach,
	Reparent,
	Attach,
	RestoreFocus,
	Close,
};

struct FakePageState final {
	std::vector<FakeStep> calls;
	std::vector<FakeStep> failures;
	std::optional<ViewContainerPageHost> attachedHost;
	ViewContainerNativeHandle parent{};
	ViewContainerNativeHandle nestedParent{};
	bool focused{};
	bool closed{};
	int factoryCalls{};
	int livePages{};
	int destructCalls{};
	int closeCalls{};
	std::optional<RECT> projectionHostBounds;
	std::optional<RECT> projectionContentBounds;
	unsigned int projectionDpi{};
	int projectionLayoutCalls{};

	[[nodiscard]] bool ShouldFail(const FakeStep step)
	{
		calls.push_back(step);
		if (failures.empty() || failures.front() != step) return false;
		failures.erase(failures.begin());
		return true;
	}
};

class FakePage final : public IViewContainerPage, public IViewContainerPageProjection {
public:
	FakePage(std::shared_ptr<FakePageState> state, std::string containerId)
		: m_state(std::move(state)), m_containerId(std::move(containerId))
	{
		++m_state->livePages;
	}

	~FakePage() override
	{
		m_state->attachedHost.reset();
		m_state->parent = 0;
		m_state->nestedParent = 0;
		m_state->focused = false;
		--m_state->livePages;
		++m_state->destructCalls;
	}

	[[nodiscard]] std::string_view ContainerId() const noexcept override { return m_containerId; }

	[[nodiscard]] ViewContainerFocusCaptureResult CaptureFocusToken() noexcept override
	{
		if (m_state->ShouldFail(FakeStep::CaptureFocus)) {
			return { EViewContainerFocusCaptureStatus::Failed, std::nullopt };
		}
		if (!m_state->focused) return { EViewContainerFocusCaptureStatus::NoFocus, std::nullopt };
		return { EViewContainerFocusCaptureStatus::Captured, kFocusToken };
	}

	[[nodiscard]] EViewContainerPageDetachStatus Detach(
		const ViewContainerPageHost& host) noexcept override
	{
		if (m_state->ShouldFail(FakeStep::Detach) || m_state->attachedHost != host) {
			return EViewContainerPageDetachStatus::Failed;
		}
		m_state->attachedHost.reset();
		m_state->focused = false;
		return EViewContainerPageDetachStatus::Detached;
	}

	[[nodiscard]] EViewContainerPageReparentStatus Reparent(
		const ViewContainerNativeHandle nativeParent) noexcept override
	{
		if (m_state->ShouldFail(FakeStep::Reparent)) {
			return EViewContainerPageReparentStatus::Failed;
		}
		m_state->parent = nativeParent;
		m_state->nestedParent = nativeParent;
		return EViewContainerPageReparentStatus::Reparented;
	}

	[[nodiscard]] EViewContainerPageAttachStatus Attach(
		const ViewContainerPageHost& host) noexcept override
	{
		if (m_state->ShouldFail(FakeStep::Attach) || m_state->attachedHost
			|| m_state->parent != host.nativeParent) {
			return EViewContainerPageAttachStatus::Failed;
		}
		m_state->attachedHost = host;
		m_state->focused = false;
		return EViewContainerPageAttachStatus::Attached;
	}

	[[nodiscard]] EViewContainerFocusRestoreStatus RestoreFocusToken(
		const ViewContainerFocusToken token) noexcept override
	{
		if (m_state->ShouldFail(FakeStep::RestoreFocus) || !m_state->attachedHost
			|| token != kFocusToken) {
			return EViewContainerFocusRestoreStatus::Failed;
		}
		m_state->focused = true;
		return EViewContainerFocusRestoreStatus::Restored;
	}

	[[nodiscard]] EViewContainerPageCloseStatus Close() noexcept override
	{
		++m_state->closeCalls;
		if (m_state->ShouldFail(FakeStep::Close)) return EViewContainerPageCloseStatus::Failed;
		m_state->closed = true;
		m_state->attachedHost.reset();
		m_state->parent = 0;
		m_state->nestedParent = 0;
		m_state->focused = false;
		return EViewContainerPageCloseStatus::Closed;
	}

	void ActivateProjection() noexcept override {}
	void DeactivateProjection() noexcept override {}
	[[nodiscard]] bool PreTranslateProjection(MSG&) noexcept override { return false; }
	void LayoutProjection(const RECT& hostBounds, const RECT& contentBounds,
		const unsigned int dpi) noexcept override
	{
		m_state->projectionHostBounds = hostBounds;
		m_state->projectionContentBounds = contentBounds;
		m_state->projectionDpi = dpi;
		++m_state->projectionLayoutCalls;
	}
	void SetProjectionVisible(bool) noexcept override {}

private:
	std::shared_ptr<FakePageState> m_state;
	std::string m_containerId;
};

class FakePageWithoutProjection final : public IViewContainerPage {
public:
	FakePageWithoutProjection(std::shared_ptr<FakePageState> state, std::string containerId)
		: m_state(std::move(state)), m_containerId(std::move(containerId))
	{
		++m_state->livePages;
	}

	~FakePageWithoutProjection() override
	{
		--m_state->livePages;
		++m_state->destructCalls;
	}

	[[nodiscard]] std::string_view ContainerId() const noexcept override { return m_containerId; }
	[[nodiscard]] ViewContainerFocusCaptureResult CaptureFocusToken() noexcept override
	{
		return { EViewContainerFocusCaptureStatus::NoFocus, std::nullopt };
	}
	[[nodiscard]] EViewContainerPageDetachStatus Detach(
		const ViewContainerPageHost&) noexcept override
	{
		return EViewContainerPageDetachStatus::Detached;
	}
	[[nodiscard]] EViewContainerPageReparentStatus Reparent(
		ViewContainerNativeHandle) noexcept override
	{
		return EViewContainerPageReparentStatus::Reparented;
	}
	[[nodiscard]] EViewContainerPageAttachStatus Attach(
		const ViewContainerPageHost&) noexcept override
	{
		return EViewContainerPageAttachStatus::Attached;
	}
	[[nodiscard]] EViewContainerFocusRestoreStatus RestoreFocusToken(
		ViewContainerFocusToken) noexcept override
	{
		return EViewContainerFocusRestoreStatus::Restored;
	}
	[[nodiscard]] EViewContainerPageCloseStatus Close() noexcept override
	{
		++m_state->closeCalls;
		m_state->closed = true;
		return EViewContainerPageCloseStatus::Closed;
	}

private:
	std::shared_ptr<FakePageState> m_state;
	std::string m_containerId;
};

ViewContainerPageDescriptor Descriptor(const std::shared_ptr<FakePageState>& state,
	std::string containerId = std::string(kContainerId),
	layout::SupportedViewContainerLocations supportedLocations = {
		layout::EViewContainerLocation::Sidebar,
		layout::EViewContainerLocation::AuxiliaryBar })
{
	const auto factoryId = containerId;
	return { .containerId = std::move(containerId), .supportedLocations = supportedLocations,
		.factory = [state, factoryId]() -> std::unique_ptr<IViewContainerPage> {
			++state->factoryCalls;
			return std::make_unique<FakePage>(state, factoryId);
		} };
}

ViewContainerPageRegistry RegistryWith(const std::shared_ptr<FakePageState>& state)
{
	ViewContainerPageRegistry registry;
	const auto result = registry.RegisterBatch({ Descriptor(state) });
	EXPECT_EQ(EViewContainerPageRegistrationStatus::Registered, result.status);
	return registry;
}

ViewContainerPageHost SideBarHost()
{
	return { "workbench.parts.sidebar", layout::EViewContainerLocation::Sidebar, 101 };
}

ViewContainerPageHost AuxiliaryHost()
{
	return { "workbench.parts.auxiliarybar", layout::EViewContainerLocation::AuxiliaryBar, 202 };
}

ViewContainerPageHost PanelHost()
{
	return { "workbench.parts.panel", layout::EViewContainerLocation::Panel, 303 };
}

class FakePaneCompositePageHostService final : public IViewContainerPageHostService {
public:
	explicit FakePaneCompositePageHostService(std::shared_ptr<FakePageState> state)
		: m_state(std::move(state)), m_pool(m_registry)
	{
		const auto registered = m_registry.RegisterBatch({ Descriptor(m_state,
			std::string(kContainerId), {
				layout::EViewContainerLocation::Sidebar,
				layout::EViewContainerLocation::Panel,
				layout::EViewContainerLocation::AuxiliaryBar,
			}) });
		m_usable = registered.Succeeded() && m_pool.Acquire(kContainerId).Succeeded();
	}

	[[nodiscard]] bool IsUsable() const noexcept override { return m_usable; }
	[[nodiscard]] ViewContainerPagePoolAttachResult Attach(const std::string_view containerId,
		const ViewContainerPageHost& host) noexcept override
	{
		return m_pool.Attach(containerId, host);
	}
	[[nodiscard]] ViewContainerPagePoolDetachResult Detach(
		const std::string_view containerId) noexcept override
	{
		return m_pool.Detach(containerId);
	}
	[[nodiscard]] HWND AttachedHost(const std::string_view containerId) const noexcept override
	{
		const auto state = m_pool.State(containerId);
		return state.state && state.state->host
			? reinterpret_cast<HWND>(state.state->host->nativeParent) : nullptr;
	}
	[[nodiscard]] bool SupportsLocation(const std::string_view containerId,
		const layout::EViewContainerLocation location) const noexcept override
	{
		const auto* descriptor = m_registry.Find(containerId);
		return descriptor != nullptr && descriptor->supportedLocations.Contains(location);
	}
	[[nodiscard]] std::vector<std::string> PageIds() const override
	{
		return { std::string(kContainerId) };
	}
	void ActivatePage(std::string_view) noexcept override {}
	void DeactivatePage(std::string_view) noexcept override {}
	[[nodiscard]] bool PreTranslatePage(std::string_view, MSG&) noexcept override { return false; }
	void LayoutPageProjection(std::string_view, const RECT& hostBounds,
		const RECT& contentBounds, unsigned int) noexcept override
	{
		wrapperBounds = hostBounds;
		pageContentBounds = contentBounds;
	}
	void SetPageVisible(std::string_view, bool visible) override { pageVisible = visible; }
	void NotifyPageLayout(std::string_view) override { ++layoutNotifications; }

	ViewContainerPageRegistry m_registry;
	std::shared_ptr<FakePageState> m_state;
	ViewContainerPagePool m_pool;
	RECT wrapperBounds{};
	RECT pageContentBounds{};
	bool pageVisible{};
	int layoutNotifications{};
	bool m_usable{};
};

TEST(ViewContainerPagePool, RepeatedAcquireCreatesOneLogicalPage)
{
	auto state = std::make_shared<FakePageState>();
	auto registry = RegistryWith(state);
	{
		ViewContainerPagePool pool(registry);
		const auto first = pool.Acquire(kContainerId);
		const auto second = pool.Acquire(kContainerId);

		EXPECT_EQ(EViewContainerPageAcquireStatus::Created, first.status);
		EXPECT_EQ(EViewContainerPageAcquireStatus::Existing, second.status);
		EXPECT_EQ(first.page, second.page);
		EXPECT_EQ(1, state->factoryCalls);
		EXPECT_EQ(1U, pool.Size());
		EXPECT_EQ(1, state->livePages);
	}
	EXPECT_EQ(1, state->closeCalls);
	EXPECT_EQ(1, state->destructCalls);
	EXPECT_EQ(0, state->livePages);
}

TEST(ViewContainerPagePool, KeepsOneLogicalPageAndAllOwnedWindowsAcrossBothSideBars)
{
	auto state = std::make_shared<FakePageState>();
	auto registry = RegistryWith(state);
	ViewContainerPagePool pool(registry);
	const auto sideBar = SideBarHost();
	const auto auxiliary = AuxiliaryHost();
	const auto logicalPage = pool.Acquire(kContainerId).page;
	ASSERT_NE(nullptr, logicalPage);

	EXPECT_EQ(EViewContainerPagePoolAttachStatus::Attached,
		pool.Attach(kContainerId, sideBar).status);
	EXPECT_EQ(logicalPage, pool.Acquire(kContainerId).page);
	EXPECT_EQ(sideBar.nativeParent, state->nestedParent);
	const auto attachCalls = state->calls.size();
	EXPECT_EQ(EViewContainerPagePoolAttachStatus::AlreadyAttached,
		pool.Attach(kContainerId, sideBar).status);
	EXPECT_EQ(attachCalls, state->calls.size());

	state->focused = true;
	EXPECT_EQ(EViewContainerPagePoolAttachStatus::Attached,
		pool.Attach(kContainerId, auxiliary).status);
	EXPECT_EQ(std::optional(auxiliary), state->attachedHost);
	EXPECT_EQ(auxiliary.nativeParent, state->parent);
	EXPECT_EQ(auxiliary.nativeParent, state->nestedParent);
	EXPECT_TRUE(state->focused);

	EXPECT_EQ(EViewContainerPagePoolDetachStatus::Detached, pool.Detach(kContainerId).status);
	EXPECT_FALSE(state->attachedHost);
	EXPECT_EQ(0U, state->parent);
	EXPECT_EQ(0U, state->nestedParent);
	EXPECT_FALSE(state->focused);
	EXPECT_EQ(EViewContainerPagePoolDetachStatus::AlreadyDetached,
		pool.Detach(kContainerId).status);

	EXPECT_EQ(EViewContainerPagePoolAttachStatus::Attached,
		pool.Attach(kContainerId, sideBar).status);
	EXPECT_EQ(std::optional(sideBar), state->attachedHost);
	EXPECT_EQ(sideBar.nativeParent, state->parent);
	EXPECT_EQ(sideBar.nativeParent, state->nestedParent);
	EXPECT_TRUE(state->focused);
	EXPECT_EQ(logicalPage, pool.Acquire(kContainerId).page);
	EXPECT_EQ(1, state->factoryCalls);
}

TEST(ViewContainerPagePool, MovesOneRetainedPageAcrossAllThreePaneCompositeHosts)
{
	auto state = std::make_shared<FakePageState>();
	ViewContainerPageRegistry registry;
	ASSERT_EQ(EViewContainerPageRegistrationStatus::Registered,
		registry.RegisterBatch({ Descriptor(state, std::string(kContainerId), {
			layout::EViewContainerLocation::Sidebar,
			layout::EViewContainerLocation::Panel,
			layout::EViewContainerLocation::AuxiliaryBar,
		}) }).status);
	ViewContainerPagePool pool(registry);
	const auto* retainedPage = pool.Acquire(kContainerId).page;
	ASSERT_NE(nullptr, retainedPage);

	const std::array hosts{ SideBarHost(), PanelHost(), AuxiliaryHost() };
	for (std::size_t index = 0; index < hosts.size(); ++index) {
		if (index != 0) state->focused = true;
		const auto attached = pool.Attach(kContainerId, hosts[index]);
		ASSERT_EQ(EViewContainerPagePoolAttachStatus::Attached, attached.status);
		EXPECT_EQ(retainedPage, pool.Acquire(kContainerId).page);
		ASSERT_TRUE(state->attachedHost.has_value());
		EXPECT_EQ(hosts[index], *state->attachedHost);
		EXPECT_EQ(hosts[index].nativeParent, state->parent);
		EXPECT_EQ(hosts[index].nativeParent, state->nestedParent);
		if (index != 0) EXPECT_TRUE(state->focused);
	}
	EXPECT_EQ(1, state->factoryCalls);
	EXPECT_EQ(1, state->livePages);
}

TEST(ViewContainerPagePool, PanelHostFocusRestoreFailureReturnsToThePrimaryHost)
{
	auto state = std::make_shared<FakePageState>();
	ViewContainerPageRegistry registry;
	ASSERT_EQ(EViewContainerPageRegistrationStatus::Registered,
		registry.RegisterBatch({ Descriptor(state, std::string(kContainerId), {
			layout::EViewContainerLocation::Sidebar,
			layout::EViewContainerLocation::Panel,
			layout::EViewContainerLocation::AuxiliaryBar,
		}) }).status);
	ViewContainerPagePool pool(registry);
	const auto* retainedPage = pool.Acquire(kContainerId).page;
	ASSERT_NE(nullptr, retainedPage);
	ASSERT_EQ(EViewContainerPagePoolAttachStatus::Attached,
		pool.Attach(kContainerId, SideBarHost()).status);
	state->focused = true;
	state->failures.push_back(FakeStep::RestoreFocus);

	const auto failed = pool.Attach(kContainerId, PanelHost());
	EXPECT_EQ(EViewContainerPagePoolAttachStatus::FocusRestoreFailed, failed.status);
	ASSERT_TRUE(failed.finalState.has_value());
	ASSERT_TRUE(failed.finalState->host.has_value());
	EXPECT_EQ(SideBarHost(), *failed.finalState->host);
	EXPECT_EQ(retainedPage, pool.Acquire(kContainerId).page);
	ASSERT_TRUE(state->attachedHost.has_value());
	EXPECT_EQ(SideBarHost(), *state->attachedHost);
	EXPECT_TRUE(state->focused);
	EXPECT_EQ(1, state->factoryCalls);
}

TEST(ViewContainerPagePool, ProductionContributionHostRoutesRootAndLocalContentTogether)
{
	auto state = std::make_shared<FakePageState>();
	ViewContainerPageRegistry registry;
	ASSERT_EQ(EViewContainerPageRegistrationStatus::Registered,
		registry.RegisterBatch({ Descriptor(state, std::string(kContainerId), {
			layout::EViewContainerLocation::Sidebar,
			layout::EViewContainerLocation::Panel,
			layout::EViewContainerLocation::AuxiliaryBar,
		}) }).status);
	ViewContainerPagePool pool(registry);
	ContributedViewContainerPageHost host(pool);
	const std::array ids{ std::string(kContainerId) };
	ASSERT_TRUE(host.Initialize(ids));
	ASSERT_TRUE(host.IsUsable());
	ASSERT_EQ(EViewContainerPagePoolAttachStatus::Attached,
		pool.Attach(kContainerId, SideBarHost()).status);
	const RECT sideBarClient{ 0, 0, 320, 500 };
	host.Layout(kContainerId, sideBarClient, sideBarClient, 96);
	ASSERT_TRUE(state->projectionHostBounds.has_value());
	EXPECT_EQ(0, state->projectionHostBounds->left);
	EXPECT_EQ(0, state->projectionHostBounds->top);
	EXPECT_EQ(320, state->projectionHostBounds->right);
	EXPECT_EQ(500, state->projectionHostBounds->bottom);
	ASSERT_TRUE(state->projectionContentBounds.has_value());
	EXPECT_EQ(0, state->projectionContentBounds->left);
	EXPECT_EQ(0, state->projectionContentBounds->top);
	EXPECT_EQ(320, state->projectionContentBounds->right);
	EXPECT_EQ(500, state->projectionContentBounds->bottom);
	EXPECT_EQ(96U, state->projectionDpi);
	EXPECT_EQ(1, state->projectionLayoutCalls);

	ASSERT_EQ(EViewContainerPagePoolAttachStatus::Attached,
		pool.Attach(kContainerId, PanelHost()).status);

	const RECT wrapper{ 0, 34, 640, 200 };
	const RECT content{ 0, 0, 640, 166 };
	host.Layout(kContainerId, wrapper, content, 144);

	ASSERT_TRUE(state->projectionHostBounds.has_value());
	EXPECT_EQ(0, state->projectionHostBounds->left);
	EXPECT_EQ(34, state->projectionHostBounds->top);
	EXPECT_EQ(640, state->projectionHostBounds->right);
	EXPECT_EQ(200, state->projectionHostBounds->bottom);
	ASSERT_TRUE(state->projectionContentBounds.has_value());
	EXPECT_EQ(0, state->projectionContentBounds->left);
	EXPECT_EQ(0, state->projectionContentBounds->top);
	EXPECT_EQ(640, state->projectionContentBounds->right);
	EXPECT_EQ(166, state->projectionContentBounds->bottom);
	EXPECT_EQ(144U, state->projectionDpi);
	EXPECT_EQ(2, state->projectionLayoutCalls);
	EXPECT_EQ(1, state->factoryCalls);
}

TEST(ViewContainerPagePool, ProductionContributionHostFailsClosedWithoutNativeCompanion)
{
	auto state = std::make_shared<FakePageState>();
	ViewContainerPageRegistry registry;
	ViewContainerPageDescriptor descriptor{
		.containerId = std::string(kContainerId),
		.supportedLocations = {
			layout::EViewContainerLocation::Sidebar,
			layout::EViewContainerLocation::Panel,
			layout::EViewContainerLocation::AuxiliaryBar,
		},
		.factory = [state]() -> std::unique_ptr<IViewContainerPage> {
			++state->factoryCalls;
			return std::make_unique<FakePageWithoutProjection>(
				state, std::string(kContainerId));
		},
	};
	ASSERT_EQ(EViewContainerPageRegistrationStatus::Registered,
		registry.RegisterBatch({ std::move(descriptor) }).status);
	ViewContainerPagePool pool(registry);
	ContributedViewContainerPageHost host(pool);
	const std::array ids{ std::string(kContainerId) };

	EXPECT_FALSE(host.Initialize(ids));
	EXPECT_FALSE(host.IsUsable());
	EXPECT_EQ(1, state->factoryCalls);
	EXPECT_EQ(1, state->closeCalls);
	EXPECT_EQ(1, state->destructCalls);
	EXPECT_EQ(0, state->livePages);
	const auto terminal = pool.State(kContainerId);
	ASSERT_EQ(EViewContainerPageStateStatus::Found, terminal.status);
	ASSERT_TRUE(terminal.state.has_value());
	EXPECT_EQ(EViewContainerPageStableState::Closed, terminal.state->state);
	EXPECT_EQ(EViewContainerPageAcquireStatus::PoolClosed,
		pool.Acquire(kContainerId).status);
	EXPECT_EQ(EViewContainerPagePoolShutdownStatus::AlreadyClosed,
		pool.Shutdown().status);
}

TEST(ViewContainerPagePool, BottomPanelAdapterPropagatesFocusFailureAndUsesWrapperLocalContent)
{
	auto state = std::make_shared<FakePageState>();
	auto pages = std::make_shared<FakePaneCompositePageHostService>(state);
	ASSERT_TRUE(pages->IsUsable());
	ASSERT_EQ(EViewContainerPagePoolAttachStatus::Attached,
		pages->Attach(kContainerId, SideBarHost()).status);
	state->focused = true;
	state->failures.push_back(FakeStep::RestoreFocus);

	const HWND parent = ::CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
		0, 0, 640, 200, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	ASSERT_NE(nullptr, parent);
	workbench::panel::CBottomPanelTool panel({}, pages);
	ASSERT_TRUE(panel.Create(parent));
	ASSERT_TRUE(panel.ApplyActiveContainer(kContainerId));
	EXPECT_EQ(workbench::panel::EBottomPanelPageAttachStatus::Failed,
		panel.AttachActivePage());
	ASSERT_TRUE(state->attachedHost.has_value());
	EXPECT_EQ(SideBarHost(), *state->attachedHost);
	EXPECT_TRUE(state->focused);

	ASSERT_EQ(workbench::panel::EBottomPanelPageAttachStatus::Attached,
		panel.AttachActivePage());
	panel.Layout(RECT{ 0, 0, 640, 200 }, 96);
	EXPECT_EQ(0, pages->wrapperBounds.left);
	EXPECT_EQ(34, pages->wrapperBounds.top);
	EXPECT_EQ(640, pages->wrapperBounds.right);
	EXPECT_EQ(200, pages->wrapperBounds.bottom);
	EXPECT_EQ(0, pages->pageContentBounds.left);
	EXPECT_EQ(0, pages->pageContentBounds.top);
	EXPECT_EQ(640, pages->pageContentBounds.right);
	EXPECT_EQ(166, pages->pageContentBounds.bottom);
	EXPECT_TRUE(pages->pageVisible);
	EXPECT_GT(pages->layoutNotifications, 0);
	EXPECT_EQ(1, state->factoryCalls);

	panel.Close();
	::DestroyWindow(parent);
}

TEST(ViewContainerPagePool, RejectsUnsupportedDestinationBeforeCreatingAPage)
{
	auto state = std::make_shared<FakePageState>();
	auto registry = RegistryWith(state);
	ViewContainerPagePool pool(registry);
	const ViewContainerPageHost panel{ "workbench.parts.panel",
		layout::EViewContainerLocation::Panel, 303 };

	const auto result = pool.Attach(kContainerId, panel);
	EXPECT_EQ(EViewContainerPagePoolAttachStatus::DestinationNotSupported, result.status);
	EXPECT_EQ(EViewContainerPageCleanupOwner::None, result.cleanupOwner);
	EXPECT_EQ(0, state->factoryCalls);
	EXPECT_EQ(0U, pool.Size());
	EXPECT_TRUE(state->calls.empty());
	EXPECT_EQ(0U, state->parent);
	EXPECT_EQ(0U, state->nestedParent);
}

TEST(ViewContainerPagePool, RejectsUnsupportedDestinationWithoutTouchingAnExistingPage)
{
	auto state = std::make_shared<FakePageState>();
	auto registry = RegistryWith(state);
	ViewContainerPagePool pool(registry);
	const auto sideBar = SideBarHost();
	ASSERT_EQ(EViewContainerPagePoolAttachStatus::Attached,
		pool.Attach(kContainerId, sideBar).status);
	state->focused = true;
	state->calls.clear();
	const ViewContainerPageHost panel{ "workbench.parts.panel",
		layout::EViewContainerLocation::Panel, 303 };

	const auto result = pool.Attach(kContainerId, panel);
	EXPECT_EQ(EViewContainerPagePoolAttachStatus::DestinationNotSupported, result.status);
	EXPECT_TRUE(state->calls.empty());
	EXPECT_EQ(std::optional(sideBar), state->attachedHost);
	EXPECT_EQ(sideBar.nativeParent, state->parent);
	EXPECT_EQ(sideBar.nativeParent, state->nestedParent);
	EXPECT_TRUE(state->focused);
	EXPECT_EQ(1, state->factoryCalls);
}

TEST(ViewContainerPagePool, EveryForwardMoveFailureRestoresTheOldAttachmentAndFocus)
{
	const std::vector<std::pair<FakeStep, EViewContainerPagePoolAttachStatus>> failures{
		{ FakeStep::CaptureFocus, EViewContainerPagePoolAttachStatus::FocusCaptureFailed },
		{ FakeStep::Detach, EViewContainerPagePoolAttachStatus::DetachFailed },
		{ FakeStep::Reparent, EViewContainerPagePoolAttachStatus::ReparentFailed },
		{ FakeStep::Attach, EViewContainerPagePoolAttachStatus::AttachFailed },
		{ FakeStep::RestoreFocus, EViewContainerPagePoolAttachStatus::FocusRestoreFailed },
	};
	for (const auto& [step, expectedStatus] : failures) {
		auto state = std::make_shared<FakePageState>();
		auto registry = RegistryWith(state);
		ViewContainerPagePool pool(registry);
		const auto oldHost = SideBarHost();
		ASSERT_EQ(EViewContainerPagePoolAttachStatus::Attached,
			pool.Attach(kContainerId, oldHost).status);
		state->focused = true;
		state->calls.clear();
		state->failures = { step };

		const auto result = pool.Attach(kContainerId, AuxiliaryHost());
		EXPECT_EQ(expectedStatus, result.status) << static_cast<int>(step);
		EXPECT_EQ(EViewContainerPageTransitionStage::None, result.rollbackFailedStage);
		ASSERT_TRUE(result.finalState);
		EXPECT_EQ(EViewContainerPageStableState::Attached, result.finalState->state);
		EXPECT_EQ(std::optional(oldHost), result.finalState->host);
		EXPECT_EQ(std::optional(oldHost), state->attachedHost);
		EXPECT_EQ(oldHost.nativeParent, state->parent);
		EXPECT_EQ(oldHost.nativeParent, state->nestedParent);
		EXPECT_TRUE(state->focused);
		EXPECT_EQ(EViewContainerPageCleanupOwner::PagePool, result.cleanupOwner);
		EXPECT_TRUE(state->failures.empty());
	}
}

TEST(ViewContainerPagePool, RollbackFailureIsExplicitAndLeavesAKnownSingleAttachmentState)
{
	auto state = std::make_shared<FakePageState>();
	auto registry = RegistryWith(state);
	ViewContainerPagePool pool(registry);
	const auto oldHost = SideBarHost();
	ASSERT_EQ(EViewContainerPagePoolAttachStatus::Attached,
		pool.Attach(kContainerId, oldHost).status);
	state->focused = true;
	// Destination Attach fails, then reparenting back to the old host fails.
	state->failures = { FakeStep::Attach, FakeStep::Reparent };

	const auto result = pool.Attach(kContainerId, AuxiliaryHost());
	EXPECT_EQ(EViewContainerPagePoolAttachStatus::RollbackFailed, result.status);
	EXPECT_EQ(EViewContainerPageTransitionStage::Attach, result.failedStage);
	EXPECT_EQ(EViewContainerPageTransitionStage::Reparent, result.rollbackFailedStage);
	ASSERT_TRUE(result.finalState);
	EXPECT_EQ(EViewContainerPageStableState::Detached, result.finalState->state);
	EXPECT_FALSE(result.finalState->host);
	EXPECT_FALSE(state->attachedHost);
	EXPECT_EQ(AuxiliaryHost().nativeParent, state->parent);
	EXPECT_EQ(AuxiliaryHost().nativeParent, state->nestedParent);
	EXPECT_EQ(EViewContainerPageCleanupOwner::PagePool, result.cleanupOwner);
}

TEST(ViewContainerPagePool, RollbackDetachFailureReportsTheDestinationAsTheStableOwner)
{
	auto state = std::make_shared<FakePageState>();
	auto registry = RegistryWith(state);
	ViewContainerPagePool pool(registry);
	const auto oldHost = SideBarHost();
	const auto destination = AuxiliaryHost();
	ASSERT_EQ(EViewContainerPagePoolAttachStatus::Attached,
		pool.Attach(kContainerId, oldHost).status);
	state->focused = true;
	// Forward focus restore fails after destination attach. Rollback then cannot
	// detach that destination, so callers must not attach another page beside it.
	state->failures = { FakeStep::RestoreFocus, FakeStep::Detach };

	const auto result = pool.Attach(kContainerId, destination);
	EXPECT_EQ(EViewContainerPagePoolAttachStatus::RollbackFailed, result.status);
	EXPECT_EQ(EViewContainerPageTransitionStage::RestoreFocus, result.failedStage);
	EXPECT_EQ(EViewContainerPageTransitionStage::Detach, result.rollbackFailedStage);
	ASSERT_TRUE(result.finalState);
	EXPECT_EQ(EViewContainerPageStableState::Attached, result.finalState->state);
	EXPECT_EQ(std::optional(destination), result.finalState->host);
	EXPECT_EQ(std::optional(destination), state->attachedHost);
	EXPECT_EQ(destination.nativeParent, state->parent);
	EXPECT_EQ(destination.nativeParent, state->nestedParent);
	EXPECT_FALSE(state->focused);
}

TEST(ViewContainerPagePool, FailedDetachReparentRollsBackTheOldAttachmentAndFocus)
{
	auto state = std::make_shared<FakePageState>();
	auto registry = RegistryWith(state);
	ViewContainerPagePool pool(registry);
	const auto oldHost = SideBarHost();
	ASSERT_EQ(EViewContainerPagePoolAttachStatus::Attached,
		pool.Attach(kContainerId, oldHost).status);
	state->focused = true;
	state->failures = { FakeStep::Reparent };

	const auto result = pool.Detach(kContainerId);
	EXPECT_EQ(EViewContainerPagePoolDetachStatus::ReparentFailed, result.status);
	EXPECT_EQ(EViewContainerPageTransitionStage::Reparent, result.failedStage);
	EXPECT_EQ(EViewContainerPageTransitionStage::None, result.rollbackFailedStage);
	ASSERT_TRUE(result.finalState);
	EXPECT_EQ(EViewContainerPageStableState::Attached, result.finalState->state);
	EXPECT_EQ(std::optional(oldHost), state->attachedHost);
	EXPECT_EQ(oldHost.nativeParent, state->parent);
	EXPECT_EQ(oldHost.nativeParent, state->nestedParent);
	EXPECT_TRUE(state->focused);
}

TEST(ViewContainerPagePool, StateStagingFailureStopsBeforeNativeWorkAndRemainsRetryable)
{
	auto state = std::make_shared<FakePageState>();
	auto registry = RegistryWith(state);
	ViewContainerPagePool pool(registry);
	const auto oldHost = SideBarHost();
	const auto destination = AuxiliaryHost();
	ASSERT_EQ(EViewContainerPagePoolAttachStatus::Attached,
		pool.Attach(kContainerId, oldHost).status);
	state->focused = true;
	state->calls.clear();

	ViewContainerPagePoolTestPeer::FailNextStateStaging(pool);
	const auto failedAttach = pool.Attach(kContainerId, destination);
	EXPECT_EQ(EViewContainerPagePoolAttachStatus::InternalFailure, failedAttach.status);
	EXPECT_FALSE(failedAttach.finalState);
	EXPECT_EQ(EViewContainerPageCleanupOwner::PagePool, failedAttach.cleanupOwner);
	EXPECT_TRUE(state->calls.empty());
	EXPECT_EQ(std::optional(oldHost), state->attachedHost);
	EXPECT_EQ(oldHost.nativeParent, state->parent);
	EXPECT_TRUE(state->focused);

	const auto unchanged = pool.State(kContainerId);
	ASSERT_EQ(EViewContainerPageStateStatus::Found, unchanged.status);
	ASSERT_TRUE(unchanged.state);
	EXPECT_EQ(EViewContainerPageStableState::Attached, unchanged.state->state);
	EXPECT_EQ(std::optional(oldHost), unchanged.state->host);
	EXPECT_EQ(EViewContainerPagePoolAttachStatus::Attached,
		pool.Attach(kContainerId, destination).status);

	state->calls.clear();
	ViewContainerPagePoolTestPeer::FailNextStateStaging(pool);
	const auto failedDetach = pool.Detach(kContainerId);
	EXPECT_EQ(EViewContainerPagePoolDetachStatus::InternalFailure, failedDetach.status);
	EXPECT_FALSE(failedDetach.finalState);
	EXPECT_EQ(EViewContainerPageCleanupOwner::PagePool, failedDetach.cleanupOwner);
	EXPECT_TRUE(state->calls.empty());
	EXPECT_EQ(std::optional(destination), state->attachedHost);
	EXPECT_EQ(destination.nativeParent, state->parent);

	ViewContainerPagePoolTestPeer::FailNextStateStaging(pool);
	const auto failedState = pool.State(kContainerId);
	EXPECT_EQ(EViewContainerPageStateStatus::InternalFailure, failedState.status);
	EXPECT_FALSE(failedState.state);
	const auto recoveredState = pool.State(kContainerId);
	ASSERT_EQ(EViewContainerPageStateStatus::Found, recoveredState.status);
	ASSERT_TRUE(recoveredState.state);
	EXPECT_EQ(std::optional(destination), recoveredState.state->host);
	EXPECT_EQ(EViewContainerPagePoolDetachStatus::Detached, pool.Detach(kContainerId).status);
}

TEST(ViewContainerPagePool, RegistryRejectsInvalidAndDuplicateBatchesAtomically)
{
	auto state = std::make_shared<FakePageState>();
	ViewContainerPageRegistry registry;
	EXPECT_EQ(EViewContainerPageRegistrationStatus::Registered,
		registry.RegisterBatch({ Descriptor(state, "existing.container") }).status);

	auto invalid = Descriptor(state, "invalid.container");
	invalid.supportedLocations = {};
	const auto invalidResult = registry.RegisterBatch(
		{ Descriptor(state, "candidate.container"), std::move(invalid) });
	EXPECT_EQ(EViewContainerPageRegistrationStatus::InvalidDescriptor, invalidResult.status);
	EXPECT_EQ(1U, registry.Size());
	EXPECT_EQ(nullptr, registry.Find("candidate.container"));

	const auto invalidId = registry.RegisterBatch({ Descriptor(state, "") });
	EXPECT_EQ(EViewContainerPageRegistrationStatus::InvalidDescriptor, invalidId.status);
	EXPECT_EQ(1U, registry.Size());

	const auto duplicateBatch = registry.RegisterBatch(
		{ Descriptor(state, "duplicate.container"), Descriptor(state, "duplicate.container") });
	EXPECT_EQ(EViewContainerPageRegistrationStatus::DuplicateContainerId, duplicateBatch.status);
	EXPECT_EQ(1U, registry.Size());
	EXPECT_EQ(nullptr, registry.Find("duplicate.container"));

	const auto duplicateExisting = registry.RegisterBatch({ Descriptor(state, "existing.container") });
	EXPECT_EQ(EViewContainerPageRegistrationStatus::DuplicateContainerId, duplicateExisting.status);
	EXPECT_EQ(1U, registry.Size());
}

TEST(ViewContainerPagePool, InvalidFactoryProductIsClosedAndDestroyedWithoutPoolOwnership)
{
	auto state = std::make_shared<FakePageState>();
	ViewContainerPageRegistry registry;
	auto descriptor = Descriptor(state);
	descriptor.factory = [state]() -> std::unique_ptr<IViewContainerPage> {
		++state->factoryCalls;
		return std::make_unique<FakePage>(state, "wrong.container");
	};
	ASSERT_EQ(EViewContainerPageRegistrationStatus::Registered,
		registry.RegisterBatch({ std::move(descriptor) }).status);
	ViewContainerPagePool pool(registry);

	const auto result = pool.Acquire(kContainerId);
	EXPECT_EQ(EViewContainerPageAcquireStatus::InvalidPage, result.status);
	EXPECT_EQ(EViewContainerPageCleanupOwner::PageDestructor, result.cleanupOwner);
	EXPECT_EQ(1, state->closeCalls);
	EXPECT_EQ(1, state->destructCalls);
	EXPECT_EQ(0, state->livePages);
	EXPECT_EQ(0U, pool.Size());
}

TEST(ViewContainerPagePool, CloseIsExactlyOnceAndHostValuesAreNeverOwned)
{
	auto state = std::make_shared<FakePageState>();
	auto registry = RegistryWith(state);
	ViewContainerPagePool pool(registry);
	ASSERT_EQ(EViewContainerPagePoolAttachStatus::Attached,
		pool.Attach(kContainerId, SideBarHost()).status);

	const auto first = pool.Close(kContainerId);
	EXPECT_EQ(EViewContainerPagePoolCloseStatus::Closed, first.status);
	ASSERT_TRUE(first.finalState);
	EXPECT_EQ(EViewContainerPageStableState::Closed, first.finalState->state);
	EXPECT_EQ(EViewContainerPageCleanupOwner::None, first.cleanupOwner);
	EXPECT_EQ(1, state->closeCalls);
	EXPECT_EQ(1, state->destructCalls);
	EXPECT_EQ(0, state->livePages);

	EXPECT_EQ(EViewContainerPagePoolCloseStatus::AlreadyClosed, pool.Close(kContainerId).status);
	EXPECT_EQ(EViewContainerPagePoolShutdownStatus::Closed, pool.Shutdown().status);
	EXPECT_EQ(1, state->closeCalls);
	EXPECT_EQ(1, state->destructCalls);
}

TEST(ViewContainerPagePool, FailedCloseStillFinalizesOnceThroughThePageDestructor)
{
	auto state = std::make_shared<FakePageState>();
	auto registry = RegistryWith(state);
	ViewContainerPagePool pool(registry);
	ASSERT_TRUE(pool.Acquire(kContainerId).Succeeded());
	state->failures = { FakeStep::Close };

	const auto result = pool.Close(kContainerId);
	EXPECT_EQ(EViewContainerPagePoolCloseStatus::CloseFailed, result.status);
	EXPECT_EQ(EViewContainerPageCleanupOwner::PageDestructor, result.cleanupOwner);
	EXPECT_EQ(1, state->closeCalls);
	EXPECT_EQ(1, state->destructCalls);
	EXPECT_EQ(0, state->livePages);
	EXPECT_EQ(EViewContainerPagePoolCloseStatus::AlreadyClosed, pool.Close(kContainerId).status);
	EXPECT_EQ(1, state->closeCalls);
}

} // namespace
} // namespace workbench::viewcontainer
