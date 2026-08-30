/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/viewcontainer/ViewContainerPagePool.h"

#include <map>
#include <type_traits>
#include <utility>

namespace workbench::viewcontainer {
namespace {

[[nodiscard]] bool IsValidLocation(const layout::EViewContainerLocation location) noexcept
{
	return location == layout::EViewContainerLocation::Sidebar
		|| location == layout::EViewContainerLocation::Panel
		|| location == layout::EViewContainerLocation::AuxiliaryBar;
}

[[nodiscard]] bool IsValidHost(const ViewContainerPageHost& host) noexcept
{
	return layout::WorkbenchContributionRegistry::IsValidStableId(host.id)
		&& IsValidLocation(host.location) && host.nativeParent != 0;
}

[[nodiscard]] ViewContainerPageState DetachedState() noexcept
{
	return { EViewContainerPageStableState::Detached, std::nullopt };
}

[[nodiscard]] ViewContainerPageState ClosedState() noexcept
{
	return { EViewContainerPageStableState::Closed, std::nullopt };
}

static_assert(std::is_nothrow_move_constructible_v<ViewContainerPageHost>);
static_assert(std::is_nothrow_move_constructible_v<std::optional<ViewContainerPageHost>>);
static_assert(std::is_nothrow_move_assignable_v<std::optional<ViewContainerPageHost>>);
static_assert(std::is_nothrow_move_constructible_v<ViewContainerPageState>);
static_assert(std::is_nothrow_move_constructible_v<std::optional<ViewContainerPageState>>);
static_assert(std::is_nothrow_move_assignable_v<std::optional<ViewContainerPageState>>);
static_assert(std::is_nothrow_move_constructible_v<ViewContainerPagePoolAttachResult>);
static_assert(std::is_nothrow_move_constructible_v<ViewContainerPagePoolDetachResult>);

} // namespace

class ViewContainerPagePool::Impl final {
public:
	explicit Impl(const ViewContainerPageRegistry& registry) : m_registry(registry) {}

	struct Entry final {
		std::unique_ptr<IViewContainerPage> page;
		std::optional<ViewContainerPageHost> attachedHost;
		std::optional<ViewContainerFocusToken> pendingFocus;
		ViewContainerNativeHandle currentParent{};
		bool closeInvoked{};

		Entry() = default;
		explicit Entry(std::unique_ptr<IViewContainerPage> value) : page(std::move(value)) {}
		Entry(Entry&&) noexcept = default;
		Entry& operator=(Entry&&) noexcept = default;
		Entry(const Entry&) = delete;
		Entry& operator=(const Entry&) = delete;
		~Entry() { (void)Finalize(); }

		[[nodiscard]] EViewContainerPageCloseStatus Finalize() noexcept
		{
			if (!page || closeInvoked) return EViewContainerPageCloseStatus::Closed;
			closeInvoked = true;
			const auto status = page->Close();
			attachedHost.reset();
			pendingFocus.reset();
			currentParent = 0;
			page.reset();
			return status;
		}
	};

	struct AttachStaging final {
		ViewContainerPageHost destinationForPool;
		ViewContainerPageState destinationState;
		std::optional<ViewContainerPageState> previousState;
	};

	using EntryMap = std::map<std::string, Entry, std::less<>>;

	void FailNextStateStagingForTest() noexcept { m_failNextStateStagingForTest = true; }

