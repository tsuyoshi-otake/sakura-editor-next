/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/layout/WorkbenchContributionRegistry.h"

#include "workbench/layout/WorkbenchIds.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace workbench::layout {
namespace {

constexpr std::size_t kMaxStableIdBytes = 160;
constexpr std::size_t kMaxOperationIdBytes = 160;
constexpr std::size_t kMaxTitleBytes = 512;
constexpr std::size_t kMaxBatchContributions = 128;
constexpr std::size_t kMaxParts = 64;
constexpr std::size_t kMaxViewContainers = 256;
constexpr std::size_t kMaxViews = 1024;
constexpr std::size_t kMaxOperationReplays = 256;
constexpr std::size_t kMaxSubscriptions = 256;

bool IsPrintableUtf8(std::string_view value, const bool permitAsciiSpace, const std::size_t maximumBytes) noexcept
{
	if (value.empty() || value.size() > maximumBytes) return false;
	for (std::size_t index = 0; index < value.size();) {
		const auto first = static_cast<unsigned char>(value[index]);
		if (first < 0x80) {
			if (first < (permitAsciiSpace ? 0x20 : 0x21) || first == 0x7f) return false;
			++index;
			continue;
		}

		std::size_t continuationCount{};
		std::uint32_t codePoint{};
		if (first >= 0xc2 && first <= 0xdf) {
			continuationCount = 1;
			codePoint = first & 0x1f;
		} else if (first >= 0xe0 && first <= 0xef) {
			continuationCount = 2;
			codePoint = first & 0x0f;
		} else if (first >= 0xf0 && first <= 0xf4) {
			continuationCount = 3;
			codePoint = first & 0x07;
		} else {
			return false;
		}
		if (index + continuationCount >= value.size()) return false;
		for (std::size_t continuation = 1; continuation <= continuationCount; ++continuation) {
			const auto next = static_cast<unsigned char>(value[index + continuation]);
			if ((next & 0xc0) != 0x80) return false;
			codePoint = (codePoint << 6) | (next & 0x3f);
		}
		const auto minimum = continuationCount == 1 ? 0x80U : continuationCount == 2 ? 0x800U : 0x10000U;
		if (codePoint < minimum || codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff)
			|| (codePoint >= 0x80 && codePoint <= 0x9f)) return false;
		index += continuationCount + 1;
	}
	return true;
}

bool IsValidTitle(const std::string_view value) noexcept
{
	return IsPrintableUtf8(value, true, kMaxTitleBytes);
}

bool IsSameOwner(const WorkbenchContributionOwner& left, const WorkbenchContributionOwner& right) noexcept
{
	return left.generation == right.generation && left.ownerId == right.ownerId;
}

void AppendToken(std::string& target, const std::string_view value)
{
	target.append(std::to_string(value.size()));
	target.push_back(':');
	target.append(value);
	target.push_back(';');
}

void AppendUnsigned(std::string& target, const std::uint64_t value)
{
	AppendToken(target, std::to_string(value));
}

void AppendOptionalRevision(std::string& target, const std::optional<std::uint64_t>& value)
{
	if (value) {
		AppendToken(target, std::to_string(*value));
	} else {
		AppendToken(target, "-");
	}
}

template <typename T, typename MakeFingerprint>
void AppendSorted(std::string& target, const std::vector<T>& values, MakeFingerprint makeFingerprint)
{
	std::vector<std::string> sorted;
	sorted.reserve(values.size());
	for (const auto& value : values) sorted.push_back(makeFingerprint(value));
	std::sort(sorted.begin(), sorted.end());
	AppendUnsigned(target, sorted.size());
	for (const auto& value : sorted) AppendToken(target, value);
}

