/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "config/CWorkspaceContextService.h"

#include <algorithm>
#include <cwctype>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace config {
namespace {

constexpr std::size_t kMaximumOperationIdLength = 128;
constexpr std::size_t kMaximumExternalIdentifierLength = 256;
constexpr std::size_t kMaximumFolders = 256;
constexpr std::size_t kMaximumUriTextLength = 4096;
constexpr std::size_t kMaximumUriComparisonKeyLength = 1024;
constexpr std::size_t kMaximumWorkspaceIdentityKeyLength = 16384;

bool IsValidUtf16(std::wstring_view value) noexcept
{
	for (std::size_t index = 0; index < value.size(); ++index) {
		const auto character = static_cast<std::uint32_t>(value[index]);
		if constexpr (sizeof(wchar_t) == 2) {
			if (character >= 0xd800 && character <= 0xdbff) {
				if (index + 1 >= value.size()) {
					return false;
				}
				const auto trailing = static_cast<std::uint32_t>(value[++index]);
				if (trailing < 0xdc00 || trailing > 0xdfff) {
					return false;
				}
			} else if (character >= 0xdc00 && character <= 0xdfff) {
				return false;
			}
		}
	}
	return true;
}

bool IsBoundedExternalIdentifier(std::wstring_view value) noexcept
{
	if (value.empty() || value.size() > kMaximumExternalIdentifierLength || !IsValidUtf16(value)) {
		return false;
	}
	return std::none_of(value.begin(), value.end(), [](wchar_t character) {
		return character <= 0x1f || character == 0x7f;
	});
}

bool IsBoundedOperationId(std::string_view value) noexcept
{
	if (value.empty() || value.size() > kMaximumOperationIdLength) {
		return false;
	}
	return std::all_of(value.begin(), value.end(), [](unsigned char character) {
		return character >= 0x21 && character <= 0x7e;
	});
}

void AppendIdentityPart(std::wstring& target, std::wstring_view value)
{
	target += std::to_wstring(value.size());
	target += L':';
	target += value;
}

std::wstring ToLowerInvariant(std::wstring_view value)
{
	std::wstring result(value);
	for (auto& character : result) {
		character = character <= 0x7f
			? (character >= L'A' && character <= L'Z' ? static_cast<wchar_t>(character - L'A' + L'a') : character)
			: static_cast<wchar_t>(std::towlower(character));
	}
	return result;
}

bool IsLocalFileAuthority(const platform::uri::Uri& uri)
{
	return ToLowerInvariant(uri.Scheme()) == L"file"
		&& (uri.Authority().empty() || ToLowerInvariant(uri.Authority()) == L"localhost");
}

bool AddBoundedLength(std::size_t& total, std::size_t value, std::size_t multiplier = 1) noexcept
{
	if (value > (kMaximumUriTextLength - total) / multiplier) {
		return false;
	}
	total += value * multiplier;
	return true;
}

//! MakeComparisonKey internally serializes the URI to reserve storage. Bound
//! every decoded component before calling it so that reservation stays bounded.
bool HasBoundedUriEncoding(const platform::uri::Uri& uri) noexcept
{
	std::size_t maximumEncodedLength = 16; // scheme delimiters, authority marker, query and fragment markers
	const auto addComponent = [&maximumEncodedLength](std::wstring_view component) {
		// A decoded UTF-16 code unit can require at most four UTF-8 bytes, each
		// percent encoded as three ASCII characters. This deliberately overbounds.
		return AddBoundedLength(maximumEncodedLength, component.size(), 12);
	};
	return addComponent(uri.Scheme())
		&& addComponent(uri.Authority())
		&& addComponent(uri.Path())
		&& (!uri.Query() || addComponent(*uri.Query()))
		&& (!uri.Fragment() || addComponent(*uri.Fragment()));
}

struct CanonicalUri final {
	platform::uri::Uri uri;
	std::wstring comparisonKey;
};

std::optional<CanonicalUri> CanonicalizeUri(const platform::uri::Uri& uri)
{
	if (!HasBoundedUriEncoding(uri)) {
		return std::nullopt;
	}
	const auto comparisonKey = platform::uri::UriIdentityService::MakeComparisonKey(uri);
	if (comparisonKey.empty() || comparisonKey.size() > kMaximumUriComparisonKeyLength) {
		return std::nullopt;
	}

	const bool isFile = ToLowerInvariant(uri.Scheme()) == L"file";
	const bool localAuthority = IsLocalFileAuthority(uri);
	auto canonical = platform::uri::Uri::FromComponents(
		ToLowerInvariant(uri.Scheme()),
		localAuthority ? std::wstring{} : uri.Authority(),
		uri.Path(),
		uri.Query(), uri.Fragment(),
		isFile ? true : uri.HasAuthority());
	if (!canonical) {
		return std::nullopt;
	}
	return CanonicalUri { std::move(*canonical.value), comparisonKey };
}

bool IsFolderUriConceptValid(const platform::uri::Uri& uri) noexcept
{
	return !uri.Path().empty();
}

bool IsWorkspaceConfigUriConceptValid(const platform::uri::Uri& uri) noexcept
{
	return !uri.Path().empty();
}

std::optional<std::wstring> MakeWorkspaceIdentity(
	EWorkspaceKind kind,
	std::wstring_view emptyWindowIdentity,
	const std::optional<std::wstring>& configKey,
	const std::vector<std::wstring>& folderKeys
)
{
	std::wstring identity;
	switch (kind) {
	case EWorkspaceKind::Empty:
		identity = L"empty:";
		AppendIdentityPart(identity, emptyWindowIdentity);
		break;
	case EWorkspaceKind::Folder:
		if (folderKeys.size() != 1 || configKey) {
			return std::nullopt;
		}
		identity = L"folder:";
		AppendIdentityPart(identity, folderKeys.front());
		break;
	case EWorkspaceKind::Workspace:
		if (!configKey) {
			return std::nullopt;
		}
		identity = L"workspace:";
		AppendIdentityPart(identity, *configKey);
		break;
	}
	return identity.size() <= kMaximumWorkspaceIdentityKeyLength ? std::optional<std::wstring>(std::move(identity)) : std::nullopt;
}

std::wstring OperationFingerprintPrefix(std::wstring_view method, const WorkspaceContextOperation& operation)
{
	std::wstring fingerprint(method);
	fingerprint += L"|expected:";
	fingerprint += operation.expectedRevision ? std::to_wstring(*operation.expectedRevision) : L"none";
	return fingerprint;
}

struct PreparedContext final {
	WorkspaceContextSnapshot snapshot;
	std::wstring fingerprint;
	bool valid = false;
	std::string failureReason;
};

PreparedContext PrepareEmpty(const WorkspaceContextOperation& operation, std::uint64_t generation, std::wstring_view emptyWindowIdentity)
{
	PreparedContext prepared;
	prepared.fingerprint = OperationFingerprintPrefix(L"empty", operation);
	const auto identity = MakeWorkspaceIdentity(EWorkspaceKind::Empty, emptyWindowIdentity, std::nullopt, {});
	if (!identity) {
		prepared.failureReason = "immutable empty-window identity is invalid";
		return prepared;
	}
	prepared.snapshot = { generation, 0, EWorkspaceKind::Empty, std::nullopt, {}, EWorkspaceTrustState::Unknown, *identity };
	prepared.valid = true;
	return prepared;
}

PreparedContext PrepareFolder(const SetFolderRequest& request, std::uint64_t generation, std::wstring_view emptyWindowIdentity)
{
	PreparedContext prepared;
	const auto canonicalFolder = CanonicalizeUri(request.folderUri);
	if (!canonicalFolder || !IsBoundedExternalIdentifier(request.displayName)) {
		prepared.failureReason = "folder URI or display identifier exceeds the supported bounds";
		return prepared;
	}
	prepared.fingerprint = OperationFingerprintPrefix(L"folder", request.operation);
	AppendIdentityPart(prepared.fingerprint, canonicalFolder->comparisonKey);
	AppendIdentityPart(prepared.fingerprint, request.displayName);
	if (!IsFolderUriConceptValid(canonicalFolder->uri)) {
		prepared.failureReason = "folder URI must identify a non-empty path";
		return prepared;
	}
	std::vector<std::wstring> folderKeys { canonicalFolder->comparisonKey };
	const auto identity = MakeWorkspaceIdentity(EWorkspaceKind::Folder, emptyWindowIdentity, std::nullopt, folderKeys);
	if (!identity) {
		prepared.failureReason = "folder identity exceeds the supported bounds";
		return prepared;
	}
	prepared.snapshot = { generation, 0, EWorkspaceKind::Folder, std::nullopt,
		{ { std::move(canonicalFolder->uri), request.displayName } }, EWorkspaceTrustState::Unknown, *identity };
	prepared.valid = true;
	return prepared;
}

PreparedContext PrepareWorkspace(const SetWorkspaceRequest& request, std::uint64_t generation, std::wstring_view emptyWindowIdentity)
{
	PreparedContext prepared;
	if (request.folders.size() > kMaximumFolders) {
		prepared.failureReason = "workspace folder count exceeds the supported bound";
		return prepared;
	}

	std::optional<CanonicalUri> configUri;
	if (request.workspaceConfigUri) {
		configUri = CanonicalizeUri(*request.workspaceConfigUri);
		if (!configUri) {
			prepared.failureReason = "workspace config URI exceeds the supported bounds";
			return prepared;
		}
	}
	std::vector<CanonicalUri> folders;
	folders.reserve(request.folders.size());
	for (const auto& folder : request.folders) {
		auto canonical = CanonicalizeUri(folder.uri);
		if (!canonical || !IsBoundedExternalIdentifier(folder.displayName)) {
			prepared.failureReason = "workspace folder URI or display identifier exceeds the supported bounds";
			return prepared;
		}
		folders.emplace_back(std::move(*canonical));
	}

	prepared.fingerprint = OperationFingerprintPrefix(L"workspace", request.operation);
	AppendIdentityPart(prepared.fingerprint, configUri ? configUri->comparisonKey : std::wstring{});
	for (const auto& folder : folders) {
		AppendIdentityPart(prepared.fingerprint, folder.comparisonKey);
	}
	for (const auto& folder : request.folders) {
		AppendIdentityPart(prepared.fingerprint, folder.displayName);
	}
	if (!configUri || !IsWorkspaceConfigUriConceptValid(configUri->uri)) {
		prepared.failureReason = "workspace requires a config URI with a non-empty path";
		return prepared;
	}
	std::vector<std::wstring> folderKeys;
	folderKeys.reserve(folders.size());
	std::vector<WorkspaceFolderDescriptor> descriptors;
	descriptors.reserve(folders.size());
	for (std::size_t index = 0; index < folders.size(); ++index) {
		if (!IsFolderUriConceptValid(folders[index].uri)
			|| std::find(folderKeys.begin(), folderKeys.end(), folders[index].comparisonKey) != folderKeys.end()) {
			prepared.failureReason = "workspace folders must be unique non-empty canonical URIs";
			return prepared;
		}
		folderKeys.emplace_back(folders[index].comparisonKey);
		descriptors.push_back({ std::move(folders[index].uri), request.folders[index].displayName });
	}
	const auto identity = MakeWorkspaceIdentity(EWorkspaceKind::Workspace, emptyWindowIdentity, configUri->comparisonKey, folderKeys);
	if (!identity) {
		prepared.failureReason = "workspace identity exceeds the supported bounds";
		return prepared;
	}
	prepared.snapshot = { generation, 0, EWorkspaceKind::Workspace, std::move(configUri->uri), std::move(descriptors),
		EWorkspaceTrustState::Unknown, *identity };
	prepared.valid = true;
	return prepared;
}

bool IsSameContext(const WorkspaceContextSnapshot& left, const WorkspaceContextSnapshot& right)
{
	if (left.kind != right.kind || left.workspaceIdentityKey != right.workspaceIdentityKey || left.workspaceConfigUri.has_value() != right.workspaceConfigUri.has_value()
		|| left.folders.size() != right.folders.size()) {
		return false;
	}
	if (left.workspaceConfigUri && !platform::uri::UriIdentityService::IsEqual(*left.workspaceConfigUri, *right.workspaceConfigUri)) {
		return false;
	}
	for (std::size_t index = 0; index < left.folders.size(); ++index) {
		if (left.folders[index].displayName != right.folders[index].displayName
			|| !platform::uri::UriIdentityService::IsEqual(left.folders[index].uri, right.folders[index].uri)) {
			return false;
		}
	}
	return true;
}

} // namespace

