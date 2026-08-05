/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "config/SettingsWritebackCoordinator.h"

#include <sakura/uri/UriIdentity.h>

#include <utility>

namespace config {

namespace {

bool IsSameUri(const std::optional<platform::uri::Uri>& left, const std::optional<platform::uri::Uri>& right) noexcept
{
	return left.has_value() == right.has_value()
		&& (!left || platform::uri::UriIdentityService::IsEqual(*left, *right));
}

bool IsSameBaseTarget(const ConfigurationTarget& source, const ConfigurationTarget& edit) noexcept
{
	return source.profileId == edit.profileId
		&& !source.languageId
		&& IsSameUri(source.workspaceUri, edit.workspaceUri)
		&& IsSameUri(source.folderUri, edit.folderUri);
}

bool MatchesDocumentScope(
	editing::EConfigurationDocumentScope documentScope,
	EConfigurationScope sourceScope) noexcept
{
	switch (documentScope) {
	case editing::EConfigurationDocumentScope::Profile:
	case editing::EConfigurationDocumentScope::User:
		return sourceScope == EConfigurationScope::Profile;
	case editing::EConfigurationDocumentScope::Workspace:
		return sourceScope == EConfigurationScope::Workspace;
	case editing::EConfigurationDocumentScope::Folder:
		return sourceScope == EConfigurationScope::Folder;
	case editing::EConfigurationDocumentScope::LanguageOverride:
		return sourceScope == EConfigurationScope::Profile
			|| sourceScope == EConfigurationScope::Workspace
			|| sourceScope == EConfigurationScope::Folder;
	}
	return false;
}

} // namespace

CSettingsWritebackCoordinator::CSettingsWritebackCoordinator(
	platform::filesystem::IFileService& fileService,
	CConfigurationFileSourceController& fileSources) noexcept
	: m_fileSources(fileSources)
	, m_editor(fileService)
{
}

SettingsWritebackResult CSettingsWritebackCoordinator::Result(
	ESettingsWritebackStatus status,
	std::string diagnostic)
{
	SettingsWritebackResult result;
	result.status = status;
	result.diagnostic = std::move(diagnostic);
	return result;
}

bool CSettingsWritebackCoordinator::IsValidRequest(const SettingsWritebackRequest& request) noexcept
{
	if (request.documentKey.empty() || request.source.sourceId.empty() || !request.edit.target.resource) return false;
	if (!IsSameBaseTarget(request.source.target, request.edit.target.target)) return false;
	return MatchesDocumentScope(request.edit.target.scope, request.source.scope);
}

SettingsWritebackResult CSettingsWritebackCoordinator::Write(const SettingsWritebackRequest& request)
{
	std::lock_guard lock(m_mutex);
	if (m_stopped) return Result(ESettingsWritebackStatus::Stopped, "settings writeback is stopped");
	if (!IsValidRequest(request)) return Result(ESettingsWritebackStatus::InvalidRequest, "settings writeback request does not match its source owner");

	bool replayed = false;
	try {
		for (std::size_t attempt = 1; attempt <= kMaximumAttempts; ++attempt) {
			auto edited = m_editor.Edit(request.edit);
			if (edited.status == editing::EConfigurationDocumentEditStatus::Conflict && attempt < kMaximumAttempts) {
				replayed = true;
				continue;
			}
			if (!edited.Succeeded()) {
				auto result = Result(
				edited.status == editing::EConfigurationDocumentEditStatus::Conflict
					? ESettingsWritebackStatus::Conflict : ESettingsWritebackStatus::EditRejected,
				edited.status == editing::EConfigurationDocumentEditStatus::Conflict
					? "settings writeback remained conflicted after bounded replay"
					: "settings document edit did not reach an accepted terminal state");
				result.attempts = attempt;
				result.edit = std::move(edited);
				return result;
			}

			auto resnapshot = m_fileSources.Reload(
				request.documentKey, request.source, *request.edit.target.resource);
			if (!resnapshot.Succeeded()) {
				auto result = Result(ESettingsWritebackStatus::ResnapshotRejected,
					"settings document was not accepted by its source owner after writeback");
				result.attempts = attempt;
				result.edit = std::move(edited);
				result.resnapshot = std::move(resnapshot);
				return result;
			}

			auto result = Result(replayed ? ESettingsWritebackStatus::Replayed
				: (edited.status == editing::EConfigurationDocumentEditStatus::NoChange
					? ESettingsWritebackStatus::NoChange : ESettingsWritebackStatus::Applied),
				replayed ? "settings writeback replayed after a conflict"
				: "settings writeback and resnapshot completed");
			result.attempts = attempt;
			result.edit = std::move(edited);
			result.resnapshot = std::move(resnapshot);
			return result;
		}
	} catch (...) {
		return Result(ESettingsWritebackStatus::Failed, "settings writeback failed unexpectedly");
	}
	return Result(ESettingsWritebackStatus::Failed, "settings writeback exhausted without a terminal result");
}

SettingsWritebackResult CSettingsWritebackCoordinator::Stop() noexcept
{
	try {
		std::lock_guard lock(m_mutex);
		m_stopped = true;
		return Result(ESettingsWritebackStatus::Stopped, "settings writeback is stopped");
	} catch (...) {
		return { .status = ESettingsWritebackStatus::Stopped };
	}
}

bool CSettingsWritebackCoordinator::IsStopped() const noexcept
{
	try {
		std::lock_guard lock(m_mutex);
		return m_stopped;
	} catch (...) {
		return true;
	}
}

} // namespace config
