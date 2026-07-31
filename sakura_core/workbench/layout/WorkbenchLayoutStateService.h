/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#pragma once

#include "workbench/layout/WorkbenchLayoutStateTypes.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace workbench::layout {

struct WorkbenchContributionSnapshot;
struct WorkbenchLayoutSubscriptionState;
struct WorkbenchLayoutStateMutator;

struct SetWorkbenchPartVisibilityRequest {
	WorkbenchLayoutOperationMetadata operation;
	std::string partId;
	bool visible = true;
};

struct SetWorkbenchPartExtentRequest {
	WorkbenchLayoutOperationMetadata operation;
	std::string partId;
	std::optional<std::uint32_t> committedExtentDip;
};

struct RevealWorkbenchContainerRequest {
	WorkbenchLayoutOperationMetadata operation;
	std::string containerId;
};

//! Activation selects a deterministic active view for the container but never changes keyboard focus.
struct ActivateWorkbenchContainerRequest {
	WorkbenchLayoutOperationMetadata operation;
	std::string containerId;
};

//! Reveal never changes the active view or the focus target.
struct RevealWorkbenchViewRequest {
	WorkbenchLayoutOperationMetadata operation;
	std::string viewId;
};

struct SetWorkbenchViewVisibilityRequest {
	WorkbenchLayoutOperationMetadata operation;
	std::string viewId;
	bool visible = true;
};

//! Activation selects this view in its current container but never changes keyboard focus.
struct ActivateWorkbenchViewRequest {
	WorkbenchLayoutOperationMetadata operation;
	std::string viewId;
};

struct MoveWorkbenchContainerRequest {
	WorkbenchLayoutOperationMetadata operation;
	std::string containerId;
	EWorkbenchViewContainerLocation location = EWorkbenchViewContainerLocation::SideBar;
	std::int32_t order = 0;
};

struct MoveWorkbenchViewRequest {
	WorkbenchLayoutOperationMetadata operation;
	std::string viewId;
	std::string targetContainerId;
	std::int32_t order = 0;
};

struct SetWorkbenchFocusRequest {
	WorkbenchLayoutOperationMetadata operation;
	WorkbenchFocusState focus;
};

struct SetWorkbenchPanelAlignmentRequest {
	WorkbenchLayoutOperationMetadata operation;
	EWorkbenchPanelAlignment alignment = EWorkbenchPanelAlignment::Center;
};

struct ReconcileWorkbenchContributionsRequest {
	WorkbenchLayoutOperationMetadata operation;
};

/*!
	@brief Revisioned, pure layout state derived from stable workbench contribution IDs.

	The service never stores HWNDs, drag state, hover state, maximized state, CShareData,
	or persistence handles. A platform adapter serializes `MementoSnapshot()` through a separate
	memento boundary. Failed operations return the last stable snapshot and cannot mutate it.
*/
class WorkbenchLayoutStateService final {
public:
	explicit WorkbenchLayoutStateService(const WorkbenchContributionSnapshot& contributions,
		std::uint64_t generation = 1,
		std::size_t maxCompletedOperations = kMaxWorkbenchLayoutCompletedOperations);
	~WorkbenchLayoutStateService();
	WorkbenchLayoutStateService(const WorkbenchLayoutStateService&) = delete;
	WorkbenchLayoutStateService& operator=(const WorkbenchLayoutStateService&) = delete;

	[[nodiscard]] WorkbenchLayoutStateSnapshot Snapshot() const;
	//! Persistence projection: includes deferred focus/active intent without exposing it as live UI state.
	[[nodiscard]] WorkbenchLayoutStateSnapshot MementoSnapshot() const;
	/*!
		@brief Atomically applies one already-decoded layout memento before normal use.

		Hydration is intentionally one-shot: a successful call leaves the model at
		revision zero, refreshes the rollback snapshot, and never notifies observers.
		A later call returns `AlreadyHydrated`; callers must create a new runtime for
		a different profile/workspace memento rather than replacing live UI state.
	*/
	[[nodiscard]] WorkbenchLayoutHydrationResult HydrateInitialState(const WorkbenchLayoutStateSnapshot& persisted);
	[[nodiscard]] WorkbenchLayoutOperationResult SetPartVisibility(const SetWorkbenchPartVisibilityRequest& request);
	[[nodiscard]] WorkbenchLayoutOperationResult SetPartExtent(const SetWorkbenchPartExtentRequest& request);
	[[nodiscard]] WorkbenchLayoutOperationResult RevealContainer(const RevealWorkbenchContainerRequest& request);
	[[nodiscard]] WorkbenchLayoutOperationResult ActivateContainer(const ActivateWorkbenchContainerRequest& request);
	[[nodiscard]] WorkbenchLayoutOperationResult RevealView(const RevealWorkbenchViewRequest& request);
	[[nodiscard]] WorkbenchLayoutOperationResult SetViewVisibility(const SetWorkbenchViewVisibilityRequest& request);
	[[nodiscard]] WorkbenchLayoutOperationResult ActivateView(const ActivateWorkbenchViewRequest& request);
	[[nodiscard]] WorkbenchLayoutOperationResult MoveContainer(const MoveWorkbenchContainerRequest& request);
	[[nodiscard]] WorkbenchLayoutOperationResult MoveView(const MoveWorkbenchViewRequest& request);
	[[nodiscard]] WorkbenchLayoutOperationResult SetFocus(const SetWorkbenchFocusRequest& request);
	[[nodiscard]] WorkbenchLayoutOperationResult SetPanelAlignment(const SetWorkbenchPanelAlignmentRequest& request);
	[[nodiscard]] WorkbenchLayoutOperationResult Reconcile(const WorkbenchContributionSnapshot& contributions,
		const ReconcileWorkbenchContributionsRequest& request);
	[[nodiscard]] std::unique_ptr<IWorkbenchLayoutSubscription> Subscribe(WorkbenchLayoutChangeCallback callback);

private:
	friend struct WorkbenchLayoutStateMutator;
	struct ContributionIndex;
	struct CompletedOperation;

