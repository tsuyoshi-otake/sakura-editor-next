/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "workbench/workspace/WorkspaceArtifactDocumentService.h"

#include <limits>
#include <utility>

namespace workbench::workspace {
namespace {

constexpr std::size_t kMaximumTrackedDocuments = 2U * 257U;
constexpr std::size_t kMaximumBatchFolders = 64U;

const wchar_t* ArtifactMemberName(EWorkspaceArtifactDocumentKind kind) noexcept
{
	switch (kind) {
	case EWorkspaceArtifactDocumentKind::Tasks: return L"tasks";
	case EWorkspaceArtifactDocumentKind::Launch: return L"launch";
	}
	return L"";
}

WorkspaceArtifactDocumentResult Failure(EWorkspaceArtifactDocumentStatus status, std::string diagnostic)
{
	return { status, std::nullopt, std::move(diagnostic) };
}

WorkspaceArtifactDocumentResult JsoncFailure(const platform::serialization::JsoncDiagnostic& diagnostic)
{
	using EJsonc = platform::serialization::EJsoncDiagnosticCode;
	auto status = EWorkspaceArtifactDocumentStatus::JsoncParseFailed;
	if (diagnostic.code == EJsonc::InputTooLarge) status = EWorkspaceArtifactDocumentStatus::InputTooLarge;
	else if (diagnostic.code == EJsonc::InvalidUtf8) status = EWorkspaceArtifactDocumentStatus::InvalidUtf8;
	else if (diagnostic.code == EJsonc::DuplicateKey) status = EWorkspaceArtifactDocumentStatus::DuplicateKey;
	return { status, diagnostic, "workspace artifact JSONC document was rejected" };
}

bool IsArray(const platform::serialization::JsoncValue& value) noexcept
{
	return std::holds_alternative<platform::serialization::JsoncValue::Array>(value.Value());
}

bool IsString(const platform::serialization::JsoncValue& value) noexcept
{
	return std::holds_alternative<std::wstring>(value.Value());
}

} // namespace

std::optional<std::wstring> CWorkspaceArtifactDocumentService::MakeKey(
	EWorkspaceArtifactDocumentKind kind,
	EWorkspaceArtifactDocumentSource source,
	const std::optional<platform::uri::Uri>& folderUri)
{
	if (ArtifactMemberName(kind)[0] == L'\0') return std::nullopt;
	if (source == EWorkspaceArtifactDocumentSource::WorkspaceFile) {
		if (folderUri) return std::nullopt;
		return std::wstring(L"workspace/") + ArtifactMemberName(kind);
	}
	if (source != EWorkspaceArtifactDocumentSource::Folder || !folderUri || !IsResourceValid(*folderUri)) return std::nullopt;
	const auto identity = platform::uri::UriIdentityService::MakeComparisonKey(*folderUri);
	if (identity.empty()) return std::nullopt;
	try {
		std::wstring key(L"folder/");
		key.reserve(key.size() + identity.size() + 16U);
		key += ArtifactMemberName(kind);
		key += L"/";
		key += identity;
		return key;
	}
	catch (...) {
		return std::nullopt;
	}
}

bool CWorkspaceArtifactDocumentService::IsResourceValid(const platform::uri::Uri& resource) noexcept
{
	return !resource.Scheme().empty() && !resource.Path().empty()
		&& !resource.Query() && !resource.Fragment()
		&& !platform::uri::UriIdentityService::MakeComparisonKey(resource).empty();
}

bool CWorkspaceArtifactDocumentService::IsSchemaValid(
	EWorkspaceArtifactDocumentKind kind,
	const platform::serialization::JsoncValue::Object& artifact) noexcept
{
	auto optionalString = [&artifact](const wchar_t* key) {
		const auto found = artifact.find(key);
		return found == artifact.end() || IsString(found->second);
	};
	auto optionalArray = [&artifact](const wchar_t* key) {
		const auto found = artifact.find(key);
		return found == artifact.end() || IsArray(found->second);
	};
	switch (kind) {
	case EWorkspaceArtifactDocumentKind::Tasks:
		return optionalString(L"version") && optionalArray(L"tasks");
	case EWorkspaceArtifactDocumentKind::Launch:
		return optionalString(L"version") && optionalArray(L"configurations") && optionalArray(L"compounds");
	}
	return false;
}

WorkspaceArtifactDocumentResult CWorkspaceArtifactDocumentService::Apply(WorkspaceArtifactDocumentUpdate update)
{
	if (update.generation == 0 || update.revision == 0) {
		return Failure(EWorkspaceArtifactDocumentStatus::InvalidSource, "artifact generation and revision are required");
	}
	if (!IsResourceValid(update.resource)) {
		return Failure(EWorkspaceArtifactDocumentStatus::InvalidResource, "artifact resource is invalid");
	}
	const auto key = MakeKey(update.kind, update.source, update.folderUri);
	if (!key) return Failure(EWorkspaceArtifactDocumentStatus::InvalidSource, "artifact source shape is invalid");
	{
		std::lock_guard lock(m_mutex);
		if (m_stopped) return Failure(EWorkspaceArtifactDocumentStatus::Stopped, "workspace artifact service is stopped");
		if (update.generation < m_generation) {
			return Failure(EWorkspaceArtifactDocumentStatus::StaleGeneration, "artifact update belongs to an older generation");
		}
	}

	const auto parsed = platform::serialization::CJsoncDocument::Parse(update.utf8);
	if (!parsed.Succeeded()) return JsoncFailure(*parsed.diagnostic);
	const auto* root = std::get_if<platform::serialization::JsoncValue::Object>(&parsed.value->Value());
	if (!root) return Failure(EWorkspaceArtifactDocumentStatus::RootMustBeObject, "artifact document root must be an object");

	std::optional<platform::serialization::JsoncValue::Object> artifact;
	if (update.source == EWorkspaceArtifactDocumentSource::WorkspaceFile) {
		const auto member = root->find(ArtifactMemberName(update.kind));
		if (member != root->end()) {
			const auto* object = std::get_if<platform::serialization::JsoncValue::Object>(&member->second.Value());
			if (!object) return Failure(EWorkspaceArtifactDocumentStatus::ArtifactMustBeObject, "workspace artifact member must be an object");
			artifact = *object;
		}
	} else {
		artifact = *root;
	}
	if (artifact && !IsSchemaValid(update.kind, *artifact)) {
		return Failure(EWorkspaceArtifactDocumentStatus::InvalidSchema, "workspace artifact schema is invalid");
	}

	WorkspaceArtifactDocumentServiceSnapshot notification;
	WorkspaceArtifactDocumentResult result;
	{
		std::lock_guard lock(m_mutex);
		if (m_stopped) return Failure(EWorkspaceArtifactDocumentStatus::Stopped, "workspace artifact service is stopped");
		if (update.generation < m_generation) {
			return Failure(EWorkspaceArtifactDocumentStatus::StaleGeneration, "artifact update belongs to an older generation");
		}
		if (update.generation > m_generation) {
			m_generation = update.generation;
			m_documents.clear();
		}
		auto found = m_documents.find(*key);
		if (found != m_documents.end()) {
			if (update.revision <= found->second.revision) {
				return Failure(EWorkspaceArtifactDocumentStatus::StaleRevision, "artifact update revision is not newer");
			}
			if (found->second.resourceIdentity != platform::uri::UriIdentityService::MakeComparisonKey(update.resource)) {
				return Failure(EWorkspaceArtifactDocumentStatus::DuplicateSource, "artifact source identity changed within one generation");
			}
		} else if (m_documents.size() >= kMaximumTrackedDocuments) {
			return Failure(EWorkspaceArtifactDocumentStatus::MaximumDocumentsExceeded, "workspace artifact document limit exceeded");
		}

		TrackedDocument& tracked = m_documents[*key];
		tracked.resourceIdentity = platform::uri::UriIdentityService::MakeComparisonKey(update.resource);
		tracked.revision = update.revision;
		if (artifact) {
			tracked.document = WorkspaceArtifactDocument {
				.kind = update.kind,
				.source = update.source,
				.folderUri = std::move(update.folderUri),
				.resource = std::move(update.resource),
				.generation = update.generation,
				.revision = update.revision,
				.rawJsonc = std::move(update.utf8),
				.root = *root,
				.artifact = std::move(*artifact),
			};
			result.status = EWorkspaceArtifactDocumentStatus::Applied;
		} else {
			tracked.document.reset();
			result.status = EWorkspaceArtifactDocumentStatus::Cleared;
		}
		notification = SnapshotLocked();
	}
	Notify(notification);
	return result;
}

WorkspaceArtifactDocumentResult CWorkspaceArtifactDocumentService::Remove(WorkspaceArtifactDocumentRemoval removal)
{
	if (removal.generation == 0 || removal.revision == 0) {
		return Failure(EWorkspaceArtifactDocumentStatus::InvalidSource, "artifact generation and revision are required");
	}
	if (!IsResourceValid(removal.resource)) {
		return Failure(EWorkspaceArtifactDocumentStatus::InvalidResource, "artifact resource is invalid");
	}
	const auto key = MakeKey(removal.kind, removal.source, removal.folderUri);
	if (!key) return Failure(EWorkspaceArtifactDocumentStatus::InvalidSource, "artifact source shape is invalid");

	WorkspaceArtifactDocumentServiceSnapshot notification;
	{
		std::lock_guard lock(m_mutex);
		if (m_stopped) return Failure(EWorkspaceArtifactDocumentStatus::Stopped, "workspace artifact service is stopped");
		if (removal.generation < m_generation) {
			return Failure(EWorkspaceArtifactDocumentStatus::StaleGeneration, "artifact removal belongs to an older generation");
		}
		if (removal.generation > m_generation) {
			m_generation = removal.generation;
			m_documents.clear();
		}
		auto found = m_documents.find(*key);
		if (found != m_documents.end()) {
			if (removal.revision <= found->second.revision) {
				return Failure(EWorkspaceArtifactDocumentStatus::StaleRevision, "artifact removal revision is not newer");
			}
			if (found->second.resourceIdentity != platform::uri::UriIdentityService::MakeComparisonKey(removal.resource)) {
				return Failure(EWorkspaceArtifactDocumentStatus::DuplicateSource, "artifact source identity changed within one generation");
			}
		} else if (m_documents.size() >= kMaximumTrackedDocuments) {
			return Failure(EWorkspaceArtifactDocumentStatus::MaximumDocumentsExceeded, "workspace artifact document limit exceeded");
		}
		TrackedDocument& tracked = m_documents[*key];
		tracked.resourceIdentity = platform::uri::UriIdentityService::MakeComparisonKey(removal.resource);
		tracked.revision = removal.revision;
		tracked.document.reset();
		notification = SnapshotLocked();
	}
	Notify(notification);
	return { EWorkspaceArtifactDocumentStatus::Cleared, std::nullopt, "workspace artifact resource is missing" };
}

WorkspaceArtifactDocumentResult CWorkspaceArtifactDocumentService::BeginGeneration(std::uint64_t generation) noexcept
{
	if (generation == 0) return Failure(EWorkspaceArtifactDocumentStatus::InvalidSource, "artifact generation is required");
	WorkspaceArtifactDocumentServiceSnapshot notification;
	{
		std::lock_guard lock(m_mutex);
		if (m_stopped) return Failure(EWorkspaceArtifactDocumentStatus::Stopped, "workspace artifact service is stopped");
		if (generation < m_generation) return Failure(EWorkspaceArtifactDocumentStatus::StaleGeneration, "artifact generation is older");
		if (generation == m_generation) return { EWorkspaceArtifactDocumentStatus::Applied, std::nullopt, "workspace artifact generation is current" };
		m_generation = generation;
		m_documents.clear();
		notification = SnapshotLocked();
	}
	Notify(notification);
	return { EWorkspaceArtifactDocumentStatus::Cleared, std::nullopt, "workspace artifact generation established" };
}

WorkspaceArtifactDocumentResult CWorkspaceArtifactDocumentService::Stop() noexcept
{
	std::lock_guard lock(m_mutex);
	if (m_stopped) return Failure(EWorkspaceArtifactDocumentStatus::Stopped, "workspace artifact service is stopped");
	m_stopped = true;
	m_documents.clear();
	m_listeners.clear();
	return { EWorkspaceArtifactDocumentStatus::Cleared, std::nullopt, "workspace artifact service stopped" };
}

std::optional<WorkspaceArtifactDocument> CWorkspaceArtifactDocumentService::Resolve(
	EWorkspaceArtifactDocumentKind kind,
	const std::optional<platform::uri::Uri>& folderUri) const
{
	if (folderUri) {
		if (const auto key = MakeKey(kind, EWorkspaceArtifactDocumentSource::Folder, folderUri)) {
			if (const auto found = m_documents.find(*key); found != m_documents.end() && found->second.document) {
				return found->second.document;
			}
		}
	}
	const auto key = MakeKey(kind, EWorkspaceArtifactDocumentSource::WorkspaceFile, std::nullopt);
	if (!key) return std::nullopt;
	const auto found = m_documents.find(*key);
	if (found == m_documents.end()) return std::nullopt;
	return found->second.document;
}

TasksDocumentSnapshot CWorkspaceArtifactDocumentService::Tasks(const std::optional<platform::uri::Uri>& folderUri) const
{
	std::lock_guard lock(m_mutex);
	return { m_stopped ? std::nullopt : Resolve(EWorkspaceArtifactDocumentKind::Tasks, folderUri) };
}

TasksDocumentBatchSnapshot CWorkspaceArtifactDocumentService::TasksForFolders(
	const std::vector<platform::uri::Uri>& folderUris) const
{
	std::lock_guard lock(m_mutex);
	TasksDocumentBatchSnapshot batch;
	batch.service = SnapshotLocked();
	if (folderUris.size() > kMaximumBatchFolders) {
		batch.status = ETasksDocumentBatchStatus::TooManyFolders;
		return batch;
	}
	for (const auto& folderUri : folderUris) {
		if (!IsResourceValid(folderUri)) {
			batch.status = ETasksDocumentBatchStatus::InvalidFolder;
			return batch;
		}
	}
	batch.documents.reserve(folderUris.size());
	if (m_stopped) {
		batch.status = ETasksDocumentBatchStatus::Stopped;
		batch.documents.resize(folderUris.size());
		return batch;
	}
	for (const auto& folderUri : folderUris) {
		batch.documents.push_back({ Resolve(EWorkspaceArtifactDocumentKind::Tasks, std::optional<platform::uri::Uri> { folderUri }) });
	}
	batch.status = ETasksDocumentBatchStatus::Applied;
	return batch;
}

LaunchDocumentSnapshot CWorkspaceArtifactDocumentService::Launch(const std::optional<platform::uri::Uri>& folderUri) const
{
	std::lock_guard lock(m_mutex);
	return { m_stopped ? std::nullopt : Resolve(EWorkspaceArtifactDocumentKind::Launch, folderUri) };
}

WorkspaceArtifactDocumentServiceSnapshot CWorkspaceArtifactDocumentService::SnapshotLocked() const
{
	WorkspaceArtifactDocumentServiceSnapshot snapshot;
	snapshot.generation = m_generation;
	snapshot.stopped = m_stopped;
	for (const auto& [ignored, tracked] : m_documents) {
		(void)ignored;
		if (tracked.document) ++snapshot.acceptedDocuments;
	}
	return snapshot;
}

WorkspaceArtifactDocumentServiceSnapshot CWorkspaceArtifactDocumentService::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	return SnapshotLocked();
}

std::optional<std::uint64_t> CWorkspaceArtifactDocumentService::Subscribe(Listener listener)
{
	if (!listener) return std::nullopt;
	std::lock_guard lock(m_mutex);
	if (m_stopped || m_nextSubscriptionId == (std::numeric_limits<std::uint64_t>::max)()) return std::nullopt;
	const auto id = m_nextSubscriptionId++;
	m_listeners.emplace(id, std::move(listener));
	return id;
}

void CWorkspaceArtifactDocumentService::Unsubscribe(std::uint64_t subscriptionId) noexcept
{
	std::lock_guard lock(m_mutex);
	m_listeners.erase(subscriptionId);
}

void CWorkspaceArtifactDocumentService::Notify(const WorkspaceArtifactDocumentServiceSnapshot& snapshot)
{
	std::vector<Listener> listeners;
	{
		std::lock_guard lock(m_mutex);
		if (m_stopped) return;
		listeners.reserve(m_listeners.size());
		for (const auto& [ignored, listener] : m_listeners) {
			(void)ignored;
			listeners.push_back(listener);
		}
	}
	for (const auto& listener : listeners) {
		try { listener(snapshot); }
		catch (...) { /* Listener ownership must not make routing fail. */ }
	}
}

} // namespace workbench::workspace