std::string Fingerprint(const RegisterWorkbenchContributionsRequest& request)
{
	std::string fingerprint{ "register;" };
	AppendToken(fingerprint, request.operation.operationId);
	AppendOptionalRevision(fingerprint, request.operation.expectedRevision);
	AppendToken(fingerprint, request.owner.ownerId);
	AppendUnsigned(fingerprint, request.owner.generation);
	AppendSorted(fingerprint, request.parts, [](const WorkbenchPartDescriptor& descriptor) {
		std::string value;
		AppendToken(value, descriptor.id);
		AppendToken(value, descriptor.title);
		AppendToken(value, descriptor.supportsVisibility ? "1" : "0");
		return value;
	});
	AppendSorted(fingerprint, request.viewContainers, [](const WorkbenchViewContainerDescriptor& descriptor) {
		std::string value;
		AppendToken(value, descriptor.id);
		AppendToken(value, descriptor.title);
		AppendUnsigned(value, static_cast<std::uint64_t>(descriptor.location));
		AppendToken(value, std::to_string(descriptor.order));
		AppendToken(value, descriptor.hideIfEmpty ? "1" : "0");
		AppendToken(value, descriptor.canMove ? "1" : "0");
		return value;
	});
	AppendSorted(fingerprint, request.views, [](const WorkbenchViewDescriptor& descriptor) {
		std::string value;
		AppendToken(value, descriptor.id);
		AppendToken(value, descriptor.containerId);
		AppendToken(value, descriptor.title);
		AppendToken(value, std::to_string(descriptor.order));
		AppendToken(value, descriptor.canToggleVisibility ? "1" : "0");
		AppendToken(value, descriptor.canMove ? "1" : "0");
		return value;
	});
	return fingerprint;
}

std::string Fingerprint(const DisposeWorkbenchContributionsRequest& request)
{
	std::string fingerprint{ "dispose;" };
	AppendToken(fingerprint, request.operation.operationId);
	AppendOptionalRevision(fingerprint, request.operation.expectedRevision);
	AppendToken(fingerprint, request.owner.ownerId);
	AppendUnsigned(fingerprint, request.owner.generation);
	return fingerprint;
}

template <typename Descriptor>
bool HasDuplicateIds(const std::vector<Descriptor>& descriptors)
{
	std::vector<std::string_view> ids;
	ids.reserve(descriptors.size());
	for (const auto& descriptor : descriptors) ids.push_back(descriptor.id);
	std::sort(ids.begin(), ids.end());
	return std::adjacent_find(ids.begin(), ids.end()) != ids.end();
}

} // namespace

struct WorkbenchContributionRegistry::Impl final {
	using Parts = std::map<std::string, RegisteredWorkbenchPart, std::less<>>;
	using ViewContainers = std::map<std::string, RegisteredWorkbenchViewContainer, std::less<>>;
	using Views = std::map<std::string, RegisteredWorkbenchView, std::less<>>;

	struct OperationReplay final {
		std::string fingerprint;
		WorkbenchContributionOperationResult result;
	};
	struct Subscription final {
		WorkbenchContributionOwner owner;
		WorkbenchContributionListener listener;
	};
	struct PendingNotification final {
		WorkbenchContributionChange change;
		std::vector<WorkbenchContributionSubscriptionId> subscriberIds;
	};

	mutable std::mutex mutex;
	Parts parts;
	ViewContainers viewContainers;
	Views views;
	std::map<std::string, std::uint64_t, std::less<>> activeOwnerGenerations;
	std::unordered_map<std::string, OperationReplay> operationReplays;
	std::deque<std::string> operationReplayOrder;
	std::map<WorkbenchContributionSubscriptionId, Subscription> subscriptions;
	std::deque<PendingNotification> pendingNotifications;
	std::uint64_t revision{ 1 };
	WorkbenchContributionSubscriptionId nextSubscriptionId{ 1 };
	bool drainingNotifications{};

	void RememberOperationLocked(std::string operationId, std::string fingerprint, const WorkbenchContributionOperationResult& result)
	{
		if (operationReplays.size() == kMaxOperationReplays) {
			operationReplays.erase(operationReplayOrder.front());
			operationReplayOrder.pop_front();
		}
		operationReplayOrder.push_back(operationId);
		operationReplays.emplace(std::move(operationId), OperationReplay{ .fingerprint = std::move(fingerprint), .result = result });
	}