struct CWorkspaceContextService::State final {
	struct CompletedOperation final {
		std::wstring fingerprint;
		WorkspaceContextResult result;
	};

	static constexpr std::size_t kMaximumCompletedOperations = 1024;
	mutable std::mutex mutex;
	std::wstring emptyWindowIdentity;
	WorkspaceContextSnapshot snapshot;
	std::map<std::string, CompletedOperation, std::less<>> completedOperations;
	std::deque<std::string> completedOperationOrder;
	std::map<std::uint64_t, WorkspaceContextListener> listeners;
	std::uint64_t nextListenerId = 1;
	std::deque<WorkspaceContextChange> pendingNotifications;
	bool notificationDrainActive = false;
};

namespace {

using State = CWorkspaceContextService::State;

WorkspaceContextResult CurrentResultLocked(const State& state, EWorkspaceContextOutcome outcome, std::string reason)
{
	return { outcome, state.snapshot.revision, std::move(reason), false, state.snapshot, std::nullopt };
}

std::optional<WorkspaceContextResult> ReplayOrConflictLocked(const State& state, std::string_view operationId, std::wstring_view fingerprint)
{
	const auto found = state.completedOperations.find(std::string(operationId));
	if (found == state.completedOperations.end()) {
		return std::nullopt;
	}
	if (found->second.fingerprint == fingerprint) {
		auto replay = found->second.result;
		replay.replayed = true;
		replay.change.reset();
		return replay;
	}
	return CurrentResultLocked(state, EWorkspaceContextOutcome::Conflict, "operationId was already used for a different request");
}

void RememberCompletedOperationLocked(State& state, std::string operationId, std::wstring fingerprint, const WorkspaceContextResult& result)
{
	if (state.completedOperations.find(operationId) != state.completedOperations.end()) {
		return;
	}
	while (state.completedOperationOrder.size() >= State::kMaximumCompletedOperations) {
		state.completedOperations.erase(state.completedOperationOrder.front());
		state.completedOperationOrder.pop_front();
	}
	state.completedOperationOrder.push_back(operationId);
	state.completedOperations.emplace(std::move(operationId), State::CompletedOperation { std::move(fingerprint), result });
}

bool EnqueueNotificationLocked(State& state, WorkspaceContextChange change)
{
	state.pendingNotifications.emplace_back(std::move(change));
	if (state.notificationDrainActive) {
		return false;
	}
	state.notificationDrainActive = true;
	return true;
}

//! One drainer delivers complete revisions in commit order. Reentrant and
//! concurrent mutations append to the queue and return; they never overtake a
//! listener which is still receiving an earlier revision.
void DrainNotifications(const std::shared_ptr<State>& state)
{
	for (;;) {
		WorkspaceContextChange change;
		std::vector<std::uint64_t> listenerIds;
		{
			std::lock_guard stateLock(state->mutex);
			if (state->pendingNotifications.empty()) {
				state->notificationDrainActive = false;
				return;
			}
			change = std::move(state->pendingNotifications.front());
			state->pendingNotifications.pop_front();
			for (const auto& [id, listener] : state->listeners) {
				(void)listener;
				listenerIds.push_back(id);
			}
		}
		for (const auto id : listenerIds) {
			WorkspaceContextListener listener;
			{
				std::lock_guard stateLock(state->mutex);
				const auto found = state->listeners.find(id);
				if (found == state->listeners.end()) {
					continue;
				}
				listener = found->second;
			}
			try {
				listener(change);
			} catch (...) {
				// Observers cannot roll back or suppress an already committed transition.
			}
		}
	}
}

WorkspaceContextResult ApplyContext(
	const std::shared_ptr<State>& state,
	const WorkspaceContextOperation& operation,
	PreparedContext prepared
)
{
	std::optional<WorkspaceContextChange> change;
	bool shouldDrain = false;
	WorkspaceContextResult result;
	{
		std::unique_lock lock(state->mutex);
		if (!prepared.valid) {
			return CurrentResultLocked(*state, EWorkspaceContextOutcome::Failed, std::move(prepared.failureReason));
		}
		if (const auto replay = ReplayOrConflictLocked(*state, operation.operationId, prepared.fingerprint)) {
			return *replay;
		}
		if (operation.expectedRevision && *operation.expectedRevision != state->snapshot.revision) {
			result = CurrentResultLocked(*state, EWorkspaceContextOutcome::Conflict, "expectedRevision does not match the current context revision");
			RememberCompletedOperationLocked(*state, operation.operationId, std::move(prepared.fingerprint), result);
			return result;
		}
		if (IsSameContext(state->snapshot, prepared.snapshot)) {
			result = CurrentResultLocked(*state, EWorkspaceContextOutcome::NotApplicable, "context already has the requested semantic value");
			RememberCompletedOperationLocked(*state, operation.operationId, std::move(prepared.fingerprint), result);
			return result;
		}
		if (state->snapshot.revision == std::numeric_limits<std::uint64_t>::max()) {
			result = CurrentResultLocked(*state, EWorkspaceContextOutcome::Failed, "context revision cannot advance without losing monotonicity");
			RememberCompletedOperationLocked(*state, operation.operationId, std::move(prepared.fingerprint), result);
			return result;
		}

		auto previous = state->snapshot;
		prepared.snapshot.revision = previous.revision + 1;
		prepared.snapshot.trust = previous.workspaceIdentityKey == prepared.snapshot.workspaceIdentityKey
			? previous.trust : EWorkspaceTrustState::Unknown;
		state->snapshot = prepared.snapshot;
		change = WorkspaceContextChange { state->snapshot.revision, std::move(previous), state->snapshot };
		result = CurrentResultLocked(*state, EWorkspaceContextOutcome::Succeeded, {});
		result.change = change;
		RememberCompletedOperationLocked(*state, operation.operationId, std::move(prepared.fingerprint), result);
		shouldDrain = EnqueueNotificationLocked(*state, *change);
	}
	if (shouldDrain) {
		DrainNotifications(state);
	}
	return result;
}

} // namespace

