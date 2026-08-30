/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "workbench/viewcontainer/ViewContainerPagePool.h"

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
	bool focused{};
	bool closed{};
	int factoryCalls{};
	int livePages{};
	int destructCalls{};
	int closeCalls{};

	[[nodiscard]] bool ShouldFail(const FakeStep step)
	{
		calls.push_back(step);
		if (failures.empty() || failures.front() != step) return false;
		failures.erase(failures.begin());
		return true;
	}
};

class FakePage final : public IViewContainerPage {
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
		m_state->focused = false;
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

TEST(ViewContainerPagePool, CoversAttachMoveDetachAndReattachTransitionsWithoutDoubleAttachment)
{
	auto state = std::make_shared<FakePageState>();
	auto registry = RegistryWith(state);
	ViewContainerPagePool pool(registry);
	const auto sideBar = SideBarHost();
	const auto auxiliary = AuxiliaryHost();

	EXPECT_EQ(EViewContainerPagePoolAttachStatus::Attached,
		pool.Attach(kContainerId, sideBar).status);
	const auto attachCalls = state->calls.size();
	EXPECT_EQ(EViewContainerPagePoolAttachStatus::AlreadyAttached,
		pool.Attach(kContainerId, sideBar).status);
	EXPECT_EQ(attachCalls, state->calls.size());

	state->focused = true;
	EXPECT_EQ(EViewContainerPagePoolAttachStatus::Attached,
		pool.Attach(kContainerId, auxiliary).status);
	EXPECT_EQ(std::optional(auxiliary), state->attachedHost);
	EXPECT_EQ(auxiliary.nativeParent, state->parent);
	EXPECT_TRUE(state->focused);

	EXPECT_EQ(EViewContainerPagePoolDetachStatus::Detached, pool.Detach(kContainerId).status);
	EXPECT_FALSE(state->attachedHost);
	EXPECT_EQ(0U, state->parent);
	EXPECT_FALSE(state->focused);
	EXPECT_EQ(EViewContainerPagePoolDetachStatus::AlreadyDetached,
		pool.Detach(kContainerId).status);

	EXPECT_EQ(EViewContainerPagePoolAttachStatus::Attached,
		pool.Attach(kContainerId, sideBar).status);
	EXPECT_EQ(std::optional(sideBar), state->attachedHost);
	EXPECT_EQ(sideBar.nativeParent, state->parent);
	EXPECT_TRUE(state->focused);
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
	EXPECT_EQ(EViewContainerPageCleanupOwner::PagePool, result.cleanupOwner);
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