	[[nodiscard]] bool QueueNotificationLocked(const WorkbenchContributionChange& change)
	{
		PendingNotification notification{ .change = change };
		notification.subscriberIds.reserve(subscriptions.size());
		for (const auto& [id, ignored] : subscriptions) notification.subscriberIds.push_back(id);
		pendingNotifications.push_back(std::move(notification));
		if (drainingNotifications) return false;
		drainingNotifications = true;
		return true;
	}

	void DrainNotifications()
	{
		for (;;) {
			PendingNotification notification;
			{
				std::lock_guard lock(mutex);
				if (pendingNotifications.empty()) {
					drainingNotifications = false;
					return;
				}
				notification = std::move(pendingNotifications.front());
				pendingNotifications.pop_front();
			}
			for (const auto id : notification.subscriberIds) {
				WorkbenchContributionListener listener;
				{
					std::lock_guard lock(mutex);
					const auto found = subscriptions.find(id);
					if (found != subscriptions.end()) listener = found->second.listener;
				}
				if (!listener) continue;
				try {
					listener(notification.change);
				} catch (...) {
					// Contributions are isolated: one extension listener must not stop ordered delivery.
				}
			}
		}
	}
};

bool WorkbenchContributionOwner::IsValid() const noexcept
{
	return WorkbenchContributionRegistry::IsValidStableId(ownerId);
}

WorkbenchContributionRegistry::WorkbenchContributionRegistry()
	: m_impl(new Impl)
{
	const WorkbenchContributionOwner owner{ .ownerId = std::string(ids::BuiltinOwner), .generation = 0 };
	m_impl->activeOwnerGenerations.emplace(owner.ownerId, owner.generation);
	const auto addPart = [this, &owner](std::string_view id, std::string_view title, const bool supportsVisibility = true) {
		m_impl->parts.emplace(std::string(id), RegisteredWorkbenchPart{
			.descriptor = { .id = std::string(id), .title = std::string(title), .supportsVisibility = supportsVisibility },
			.owner = owner,
			.isBuiltin = true,
		});
	};
	const auto addContainer = [this, &owner](std::string_view id, std::string_view title, const EViewContainerLocation location, const std::int32_t order) {
		m_impl->viewContainers.emplace(std::string(id), RegisteredWorkbenchViewContainer{
			.descriptor = { .id = std::string(id), .title = std::string(title), .location = location, .order = order },
			.owner = owner,
			.isBuiltin = true,
		});
	};
	const auto addView = [this, &owner](std::string_view id, std::string_view containerId, std::string_view title, const std::int32_t order) {
		m_impl->views.emplace(std::string(id), RegisteredWorkbenchView{
			.descriptor = { .id = std::string(id), .containerId = std::string(containerId), .title = std::string(title), .order = order },
			.owner = owner,
			.isBuiltin = true,
		});
	};

	addPart(ids::part::Titlebar, "Title Bar");
	addPart(ids::part::Banner, "Banner");
	addPart(ids::part::Activitybar, "Activity Bar");
	addPart(ids::part::Sidebar, "Side Bar");
	addPart(ids::part::Panel, "Panel");
	addPart(ids::part::Auxiliarybar, "Auxiliary Bar");
	addPart(ids::part::Editor, "Editor", false);
	addPart(ids::part::Statusbar, "Status Bar");
	addPart(ids::part::Sessions, "Sessions");

	addContainer(ids::viewContainer::Explorer, "Explorer", EViewContainerLocation::Sidebar, 10);
	addContainer(ids::viewContainer::Search, "Search", EViewContainerLocation::Sidebar, 20);
	addContainer(ids::viewContainer::RunAndDebug, "Run and Debug", EViewContainerLocation::Sidebar, 30);
	addContainer(ids::viewContainer::SourceControl, "Source Control", EViewContainerLocation::Sidebar, 40);
	addContainer(ids::viewContainer::Extensions, "Extensions", EViewContainerLocation::Sidebar, 50);
	addContainer(ids::viewContainer::Problems, "Problems", EViewContainerLocation::Panel, 10);
	addContainer(ids::viewContainer::Output, "Output", EViewContainerLocation::Panel, 20);
	addContainer(ids::viewContainer::Terminal, "Terminal", EViewContainerLocation::Panel, 30);
	addContainer(ids::viewContainer::Ports, "Ports", EViewContainerLocation::Panel, 40);
	addContainer(ids::viewContainer::DebugConsole, "Debug Console", EViewContainerLocation::Panel, 50);
	addContainer(ids::viewContainer::LegacyExtensionViewsAuxiliary, "Extension Views", EViewContainerLocation::AuxiliaryBar, 10);

	addView(ids::view::Explorer, ids::viewContainer::Explorer, "Explorer", 10);
	addView(ids::view::Outline, ids::viewContainer::Explorer, "Outline", 20);
	addView(ids::view::Search, ids::viewContainer::Search, "Search", 10);
	addView(ids::view::DebugVariables, ids::viewContainer::RunAndDebug, "Variables", 10);
	addView(ids::view::DebugWatch, ids::viewContainer::RunAndDebug, "Watch", 20);
	addView(ids::view::DebugCallStack, ids::viewContainer::RunAndDebug, "Call Stack", 30);
	addView(ids::view::DebugLoadedScripts, ids::viewContainer::RunAndDebug, "Loaded Scripts", 40);
	addView(ids::view::DebugBreakpoints, ids::viewContainer::RunAndDebug, "Breakpoints", 50);
	addView(ids::view::SourceControl, ids::viewContainer::SourceControl, "Source Control", 10);
	addView(ids::view::Extensions, ids::viewContainer::Extensions, "Extensions", 10);
	addView(ids::view::Problems, ids::viewContainer::Problems, "Problems", 10);
	addView(ids::view::Output, ids::viewContainer::Output, "Output", 10);
	addView(ids::view::Terminal, ids::viewContainer::Terminal, "Terminal", 10);
	addView(ids::view::Ports, ids::viewContainer::Ports, "Ports", 10);
	addView(ids::view::DebugConsole, ids::viewContainer::DebugConsole, "Debug Console", 10);
	addView(ids::view::LegacyExtensionViews, ids::viewContainer::LegacyExtensionViewsAuxiliary, "Extension Views", 10);
}