WorkspaceContextSubscription::WorkspaceContextSubscription(WorkspaceContextSubscription&& other) noexcept
	: m_unsubscribe(std::move(other.m_unsubscribe))
{
}

WorkspaceContextSubscription& WorkspaceContextSubscription::operator=(WorkspaceContextSubscription&& other) noexcept
{
	if (this != &other) {
		Reset();
		m_unsubscribe = std::move(other.m_unsubscribe);
	}
	return *this;
}

void WorkspaceContextSubscription::Reset() noexcept
{
	auto unsubscribe = std::move(m_unsubscribe);
	if (!unsubscribe) {
		return;
	}
	try {
		unsubscribe();
	} catch (...) {
		// Subscription cleanup is a boundary and must be safe in a destructor.
	}
}

CWorkspaceContextService::CWorkspaceContextService(std::wstring emptyWindowIdentity, std::uint64_t generation)
	: m_state(std::make_shared<State>())
{
	if (generation == 0 || !IsBoundedExternalIdentifier(emptyWindowIdentity)) {
		throw std::invalid_argument("workspace context service requires a bounded non-empty empty-window identity and generation");
	}
	m_state->emptyWindowIdentity = std::move(emptyWindowIdentity);
	const auto prepared = PrepareEmpty({ "initial", std::nullopt }, generation, m_state->emptyWindowIdentity);
	if (!prepared.valid) {
		throw std::invalid_argument("workspace context service cannot build its empty workspace identity");
	}
	m_state->snapshot = prepared.snapshot;
}

