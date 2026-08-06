/*! @file
 * @brief Thread-safe SourceControl/SourceControlResourceGroup authority.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "workbench/scm/SourceControlService.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace workbench::scm {
namespace {

constexpr std::size_t kMaximumStableIdBytes = 160;
constexpr std::size_t kMaximumLabelBytes = 512;
constexpr std::size_t kMaximumInputValueBytes = 1U << 20;
constexpr std::size_t kMaximumCommandArgumentsBytes = 256U << 10;
constexpr std::size_t kMaximumTooltipBytes = 4096;
constexpr std::size_t kMaximumContextValueBytes = 512;
constexpr std::size_t kMaximumIconPathBytes = 4096;

bool IsValidUtf8(const std::string_view value, const bool permitControls) noexcept
{
	for (std::size_t index = 0; index < value.size();) {
		const auto first = static_cast<unsigned char>(value[index]);
		if (first < 0x80) {
			if ((!permitControls && (first < 0x20 || first == 0x7f)) || first == 0) return false;
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
		if (codePoint < minimum || codePoint > 0x10ffff || (codePoint >= 0xd800 && codePoint <= 0xdfff)) return false;
		if (!permitControls && (codePoint >= 0x80 && codePoint <= 0x9f)) return false;
		index += continuationCount + 1;
	}
	return true;
}

bool IsValidText(const std::string_view value, const std::size_t maximumBytes, const bool allowEmpty = true) noexcept
{
	return (allowEmpty || !value.empty()) && value.size() <= maximumBytes && IsValidUtf8(value, false);
}

bool IsValidStableIdValue(const std::string_view value) noexcept
{
	if (value.empty() || value.size() > kMaximumStableIdBytes || !IsValidUtf8(value, false)) return false;
	return std::none_of(value.begin(), value.end(), [](const unsigned char character) {
		return character <= 0x20 || character == 0x7f;
	});
}

bool IsValidInputBox(const ScmInputBoxState& input) noexcept
{
	return IsValidText(input.value, kMaximumInputValueBytes) &&
		IsValidText(input.placeholder, kMaximumLabelBytes);
}

bool IsValidCommand(const ScmCommand& command) noexcept
{
	return IsValidStableIdValue(command.command) &&
		IsValidText(command.title, kMaximumLabelBytes, false) &&
		IsValidText(command.tooltip, kMaximumTooltipBytes) &&
		IsValidText(command.argumentsJson, kMaximumCommandArgumentsBytes, false);
}

bool IsValidResource(const ScmResourceState& resource) noexcept
{
	if (!IsValidText(resource.tooltip.value_or({}), kMaximumTooltipBytes)) return false;
	if (!IsValidText(resource.lightIconPath.value_or({}), kMaximumIconPathBytes)) return false;
	if (!IsValidText(resource.darkIconPath.value_or({}), kMaximumIconPathBytes)) return false;
	if (!IsValidText(resource.contextValue, kMaximumContextValueBytes)) return false;
	return !resource.command || IsValidCommand(*resource.command);
}

bool IsValidGroupMetadata(const ScmResourceGroupState& group) noexcept
{
	return IsValidStableIdValue(group.providerHandle) && IsValidStableIdValue(group.id) &&
		IsValidText(group.label, kMaximumLabelBytes, false) &&
		IsValidText(group.contextValue, kMaximumContextValueBytes);
}

bool IsValidProviderMetadata(const ScmProviderState& provider) noexcept
{
	if (!provider.owner.IsValid() || !IsValidStableIdValue(provider.handle) ||
		!IsValidStableIdValue(provider.id) || !IsValidText(provider.label, kMaximumLabelBytes, false) ||
		!IsValidInputBox(provider.inputBox)) {
		return false;
	}
	// `name` is optional exactly as upstream's `_name` is. When it is present it
	// is rendered as the repository row's label, so it obeys the label bounds.
	if (!IsValidText(provider.name, kMaximumLabelBytes)) return false;
	if (provider.commitTemplate && !IsValidText(*provider.commitTemplate, kMaximumInputValueBytes)) return false;
	if (provider.acceptInputCommand && !IsValidCommand(*provider.acceptInputCommand)) return false;
	if (!std::all_of(provider.statusBarCommands.begin(), provider.statusBarCommands.end(), IsValidCommand)) return false;
	return true;
}

std::size_t SaturatingAdd(const std::size_t left, const std::size_t right) noexcept
{
	return right > (std::numeric_limits<std::size_t>::max)() - left
		? (std::numeric_limits<std::size_t>::max)() : left + right;
}

std::size_t PayloadBytes(const ScmCommand& command) noexcept
{
	std::size_t total = SaturatingAdd(command.command.size(), command.title.size());
	total = SaturatingAdd(total, command.tooltip.size());
	return SaturatingAdd(total, command.argumentsJson.size());
}

std::size_t PayloadBytes(const ScmResourceState& resource) noexcept
{
	std::size_t total = resource.resourceUri.ToString().size() * sizeof(wchar_t);
	if (resource.command) total = SaturatingAdd(total, PayloadBytes(*resource.command));
	if (resource.tooltip) total = SaturatingAdd(total, resource.tooltip->size());
	if (resource.lightIconPath) total = SaturatingAdd(total, resource.lightIconPath->size());
	if (resource.darkIconPath) total = SaturatingAdd(total, resource.darkIconPath->size());
	return SaturatingAdd(total, resource.contextValue.size());
}

std::size_t PayloadBytes(const ScmResourceGroupState& group) noexcept
{
	std::size_t total = group.providerHandle.size();
	total = SaturatingAdd(total, group.id.size());
	total = SaturatingAdd(total, group.label.size());
	total = SaturatingAdd(total, group.contextValue.size());
	for (const auto& resource : group.resources) total = SaturatingAdd(total, PayloadBytes(resource));
	return total;
}

std::size_t PayloadBytes(const ScmProviderState& provider) noexcept
{
	std::size_t total = provider.handle.size();
	total = SaturatingAdd(total, provider.id.size());
	total = SaturatingAdd(total, provider.label.size());
	total = SaturatingAdd(total, provider.name.size());
	total = SaturatingAdd(total, provider.inputBox.value.size());
	total = SaturatingAdd(total, provider.inputBox.placeholder.size());
	if (provider.rootUri) total = SaturatingAdd(total, provider.rootUri->ToString().size() * sizeof(wchar_t));
	if (provider.commitTemplate) total = SaturatingAdd(total, provider.commitTemplate->size());
	if (provider.acceptInputCommand) total = SaturatingAdd(total, PayloadBytes(*provider.acceptInputCommand));
	for (const auto& command : provider.statusBarCommands) total = SaturatingAdd(total, PayloadBytes(command));
	for (const auto& group : provider.groups) total = SaturatingAdd(total, PayloadBytes(group));
	return total;
}

bool SameOwner(const ScmOwner& left, const ScmOwner& right) noexcept
{
	return left == right;
}

} // namespace

struct SourceControlService::Impl final {
	struct Provider final {
		ScmProviderState state;
		std::map<std::string, ScmResourceGroupState, std::less<>> groups;
	};

	mutable std::mutex mutex;
	SourceControlServiceLimits limits;
	std::map<std::string, Provider, std::less<>> providers;
	//! Retained after disposal so a late callback from an older generation cannot resurrect state.
	std::map<std::string, std::uint64_t, std::less<>> ownerGenerations;
	std::optional<ScmGlobalInputState> globalInput;
	std::map<ScmServiceSubscriptionId, ScmServiceListener> subscriptions;
	ScmServiceSubscriptionId nextSubscriptionId{ 1 };
	std::uint64_t revision{ 1 };
	bool stopped{};

	explicit Impl(SourceControlServiceLimits initialLimits)
		: limits(std::move(initialLimits))
	{
		if (limits.maximumOwners == 0) limits.maximumOwners = 1;
		if (limits.maximumProviders == 0) limits.maximumProviders = 1;
		if (limits.maximumGroupsPerProvider == 0) limits.maximumGroupsPerProvider = 1;
		if (limits.maximumResourcesPerGroup == 0) limits.maximumResourcesPerGroup = 1;
		if (limits.maximumPayloadBytes == 0) limits.maximumPayloadBytes = 1;
		if (limits.maximumSubscriptions == 0) limits.maximumSubscriptions = 1;
	}

	[[nodiscard]] ScmOperationResult Current(const EScmOperationStatus status) const noexcept
	{
		return { .status = status, .revision = revision };
	}

	[[nodiscard]] bool AdvanceRevisionLocked() noexcept
	{
		if (revision == (std::numeric_limits<std::uint64_t>::max)()) return false;
		++revision;
		return true;
	}

	[[nodiscard]] bool OwnerHasLiveStateLocked(const std::string_view extensionId) const noexcept
	{
		if (globalInput && globalInput->owner.extensionId == extensionId) return true;
		return std::any_of(providers.begin(), providers.end(), [extensionId](const auto& entry) {
			return entry.second.state.owner.extensionId == extensionId;
		});
	}

	[[nodiscard]] EScmOperationStatus PrepareOwnerLocked(const ScmOwner& owner) noexcept
	{
		if (!owner.IsValid()) return EScmOperationStatus::InvalidOwner;
		const auto found = ownerGenerations.find(owner.extensionId);
		if (found == ownerGenerations.end()) {
			if (ownerGenerations.size() >= limits.maximumOwners) return EScmOperationStatus::OwnerLimitExceeded;
			try {
				ownerGenerations.emplace(owner.extensionId, owner.generation);
			} catch (...) {
				return EScmOperationStatus::OwnerLimitExceeded;
			}
			return EScmOperationStatus::Succeeded;
		}
		if (found->second == owner.generation) return EScmOperationStatus::Succeeded;
		if (owner.generation < found->second || OwnerHasLiveStateLocked(owner.extensionId)) {
			return EScmOperationStatus::OwnerGenerationConflict;
		}
		found->second = owner.generation;
		return EScmOperationStatus::Succeeded;
	}

	[[nodiscard]] EScmOperationStatus ValidateOwnerLocked(const ScmOwner& owner) const noexcept
	{
		if (!owner.IsValid()) return EScmOperationStatus::InvalidOwner;
		const auto found = ownerGenerations.find(owner.extensionId);
		if (found == ownerGenerations.end()) return EScmOperationStatus::InvalidOwner;
		return found->second == owner.generation
			? EScmOperationStatus::Succeeded : EScmOperationStatus::OwnerGenerationConflict;
	}

	[[nodiscard]] bool CollectListenersLocked(std::vector<ScmServiceListener>& listeners) const noexcept
	{
		try {
			listeners.reserve(subscriptions.size());
			for (const auto& [ignored, listener] : subscriptions) {
				(void)ignored;
				if (listener) listeners.push_back(listener);
			}
			return true;
		} catch (...) {
			listeners.clear();
			return false;
		}
	}
};

namespace {

void Notify(const std::vector<ScmServiceListener>& listeners, const ScmServiceChange& change) noexcept
{
	for (const auto& listener : listeners) {
		try {
			listener(change);
		} catch (...) {
			// Notifications are advisory. A broken projection must not undo a committed SCM mutation.
		}
	}
}

template <typename ImplType, typename Mutation>
ScmOperationResult ApplyMutation(ImplType& impl, Mutation mutation)
{
	std::vector<ScmServiceListener> listeners;
	ScmServiceChange change;
	ScmOperationResult result;
	{
		std::lock_guard lock(impl.mutex);
		if (impl.stopped) return impl.Current(EScmOperationStatus::Stopped);
		result = mutation(change, listeners);
	}
	if (result.Succeeded() && change.revision != 0) Notify(listeners, change);
	return result;
}

template <typename ProviderType>
bool MatchesOwner(const ProviderType& provider, const ScmOwner& owner) noexcept
{
	return SameOwner(provider.state.owner, owner);
}

} // namespace

bool ScmOwner::IsValid() const noexcept
{
	return generation != 0 && SourceControlService::IsValidStableId(extensionId);
}

SourceControlService::SourceControlService(SourceControlServiceLimits limits)
	: m_impl(std::make_unique<Impl>(std::move(limits)))
{
}

SourceControlService::~SourceControlService()
{
	(void)Stop();
}

bool SourceControlService::IsValidStableId(const std::string_view value) noexcept
{
	return IsValidStableIdValue(value);
}

ScmOperationResult SourceControlService::CreateProvider(const ScmCreateProviderRequest& request)
{
	return ApplyMutation(*m_impl, [&](ScmServiceChange& change, std::vector<ScmServiceListener>& listeners) {
		const auto ownerStatus = m_impl->PrepareOwnerLocked(request.provider.owner);
		if (ownerStatus != EScmOperationStatus::Succeeded) return m_impl->Current(ownerStatus);
		const auto& provider = request.provider;
		if (!IsValidProviderMetadata(provider)) return m_impl->Current(EScmOperationStatus::InvalidPayload);
		if (provider.groups.size() > m_impl->limits.maximumGroupsPerProvider ||
			PayloadBytes(provider) > m_impl->limits.maximumPayloadBytes) {
			return m_impl->Current(provider.groups.size() > m_impl->limits.maximumGroupsPerProvider
				? EScmOperationStatus::GroupLimitExceeded : EScmOperationStatus::PayloadLimitExceeded);
		}
		std::map<std::string, ScmResourceGroupState, std::less<>> groups;
		for (const auto& group : provider.groups) {
			if (!SameOwner(group.owner, provider.owner) || group.providerHandle != provider.handle ||
				!IsValidGroupMetadata(group) || group.resources.size() > m_impl->limits.maximumResourcesPerGroup) {
				return m_impl->Current(EScmOperationStatus::InvalidPayload);
			}
			if (PayloadBytes(group) > m_impl->limits.maximumPayloadBytes ||
				!std::all_of(group.resources.begin(), group.resources.end(), IsValidResource)) {
				return m_impl->Current(PayloadBytes(group) > m_impl->limits.maximumPayloadBytes
					? EScmOperationStatus::PayloadLimitExceeded : EScmOperationStatus::InvalidResource);
			}
			if (!groups.emplace(group.id, group).second) return m_impl->Current(EScmOperationStatus::InvalidPayload);
		}
		const auto found = m_impl->providers.find(provider.handle);
		if (found != m_impl->providers.end()) {
			if (!MatchesOwner(found->second, provider.owner)) return m_impl->Current(EScmOperationStatus::OwnerGenerationConflict);
			found->second.state = provider;
			found->second.state.groups.clear();
			found->second.groups = std::move(groups);
			if (!m_impl->AdvanceRevisionLocked()) return m_impl->Current(EScmOperationStatus::RevisionExhausted);
			change = { .revision = m_impl->revision, .kind = EScmChangeKind::ProviderUpdated,
				.providerHandle = provider.handle };
			(void)m_impl->CollectListenersLocked(listeners);
			return m_impl->Current(EScmOperationStatus::Replayed);
		}
		if (m_impl->providers.size() >= m_impl->limits.maximumProviders) return m_impl->Current(EScmOperationStatus::ProviderLimitExceeded);
		try {
			Impl::Provider stored{ .state = provider, .groups = std::move(groups) };
			stored.state.groups.clear();
			m_impl->providers.emplace(provider.handle, std::move(stored));
		} catch (...) {
			return m_impl->Current(EScmOperationStatus::ProviderLimitExceeded);
		}
		if (!m_impl->AdvanceRevisionLocked()) return m_impl->Current(EScmOperationStatus::RevisionExhausted);
		change = { .revision = m_impl->revision, .kind = EScmChangeKind::ProviderCreated,
			.providerHandle = provider.handle };
		(void)m_impl->CollectListenersLocked(listeners);
		return m_impl->Current(EScmOperationStatus::Succeeded);
	});
}

ScmOperationResult SourceControlService::UpdateProvider(const ScmUpdateProviderRequest& request)
{
	return ApplyMutation(*m_impl, [&](ScmServiceChange& change, std::vector<ScmServiceListener>& listeners) {
		const auto ownerStatus = m_impl->ValidateOwnerLocked(request.owner);
		if (ownerStatus != EScmOperationStatus::Succeeded) return m_impl->Current(ownerStatus);
		if (!IsValidStableIdValue(request.handle)) return m_impl->Current(EScmOperationStatus::InvalidPayload);
		const auto found = m_impl->providers.find(request.handle);
		if (found == m_impl->providers.end()) return m_impl->Current(EScmOperationStatus::InvalidProvider);
		if (!MatchesOwner(found->second, request.owner)) return m_impl->Current(EScmOperationStatus::OwnerGenerationConflict);
		if (request.label && !IsValidText(*request.label, kMaximumLabelBytes, false)) return m_impl->Current(EScmOperationStatus::InvalidPayload);
		if (request.inputBox && !IsValidInputBox(*request.inputBox)) return m_impl->Current(EScmOperationStatus::InvalidPayload);
		if (request.commitTemplate && !IsValidText(*request.commitTemplate, kMaximumInputValueBytes)) return m_impl->Current(EScmOperationStatus::InvalidPayload);
		if (request.acceptInputCommand && !IsValidCommand(*request.acceptInputCommand)) return m_impl->Current(EScmOperationStatus::InvalidPayload);
		if (request.statusBarCommands && !std::all_of(request.statusBarCommands->begin(), request.statusBarCommands->end(), IsValidCommand)) return m_impl->Current(EScmOperationStatus::InvalidPayload);
		std::size_t payload{};
		if (request.label) payload = SaturatingAdd(payload, request.label->size());
		if (request.inputBox) payload = SaturatingAdd(payload, request.inputBox->value.size() + request.inputBox->placeholder.size());
		if (request.commitTemplate) payload = SaturatingAdd(payload, request.commitTemplate->size());
		if (request.acceptInputCommand) payload = SaturatingAdd(payload, PayloadBytes(*request.acceptInputCommand));
		if (request.statusBarCommands) for (const auto& command : *request.statusBarCommands) payload = SaturatingAdd(payload, PayloadBytes(command));
		if (payload > m_impl->limits.maximumPayloadBytes) return m_impl->Current(EScmOperationStatus::PayloadLimitExceeded);
		if (request.label) found->second.state.label = *request.label;
		if (request.inputBox) found->second.state.inputBox = *request.inputBox;
		if (request.clearCount) found->second.state.count.reset();
		else if (request.count) found->second.state.count = *request.count;
		if (request.clearCommitTemplate) found->second.state.commitTemplate.reset();
		else if (request.commitTemplate) found->second.state.commitTemplate = *request.commitTemplate;
		if (request.clearAcceptInputCommand) found->second.state.acceptInputCommand.reset();
		else if (request.acceptInputCommand) found->second.state.acceptInputCommand = *request.acceptInputCommand;
		if (request.clearStatusBarCommands) found->second.state.statusBarCommands.clear();
		else if (request.statusBarCommands) found->second.state.statusBarCommands = *request.statusBarCommands;
		if (!m_impl->AdvanceRevisionLocked()) return m_impl->Current(EScmOperationStatus::RevisionExhausted);
		change = { .revision = m_impl->revision, .kind = EScmChangeKind::ProviderUpdated,
			.providerHandle = request.handle };
		(void)m_impl->CollectListenersLocked(listeners);
		return m_impl->Current(EScmOperationStatus::Succeeded);
	});
}

ScmOperationResult SourceControlService::CreateGroup(const ScmCreateGroupRequest& request)
{
	return ApplyMutation(*m_impl, [&](ScmServiceChange& change, std::vector<ScmServiceListener>& listeners) {
		const auto ownerStatus = m_impl->PrepareOwnerLocked(request.group.owner);
		if (ownerStatus != EScmOperationStatus::Succeeded) return m_impl->Current(ownerStatus);
		const auto& group = request.group;
		if (!IsValidGroupMetadata(group) || group.resources.size() > m_impl->limits.maximumResourcesPerGroup ||
			PayloadBytes(group) > m_impl->limits.maximumPayloadBytes) {
			return m_impl->Current(group.resources.size() > m_impl->limits.maximumResourcesPerGroup
				? EScmOperationStatus::ResourceLimitExceeded : EScmOperationStatus::PayloadLimitExceeded);
		}
		const auto provider = m_impl->providers.find(group.providerHandle);
		if (provider == m_impl->providers.end()) return m_impl->Current(EScmOperationStatus::InvalidProvider);
		if (!MatchesOwner(provider->second, group.owner)) return m_impl->Current(EScmOperationStatus::OwnerGenerationConflict);
		if (!std::all_of(group.resources.begin(), group.resources.end(), IsValidResource)) return m_impl->Current(EScmOperationStatus::InvalidResource);
		if (provider->second.groups.size() >= m_impl->limits.maximumGroupsPerProvider && !provider->second.groups.contains(group.id)) return m_impl->Current(EScmOperationStatus::GroupLimitExceeded);
		const auto found = provider->second.groups.find(group.id);
		if (found != provider->second.groups.end()) {
			found->second = group;
			if (!m_impl->AdvanceRevisionLocked()) return m_impl->Current(EScmOperationStatus::RevisionExhausted);
			change = { .revision = m_impl->revision, .kind = EScmChangeKind::GroupUpdated,
				.providerHandle = group.providerHandle, .groupId = group.id };
			(void)m_impl->CollectListenersLocked(listeners);
			return m_impl->Current(EScmOperationStatus::Replayed);
		}
		try {
			provider->second.groups.emplace(group.id, group);
		} catch (...) {
			return m_impl->Current(EScmOperationStatus::GroupLimitExceeded);
		}
		if (!m_impl->AdvanceRevisionLocked()) return m_impl->Current(EScmOperationStatus::RevisionExhausted);
		change = { .revision = m_impl->revision, .kind = EScmChangeKind::GroupCreated,
			.providerHandle = group.providerHandle, .groupId = group.id };
		(void)m_impl->CollectListenersLocked(listeners);
		return m_impl->Current(EScmOperationStatus::Succeeded);
	});
}

ScmOperationResult SourceControlService::UpdateGroup(const ScmUpdateGroupRequest& request)
{
	return ApplyMutation(*m_impl, [&](ScmServiceChange& change, std::vector<ScmServiceListener>& listeners) {
		const auto ownerStatus = m_impl->ValidateOwnerLocked(request.owner);
		if (ownerStatus != EScmOperationStatus::Succeeded) return m_impl->Current(ownerStatus);
		if (!IsValidStableIdValue(request.providerHandle) || !IsValidStableIdValue(request.groupId)) return m_impl->Current(EScmOperationStatus::InvalidPayload);
		const auto provider = m_impl->providers.find(request.providerHandle);
		if (provider == m_impl->providers.end()) return m_impl->Current(EScmOperationStatus::InvalidProvider);
		if (!MatchesOwner(provider->second, request.owner)) return m_impl->Current(EScmOperationStatus::OwnerGenerationConflict);
		const auto found = provider->second.groups.find(request.groupId);
		if (found == provider->second.groups.end()) return m_impl->Current(EScmOperationStatus::InvalidGroup);
		if (request.label && !IsValidText(*request.label, kMaximumLabelBytes, false)) return m_impl->Current(EScmOperationStatus::InvalidPayload);
		if (request.contextValue && !IsValidText(*request.contextValue, kMaximumContextValueBytes)) return m_impl->Current(EScmOperationStatus::InvalidPayload);
		if (request.label) found->second.label = *request.label;
		if (request.hideWhenEmpty) found->second.hideWhenEmpty = *request.hideWhenEmpty;
		if (request.contextValue) found->second.contextValue = *request.contextValue;
		if (!m_impl->AdvanceRevisionLocked()) return m_impl->Current(EScmOperationStatus::RevisionExhausted);
		change = { .revision = m_impl->revision, .kind = EScmChangeKind::GroupUpdated,
			.providerHandle = request.providerHandle, .groupId = request.groupId };
		(void)m_impl->CollectListenersLocked(listeners);
		return m_impl->Current(EScmOperationStatus::Succeeded);
	});
}

ScmOperationResult SourceControlService::ReplaceResources(const ScmReplaceResourcesRequest& request)
{
	return ApplyMutation(*m_impl, [&](ScmServiceChange& change, std::vector<ScmServiceListener>& listeners) {
		const auto ownerStatus = m_impl->ValidateOwnerLocked(request.owner);
		if (ownerStatus != EScmOperationStatus::Succeeded) return m_impl->Current(ownerStatus);
		if (!IsValidStableIdValue(request.providerHandle) || !IsValidStableIdValue(request.groupId)) return m_impl->Current(EScmOperationStatus::InvalidPayload);
		if (request.resources.size() > m_impl->limits.maximumResourcesPerGroup) return m_impl->Current(EScmOperationStatus::ResourceLimitExceeded);
		std::size_t payload{};
		for (const auto& resource : request.resources) {
			if (!IsValidResource(resource)) return m_impl->Current(EScmOperationStatus::InvalidResource);
			payload = SaturatingAdd(payload, PayloadBytes(resource));
		}
		if (payload > m_impl->limits.maximumPayloadBytes) return m_impl->Current(EScmOperationStatus::PayloadLimitExceeded);
		const auto provider = m_impl->providers.find(request.providerHandle);
		if (provider == m_impl->providers.end()) return m_impl->Current(EScmOperationStatus::InvalidProvider);
		if (!MatchesOwner(provider->second, request.owner)) return m_impl->Current(EScmOperationStatus::OwnerGenerationConflict);
		const auto group = provider->second.groups.find(request.groupId);
		if (group == provider->second.groups.end()) return m_impl->Current(EScmOperationStatus::InvalidGroup);
		group->second.resources = request.resources;
		if (!m_impl->AdvanceRevisionLocked()) return m_impl->Current(EScmOperationStatus::RevisionExhausted);
		change = { .revision = m_impl->revision, .kind = EScmChangeKind::ResourcesReplaced,
			.providerHandle = request.providerHandle, .groupId = request.groupId };
		(void)m_impl->CollectListenersLocked(listeners);
		return m_impl->Current(EScmOperationStatus::Succeeded);
	});
}

ScmOperationResult SourceControlService::UpdateInputBox(const ScmInputBoxUpdateRequest& request)
{
	return ApplyMutation(*m_impl, [&](ScmServiceChange& change, std::vector<ScmServiceListener>& listeners) {
		const auto ownerStatus = m_impl->ValidateOwnerLocked(request.owner);
		if (ownerStatus != EScmOperationStatus::Succeeded) return m_impl->Current(ownerStatus);
		if (!IsValidStableIdValue(request.handle) || !IsValidInputBox(request.inputBox)) return m_impl->Current(EScmOperationStatus::InvalidPayload);
		if (request.global) {
			if (m_impl->globalInput && !SameOwner(m_impl->globalInput->owner, request.owner)) return m_impl->Current(EScmOperationStatus::OwnerGenerationConflict);
			m_impl->globalInput = ScmGlobalInputState{ .owner = request.owner, .handle = request.handle, .inputBox = request.inputBox };
		} else {
			const auto provider = m_impl->providers.find(request.handle);
			if (provider == m_impl->providers.end()) return m_impl->Current(EScmOperationStatus::InvalidProvider);
			if (!MatchesOwner(provider->second, request.owner)) return m_impl->Current(EScmOperationStatus::OwnerGenerationConflict);
			provider->second.state.inputBox = request.inputBox;
		}
		if (!m_impl->AdvanceRevisionLocked()) return m_impl->Current(EScmOperationStatus::RevisionExhausted);
		change = { .revision = m_impl->revision, .kind = EScmChangeKind::InputBoxChanged,
			.providerHandle = request.global ? std::nullopt : std::optional(request.handle) };
		(void)m_impl->CollectListenersLocked(listeners);
		return m_impl->Current(EScmOperationStatus::Succeeded);
	});
}

ScmOperationResult SourceControlService::DisposeProvider(const ScmDisposeProviderRequest& request)
{
	return ApplyMutation(*m_impl, [&](ScmServiceChange& change, std::vector<ScmServiceListener>& listeners) {
		const auto ownerStatus = m_impl->ValidateOwnerLocked(request.owner);
		if (ownerStatus != EScmOperationStatus::Succeeded) return m_impl->Current(ownerStatus);
		if (!IsValidStableIdValue(request.handle)) return m_impl->Current(EScmOperationStatus::InvalidPayload);
		const auto found = m_impl->providers.find(request.handle);
		if (found == m_impl->providers.end()) return m_impl->Current(EScmOperationStatus::NotApplicable);
		if (!MatchesOwner(found->second, request.owner)) return m_impl->Current(EScmOperationStatus::OwnerGenerationConflict);
		m_impl->providers.erase(found);
		if (!m_impl->AdvanceRevisionLocked()) return m_impl->Current(EScmOperationStatus::RevisionExhausted);
		change = { .revision = m_impl->revision, .kind = EScmChangeKind::ProviderDisposed,
			.providerHandle = request.handle };
		(void)m_impl->CollectListenersLocked(listeners);
		return m_impl->Current(EScmOperationStatus::Succeeded);
	});
}

ScmOperationResult SourceControlService::DisposeGroup(const ScmDisposeGroupRequest& request)
{
	return ApplyMutation(*m_impl, [&](ScmServiceChange& change, std::vector<ScmServiceListener>& listeners) {
		const auto ownerStatus = m_impl->ValidateOwnerLocked(request.owner);
		if (ownerStatus != EScmOperationStatus::Succeeded) return m_impl->Current(ownerStatus);
		if (!IsValidStableIdValue(request.providerHandle) || !IsValidStableIdValue(request.groupId)) return m_impl->Current(EScmOperationStatus::InvalidPayload);
		const auto provider = m_impl->providers.find(request.providerHandle);
		if (provider == m_impl->providers.end()) return m_impl->Current(EScmOperationStatus::NotApplicable);
		if (!MatchesOwner(provider->second, request.owner)) return m_impl->Current(EScmOperationStatus::OwnerGenerationConflict);
		const auto group = provider->second.groups.find(request.groupId);
		if (group == provider->second.groups.end()) return m_impl->Current(EScmOperationStatus::NotApplicable);
		provider->second.groups.erase(group);
		if (!m_impl->AdvanceRevisionLocked()) return m_impl->Current(EScmOperationStatus::RevisionExhausted);
		change = { .revision = m_impl->revision, .kind = EScmChangeKind::GroupDisposed,
			.providerHandle = request.providerHandle, .groupId = request.groupId };
		(void)m_impl->CollectListenersLocked(listeners);
		return m_impl->Current(EScmOperationStatus::Succeeded);
	});
}

ScmOperationResult SourceControlService::DisposeOwner(const ScmOwner& owner)
{
	return ApplyMutation(*m_impl, [&](ScmServiceChange& change, std::vector<ScmServiceListener>& listeners) {
		const auto ownerStatus = m_impl->ValidateOwnerLocked(owner);
		if (ownerStatus != EScmOperationStatus::Succeeded) return m_impl->Current(ownerStatus);
		bool changed = false;
		for (auto iterator = m_impl->providers.begin(); iterator != m_impl->providers.end();) {
			if (MatchesOwner(iterator->second, owner)) {
				iterator = m_impl->providers.erase(iterator);
				changed = true;
			} else {
				++iterator;
			}
		}
		if (m_impl->globalInput && SameOwner(m_impl->globalInput->owner, owner)) {
			m_impl->globalInput.reset();
			changed = true;
		}
		if (!changed) return m_impl->Current(EScmOperationStatus::NotApplicable);
		if (!m_impl->AdvanceRevisionLocked()) return m_impl->Current(EScmOperationStatus::RevisionExhausted);
		change = { .revision = m_impl->revision, .kind = EScmChangeKind::OwnerDisposed };
		(void)m_impl->CollectListenersLocked(listeners);
		return m_impl->Current(EScmOperationStatus::Succeeded);
	});
}

ScmOperationResult SourceControlService::DisposeAll()
{
	return ApplyMutation(*m_impl, [&](ScmServiceChange& change, std::vector<ScmServiceListener>& listeners) {
		if (m_impl->providers.empty() && !m_impl->globalInput) return m_impl->Current(EScmOperationStatus::NotApplicable);
		m_impl->providers.clear();
		m_impl->globalInput.reset();
		if (!m_impl->AdvanceRevisionLocked()) return m_impl->Current(EScmOperationStatus::RevisionExhausted);
		change = { .revision = m_impl->revision, .kind = EScmChangeKind::OwnerDisposed };
		(void)m_impl->CollectListenersLocked(listeners);
		return m_impl->Current(EScmOperationStatus::Succeeded);
	});
}

ScmOperationResult SourceControlService::Stop() noexcept
{
	std::lock_guard lock(m_impl->mutex);
	if (m_impl->stopped) return m_impl->Current(EScmOperationStatus::Stopped);
	m_impl->stopped = true;
	m_impl->providers.clear();
	m_impl->globalInput.reset();
	m_impl->subscriptions.clear();
	if (m_impl->revision != (std::numeric_limits<std::uint64_t>::max)()) ++m_impl->revision;
	return m_impl->Current(EScmOperationStatus::Succeeded);
}

ScmServiceSnapshot SourceControlService::Snapshot() const
{
	std::lock_guard lock(m_impl->mutex);
	ScmServiceSnapshot snapshot{ .revision = m_impl->revision, .stopped = m_impl->stopped, .globalInput = m_impl->globalInput };
	try {
		snapshot.providers.reserve(m_impl->providers.size());
		for (const auto& [ignored, provider] : m_impl->providers) {
			(void)ignored;
			ScmProviderState state = provider.state;
			state.groups.reserve(provider.groups.size());
			for (const auto& [groupId, group] : provider.groups) {
				(void)groupId;
				state.groups.push_back(group);
			}
			snapshot.providers.push_back(std::move(state));
		}
	} catch (...) {
		// A snapshot is an advisory projection. Returning the already-copied prefix is safer than exposing mutable state.
	}
	return snapshot;
}

std::optional<ScmServiceSubscriptionId> SourceControlService::Subscribe(ScmServiceListener listener)
{
	if (!listener) return std::nullopt;
	std::lock_guard lock(m_impl->mutex);
	if (m_impl->stopped || m_impl->subscriptions.size() >= m_impl->limits.maximumSubscriptions ||
		m_impl->nextSubscriptionId == 0) return std::nullopt;
	const auto id = m_impl->nextSubscriptionId++;
	try {
		m_impl->subscriptions.emplace(id, std::move(listener));
	} catch (...) {
		return std::nullopt;
	}
	return id;
}

void SourceControlService::Unsubscribe(const ScmServiceSubscriptionId subscriptionId) noexcept
{
	if (subscriptionId == 0) return;
	std::lock_guard lock(m_impl->mutex);
	m_impl->subscriptions.erase(subscriptionId);
}

} // namespace workbench::scm