WorkbenchContributionRegistry::~WorkbenchContributionRegistry()
{
	delete m_impl;
}

bool WorkbenchContributionRegistry::IsValidStableId(const std::string_view value) noexcept
{
	return IsPrintableUtf8(value, false, kMaxStableIdBytes);
}

bool WorkbenchContributionRegistry::IsValidOperationId(const std::string_view value) noexcept
{
	return IsPrintableUtf8(value, false, kMaxOperationIdBytes);
}

WorkbenchContributionOperationResult WorkbenchContributionRegistry::Register(const RegisterWorkbenchContributionsRequest& request)
{
	bool drain{};
	WorkbenchContributionOperationResult result;
	{
		std::lock_guard lock(m_impl->mutex);
		if (!IsValidOperationId(request.operation.operationId)) {
			return { .status = EWorkbenchContributionOperationStatus::Rejected, .reason = EWorkbenchContributionOperationReason::InvalidOperationId, .revision = m_impl->revision };
		}
		if (!request.owner.IsValid()) return { .status = EWorkbenchContributionOperationStatus::Rejected, .reason = EWorkbenchContributionOperationReason::InvalidOwner, .revision = m_impl->revision };
		if (request.parts.empty() && request.viewContainers.empty() && request.views.empty()) return { .status = EWorkbenchContributionOperationStatus::Rejected, .reason = EWorkbenchContributionOperationReason::EmptyBatch, .revision = m_impl->revision };
		const auto batchSize = request.parts.size() + request.viewContainers.size() + request.views.size();
		if (batchSize > kMaxBatchContributions) return { .status = EWorkbenchContributionOperationStatus::Rejected, .reason = EWorkbenchContributionOperationReason::BatchLimitExceeded, .revision = m_impl->revision };
		for (const auto& descriptor : request.parts) {
			if (!IsValidStableId(descriptor.id) || !IsValidTitle(descriptor.title)) return { .status = EWorkbenchContributionOperationStatus::Rejected, .reason = EWorkbenchContributionOperationReason::InvalidDescriptor, .revision = m_impl->revision };
		}
		for (const auto& descriptor : request.viewContainers) {
			if (!IsValidStableId(descriptor.id) || !IsValidTitle(descriptor.title)) return { .status = EWorkbenchContributionOperationStatus::Rejected, .reason = EWorkbenchContributionOperationReason::InvalidDescriptor, .revision = m_impl->revision };
		}
		for (const auto& descriptor : request.views) {
			if (!IsValidStableId(descriptor.id) || !IsValidStableId(descriptor.containerId) || !IsValidTitle(descriptor.title)) return { .status = EWorkbenchContributionOperationStatus::Rejected, .reason = EWorkbenchContributionOperationReason::InvalidDescriptor, .revision = m_impl->revision };
		}
		const auto fingerprint = Fingerprint(request);
		const auto replay = m_impl->operationReplays.find(request.operation.operationId);
		if (replay != m_impl->operationReplays.end()) {
			if (replay->second.fingerprint == fingerprint) {
				result = replay->second.result;
				result.status = EWorkbenchContributionOperationStatus::Replayed;
				return result;
			}
			return { .status = EWorkbenchContributionOperationStatus::Conflict, .reason = EWorkbenchContributionOperationReason::OperationIdConflict, .revision = m_impl->revision };
		}
		const auto reject = [this, &request, &fingerprint](const EWorkbenchContributionOperationReason reason) {
			const WorkbenchContributionOperationResult rejected{ .status = EWorkbenchContributionOperationStatus::Rejected, .reason = reason, .revision = m_impl->revision };
			m_impl->RememberOperationLocked(request.operation.operationId, fingerprint, rejected);
			return rejected;
		};
		if (request.parts.size() + m_impl->parts.size() > kMaxParts || request.viewContainers.size() + m_impl->viewContainers.size() > kMaxViewContainers || request.views.size() + m_impl->views.size() > kMaxViews) return reject(EWorkbenchContributionOperationReason::RegistryLimitExceeded);
		if (request.operation.expectedRevision && *request.operation.expectedRevision != m_impl->revision) {
			const WorkbenchContributionOperationResult stale{ .status = EWorkbenchContributionOperationStatus::StaleRevision, .revision = m_impl->revision };
			m_impl->RememberOperationLocked(request.operation.operationId, fingerprint, stale);
			return stale;
		}
		if (m_impl->revision == std::numeric_limits<std::uint64_t>::max()) {
			const WorkbenchContributionOperationResult exhausted{ .status = EWorkbenchContributionOperationStatus::RevisionExhausted, .revision = m_impl->revision };
			m_impl->RememberOperationLocked(request.operation.operationId, fingerprint, exhausted);
			return exhausted;
		}
		const auto ownerGeneration = m_impl->activeOwnerGenerations.find(request.owner.ownerId);
		if (ownerGeneration != m_impl->activeOwnerGenerations.end() && ownerGeneration->second != request.owner.generation) return reject(EWorkbenchContributionOperationReason::OwnerGenerationConflict);
		if (HasDuplicateIds(request.parts)) return reject(EWorkbenchContributionOperationReason::DuplicatePartId);
		if (HasDuplicateIds(request.viewContainers)) return reject(EWorkbenchContributionOperationReason::DuplicateViewContainerId);
		if (HasDuplicateIds(request.views)) return reject(EWorkbenchContributionOperationReason::DuplicateViewId);
		for (const auto& descriptor : request.parts) {
			if (m_impl->parts.contains(descriptor.id)) return reject(EWorkbenchContributionOperationReason::DuplicatePartId);
		}
		for (const auto& descriptor : request.viewContainers) {
			if (m_impl->viewContainers.contains(descriptor.id)) return reject(EWorkbenchContributionOperationReason::DuplicateViewContainerId);
		}
		for (const auto& descriptor : request.views) {
			if (m_impl->views.contains(descriptor.id)) return reject(EWorkbenchContributionOperationReason::DuplicateViewId);
			const auto inBatch = std::any_of(request.viewContainers.begin(), request.viewContainers.end(), [&descriptor](const auto& container) { return container.id == descriptor.containerId; });
			if (!inBatch && !m_impl->viewContainers.contains(descriptor.containerId)) return reject(EWorkbenchContributionOperationReason::UnknownViewContainer);
		}

		// Build a complete staged graph before changing the published maps. This keeps a failed
		// allocation from turning an otherwise rejected extension contribution into a partial commit.
		auto stagedParts = m_impl->parts;
		auto stagedViewContainers = m_impl->viewContainers;
		auto stagedViews = m_impl->views;
		auto stagedOwnerGenerations = m_impl->activeOwnerGenerations;
		for (const auto& descriptor : request.parts) stagedParts.emplace(descriptor.id, RegisteredWorkbenchPart{ .descriptor = descriptor, .owner = request.owner });
		for (const auto& descriptor : request.viewContainers) stagedViewContainers.emplace(descriptor.id, RegisteredWorkbenchViewContainer{ .descriptor = descriptor, .owner = request.owner });
		for (const auto& descriptor : request.views) stagedViews.emplace(descriptor.id, RegisteredWorkbenchView{ .descriptor = descriptor, .owner = request.owner });
		stagedOwnerGenerations.emplace(request.owner.ownerId, request.owner.generation);
		m_impl->parts.swap(stagedParts);
		m_impl->viewContainers.swap(stagedViewContainers);
		m_impl->views.swap(stagedViews);
		m_impl->activeOwnerGenerations.swap(stagedOwnerGenerations);
		++m_impl->revision;
		result = { .status = EWorkbenchContributionOperationStatus::Succeeded, .revision = m_impl->revision };
		m_impl->RememberOperationLocked(request.operation.operationId, fingerprint, result);
		drain = m_impl->QueueNotificationLocked({ .revision = m_impl->revision, .kind = EWorkbenchContributionChangeKind::Registered, .owner = request.owner });
	}
	if (drain) m_impl->DrainNotifications();
	return result;
}

