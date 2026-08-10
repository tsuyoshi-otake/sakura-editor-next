/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace editor::document {

//! Every persistence port call has exactly one typed terminal state.
enum class EDocumentPersistenceTerminal : std::uint8_t {
	Succeeded,
	Cancelled,
	Failed,
};

class DocumentLoadRequest final {
public:
	explicit DocumentLoadRequest(std::string resourceId) : m_resourceId(std::move(resourceId)) {}
	[[nodiscard]] const std::string& ResourceId() const noexcept { return m_resourceId; }

private:
	const std::string m_resourceId;
};

class DocumentSaveRequest final {
public:
	DocumentSaveRequest(std::string resourceId, std::string content, std::uint64_t contentVersion)
		: m_resourceId(std::move(resourceId)), m_content(std::move(content)), m_contentVersion(contentVersion) {}
	[[nodiscard]] const std::string& ResourceId() const noexcept { return m_resourceId; }
	[[nodiscard]] const std::string& Content() const noexcept { return m_content; }
	[[nodiscard]] std::uint64_t ContentVersion() const noexcept { return m_contentVersion; }

private:
	const std::string m_resourceId;
	const std::string m_content;
	const std::uint64_t m_contentVersion;
};

class DocumentLoadResult final {
public:
	DocumentLoadResult(EDocumentPersistenceTerminal terminal = EDocumentPersistenceTerminal::Failed,
		bool complete = false, std::string content = {})
		: m_terminal(terminal), m_complete(complete), m_content(std::move(content)) {}
	[[nodiscard]] EDocumentPersistenceTerminal Terminal() const noexcept { return m_terminal; }
	//! Meaningful only for Succeeded. A successful response with incomplete data is rejected by DocumentSession.
	[[nodiscard]] bool IsComplete() const noexcept { return m_complete; }
	[[nodiscard]] const std::string& Content() const noexcept { return m_content; }

private:
	const EDocumentPersistenceTerminal m_terminal;
	const bool m_complete;
	const std::string m_content;
};

class DocumentSaveResult final {
public:
	DocumentSaveResult(EDocumentPersistenceTerminal terminal = EDocumentPersistenceTerminal::Failed,
		std::uint64_t committedVersion = 0) noexcept
		: m_terminal(terminal), m_committedVersion(committedVersion) {}
	[[nodiscard]] EDocumentPersistenceTerminal Terminal() const noexcept { return m_terminal; }
	//! The port confirms precisely the version it committed. Any other value is a conflict, never a clean save.
	[[nodiscard]] std::uint64_t CommittedVersion() const noexcept { return m_committedVersion; }

private:
	const EDocumentPersistenceTerminal m_terminal;
	const std::uint64_t m_committedVersion;
};

//! File, URI, encoding, dialog, and transport adapters implement this boundary.
//! The document core makes no direct filesystem or UI calls.
class IDocumentPersistence {
public:
	virtual ~IDocumentPersistence() = default;
	[[nodiscard]] virtual DocumentLoadResult Load(const DocumentLoadRequest& request) = 0;
	[[nodiscard]] virtual DocumentSaveResult Save(const DocumentSaveRequest& request) = 0;
};

} // namespace editor::document
