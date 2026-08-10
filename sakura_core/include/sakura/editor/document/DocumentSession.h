/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#pragma once

#include <sakura/editor/document/DocumentPersistence.h>
#include <sakura/editor/document/TextDocument.h>
#include <sakura/editor/document/UndoHistory.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace editor::document {

enum class EDocumentSessionTerminal : std::uint8_t {
	Succeeded,
	NoChange,
	Cancelled,
	Failed,
	RolledBack,
	Conflict,
	InvalidRequest,
};

class DocumentSessionResult final {
public:
	DocumentSessionResult(EDocumentSessionTerminal terminal = EDocumentSessionTerminal::Failed,
		std::uint64_t version = 0, bool dirty = false) noexcept
		: m_terminal(terminal), m_version(version), m_dirty(dirty) {}
	[[nodiscard]] EDocumentSessionTerminal Terminal() const noexcept { return m_terminal; }
	[[nodiscard]] std::uint64_t Version() const noexcept { return m_version; }
	[[nodiscard]] bool IsDirty() const noexcept { return m_dirty; }

private:
	const EDocumentSessionTerminal m_terminal;
	const std::uint64_t m_version;
	const bool m_dirty;
};

//! The aggregate root for one presentation-neutral document session.
//!
//! It owns the current TextDocument, undo/redo history, saved-content baseline,
//! and resource identity. Persistence and every native adapter remain outside.
class DocumentSession final {
public:
	explicit DocumentSession(std::string initialText = {});

	[[nodiscard]] const TextDocument& Document() const noexcept { return m_document; }
	[[nodiscard]] const UndoHistory& History() const noexcept { return m_history; }
	[[nodiscard]] bool IsDirty() const noexcept { return m_document.Text() != m_savedContent; }
	[[nodiscard]] const std::string& ResourceId() const noexcept { return m_resourceId; }

	[[nodiscard]] DocumentSessionResult Replace(TextRange range, std::string_view replacement) noexcept;
	[[nodiscard]] DocumentSessionResult Undo() noexcept;
	[[nodiscard]] DocumentSessionResult Redo() noexcept;
	[[nodiscard]] DocumentSessionResult Load(std::string resourceId, IDocumentPersistence& persistence) noexcept;
	[[nodiscard]] DocumentSessionResult Save(IDocumentPersistence& persistence) noexcept;

private:
	[[nodiscard]] DocumentSessionResult Result(EDocumentSessionTerminal terminal) const noexcept;
	[[nodiscard]] bool RestoreText(const std::string& text) noexcept;

	TextDocument m_document;
	UndoHistory m_history;
	std::string m_savedContent;
	std::string m_resourceId;
};

} // namespace editor::document