CWorkspaceContextService::~CWorkspaceContextService() = default;

WorkspaceContextSnapshot CWorkspaceContextService::Snapshot() const
{
	std::lock_guard lock(m_state->mutex);
	return m_state->snapshot;
}

WorkspaceContextResult CWorkspaceContextService::SetEmpty(const WorkspaceContextOperation& operation)
{
	if (!IsBoundedOperationId(operation.operationId)) {
		std::lock_guard lock(m_state->mutex);
		return CurrentResultLocked(*m_state, EWorkspaceContextOutcome::Failed, "operationId must be a bounded printable ASCII identifier");
	}
	std::uint64_t generation;
	std::wstring emptyWindowIdentity;
	{
		std::lock_guard lock(m_state->mutex);
		generation = m_state->snapshot.generation;
		emptyWindowIdentity = m_state->emptyWindowIdentity;
	}
	return ApplyContext(m_state, operation, PrepareEmpty(operation, generation, emptyWindowIdentity));
}

WorkspaceContextResult CWorkspaceContextService::SetFolder(const SetFolderRequest& request)
{
	if (!IsBoundedOperationId(request.operation.operationId)) {
		std::lock_guard lock(m_state->mutex);
		return CurrentResultLocked(*m_state, EWorkspaceContextOutcome::Failed, "operationId must be a bounded printable ASCII identifier");
	}
	std::uint64_t generation;
	std::wstring emptyWindowIdentity;
	{
		std::lock_guard lock(m_state->mutex);
		generation = m_state->snapshot.generation;
		emptyWindowIdentity = m_state->emptyWindowIdentity;
	}
	return ApplyContext(m_state, request.operation, PrepareFolder(request, generation, emptyWindowIdentity));
}