	[[nodiscard]] ViewContainerPageAcquireResult Acquire(const std::string_view containerId) noexcept
	{
		if (m_shutdown) {
			return { EViewContainerPageAcquireStatus::PoolClosed, nullptr,
				EViewContainerPageCleanupOwner::None };
		}
		try {
			if (const auto found = m_entries.find(containerId); found != m_entries.end()) {
				if (!found->second.page) {
					return { EViewContainerPageAcquireStatus::PageClosed, nullptr,
						EViewContainerPageCleanupOwner::None };
				}
				return { EViewContainerPageAcquireStatus::Existing, found->second.page.get(),
					EViewContainerPageCleanupOwner::PagePool };
			}
			const auto* descriptor = m_registry.Find(containerId);
			if (descriptor == nullptr) {
				return { EViewContainerPageAcquireStatus::UnknownContainer, nullptr,
					EViewContainerPageCleanupOwner::None };
			}

			std::unique_ptr<IViewContainerPage> page;
			try {
				page = descriptor->factory();
			} catch (...) {
				return { EViewContainerPageAcquireStatus::FactoryFailed, nullptr,
					EViewContainerPageCleanupOwner::None };
			}
			if (!page) {
				return { EViewContainerPageAcquireStatus::FactoryFailed, nullptr,
					EViewContainerPageCleanupOwner::None };
			}
			Entry candidate(std::move(page));
			if (candidate.page->ContainerId() != containerId
				|| !layout::WorkbenchContributionRegistry::IsValidStableId(
					candidate.page->ContainerId())) {
				(void)candidate.Finalize();
				return { EViewContainerPageAcquireStatus::InvalidPage, nullptr,
					EViewContainerPageCleanupOwner::PageDestructor };
			}

			const auto [inserted, created] = m_entries.emplace(
				std::string(containerId), std::move(candidate));
			if (!created) {
				return { EViewContainerPageAcquireStatus::Existing, inserted->second.page.get(),
					EViewContainerPageCleanupOwner::PagePool };
			}
			return { EViewContainerPageAcquireStatus::Created, inserted->second.page.get(),
				EViewContainerPageCleanupOwner::PagePool };
		} catch (...) {
			return { EViewContainerPageAcquireStatus::Failed, nullptr,
				EViewContainerPageCleanupOwner::PageDestructor };
		}
	}

	[[nodiscard]] EViewContainerPageTransitionStage Rollback(Entry& entry,
		std::optional<ViewContainerPageHost>& targetHost,
		const std::optional<ViewContainerFocusToken>& focusToken) noexcept
	{
		if (entry.attachedHost) {
			if (entry.page->Detach(*entry.attachedHost)
				!= EViewContainerPageDetachStatus::Detached) {
				return EViewContainerPageTransitionStage::Detach;
			}
			entry.attachedHost.reset();
		}
		const auto targetParent = targetHost ? targetHost->nativeParent : 0;
		if (entry.currentParent != targetParent) {
			if (entry.page->Reparent(targetParent)
				!= EViewContainerPageReparentStatus::Reparented) {
				return EViewContainerPageTransitionStage::Reparent;
			}
			entry.currentParent = targetParent;
		}
		if (targetHost) {
			if (entry.page->Attach(*targetHost) != EViewContainerPageAttachStatus::Attached) {
				return EViewContainerPageTransitionStage::Attach;
			}
			entry.attachedHost = std::move(targetHost);
			if (focusToken && entry.page->RestoreFocusToken(*focusToken)
				!= EViewContainerFocusRestoreStatus::Restored) {
				return EViewContainerPageTransitionStage::RestoreFocus;
			}
		}
		return EViewContainerPageTransitionStage::None;
	}

