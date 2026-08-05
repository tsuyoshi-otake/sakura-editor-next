/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "config/ConfigurationFileSourceController.h"

#include <sakura/uri/UriIdentity.h>

#include <atomic>
#include <limits>
#include <utility>

namespace config {

namespace {

using EStatus = EConfigurationFileSourceControllerStatus;
using platform::filesystem::EFileResultStatus;

std::atomic<std::uint64_t> g_nextFileSourceOperation { 1 };

std::optional<std::uint64_t> NextProcessOperationSequence() noexcept
{
	auto current = g_nextFileSourceOperation.load(std::memory_order_relaxed);
	while (current != (std::numeric_limits<std::uint64_t>::max)()) {
		if (g_nextFileSourceOperation.compare_exchange_weak(
			current, current + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
			return current;
		}
	}
	return std::nullopt;
}

ConfigurationFileSourceControllerResult Failure(
	EStatus status,
	std::string diagnostic,
	std::optional<EFileResultStatus> fileStatus = std::nullopt)
{
	ConfigurationFileSourceControllerResult result;
	result.status = status;
	result.fileStatus = fileStatus;
	result.diagnostic = std::move(diagnostic);
	return result;
}

EStatus StatusFor(EConfigurationOutcome outcome) noexcept
{
	switch (outcome) {
	case EConfigurationOutcome::Applied: return EStatus::Applied;
	case EConfigurationOutcome::NoChange: return EStatus::NoChange;
	case EConfigurationOutcome::Replayed: return EStatus::Replayed;
	case EConfigurationOutcome::Conflict: return EStatus::Conflict;
	case EConfigurationOutcome::OperationIdConflict: return EStatus::OperationIdConflict;
	default: return EStatus::ConfigurationRejected;
	}
}

bool IsSameTarget(const ConfigurationTarget& left, const ConfigurationTarget& right) noexcept
{
	if (left.profileId != right.profileId || left.languageId != right.languageId
		|| left.workspaceUri.has_value() != right.workspaceUri.has_value()
		|| left.folderUri.has_value() != right.folderUri.has_value()) {
		return false;
	}
	if (left.workspaceUri && !platform::uri::UriIdentityService::IsEqual(*left.workspaceUri, *right.workspaceUri)) {
		return false;
	}
	return !left.folderUri || platform::uri::UriIdentityService::IsEqual(*left.folderUri, *right.folderUri);
}

} // namespace

CConfigurationFileSourceController::CConfigurationFileSourceController(
	platform::filesystem::IFileService& fileService,
	IConfigurationService& configurationService) noexcept
	: m_fileService(fileService)
	, m_configurationService(configurationService)
{
}

std::optional<std::string> CConfigurationFileSourceController::NextOperationId()
{
	const auto sequence = NextProcessOperationSequence();
	if (!sequence) return std::nullopt;
	// IConfigurationService deduplicates operation IDs across every source and
	// controller bound to the service. A process-global sequence therefore keeps
	// independently composed controllers from colliding in that replay ledger.
	return "sakura.config.file-source-controller.v1/" + std::to_string(*sequence);
}

bool CConfigurationFileSourceController::IsSameIdentity(
	const TrackedDocument& tracked,
	const ConfigurationSource& source,
	const platform::uri::Uri& resource) noexcept
{
	return tracked.source.scope == source.scope
		&& tracked.source.sourceId == source.sourceId
		&& tracked.source.priority == source.priority
		&& IsSameTarget(tracked.source.target, source.target)
		&& tracked.resourceIdentity == platform::uri::UriIdentityService::MakeComparisonKey(resource);
}

void CConfigurationFileSourceController::RememberAcceptedRevisions(
	TrackedDocument& tracked,
	const ConfigurationBatchResult& result)
{
	JsoncConfigurationSourceRevisions revisions;
	for (const auto& sourceRevision : result.revisions) {
		if (sourceRevision.source.scope == EConfigurationScope::LanguageOverride
			&& sourceRevision.source.target.languageId) {
			revisions.languageRevisions[*sourceRevision.source.target.languageId] = sourceRevision.revision;
		} else if (sourceRevision.source.scope == tracked.source.scope
			&& sourceRevision.source.sourceId == tracked.source.sourceId
			&& IsSameTarget(sourceRevision.source.target, tracked.source.target)) {
			revisions.baseRevision = sourceRevision.revision;
		}
	}
	tracked.revisions = std::move(revisions);
}

ConfigurationFileSourceControllerResult CConfigurationFileSourceController::ApplyDocument(
	TrackedDocument& tracked,
	std::string_view utf8,
	bool resourceWasMissing)
{
	auto operationId = NextOperationId();
	if (!operationId) {
		return Failure(EStatus::OperationIdExhausted, "configuration file operation identifier space is exhausted");
	}

	auto applied = CJsoncConfigurationSource::Apply(
		m_configurationService, utf8, tracked.source, std::move(*operationId), tracked.revisions);
	if (!applied.Parsed()) {
		ConfigurationFileSourceControllerResult result = Failure(EStatus::ParseFailed, "configuration document could not be parsed");
		result.jsoncDiagnostic = std::move(applied.diagnostic);
		return result;
	}

	ConfigurationFileSourceControllerResult result;
	result.status = StatusFor(applied.result.outcome);
	result.configurationOutcome = applied.result.outcome;
	result.resourceWasMissing = resourceWasMissing;
	if (result.Succeeded()) {
		RememberAcceptedRevisions(tracked, applied.result);
		return result;
	}
	if (result.status == EStatus::Conflict) {
		result.diagnostic = "configuration document changed concurrently";
	} else if (result.status == EStatus::OperationIdConflict) {
		result.diagnostic = "configuration operation identifier was reused for a different request";
	} else {
		result.diagnostic = "configuration service rejected the document replacement";
	}
	return result;
}

ConfigurationFileSourceControllerResult CConfigurationFileSourceController::Reload(
	std::string_view documentKey,
	const ConfigurationSource& source,
	const platform::uri::Uri& resource)
{
	std::lock_guard lock(m_mutex);
	if (documentKey.empty()) {
		return Failure(EStatus::IdentityConflict, "configuration document key is required");
	}

	const std::string key(documentKey);
	auto found = m_documents.find(key);
	if (found != m_documents.end() && !IsSameIdentity(found->second, source, resource)) {
		return Failure(EStatus::IdentityConflict, "configuration document identity changed for a tracked key");
	}

	const auto read = m_fileService.Read(resource, { .maximumBytes = CJsoncConfigurationSource::kMaximumInputBytes });
	if (!read.Succeeded() && read.status != EFileResultStatus::NotFound) {
		return Failure(EStatus::ReadFailed, "configuration document could not be read", read.status);
	}
	if (read.Succeeded() && !read.value) {
		return Failure(EStatus::ReadFailed, "configuration document read returned no bytes", read.status);
	}

	const bool isNew = found == m_documents.end();
	TrackedDocument candidate;
	TrackedDocument* tracked = nullptr;
	if (isNew) {
		candidate.source = source;
		candidate.resourceIdentity = platform::uri::UriIdentityService::MakeComparisonKey(resource);
		tracked = &candidate;
	} else {
		tracked = &found->second;
	}

	if (read.status == EFileResultStatus::NotFound) {
		auto result = ApplyDocument(*tracked, "{}", true);
		result.fileStatus = EFileResultStatus::NotFound;
		if (result.Succeeded() && isNew) {
			m_documents.emplace(key, std::move(candidate));
		}
		return result;
	}

	const auto* bytes = &*read.value;
	const std::string utf8(bytes->begin(), bytes->end());
	auto result = ApplyDocument(*tracked, utf8, false);
	if (result.Succeeded() && isNew) {
		m_documents.emplace(key, std::move(candidate));
	}
	return result;
}

ConfigurationFileSourceControllerResult CConfigurationFileSourceController::Remove(std::string_view documentKey)
{
	std::lock_guard lock(m_mutex);
	if (documentKey.empty()) {
		return Failure(EStatus::NotTracked, "configuration document key is not tracked");
	}
	auto found = m_documents.find(std::string(documentKey));
	if (found == m_documents.end()) {
		return Failure(EStatus::NotTracked, "configuration document key is not tracked");
	}
	auto result = ApplyDocument(found->second, "{}", false);
	if (result.Succeeded()) {
		m_documents.erase(found);
	}
	return result;
}

} // namespace config