	[[nodiscard]] static std::unique_ptr<ContributionIndex> MakeContributionIndex(const WorkbenchContributionSnapshot& contributions);
	[[nodiscard]] static bool IsValidPersistedSnapshot(const WorkbenchLayoutStateSnapshot& persisted) noexcept;
	[[nodiscard]] WorkbenchLayoutStateSnapshot SnapshotLocked() const;
	[[nodiscard]] WorkbenchLayoutOperationResult ResultLocked(EWorkbenchLayoutOperationStatus status,
		EWorkbenchLayoutOperationReason reason) const;
	[[nodiscard]] WorkbenchLayoutOperationResult CheckOperationLocked(const WorkbenchLayoutOperationMetadata& operation,
		const std::string& fingerprint, bool& handled) const;
	[[nodiscard]] bool IsExpectedRevisionCurrentLocked(const WorkbenchLayoutOperationMetadata& operation) const noexcept;
	[[nodiscard]] WorkbenchLayoutOperationResult CommitLocked(std::vector<WorkbenchLayoutChange> changes);
	void RestoreLastStableLocked() noexcept;
	void RememberCompletedLocked(const std::string& operationId, std::string fingerprint,
		const WorkbenchLayoutOperationResult& result);
	[[nodiscard]] bool EnqueueNotification(const WorkbenchLayoutChangeBatch& batch);
	void DrainNotifications();

	mutable std::mutex m_mutex;
	std::uint64_t m_generation = 1;
	std::uint64_t m_revision = 0;
	std::size_t m_maxCompletedOperations = kMaxWorkbenchLayoutCompletedOperations;
	std::unique_ptr<ContributionIndex> m_contributionIndex;
	std::map<std::string, WorkbenchPartState> m_parts;
	std::map<std::string, WorkbenchViewContainerState> m_containers;
	std::map<std::string, WorkbenchViewState> m_views;
	//! Structurally valid memento entries for contributions not registered yet.
	//! They are intentionally separate from live state so regular APIs continue
	//! to reject unknown IDs and disposed entries can never be resurrected.
	std::map<std::string, WorkbenchPartState> m_deferredParts;
	std::map<std::string, WorkbenchViewContainerState> m_deferredContainers;
	std::map<std::string, WorkbenchViewState> m_deferredViews;
	//! A registered View may be persisted inside a container whose contribution
	//! is not registered yet. Keep rendering it at its live/default placement
	//! while preserving the unknown target exclusively for the memento.
	std::map<std::string, WorkbenchViewState> m_deferredViewPlacements;
	//! A persisted active/focus target may be valid but unavailable until an
	//! extension registers it.  It must not become live focus prematurely.
	std::map<std::string, std::string> m_deferredActiveViews;
	std::optional<WorkbenchFocusState> m_deferredFocus;
	bool m_initialHydrationCompleted = false;
	//! Deterministic per-container fallback index: (order, stable view ID).
	std::map<std::string, std::set<std::pair<std::int32_t, std::string>>> m_viewsByContainer;
	//! One live active ViewContainer per physical location. Unknown persisted
	//! targets stay in the separate deferred state until their contribution exists.
	WorkbenchActiveContainerState m_activeContainers;
	WorkbenchActiveContainerState m_deferredActiveContainers;
	EWorkbenchPanelAlignment m_panelAlignment = EWorkbenchPanelAlignment::Center;
	WorkbenchFocusState m_focus;
	//! Rebuilt only after a committed snapshot; used exclusively for exceptional rollback, never as a normal mutation copy.
	WorkbenchLayoutStateSnapshot m_lastStableSnapshot;
	std::optional<WorkbenchFocusState> m_lastStableDeferredFocus;
	std::map<std::string, std::string> m_lastStableDeferredActiveViews;
	std::map<std::string, WorkbenchViewState> m_lastStableDeferredViewPlacements;
	WorkbenchActiveContainerState m_lastStableDeferredActiveContainers;
	std::map<std::string, CompletedOperation> m_completedOperations;
	std::deque<std::string> m_completedOperationOrder;

	std::shared_ptr<WorkbenchLayoutSubscriptionState> m_subscriptionState;
	std::mutex m_notificationMutex;
	std::deque<WorkbenchLayoutChangeBatch> m_notificationQueue;
	bool m_dispatchingNotifications = false;
};

} // namespace workbench::layout