	[[nodiscard]] ViewContainerPagePoolAttachResult Attach(const std::string_view containerId,
		const ViewContainerPageHost& destination) noexcept
	{
		if (m_shutdown) return AttachEmpty(EViewContainerPagePoolAttachStatus::PoolClosed);
		const auto* descriptor = m_registry.Find(containerId);
		if (descriptor == nullptr) return AttachEmpty(EViewContainerPagePoolAttachStatus::UnknownContainer);
		if (!IsValidHost(destination)) return AttachEmpty(EViewContainerPagePoolAttachStatus::InvalidHost);
		if (!descriptor->supportedLocations.Contains(destination.location)) {
			return AttachEmpty(EViewContainerPagePoolAttachStatus::DestinationNotSupported);
		}

		const auto acquired = Acquire(containerId);
		if (!acquired.Succeeded()) {
			const auto status = acquired.status == EViewContainerPageAcquireStatus::PageClosed
				? EViewContainerPagePoolAttachStatus::PageClosed
				: acquired.status == EViewContainerPageAcquireStatus::PoolClosed
					? EViewContainerPagePoolAttachStatus::PoolClosed
					: EViewContainerPagePoolAttachStatus::PageUnavailable;
			return { status, EViewContainerPageTransitionStage::None,
				EViewContainerPageTransitionStage::None, std::nullopt, acquired.cleanupOwner };
		}

		auto found = m_entries.find(containerId);
		if (found == m_entries.end() || !found->second.page) {
			return AttachEmpty(EViewContainerPagePoolAttachStatus::PageUnavailable);
		}
		auto& entry = found->second;
		if (entry.attachedHost == destination) {
			auto state = StageCurrentState(entry);
			if (!state) return AttachInternalFailure(entry);
			return AttachWithState(EViewContainerPagePoolAttachStatus::AlreadyAttached,
				std::move(*state));
		}

		auto staging = PrepareAttachStaging(entry, destination);
		if (!staging) return AttachInternalFailure(entry);
		// The owning copies are complete. Native work below uses only noexcept moves,
		// scalar state changes, and the noexcept page contract.
		auto focusToken = entry.pendingFocus;
		std::optional<ViewContainerPageHost> rollbackHost;
		if (entry.attachedHost) {
			const auto capture = entry.page->CaptureFocusToken();
			if (capture.status == EViewContainerFocusCaptureStatus::Failed
				|| (capture.status == EViewContainerFocusCaptureStatus::Captured && !capture.token)) {
				return AttachFailure(EViewContainerPagePoolAttachStatus::FocusCaptureFailed,
					EViewContainerPageTransitionStage::CaptureFocus, std::move(staging->previousState));
			}
			if (capture.status == EViewContainerFocusCaptureStatus::Captured) focusToken = capture.token;
			if (entry.page->Detach(*entry.attachedHost)
				!= EViewContainerPageDetachStatus::Detached) {
				return AttachFailure(EViewContainerPagePoolAttachStatus::DetachFailed,
					EViewContainerPageTransitionStage::Detach, std::move(staging->previousState));
			}
			rollbackHost = std::move(entry.attachedHost);
			entry.attachedHost.reset();
		}

		if (entry.currentParent != destination.nativeParent) {
			if (entry.page->Reparent(destination.nativeParent)
				!= EViewContainerPageReparentStatus::Reparented) {
				return AttachRollback(EViewContainerPagePoolAttachStatus::ReparentFailed,
					EViewContainerPageTransitionStage::Reparent, entry, rollbackHost,
					focusToken, *staging);
			}
			entry.currentParent = destination.nativeParent;
		}
		if (entry.page->Attach(destination) != EViewContainerPageAttachStatus::Attached) {
			return AttachRollback(EViewContainerPagePoolAttachStatus::AttachFailed,
				EViewContainerPageTransitionStage::Attach, entry, rollbackHost,
				focusToken, *staging);
		}
		entry.attachedHost.emplace(std::move(staging->destinationForPool));
		if (focusToken && entry.page->RestoreFocusToken(*focusToken)
			!= EViewContainerFocusRestoreStatus::Restored) {
			return AttachRollback(EViewContainerPagePoolAttachStatus::FocusRestoreFailed,
				EViewContainerPageTransitionStage::RestoreFocus, entry, rollbackHost,
				focusToken, *staging);
		}
		entry.pendingFocus.reset();
		return AttachWithState(EViewContainerPagePoolAttachStatus::Attached,
			std::move(staging->destinationState));
	}

	[[nodiscard]] ViewContainerPagePoolDetachResult Detach(const std::string_view containerId) noexcept
	{
		if (m_shutdown) return DetachEmpty(EViewContainerPagePoolDetachStatus::PoolClosed);
		if (m_registry.Find(containerId) == nullptr) {
			return DetachEmpty(EViewContainerPagePoolDetachStatus::UnknownContainer);
		}
		auto found = m_entries.find(containerId);
		if (found == m_entries.end()) return DetachEmpty(EViewContainerPagePoolDetachStatus::NotAcquired);
		auto& entry = found->second;
		if (!entry.page) {
			return DetachWithState(EViewContainerPagePoolDetachStatus::PageClosed,
				ClosedState(), EViewContainerPageCleanupOwner::None);
		}
		if (!entry.attachedHost) {
			return DetachWithState(EViewContainerPagePoolDetachStatus::AlreadyDetached,
				DetachedState(), EViewContainerPageCleanupOwner::PagePool);
		}

		auto previousState = StageCurrentState(entry);
		if (!previousState) return DetachInternalFailure();
		// The owning result copy is complete before the first native page operation.
		auto focusToken = entry.pendingFocus;
		const auto capture = entry.page->CaptureFocusToken();
		if (capture.status == EViewContainerFocusCaptureStatus::Failed
			|| (capture.status == EViewContainerFocusCaptureStatus::Captured && !capture.token)) {
			return DetachFailure(EViewContainerPagePoolDetachStatus::FocusCaptureFailed,
				EViewContainerPageTransitionStage::CaptureFocus, std::move(*previousState));
		}
		if (capture.status == EViewContainerFocusCaptureStatus::Captured) focusToken = capture.token;
		if (entry.page->Detach(*entry.attachedHost) != EViewContainerPageDetachStatus::Detached) {
			return DetachFailure(EViewContainerPagePoolDetachStatus::DetachFailed,
				EViewContainerPageTransitionStage::Detach, std::move(*previousState));
		}
		auto rollbackHost = std::move(entry.attachedHost);
		entry.attachedHost.reset();
		if (entry.currentParent != 0) {
			if (entry.page->Reparent(0) != EViewContainerPageReparentStatus::Reparented) {
				return DetachRollback(EViewContainerPagePoolDetachStatus::ReparentFailed,
					EViewContainerPageTransitionStage::Reparent, entry, rollbackHost,
					focusToken, std::move(*previousState));
			}
			entry.currentParent = 0;
		}
		entry.pendingFocus = focusToken;
		return DetachWithState(EViewContainerPagePoolDetachStatus::Detached,
			DetachedState(), EViewContainerPageCleanupOwner::PagePool);
	}