WorkspaceContextResult CWorkspaceContextService::SetWorkspace(const SetWorkspaceRequest& request)
{
	if (!IsBoundedOperationId(request.operation.operationId)) {
		std::lock_guard lock(m_state->mutex);
		return CurrentResultLocked(*m_state, EWorkspaceContextOutcome::Failed, "operationId must be a bounded printable ASCII identifier");
	}
	std::uint64_t generation;
	std::wstring emptyWindowIdentity;
	{
		std::lock_guard lock(m_state->mutex);
		generation = m_state->snapshot.generation;
		emptyWindowIdentity = m_state->emptyWindowIdentity;
	}
	return ApplyContext(m_state, request.operation, PrepareWorkspace(request, generation, emptyWindowIdentity));
}

WorkspaceContextResult CWorkspaceContextService::SetTrust(const SetTrustRequest& request)
{
	if (!IsBoundedOperationId(request.operation.operationId)) {
		std::lock_guard lock(m_state->mutex);
		return CurrentResultLocked(*m_state, EWorkspaceContextOutcome::Failed, "operationId must be a bounded printable ASCII identifier");
	}
	const auto fingerprint = OperationFingerprintPrefix(L"trust", request.operation) + L"|value:" + std::to_wstring(static_cast<unsigned int>(request.trust));

	std::optional<WorkspaceContextChange> change;
	bool shouldDrain = false;
	WorkspaceContextResult result;
	{
		std::unique_lock lock(m_state->mutex);
		if (const auto replay = ReplayOrConflictLocked(*m_state, request.operation.operationId, fingerprint)) {
			return *replay;
		}
		if (request.trust != EWorkspaceTrustState::Unknown && request.trust != EWorkspaceTrustState::Trusted && request.trust != EWorkspaceTrustState::Untrusted) {
			return CurrentResultLocked(*m_state, EWorkspaceContextOutcome::Failed, "trust state is invalid");
		}
		if (request.operation.expectedRevision && *request.operation.expectedRevision != m_state->snapshot.revision) {
			result = CurrentResultLocked(*m_state, EWorkspaceContextOutcome::Conflict, "expectedRevision does not match the current context revision");
			RememberCompletedOperationLocked(*m_state, request.operation.operationId, fingerprint, result);
			return result;
		}
		if (m_state->snapshot.trust == request.trust) {
			result = CurrentResultLocked(*m_state, EWorkspaceContextOutcome::NotApplicable, "trust already has the requested explicit value");
			RememberCompletedOperationLocked(*m_state, request.operation.operationId, fingerprint, result);
			return result;
		}
		if (m_state->snapshot.revision == std::numeric_limits<std::uint64_t>::max()) {
			result = CurrentResultLocked(*m_state, EWorkspaceContextOutcome::Failed, "context revision cannot advance without losing monotonicity");
			RememberCompletedOperationLocked(*m_state, request.operation.operationId, fingerprint, result);
			return result;
		}
		auto previous = m_state->snapshot;
		++m_state->snapshot.revision;
		m_state->snapshot.trust = request.trust;
		change = WorkspaceContextChange { m_state->snapshot.revision, std::move(previous), m_state->snapshot };
		result = CurrentResultLocked(*m_state, EWorkspaceContextOutcome::Succeeded, {});
		result.change = change;
		RememberCompletedOperationLocked(*m_state, request.operation.operationId, fingerprint, result);
		shouldDrain = EnqueueNotificationLocked(*m_state, *change);
	}
	if (shouldDrain) {
		DrainNotifications(m_state);
	}
	return result;
}

WorkspaceContextSubscription CWorkspaceContextService::Subscribe(WorkspaceContextListener listener)
{
	if (!listener) {
		return {};
	}
	std::uint64_t id;
	{
		std::lock_guard lock(m_state->mutex);
		id = m_state->nextListenerId++;
		m_state->listeners.emplace(id, std::move(listener));
	}
	std::weak_ptr<State> state = m_state;
	return WorkspaceContextSubscription([state = std::move(state), id]() noexcept {
		if (const auto locked = state.lock()) {
			std::lock_guard lock(locked->mutex);
			locked->listeners.erase(id);
		}
	});
}

} // namespace config