WorkbenchContributionOperationResult WorkbenchContributionRegistry::DisposeOwner(const DisposeWorkbenchContributionsRequest& request)
{
	bool drain{};
	WorkbenchContributionOperationResult result;
	{
		std::lock_guard lock(m_impl->mutex);
		if (!IsValidOperationId(request.operation.operationId)) {
			return { .status = EWorkbenchContributionOperationStatus::Rejected, .reason = EWorkbenchContributionOperationReason::InvalidOperationId, .revision = m_impl->revision };
		}
		if (!request.owner.IsValid()) return { .status = EWorkbenchContributionOperationStatus::Rejected, .reason = EWorkbenchContributionOperationReason::InvalidOwner, .revision = m_impl->revision };
		const auto fingerprint = Fingerprint(request);
		const auto replay = m_impl->operationReplays.find(request.operation.operationId);
		if (replay != m_impl->operationReplays.end()) {
			if (replay->second.fingerprint == fingerprint) {
				result = replay->second.result;
				result.status = EWorkbenchContributionOperationStatus::Replayed;
				return result;
			}
			return { .status = EWorkbenchContributionOperationStatus::Conflict, .reason = EWorkbenchContributionOperationReason::OperationIdConflict, .revision = m_impl->revision };
		}
		const auto remember = [this, &request, &fingerprint](const WorkbenchContributionOperationResult& value) {
			m_impl->RememberOperationLocked(request.operation.operationId, fingerprint, value);
			return value;
		};
		if (request.owner.ownerId == ids::BuiltinOwner) return remember({ .status = EWorkbenchContributionOperationStatus::Rejected, .reason = EWorkbenchContributionOperationReason::BuiltinProtected, .revision = m_impl->revision });
		if (request.operation.expectedRevision && *request.operation.expectedRevision != m_impl->revision) return remember({ .status = EWorkbenchContributionOperationStatus::StaleRevision, .revision = m_impl->revision });
		if (m_impl->revision == std::numeric_limits<std::uint64_t>::max()) return remember({ .status = EWorkbenchContributionOperationStatus::RevisionExhausted, .revision = m_impl->revision });
		const auto ownerGeneration = m_impl->activeOwnerGenerations.find(request.owner.ownerId);
		if (ownerGeneration == m_impl->activeOwnerGenerations.end()) return remember({ .status = EWorkbenchContributionOperationStatus::NotApplicable, .revision = m_impl->revision });
		if (ownerGeneration->second != request.owner.generation) return remember({ .status = EWorkbenchContributionOperationStatus::Conflict, .reason = EWorkbenchContributionOperationReason::OwnerGenerationConflict, .revision = m_impl->revision });
		const auto ownedBy = [&request](const auto& pair) { return IsSameOwner(pair.second.owner, request.owner); };
		std::erase_if(m_impl->parts, ownedBy);
		std::erase_if(m_impl->viewContainers, ownedBy);
		std::erase_if(m_impl->views, ownedBy);
		std::erase_if(m_impl->subscriptions, [&request](const auto& pair) { return IsSameOwner(pair.second.owner, request.owner); });
		m_impl->activeOwnerGenerations.erase(ownerGeneration);
		++m_impl->revision;
		result = { .status = EWorkbenchContributionOperationStatus::Succeeded, .revision = m_impl->revision };
		m_impl->RememberOperationLocked(request.operation.operationId, fingerprint, result);
		drain = m_impl->QueueNotificationLocked({ .revision = m_impl->revision, .kind = EWorkbenchContributionChangeKind::OwnerDisposed, .owner = request.owner });
	}
	if (drain) m_impl->DrainNotifications();
	return result;
}