	[[nodiscard]] ViewContainerPagePoolCloseResult Close(const std::string_view containerId) noexcept
	{
		if (m_registry.Find(containerId) == nullptr) {
			return { EViewContainerPagePoolCloseStatus::UnknownContainer, std::nullopt,
				EViewContainerPageCleanupOwner::None };
		}
		auto found = m_entries.find(containerId);
		if (found == m_entries.end()) {
			return { EViewContainerPagePoolCloseStatus::NotAcquired, std::nullopt,
				EViewContainerPageCleanupOwner::None };
		}
		auto& entry = found->second;
		if (!entry.page || entry.closeInvoked) {
			return { EViewContainerPagePoolCloseStatus::AlreadyClosed, ClosedState(),
				EViewContainerPageCleanupOwner::None };
		}
		const auto status = entry.Finalize();
		return { status == EViewContainerPageCloseStatus::Closed
				? EViewContainerPagePoolCloseStatus::Closed
				: EViewContainerPagePoolCloseStatus::CloseFailed,
			ClosedState(), status == EViewContainerPageCloseStatus::Closed
				? EViewContainerPageCleanupOwner::None
				: EViewContainerPageCleanupOwner::PageDestructor };
	}

	[[nodiscard]] ViewContainerPagePoolShutdownResult Shutdown() noexcept
	{
		if (m_shutdown) return { EViewContainerPagePoolShutdownStatus::AlreadyClosed, 0, 0 };
		m_shutdown = true;
		std::size_t closed{};
		std::size_t failed{};
		for (auto& [id, entry] : m_entries) {
			(void)id;
			if (!entry.page || entry.closeInvoked) continue;
			if (entry.Finalize() == EViewContainerPageCloseStatus::Closed) ++closed;
			else ++failed;
		}
		return { failed == 0 ? EViewContainerPagePoolShutdownStatus::Closed
				: EViewContainerPagePoolShutdownStatus::CloseFailed,
			closed, failed };
	}

	[[nodiscard]] ViewContainerPageStateResult State(
		const std::string_view containerId) const noexcept
	{
		try {
			const auto found = m_entries.find(containerId);
			if (found == m_entries.end()) {
				return { EViewContainerPageStateStatus::NotAcquired, std::nullopt };
			}
			auto state = StageCurrentState(found->second);
			if (!state) return { EViewContainerPageStateStatus::InternalFailure, std::nullopt };
			return { EViewContainerPageStateStatus::Found, std::move(state) };
		} catch (...) {
			return { EViewContainerPageStateStatus::InternalFailure, std::nullopt };
		}
	}

	[[nodiscard]] std::size_t Size() const noexcept { return m_entries.size(); }

private:
	[[nodiscard]] bool ConsumeStateStagingFailureForTest() const noexcept
	{
		if (!m_failNextStateStagingForTest) return false;
		m_failNextStateStagingForTest = false;
		return true;
	}

