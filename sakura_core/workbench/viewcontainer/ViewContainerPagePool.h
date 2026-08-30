/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/viewcontainer/ViewContainerPageRegistry.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace workbench::viewcontainer {

enum class EViewContainerPageStableState : std::uint8_t {
	Detached,
	Attached,
	Closed,
};

struct ViewContainerPageState final {
	EViewContainerPageStableState state{ EViewContainerPageStableState::Closed };
	std::optional<ViewContainerPageHost> host;
};

enum class EViewContainerPageCleanupOwner : std::uint8_t {
	None,
	PagePool,
	PageDestructor,
};

enum class EViewContainerPageAcquireStatus : std::uint8_t {
	Created,
	Existing,
	UnknownContainer,
	PageClosed,
	PoolClosed,
	FactoryFailed,
	InvalidPage,
	Failed,
};

struct ViewContainerPageAcquireResult final {
	EViewContainerPageAcquireStatus status{ EViewContainerPageAcquireStatus::Failed };
	IViewContainerPage* page{};
	EViewContainerPageCleanupOwner cleanupOwner{ EViewContainerPageCleanupOwner::None };

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EViewContainerPageAcquireStatus::Created
			|| status == EViewContainerPageAcquireStatus::Existing;
	}
};

enum class EViewContainerPageTransitionStage : std::uint8_t {
	None,
	CaptureFocus,
	Detach,
	Reparent,
	Attach,
	RestoreFocus,
};

enum class EViewContainerPagePoolAttachStatus : std::uint8_t {
	Attached,
	AlreadyAttached,
	UnknownContainer,
	InvalidHost,
	DestinationNotSupported,
	PageUnavailable,
	PageClosed,
	PoolClosed,
	FocusCaptureFailed,
	DetachFailed,
	ReparentFailed,
	AttachFailed,
	FocusRestoreFailed,
	RollbackFailed,
	InternalFailure,
};

struct ViewContainerPagePoolAttachResult final {
	EViewContainerPagePoolAttachStatus status{ EViewContainerPagePoolAttachStatus::PageUnavailable };
	EViewContainerPageTransitionStage failedStage{ EViewContainerPageTransitionStage::None };
	EViewContainerPageTransitionStage rollbackFailedStage{ EViewContainerPageTransitionStage::None };
	//! Empty only when an owning state copy could not be staged before native work.
	std::optional<ViewContainerPageState> finalState;
	EViewContainerPageCleanupOwner cleanupOwner{ EViewContainerPageCleanupOwner::None };

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EViewContainerPagePoolAttachStatus::Attached
			|| status == EViewContainerPagePoolAttachStatus::AlreadyAttached;
	}
};

enum class EViewContainerPagePoolDetachStatus : std::uint8_t {
	Detached,
	AlreadyDetached,
	UnknownContainer,
	NotAcquired,
	PageClosed,
	PoolClosed,
	FocusCaptureFailed,
	DetachFailed,
	ReparentFailed,
	RollbackFailed,
	InternalFailure,
};

struct ViewContainerPagePoolDetachResult final {
	EViewContainerPagePoolDetachStatus status{ EViewContainerPagePoolDetachStatus::NotAcquired };
	EViewContainerPageTransitionStage failedStage{ EViewContainerPageTransitionStage::None };
	EViewContainerPageTransitionStage rollbackFailedStage{ EViewContainerPageTransitionStage::None };
	//! Empty only when an owning state copy could not be staged before native work.
	std::optional<ViewContainerPageState> finalState;
	EViewContainerPageCleanupOwner cleanupOwner{ EViewContainerPageCleanupOwner::None };

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EViewContainerPagePoolDetachStatus::Detached
			|| status == EViewContainerPagePoolDetachStatus::AlreadyDetached;
	}
};

enum class EViewContainerPagePoolCloseStatus : std::uint8_t {
	Closed,
	AlreadyClosed,
	UnknownContainer,
	NotAcquired,
	CloseFailed,
};

struct ViewContainerPagePoolCloseResult final {
	EViewContainerPagePoolCloseStatus status{ EViewContainerPagePoolCloseStatus::NotAcquired };
	std::optional<ViewContainerPageState> finalState;
	EViewContainerPageCleanupOwner cleanupOwner{ EViewContainerPageCleanupOwner::None };
};

enum class EViewContainerPagePoolShutdownStatus : std::uint8_t {
	Closed,
	CloseFailed,
	AlreadyClosed,
};

struct ViewContainerPagePoolShutdownResult final {
	EViewContainerPagePoolShutdownStatus status{ EViewContainerPagePoolShutdownStatus::AlreadyClosed };
	std::size_t closedCount{};
	std::size_t failedCount{};
};

enum class EViewContainerPageStateStatus : std::uint8_t {
	Found,
	NotAcquired,
	InternalFailure,
};

struct ViewContainerPageStateResult final {
	EViewContainerPageStateStatus status{ EViewContainerPageStateStatus::NotAcquired };
	std::optional<ViewContainerPageState> state;
};

struct ViewContainerPagePoolTestPeer;

//! Indexed, single-owner pool. One container ID maps to one page lifetime and a
//! copied zero-or-one host value; the pool never retains host ownership. The
//! borrowed registry must outlive the pool. Every non-close terminal that owns a
//! page reports PagePool cleanup ownership; Close releases the page immediately.
class ViewContainerPagePool final {
public:
	explicit ViewContainerPagePool(const ViewContainerPageRegistry& registry);
	~ViewContainerPagePool();

	ViewContainerPagePool(const ViewContainerPagePool&) = delete;
	ViewContainerPagePool& operator=(const ViewContainerPagePool&) = delete;

	[[nodiscard]] ViewContainerPageAcquireResult Acquire(std::string_view containerId) noexcept;
	[[nodiscard]] ViewContainerPagePoolAttachResult Attach(
		std::string_view containerId, const ViewContainerPageHost& destination) noexcept;
	[[nodiscard]] ViewContainerPagePoolDetachResult Detach(std::string_view containerId) noexcept;
	[[nodiscard]] ViewContainerPagePoolCloseResult Close(std::string_view containerId) noexcept;
	[[nodiscard]] ViewContainerPagePoolShutdownResult Shutdown() noexcept;
	[[nodiscard]] ViewContainerPageStateResult State(
		std::string_view containerId) const noexcept;
	[[nodiscard]] std::size_t Size() const noexcept;

private:
	friend struct ViewContainerPagePoolTestPeer;
	//! Deterministic private seam; production callers cannot inject staging failure.
	void FailNextStateStagingForTest() noexcept;

	class Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace workbench::viewcontainer
