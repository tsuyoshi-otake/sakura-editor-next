/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "workbench/editor/EditorCoreService.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <string_view>
#include <utility>

namespace workbench::editor {
namespace {

struct SubscriptionSlot {
	explicit SubscriptionSlot(EditorCoreChangeCallback listener)
		: callback(std::move(listener))
	{
	}

	std::atomic_bool active = true;
	EditorCoreChangeCallback callback;
};

class EditorCoreSubscription final : public IEditorCoreSubscription {
public:
	EditorCoreSubscription(std::weak_ptr<EditorCoreSubscriptionState> state, std::uint64_t subscriptionId) noexcept
		: m_state(std::move(state))
		, m_subscriptionId(subscriptionId)
	{
	}

	~EditorCoreSubscription() override { Unsubscribe(); }
	void Unsubscribe() noexcept override;
	[[nodiscard]] bool IsSubscribed() const noexcept override;

private:
	std::weak_ptr<EditorCoreSubscriptionState> m_state;
	std::uint64_t m_subscriptionId = 0;
};

void AppendField(std::string& output, std::string_view field)
{
	output.append(std::to_string(field.size()));
	output.push_back(':');
	output.append(field);
}

void AppendWideField(std::string& output, std::wstring_view field)
{
	AppendField(output, std::to_string(field.size()));
	for (const auto value : field) {
		AppendField(output, std::to_string(static_cast<unsigned int>(value)));
	}
}

void AppendIdentity(std::string& output, const EditorDocumentIdentity& identity)
{
	std::wstring comparisonKey;
	if (identity.TryComparisonKey(comparisonKey)) {
		AppendField(output, identity.resource ? "resource" : "opaque");
		AppendWideField(output, comparisonKey);
	} else {
		AppendField(output, "invalid");
	}
}

void AppendOperation(std::string& output, const EditorOperationMetadata& operation)
{
	if (operation.expectedModelRevision) {
		AppendField(output, "expected");
		AppendField(output, std::to_string(*operation.expectedModelRevision));
	} else {
		AppendField(output, "any-revision");
	}
}

void DeliverNotification(const std::shared_ptr<EditorCoreSubscriptionState>& state,
	const EditorCoreChangeBatch& batch);

bool HasBoundedResourceComponents(const platform::uri::Uri& uri) noexcept
{
	constexpr std::size_t kReservedKeyOverhead = 256;
	std::size_t total = 0;
	const auto append = [&total](std::size_t count) noexcept {
		if (count > kMaxEditorResourceComparisonKeyLength - kReservedKeyOverhead - total) return false;
		total += count;
		return true;
	};
	return append(uri.Scheme().size()) && append(uri.Authority().size()) && append(uri.Path().size())
		&& append(uri.Query() ? uri.Query()->size() : 0) && append(uri.Fragment() ? uri.Fragment()->size() : 0);
}

bool TryValidatedIdentityKey(const EditorDocumentIdentity& identity, std::wstring& key) noexcept
{
	return identity.IsValid() && identity.TryComparisonKey(key);
}

bool IsValidOperationId(const EditorOperationMetadata& operation) noexcept
{
	return IsValidEditorExternalId(operation.operationId, kMaxEditorOperationIdLength);
}

} // namespace

struct EditorCoreSubscriptionState {
	std::mutex mutex;
	bool closed = false;
	std::uint64_t nextSubscriptionId = 1;
	//! map preserves subscription-registration order.
	std::map<std::uint64_t, std::shared_ptr<SubscriptionSlot>> slots;
};

namespace {

void EditorCoreSubscription::Unsubscribe() noexcept
{
	try {
		const auto state = m_state.lock();
		if (!state || m_subscriptionId == 0) {
			m_subscriptionId = 0;
			return;
		}
		std::scoped_lock lock(state->mutex);
		if (const auto slot = state->slots.find(m_subscriptionId); slot != state->slots.end()) {
			slot->second->active.store(false, std::memory_order_release);
			state->slots.erase(slot);
		}
		m_subscriptionId = 0;
	} catch (...) {
		m_subscriptionId = 0;
	}
}

bool EditorCoreSubscription::IsSubscribed() const noexcept
{
	try {
		const auto state = m_state.lock();
		if (!state || m_subscriptionId == 0) return false;
		std::scoped_lock lock(state->mutex);
		if (state->closed) return false;
		const auto slot = state->slots.find(m_subscriptionId);
		return slot != state->slots.end() && slot->second->active.load(std::memory_order_acquire);
	} catch (...) {
		return false;
	}
}

void DeliverNotification(const std::shared_ptr<EditorCoreSubscriptionState>& state,
	const EditorCoreChangeBatch& batch)
{
	std::vector<std::shared_ptr<SubscriptionSlot>> listeners;
	{
		std::scoped_lock lock(state->mutex);
		if (state->closed) return;
		listeners.reserve(state->slots.size());
		for (const auto& [subscriptionId, slot] : state->slots) {
			(void)subscriptionId;
			listeners.push_back(slot);
		}
	}

	for (const auto& listener : listeners) {
		if (!listener->active.load(std::memory_order_acquire)) continue;
		try {
			listener->callback(batch);
		} catch (...) {
			// User callbacks are an observer boundary: one failure cannot suppress later listeners.
		}
	}
}

} // namespace

bool EditorDocumentIdentity::IsValid() const noexcept
{
	if (resource.has_value() == opaqueId.has_value()) return false;
	return resource ? HasBoundedResourceComponents(*resource)
		: IsValidEditorExternalId(*opaqueId, kMaxEditorOpaqueDocumentIdLength);
}

bool EditorDocumentIdentity::TryComparisonKey(std::wstring& key) const noexcept
{
	try {
		key.clear();
		if (!IsValid()) return false;
		if (resource) {
			constexpr std::size_t kResourcePrefixLength = 9;
			auto resourceKey = platform::uri::UriIdentityService::MakeComparisonKey(*resource);
			if (resourceKey.size() > kMaxEditorResourceComparisonKeyLength - kResourcePrefixLength) return false;
			key.reserve(kResourcePrefixLength + resourceKey.size());
			key.assign(L"resource:");
			key.append(resourceKey);
			return true;
		}
		if (opaqueId) {
			key.reserve(7 + opaqueId->size());
			key.assign(L"opaque:");
			for (const unsigned char value : *opaqueId) key.push_back(static_cast<wchar_t>(value));
			return true;
		}
	} catch (...) {
		key.clear();
	}
	return false;
}

bool EditorInputDescriptor::IsValid() const noexcept
{
	return IsValidEditorExternalId(inputId, kMaxEditorInputIdLength) && documentIdentity.IsValid();
}

bool ResolvedEditorDocument::IsValid() const noexcept
{
	return identity.IsValid();
}

EditorCoreService::EditorCoreService(std::uint64_t generation, std::size_t maxCompletedOperations)
	: m_generation(generation == 0 ? 1 : generation)
	, m_maxCompletedOperations(std::max<std::size_t>(1, maxCompletedOperations))
	, m_subscriptionState(std::make_shared<EditorCoreSubscriptionState>())
{
}

EditorCoreService::~EditorCoreService()
{
	{
		std::scoped_lock lock(m_subscriptionState->mutex);
		m_subscriptionState->closed = true;
		for (const auto& [subscriptionId, slot] : m_subscriptionState->slots) {
			(void)subscriptionId;
			slot->active.store(false, std::memory_order_release);
		}
		m_subscriptionState->slots.clear();
	}
	std::scoped_lock notificationLock(m_notificationMutex);
	m_notificationQueue.clear();
}

EditorCoreSnapshot EditorCoreService::Snapshot() const
{
	std::scoped_lock lock(m_mutex);
	return {
		.generation = m_generation,
		.revision = m_revision,
		.group = m_group.Snapshot(),
		.documents = m_documents.Snapshot(),
	};
}

EditorOperationResult EditorCoreService::ResolveDocument(const ResolveDocumentRequest& request)
{
	std::optional<EditorCoreChangeBatch> batch;
	EditorOperationResult result;
	bool drainNotifications = false;
	{
		std::scoped_lock lock(m_mutex);
		if (!IsValidOperationId(request.operation)) {
			return { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::InvalidOperationId, .revision = m_revision };
		}
		std::wstring resolvedDocumentKey;
		if (!request.resolvedDocument || !request.resolvedDocument->IsValid()
			|| !TryValidatedIdentityKey(request.resolvedDocument->identity, resolvedDocumentKey)) {
			return { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::DocumentNotResolved, .revision = m_revision };
		}
		const auto fingerprint = Fingerprint(request);
		bool handled = false;
		result = CheckCompletedOrConflict(request.operation, fingerprint, handled);
		if (handled) return result;
		if (request.operation.expectedModelRevision && *request.operation.expectedModelRevision != m_revision) {
			result = RevisionConflictResult();
		} else if (m_revision == std::numeric_limits<std::uint64_t>::max()) {
			// This model cannot publish a distinct next revision, so no mutation may begin.
			result = RevisionConflictResult();
		} else {
			const auto acquired = m_documents.AcquireResolver(*request.resolvedDocument);
			if (acquired.authoritativeStateConflict) {
				result = { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::DocumentStateConflict, .revision = m_revision };
			} else {
				std::vector<EditorCoreChange> changes;
				if (acquired.added) changes.push_back({ .kind = EEditorCoreChangeKind::DocumentAdded,
					.documentKey = acquired.document.documentKey, .documentRevision = acquired.document.documentRevision,
					.dirty = acquired.document.dirty });
				changes.push_back({ .kind = EEditorCoreChangeKind::DocumentResolved,
					.documentKey = acquired.document.documentKey, .documentRevision = acquired.document.documentRevision,
					.dirty = acquired.document.dirty });
				batch = Commit(std::move(changes));
				result = { .status = EEditorOperationStatus::Succeeded, .revision = batch->revision, .changeBatch = batch };
			}
		}
		RememberCompleted(request.operation.operationId, fingerprint, result);
		if (batch) drainNotifications = EnqueueNotification(*batch);
	}
	if (drainNotifications) DrainNotifications();
	return result;
}

EditorOperationResult EditorCoreService::ReleaseDocument(const ReleaseDocumentRequest& request)
{
	std::optional<EditorCoreChangeBatch> batch;
	EditorOperationResult result;
	bool drainNotifications = false;
	{
		std::scoped_lock lock(m_mutex);
		if (!IsValidOperationId(request.operation)) {
			return { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::InvalidOperationId, .revision = m_revision };
		}
		std::wstring documentKey;
		if (!TryValidatedIdentityKey(request.identity, documentKey)) {
			return { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::InvalidInput, .revision = m_revision };
		}
		const auto fingerprint = Fingerprint(request);
		bool handled = false;
		result = CheckCompletedOrConflict(request.operation, fingerprint, handled);
		if (handled) return result;
		if (request.operation.expectedModelRevision && *request.operation.expectedModelRevision != m_revision) {
			result = RevisionConflictResult();
		} else if (m_revision == std::numeric_limits<std::uint64_t>::max()) {
			result = RevisionConflictResult();
		} else if (const auto released = m_documents.ReleaseResolver(documentKey); !released) {
			result = { .status = EEditorOperationStatus::NotApplicable, .reason = EEditorOperationReason::ResolverNotFound, .revision = m_revision };
		} else {
			std::vector<EditorCoreChange> changes;
			changes.push_back({ .kind = EEditorCoreChangeKind::DocumentResolverReleased,
				.documentKey = released->document.documentKey, .documentRevision = released->document.documentRevision,
				.dirty = released->document.dirty });
			if (released->disposed) changes.push_back({ .kind = EEditorCoreChangeKind::DocumentReleased,
				.documentKey = released->document.documentKey, .documentRevision = released->document.documentRevision,
				.dirty = released->document.dirty });
			batch = Commit(std::move(changes));
			result = { .status = EEditorOperationStatus::Succeeded, .revision = batch->revision, .changeBatch = batch };
		}
		RememberCompleted(request.operation.operationId, fingerprint, result);
		if (batch) drainNotifications = EnqueueNotification(*batch);
	}
	if (drainNotifications) DrainNotifications();
	return result;
}

EditorOperationResult EditorCoreService::OpenResolvedInput(const OpenResolvedInputRequest& request)
{
	std::optional<EditorCoreChangeBatch> batch;
	EditorOperationResult result;
	bool drainNotifications = false;
	{
		std::scoped_lock lock(m_mutex);
		if (!IsValidOperationId(request.operation)) {
			return { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::InvalidOperationId, .revision = m_revision };
		}
		std::wstring inputDocumentKey;
		if (!request.input.IsValid() || !TryValidatedIdentityKey(request.input.documentIdentity, inputDocumentKey)) {
			return { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::InvalidInput, .revision = m_revision };
		}
		if (request.resolvedDocument) {
			std::wstring key;
			if (!request.resolvedDocument->IsValid() || !TryValidatedIdentityKey(request.resolvedDocument->identity, key)
				|| inputDocumentKey != key) {
				return { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::DocumentNotResolved, .revision = m_revision };
			}
		}
		const auto fingerprint = Fingerprint(request);
		bool handled = false;
		result = CheckCompletedOrConflict(request.operation, fingerprint, handled);
		if (handled) return result;
		if (request.operation.expectedModelRevision && *request.operation.expectedModelRevision != m_revision) {
			result = RevisionConflictResult();
		} else if (m_revision == std::numeric_limits<std::uint64_t>::max()) {
			result = RevisionConflictResult();
		} else if (m_group.Contains(request.input.inputId)) {
			result = { .status = EEditorOperationStatus::NotApplicable, .reason = EEditorOperationReason::InputAlreadyOpen, .revision = m_revision };
		} else {
			std::optional<DocumentRegistry::AcquireResult> acquired;
			if (request.resolvedDocument) {
				acquired = m_documents.AcquireInput(*request.resolvedDocument);
			} else {
				acquired = m_documents.AcquireExistingInput(inputDocumentKey);
			}
			if (!acquired) {
				result = { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::DocumentNotResolved, .revision = m_revision };
			} else if (acquired->authoritativeStateConflict) {
				result = { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::DocumentStateConflict, .revision = m_revision };
			} else {
				bool opened = false;
				try {
					opened = m_group.Open(request.input, acquired->document.documentKey, request.activate);
				} catch (...) {
					// Allocation failure while opening must not leak the acquired input reference.
				}
				if (!opened) {
					// This cannot follow the validation above; retain an explicit terminal result if the model changes.
					m_documents.ReleaseInput(acquired->document.documentKey);
					result = { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::InvalidInput, .revision = m_revision };
				} else {
					std::vector<EditorCoreChange> changes;
					if (acquired->added) changes.push_back({ .kind = EEditorCoreChangeKind::DocumentAdded,
						.documentKey = acquired->document.documentKey, .documentRevision = acquired->document.documentRevision,
						.dirty = acquired->document.dirty });
					changes.push_back({ .kind = EEditorCoreChangeKind::InputOpened, .inputId = request.input.inputId,
						.documentKey = acquired->document.documentKey });
					// The input ID was proven absent while holding m_mutex, so selecting this newly added
					// input necessarily changes the active selection. An inactive open never changes it.
					if (request.activate) {
						changes.push_back({ .kind = EEditorCoreChangeKind::ActiveInputChanged,
							.activeInputId = request.input.inputId });
					}
					batch = Commit(std::move(changes));
					result = { .status = EEditorOperationStatus::Succeeded, .reason = EEditorOperationReason::None,
						.revision = batch->revision, .changeBatch = batch };
				}
			}
		}
		RememberCompleted(request.operation.operationId, fingerprint, result);
		if (batch) drainNotifications = EnqueueNotification(*batch);
	}
	if (drainNotifications) DrainNotifications();
	return result;
}

EditorOperationResult EditorCoreService::ShowInput(const ShowInputRequest& request)
{
	std::optional<EditorCoreChangeBatch> batch;
	EditorOperationResult result;
	bool drainNotifications = false;
	{
		std::scoped_lock lock(m_mutex);
		if (!IsValidOperationId(request.operation)) {
			return { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::InvalidOperationId, .revision = m_revision };
		}
		if (!IsValidEditorExternalId(request.inputId, kMaxEditorInputIdLength)) {
			return { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::InvalidInput, .revision = m_revision };
		}
		const auto fingerprint = Fingerprint(request);
		bool handled = false;
		result = CheckCompletedOrConflict(request.operation, fingerprint, handled);
		if (handled) return result;
		if (request.operation.expectedModelRevision && *request.operation.expectedModelRevision != m_revision) {
			result = RevisionConflictResult();
		} else if (m_revision == std::numeric_limits<std::uint64_t>::max()) {
			result = RevisionConflictResult();
		} else if (!m_group.Contains(request.inputId)) {
			result = { .status = EEditorOperationStatus::NotApplicable, .reason = EEditorOperationReason::InputNotFound, .revision = m_revision };
		} else if (!m_group.Show(request.inputId)) {
			result = { .status = EEditorOperationStatus::NotApplicable, .reason = EEditorOperationReason::AlreadyActive, .revision = m_revision };
		} else {
			batch = Commit({ { .kind = EEditorCoreChangeKind::ActiveInputChanged, .activeInputId = request.inputId } });
			result = { .status = EEditorOperationStatus::Succeeded, .reason = EEditorOperationReason::None,
				.revision = batch->revision, .changeBatch = batch };
		}
		RememberCompleted(request.operation.operationId, fingerprint, result);
		if (batch) drainNotifications = EnqueueNotification(*batch);
	}
	if (drainNotifications) DrainNotifications();
	return result;
}

EditorOperationResult EditorCoreService::SetDocumentState(const SetDocumentStateRequest& request)
{
	std::optional<EditorCoreChangeBatch> batch;
	EditorOperationResult result;
	bool drainNotifications = false;
	{
		std::scoped_lock lock(m_mutex);
		if (!IsValidOperationId(request.operation)) {
			return { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::InvalidOperationId, .revision = m_revision };
		}
		if (!IsValidEditorExternalId(request.inputId, kMaxEditorInputIdLength)) {
			return { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::InvalidInput, .revision = m_revision };
		}
		const auto fingerprint = Fingerprint(request);
		bool handled = false;
		result = CheckCompletedOrConflict(request.operation, fingerprint, handled);
		if (handled) return result;
		if (request.operation.expectedModelRevision && *request.operation.expectedModelRevision != m_revision) {
			result = RevisionConflictResult();
		} else if (m_revision == std::numeric_limits<std::uint64_t>::max()) {
			result = RevisionConflictResult();
		} else if (const auto documentKey = m_group.DocumentKeyFor(request.inputId); !documentKey) {
			result = { .status = EEditorOperationStatus::NotApplicable, .reason = EEditorOperationReason::InputNotFound, .revision = m_revision };
		} else if (!m_documents.Find(*documentKey)) {
			result = { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::DocumentNotFound, .revision = m_revision };
		} else if (!m_documents.SetState(*documentKey, request.dirty, request.documentRevision)) {
			result = { .status = EEditorOperationStatus::NotApplicable, .reason = EEditorOperationReason::NoDocumentStateChange, .revision = m_revision };
		} else {
			batch = Commit({ { .kind = EEditorCoreChangeKind::DocumentStateChanged, .inputId = request.inputId,
				.documentKey = *documentKey, .documentRevision = request.documentRevision, .dirty = request.dirty } });
			result = { .status = EEditorOperationStatus::Succeeded, .reason = EEditorOperationReason::None,
				.revision = batch->revision, .changeBatch = batch };
		}
		RememberCompleted(request.operation.operationId, fingerprint, result);
		if (batch) drainNotifications = EnqueueNotification(*batch);
	}
	if (drainNotifications) DrainNotifications();
	return result;
}

EditorOperationResult EditorCoreService::ReplaceInputDocument(const ReplaceInputDocumentRequest& request)
{
	std::optional<EditorCoreChangeBatch> batch;
	EditorOperationResult result;
	bool drainNotifications = false;
	{
		std::scoped_lock lock(m_mutex);
		if (!IsValidOperationId(request.operation)) {
			return { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::InvalidOperationId, .revision = m_revision };
		}
		std::wstring targetDocumentKey;
		if (!IsValidEditorExternalId(request.inputId, kMaxEditorInputIdLength) || !request.resolvedDocument
			|| !request.resolvedDocument->IsValid()
			|| !TryValidatedIdentityKey(request.resolvedDocument->identity, targetDocumentKey)) {
			return { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::InvalidInput, .revision = m_revision };
		}
		const auto fingerprint = Fingerprint(request);
		bool handled = false;
		result = CheckCompletedOrConflict(request.operation, fingerprint, handled);
		if (handled) return result;
		if (request.operation.expectedModelRevision && *request.operation.expectedModelRevision != m_revision) {
			result = RevisionConflictResult();
		} else if (m_revision == std::numeric_limits<std::uint64_t>::max()) {
			result = RevisionConflictResult();
		} else if (const auto sourceDocumentKey = m_group.DocumentKeyFor(request.inputId); !sourceDocumentKey) {
			result = { .status = EEditorOperationStatus::NotApplicable, .reason = EEditorOperationReason::InputNotFound, .revision = m_revision };
		} else if (const auto sourceDocument = m_documents.Find(*sourceDocumentKey); !sourceDocument) {
			result = { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::DocumentNotFound, .revision = m_revision };
		} else if (sourceDocument->inputReferenceCount == 0) {
			// A visible input without a registry input reference is an invariant violation; do not begin
			// a cross-document transfer from it because that could not be rolled back safely.
			result = { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::DocumentNotFound, .revision = m_revision };
		} else {
			const auto targetDocument = m_documents.Find(targetDocumentKey);
			const bool targetStateMatches = !targetDocument
				|| (targetDocument->documentRevision == request.resolvedDocument->documentRevision
					&& targetDocument->dirty == request.resolvedDocument->dirty);
			if (!targetStateMatches) {
				result = { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::DocumentStateConflict, .revision = m_revision };
			} else if (*sourceDocumentKey == targetDocumentKey) {
				result = { .status = EEditorOperationStatus::NotApplicable, .reason = EEditorOperationReason::NoDocumentStateChange, .revision = m_revision };
			} else {
				// Materialize every notification value before either reference graph or visible group changes.
				std::vector<EditorCoreChange> changes;
				changes.reserve(targetDocument ? 2U : 3U);
				if (!targetDocument) changes.push_back({ .kind = EEditorCoreChangeKind::DocumentAdded,
					.documentKey = targetDocumentKey, .documentRevision = request.resolvedDocument->documentRevision,
					.dirty = request.resolvedDocument->dirty });
				changes.push_back({ .kind = EEditorCoreChangeKind::InputDocumentReplaced, .inputId = request.inputId,
					.documentKey = targetDocumentKey });
				const bool sourceWillDispose = sourceDocument->inputReferenceCount == 1
					&& sourceDocument->resolverReferenceCount == 0;
				if (sourceWillDispose) changes.push_back({ .kind = EEditorCoreChangeKind::DocumentReleased,
					.documentKey = *sourceDocumentKey, .documentRevision = sourceDocument->documentRevision,
					.dirty = sourceDocument->dirty });

				std::optional<DocumentRegistry::AcquireResult> acquired;
				try {
					acquired = m_documents.AcquireInput(*request.resolvedDocument);
				} catch (...) {
					// Acquire guarantees no mutation before an allocation failure can escape.
				}
				if (!acquired) {
					result = { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::InvalidInput, .revision = m_revision };
				} else if (acquired->authoritativeStateConflict) {
					result = { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::DocumentStateConflict, .revision = m_revision };
				} else {
					bool replaced = false;
					try {
						replaced = m_group.ReplaceDocument(request.inputId, request.resolvedDocument->identity, targetDocumentKey);
					} catch (...) {
						// All mutable model state remains unchanged until EditorGroupModel has prepared its replacement.
					}
					if (!replaced) {
						// This rollback does not allocate and therefore cannot leave a partially acquired target reference.
						(void)m_documents.ReleaseInputReferenceWithoutSnapshot(acquired->document.documentKey);
						result = { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::InvalidInput, .revision = m_revision };
					} else {
						// The source input reference was validated while holding m_mutex, so this non-allocating
						// release cannot fail and no post-replacement rollback path remains.
						(void)m_documents.ReleaseInputReferenceWithoutSnapshot(*sourceDocumentKey);
						batch = Commit(std::move(changes));
						result = { .status = EEditorOperationStatus::Succeeded, .reason = EEditorOperationReason::None,
							.revision = batch->revision, .changeBatch = batch };
					}
				}
			}
		}
		RememberCompleted(request.operation.operationId, fingerprint, result);
		if (batch) drainNotifications = EnqueueNotification(*batch);
	}
	if (drainNotifications) DrainNotifications();
	return result;
}

EditorOperationResult EditorCoreService::CloseInput(const CloseInputRequest& request)
{
	std::optional<EditorCoreChangeBatch> batch;
	EditorOperationResult result;
	bool drainNotifications = false;
	{
		std::scoped_lock lock(m_mutex);
		if (!IsValidOperationId(request.operation)) {
			return { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::InvalidOperationId, .revision = m_revision };
		}
		if (!IsValidEditorExternalId(request.inputId, kMaxEditorInputIdLength)) {
			return { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::InvalidInput, .revision = m_revision };
		}
		const auto fingerprint = Fingerprint(request);
		bool handled = false;
		result = CheckCompletedOrConflict(request.operation, fingerprint, handled);
		if (handled) return result;
		if (request.operation.expectedModelRevision && *request.operation.expectedModelRevision != m_revision) {
			result = RevisionConflictResult();
		} else if (m_revision == std::numeric_limits<std::uint64_t>::max()) {
			result = RevisionConflictResult();
		} else {
			const auto before = m_group.Snapshot().activeInputId;
			const auto closed = m_group.Close(request.inputId);
			if (!closed) {
				result = { .status = EEditorOperationStatus::NotApplicable, .reason = EEditorOperationReason::InputNotFound, .revision = m_revision };
			} else {
				const auto released = m_documents.ReleaseInput(closed->documentKey);
				const auto after = m_group.Snapshot().activeInputId;
				std::vector<EditorCoreChange> changes;
				if (before != after) changes.push_back({ .kind = EEditorCoreChangeKind::ActiveInputChanged, .activeInputId = after });
				changes.push_back({ .kind = EEditorCoreChangeKind::InputClosed, .inputId = closed->descriptor.inputId,
					.documentKey = closed->documentKey });
				if (released && released->disposed) changes.push_back({ .kind = EEditorCoreChangeKind::DocumentReleased,
					.documentKey = released->document.documentKey, .documentRevision = released->document.documentRevision,
					.dirty = released->document.dirty });
				batch = Commit(std::move(changes));
				result = { .status = EEditorOperationStatus::Succeeded, .reason = EEditorOperationReason::None,
					.revision = batch->revision, .changeBatch = batch };
			}
		}
		RememberCompleted(request.operation.operationId, fingerprint, result);
		if (batch) drainNotifications = EnqueueNotification(*batch);
	}
	if (drainNotifications) DrainNotifications();
	return result;
}

std::unique_ptr<IEditorCoreSubscription> EditorCoreService::Subscribe(EditorCoreChangeCallback callback)
{
	if (!callback) return nullptr;
	std::scoped_lock lock(m_subscriptionState->mutex);
	if (m_subscriptionState->closed) return nullptr;
	const auto id = m_subscriptionState->nextSubscriptionId++;
	m_subscriptionState->slots.emplace(id, std::make_shared<SubscriptionSlot>(std::move(callback)));
	return std::make_unique<EditorCoreSubscription>(m_subscriptionState, id);
}

std::string EditorCoreService::Fingerprint(const OpenResolvedInputRequest& request)
{
	std::string result = "open|";
	AppendOperation(result, request.operation);
	AppendField(result, request.input.inputId);
	AppendIdentity(result, request.input.documentIdentity);
	AppendField(result, request.activate ? "activate" : "inactive");
	if (request.resolvedDocument) {
		AppendField(result, "resolved");
		AppendIdentity(result, request.resolvedDocument->identity);
		AppendField(result, std::to_string(request.resolvedDocument->documentRevision));
		AppendField(result, request.resolvedDocument->dirty ? "dirty" : "clean");
	} else {
		AppendField(result, "not-resolved");
	}
	return result;
}

std::string EditorCoreService::Fingerprint(const ResolveDocumentRequest& request)
{
	std::string result = "resolve|";
	AppendOperation(result, request.operation);
	if (request.resolvedDocument) {
		AppendField(result, "resolved");
		AppendIdentity(result, request.resolvedDocument->identity);
		AppendField(result, std::to_string(request.resolvedDocument->documentRevision));
		AppendField(result, request.resolvedDocument->dirty ? "dirty" : "clean");
	} else {
		AppendField(result, "not-resolved");
	}
	return result;
}

std::string EditorCoreService::Fingerprint(const ReleaseDocumentRequest& request)
{
	std::string result = "release-document|";
	AppendOperation(result, request.operation);
	AppendIdentity(result, request.identity);
	return result;
}

std::string EditorCoreService::Fingerprint(const ShowInputRequest& request)
{
	std::string result = "show|";
	AppendOperation(result, request.operation);
	AppendField(result, request.inputId);
	return result;
}

std::string EditorCoreService::Fingerprint(const SetDocumentStateRequest& request)
{
	std::string result = "state|";
	AppendOperation(result, request.operation);
	AppendField(result, request.inputId);
	AppendField(result, request.dirty ? "dirty" : "clean");
	AppendField(result, std::to_string(request.documentRevision));
	return result;
}

std::string EditorCoreService::Fingerprint(const ReplaceInputDocumentRequest& request)
{
	std::string result = "replace-input-document|";
	AppendOperation(result, request.operation);
	AppendField(result, request.inputId);
	if (request.resolvedDocument) {
		AppendField(result, "resolved");
		AppendIdentity(result, request.resolvedDocument->identity);
		AppendField(result, std::to_string(request.resolvedDocument->documentRevision));
		AppendField(result, request.resolvedDocument->dirty ? "dirty" : "clean");
	} else {
		AppendField(result, "not-resolved");
	}
	return result;
}

std::string EditorCoreService::Fingerprint(const CloseInputRequest& request)
{
	std::string result = "close|";
	AppendOperation(result, request.operation);
	AppendField(result, request.inputId);
	return result;
}

EditorOperationResult EditorCoreService::CheckCompletedOrConflict(
	const EditorOperationMetadata& operation, const std::string& fingerprint, bool& handled) const
{
	handled = true;
	if (!IsValidEditorExternalId(operation.operationId, kMaxEditorOperationIdLength)) {
		return { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::InvalidOperationId, .revision = m_revision };
	}
	if (const auto completed = m_completedOperations.find(operation.operationId); completed != m_completedOperations.end()) {
		if (completed->second.fingerprint == fingerprint) {
			auto replay = completed->second.result;
			replay.replayed = true;
			return replay;
		}
		return { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::OperationIdConflict, .revision = m_revision };
	}
	handled = false;
	return {};
}

EditorOperationResult EditorCoreService::RevisionConflictResult() const
{
	return { .status = EEditorOperationStatus::Failed, .reason = EEditorOperationReason::RevisionConflict, .revision = m_revision };
}

EditorCoreChangeBatch EditorCoreService::Commit(std::vector<EditorCoreChange> changes)
{
	const auto baseRevision = m_revision;
	++m_revision;
	return { .generation = m_generation, .baseRevision = baseRevision, .revision = m_revision, .changes = std::move(changes) };
}

void EditorCoreService::RememberCompleted(const std::string& operationId, std::string fingerprint,
	const EditorOperationResult& result)
{
	if (operationId.empty()) return;
	m_completedOperations.emplace(operationId, CompletedOperation{ .fingerprint = std::move(fingerprint), .result = result });
	m_completedOperationOrder.push_back(operationId);
	while (m_completedOperationOrder.size() > m_maxCompletedOperations) {
		m_completedOperations.erase(m_completedOperationOrder.front());
		m_completedOperationOrder.pop_front();
	}
}

bool EditorCoreService::EnqueueNotification(const EditorCoreChangeBatch& batch)
{
	std::scoped_lock lock(m_notificationMutex);
	m_notificationQueue.push_back(batch);
	if (m_dispatchingNotifications) return false;
	m_dispatchingNotifications = true;
	return true;
}

void EditorCoreService::DrainNotifications()
{
	for (;;) {
		EditorCoreChangeBatch batch;
		{
			std::scoped_lock lock(m_notificationMutex);
			if (m_notificationQueue.empty()) {
				m_dispatchingNotifications = false;
				return;
			}
			batch = std::move(m_notificationQueue.front());
			m_notificationQueue.pop_front();
		}
		DeliverNotification(m_subscriptionState, batch);
	}
}

} // namespace workbench::editor