	[[nodiscard]] std::optional<ViewContainerPageState> StageCurrentState(
		const Entry& entry) const noexcept
	{
		if (ConsumeStateStagingFailureForTest()) return std::nullopt;
		try {
			if (!entry.page) return ClosedState();
			if (entry.attachedHost) {
				return ViewContainerPageState{
					EViewContainerPageStableState::Attached, entry.attachedHost };
			}
			return DetachedState();
		} catch (...) {
			return std::nullopt;
		}
	}

	[[nodiscard]] std::optional<AttachStaging> PrepareAttachStaging(
		const Entry& entry, const ViewContainerPageHost& destination) const noexcept
	{
		if (ConsumeStateStagingFailureForTest()) return std::nullopt;
		try {
			AttachStaging staging{
				destination,
				{ EViewContainerPageStableState::Attached, destination },
				std::nullopt,
			};
			if (entry.attachedHost) {
				staging.previousState.emplace(ViewContainerPageState{
					EViewContainerPageStableState::Attached, entry.attachedHost });
			}
			return std::move(staging);
		} catch (...) {
			return std::nullopt;
		}
	}

	[[nodiscard]] static ViewContainerPagePoolAttachResult AttachEmpty(
		const EViewContainerPagePoolAttachStatus status) noexcept
	{
		return { status, EViewContainerPageTransitionStage::None,
			EViewContainerPageTransitionStage::None, std::nullopt,
			EViewContainerPageCleanupOwner::None };
	}

	[[nodiscard]] static ViewContainerPagePoolAttachResult AttachInternalFailure(
		const Entry& entry) noexcept
	{
		return { EViewContainerPagePoolAttachStatus::InternalFailure,
			EViewContainerPageTransitionStage::None,
			EViewContainerPageTransitionStage::None, std::nullopt,
			entry.page ? EViewContainerPageCleanupOwner::PagePool
				: EViewContainerPageCleanupOwner::None };
	}

	[[nodiscard]] static ViewContainerPagePoolAttachResult AttachWithState(
		const EViewContainerPagePoolAttachStatus status,
		ViewContainerPageState state) noexcept
	{
		return { status, EViewContainerPageTransitionStage::None,
			EViewContainerPageTransitionStage::None, std::move(state),
			EViewContainerPageCleanupOwner::PagePool };
	}

	[[nodiscard]] static ViewContainerPagePoolAttachResult AttachFailure(
		const EViewContainerPagePoolAttachStatus status,
		const EViewContainerPageTransitionStage failedStage,
		std::optional<ViewContainerPageState> state) noexcept
	{
		return { status, failedStage, EViewContainerPageTransitionStage::None,
			std::move(state), EViewContainerPageCleanupOwner::PagePool };
	}

	[[nodiscard]] static std::optional<ViewContainerPageState> SelectAttachState(
		const Entry& entry, AttachStaging& staging) noexcept
	{
		if (!entry.page) return ClosedState();
		if (!entry.attachedHost) return DetachedState();
		if (staging.destinationState.host
			&& *entry.attachedHost == *staging.destinationState.host) {
			return std::move(staging.destinationState);
		}
		if (staging.previousState && staging.previousState->host
			&& *entry.attachedHost == *staging.previousState->host) {
			return std::move(staging.previousState);
		}
		return std::nullopt;
	}

	[[nodiscard]] ViewContainerPagePoolAttachResult AttachRollback(
		const EViewContainerPagePoolAttachStatus failureStatus,
		const EViewContainerPageTransitionStage failedStage, Entry& entry,
		std::optional<ViewContainerPageHost>& rollbackHost,
		const std::optional<ViewContainerFocusToken>& focusToken,
		AttachStaging& staging) noexcept
	{
		const auto rollbackStage = Rollback(entry, rollbackHost, focusToken);
		if (rollbackStage == EViewContainerPageTransitionStage::None && staging.previousState) {
			entry.pendingFocus.reset();
		} else if (focusToken) {
			entry.pendingFocus = focusToken;
		}
		return { rollbackStage == EViewContainerPageTransitionStage::None
				? failureStatus : EViewContainerPagePoolAttachStatus::RollbackFailed,
			failedStage, rollbackStage, SelectAttachState(entry, staging),
			EViewContainerPageCleanupOwner::PagePool };
	}