WorkbenchContributionSnapshot WorkbenchContributionRegistry::Snapshot() const
{
	std::lock_guard lock(m_impl->mutex);
	WorkbenchContributionSnapshot snapshot{ .revision = m_impl->revision };
	snapshot.parts.reserve(m_impl->parts.size());
	snapshot.viewContainers.reserve(m_impl->viewContainers.size());
	snapshot.views.reserve(m_impl->views.size());
	for (const auto& [id, value] : m_impl->parts) snapshot.parts.push_back(value);
	for (const auto& [id, value] : m_impl->viewContainers) snapshot.viewContainers.push_back(value);
	for (const auto& [id, value] : m_impl->views) snapshot.views.push_back(value);
	return snapshot;
}

std::optional<WorkbenchContributionSubscriptionId> WorkbenchContributionRegistry::Subscribe(const WorkbenchContributionOwner& subscriber, WorkbenchContributionListener listener)
{
	if (!subscriber.IsValid() || !listener) return std::nullopt;
	std::lock_guard lock(m_impl->mutex);
	if (m_impl->subscriptions.size() == kMaxSubscriptions || m_impl->nextSubscriptionId == 0) return std::nullopt;
	const auto activeGeneration = m_impl->activeOwnerGenerations.find(subscriber.ownerId);
	if (activeGeneration != m_impl->activeOwnerGenerations.end() && activeGeneration->second != subscriber.generation) return std::nullopt;
	const auto id = m_impl->nextSubscriptionId++;
	m_impl->subscriptions.emplace(id, Impl::Subscription{ .owner = subscriber, .listener = std::move(listener) });
	m_impl->activeOwnerGenerations.emplace(subscriber.ownerId, subscriber.generation);
	return id;
}

void WorkbenchContributionRegistry::Unsubscribe(const WorkbenchContributionSubscriptionId subscriptionId) noexcept
{
	std::lock_guard lock(m_impl->mutex);
	m_impl->subscriptions.erase(subscriptionId);
}

} // namespace workbench::layout