	[[nodiscard]] static ViewContainerPagePoolDetachResult DetachEmpty(
		const EViewContainerPagePoolDetachStatus status) noexcept
	{
		return { status, EViewContainerPageTransitionStage::None,
			EViewContainerPageTransitionStage::None, std::nullopt,
			EViewContainerPageCleanupOwner::None };
	}

	[[nodiscard]] static ViewContainerPagePoolDetachResult DetachInternalFailure() noexcept
	{
		return { EViewContainerPagePoolDetachStatus::InternalFailure,
			EViewContainerPageTransitionStage::None,
			EViewContainerPageTransitionStage::None, std::nullopt,
			EViewContainerPageCleanupOwner::PagePool };
	}

	[[nodiscard]] static ViewContainerPagePoolDetachResult DetachWithState(
		const EViewContainerPagePoolDetachStatus status, ViewContainerPageState state,
		const EViewContainerPageCleanupOwner cleanupOwner) noexcept
	{
		return { status, EViewContainerPageTransitionStage::None,
			EViewContainerPageTransitionStage::None, std::move(state), cleanupOwner };
	}

	[[nodiscard]] static ViewContainerPagePoolDetachResult DetachFailure(
		const EViewContainerPagePoolDetachStatus status,
		const EViewContainerPageTransitionStage failedStage,
		ViewContainerPageState state) noexcept
	{
		return { status, failedStage, EViewContainerPageTransitionStage::None,
			std::move(state), EViewContainerPageCleanupOwner::PagePool };
	}

	[[nodiscard]] ViewContainerPagePoolDetachResult DetachRollback(
		const EViewContainerPagePoolDetachStatus failureStatus,
		const EViewContainerPageTransitionStage failedStage, Entry& entry,
		std::optional<ViewContainerPageHost>& rollbackHost,
		const std::optional<ViewContainerFocusToken>& focusToken,
		ViewContainerPageState previousState) noexcept
	{
		const auto rollbackStage = Rollback(entry, rollbackHost, focusToken);
		if (rollbackStage == EViewContainerPageTransitionStage::None) entry.pendingFocus.reset();
		else if (focusToken) entry.pendingFocus = focusToken;
		std::optional<ViewContainerPageState> finalState;
		if (entry.attachedHost) finalState.emplace(std::move(previousState));
		else finalState.emplace(DetachedState());
		return { rollbackStage == EViewContainerPageTransitionStage::None
				? failureStatus : EViewContainerPagePoolDetachStatus::RollbackFailed,
			failedStage, rollbackStage, std::move(finalState),
			EViewContainerPageCleanupOwner::PagePool };
	}

	const ViewContainerPageRegistry& m_registry;
	EntryMap m_entries;
	bool m_shutdown{};
	mutable bool m_failNextStateStagingForTest{};
};

ViewContainerPagePool::ViewContainerPagePool(const ViewContainerPageRegistry& registry)
	: m_impl(std::make_unique<Impl>(registry))
{
}

ViewContainerPagePool::~ViewContainerPagePool()
{
	if (m_impl) (void)m_impl->Shutdown();
}

ViewContainerPageAcquireResult ViewContainerPagePool::Acquire(
	const std::string_view containerId) noexcept
{
	return m_impl->Acquire(containerId);
}

ViewContainerPagePoolAttachResult ViewContainerPagePool::Attach(
	const std::string_view containerId, const ViewContainerPageHost& destination) noexcept
{
	return m_impl->Attach(containerId, destination);
}

ViewContainerPagePoolDetachResult ViewContainerPagePool::Detach(
	const std::string_view containerId) noexcept
{
	return m_impl->Detach(containerId);
}

ViewContainerPagePoolCloseResult ViewContainerPagePool::Close(
	const std::string_view containerId) noexcept
{
	return m_impl->Close(containerId);
}

ViewContainerPagePoolShutdownResult ViewContainerPagePool::Shutdown() noexcept
{
	return m_impl->Shutdown();
}

ViewContainerPageStateResult ViewContainerPagePool::State(
	const std::string_view containerId) const noexcept
{
	return m_impl->State(containerId);
}

std::size_t ViewContainerPagePool::Size() const noexcept
{
	return m_impl->Size();
}

void ViewContainerPagePool::FailNextStateStagingForTest() noexcept
{
	m_impl->FailNextStateStagingForTest();
}

} // namespace workbench::viewcontainer
