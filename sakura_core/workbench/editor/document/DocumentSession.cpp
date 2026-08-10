/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include <sakura/editor/document/DocumentSession.h>
#include <sakura/editor/document/LayoutProjection.h>

#include <algorithm>
#include <exception>
#include <limits>
#include <optional>
#include <utility>

namespace editor::document {

TextDocument::TextDocument(std::string initialText)
	: m_text(std::move(initialText))
{
}

TextEditResult TextDocument::Replace(TextRange range, std::string_view replacement) noexcept
{
	if (range.Start() > range.End() || range.End() > m_text.size()) {
		return { ETextEditOutcome::InvalidRange, m_version, false };
	}
	if (m_text.compare(range.Start(), range.End() - range.Start(), replacement) == 0) {
		return { ETextEditOutcome::NoChange, m_version, false };
	}
	try {
		std::string next;
		next.reserve(m_text.size() - (range.End() - range.Start()) + replacement.size());
		next.append(m_text, 0, range.Start());
		next.append(replacement);
		next.append(m_text, range.End(), std::string::npos);
		return Commit(std::move(next));
	}
	catch (const std::exception&) {
		return { ETextEditOutcome::Failed, m_version, false };
	}
}

TextEditResult TextDocument::ReplaceAll(std::string replacement) noexcept
{
	if (m_text == replacement) return { ETextEditOutcome::NoChange, m_version, false };
	return Commit(std::move(replacement));
}

TextEditResult TextDocument::Commit(std::string replacement) noexcept
{
	if (m_version == std::numeric_limits<std::uint64_t>::max()) {
		return { ETextEditOutcome::Failed, m_version, false };
	}
	m_text.swap(replacement);
	++m_version;
	return { ETextEditOutcome::Applied, m_version, true };
}

bool UndoHistory::Record(UndoEntry entry) noexcept
{
	if (entry.Before() == entry.After()) return true;
	try {
		std::vector<UndoEntry> next(m_entries.begin(),
			m_entries.begin() + static_cast<std::ptrdiff_t>(m_cursor));
		next.push_back(std::move(entry));
		m_entries.swap(next);
		m_cursor = m_entries.size();
		return true;
	}
	catch (const std::exception&) {
		return false;
	}
}

const UndoEntry* UndoHistory::NextUndo() const noexcept
{
	return CanUndo() ? &m_entries[m_cursor - 1] : nullptr;
}

const UndoEntry* UndoHistory::NextRedo() const noexcept
{
	return CanRedo() ? &m_entries[m_cursor] : nullptr;
}

void UndoHistory::CommitUndo() noexcept
{
	if (CanUndo()) --m_cursor;
}

void UndoHistory::CommitRedo() noexcept
{
	if (CanRedo()) ++m_cursor;
}

void UndoHistory::Clear() noexcept
{
	m_entries.clear();
	m_cursor = 0;
}

DocumentPosition LayoutProjection::PositionAt(std::string_view text, std::size_t offset) noexcept
{
	offset = (std::min)(offset, text.size());
	std::size_t line = 0;
	std::size_t column = 0;
	for (std::size_t index = 0; index < offset; ++index) {
		if (text[index] == '\n') {
			++line;
			column = 0;
		}
		else {
			++column;
		}
	}
	return { line, column };
}

std::size_t LayoutProjection::OffsetAt(std::string_view text, DocumentPosition position) noexcept
{
	std::size_t offset = 0;
	for (std::size_t line = 0; line < position.Line() && offset < text.size(); ++line) {
		while (offset < text.size() && text[offset] != '\n') ++offset;
		if (offset < text.size()) ++offset;
	}
	if (position.Line() != 0) {
		std::size_t availableLines = 0;
		for (char character : text) if (character == '\n') ++availableLines;
		if (position.Line() > availableLines) return text.size();
	}
	std::size_t column = 0;
	while (offset < text.size() && text[offset] != '\n' && column < position.Column()) {
		++offset;
		++column;
	}
	return offset;
}

DocumentSession::DocumentSession(std::string initialText)
	: m_document(initialText)
	, m_savedContent(std::move(initialText))
{
}

DocumentSessionResult DocumentSession::Result(EDocumentSessionTerminal terminal) const noexcept
{
	return { terminal, m_document.Version(), IsDirty() };
}

bool DocumentSession::RestoreText(const std::string& text) noexcept
{
	return m_document.ReplaceAll(text).Outcome() != ETextEditOutcome::Failed;
}

DocumentSessionResult DocumentSession::Replace(TextRange range, std::string_view replacement) noexcept
{
	try {
		const std::string before = m_document.Text();
		const auto edit = m_document.Replace(range, replacement);
		if (edit.Outcome() == ETextEditOutcome::NoChange) return Result(EDocumentSessionTerminal::NoChange);
		if (edit.Outcome() == ETextEditOutcome::InvalidRange) return Result(EDocumentSessionTerminal::InvalidRequest);
		if (edit.Outcome() != ETextEditOutcome::Applied) return Result(EDocumentSessionTerminal::Failed);
		if (m_history.Record({ before, m_document.Text() })) return Result(EDocumentSessionTerminal::Succeeded);
		return RestoreText(before) ? Result(EDocumentSessionTerminal::RolledBack) : Result(EDocumentSessionTerminal::Failed);
	}
	catch (const std::exception&) {
		return Result(EDocumentSessionTerminal::Failed);
	}
}

DocumentSessionResult DocumentSession::Undo() noexcept
{
	const auto* entry = m_history.NextUndo();
	if (!entry) return Result(EDocumentSessionTerminal::NoChange);
	if (!RestoreText(entry->Before())) return Result(EDocumentSessionTerminal::Failed);
	m_history.CommitUndo();
	return Result(EDocumentSessionTerminal::Succeeded);
}

DocumentSessionResult DocumentSession::Redo() noexcept
{
	const auto* entry = m_history.NextRedo();
	if (!entry) return Result(EDocumentSessionTerminal::NoChange);
	if (!RestoreText(entry->After())) return Result(EDocumentSessionTerminal::Failed);
	m_history.CommitRedo();
	return Result(EDocumentSessionTerminal::Succeeded);
}

DocumentSessionResult DocumentSession::Load(std::string resourceId, IDocumentPersistence& persistence) noexcept
{
	if (resourceId.empty()) return Result(EDocumentSessionTerminal::InvalidRequest);
	std::optional<DocumentLoadResult> loaded;
	try {
		loaded.emplace(persistence.Load(DocumentLoadRequest(resourceId)));
	}
	catch (const std::exception&) {
		return Result(EDocumentSessionTerminal::Failed);
	}
	if (loaded->Terminal() == EDocumentPersistenceTerminal::Cancelled) return Result(EDocumentSessionTerminal::Cancelled);
	if (loaded->Terminal() != EDocumentPersistenceTerminal::Succeeded) return Result(EDocumentSessionTerminal::Failed);
	if (!loaded->IsComplete()) return Result(EDocumentSessionTerminal::RolledBack);

	try {
		// Allocate every new session value before the document commit. The swaps
		// after it are noexcept, so a successful load cannot leave a half-published
		// resource/baseline pair behind.
		std::string nextText = loaded->Content();
		std::string nextSaved = nextText;
		std::string nextResource = std::move(resourceId);
		const auto edit = m_document.ReplaceAll(std::move(nextText));
		if (edit.Outcome() == ETextEditOutcome::Failed) return Result(EDocumentSessionTerminal::RolledBack);
		m_savedContent.swap(nextSaved);
		m_resourceId.swap(nextResource);
		m_history.Clear();
		return Result(EDocumentSessionTerminal::Succeeded);
	}
	catch (const std::exception&) {
		return Result(EDocumentSessionTerminal::Failed);
	}
}

DocumentSessionResult DocumentSession::Save(IDocumentPersistence& persistence) noexcept
{
	if (m_resourceId.empty()) return Result(EDocumentSessionTerminal::InvalidRequest);
	if (!IsDirty()) return Result(EDocumentSessionTerminal::NoChange);
	const auto version = m_document.Version();
	std::optional<DocumentSaveResult> saved;
	try {
		saved.emplace(persistence.Save(DocumentSaveRequest(m_resourceId, m_document.Text(), version)));
	}
	catch (const std::exception&) {
		return Result(EDocumentSessionTerminal::Failed);
	}
	if (saved->Terminal() == EDocumentPersistenceTerminal::Cancelled) return Result(EDocumentSessionTerminal::Cancelled);
	if (saved->Terminal() != EDocumentPersistenceTerminal::Succeeded) return Result(EDocumentSessionTerminal::Failed);
	if (saved->CommittedVersion() != version || m_document.Version() != version) return Result(EDocumentSessionTerminal::Conflict);
	try {
		m_savedContent = m_document.Text();
		return Result(EDocumentSessionTerminal::Succeeded);
	}
	catch (const std::exception&) {
		return Result(EDocumentSessionTerminal::Failed);
	}
}

} // namespace editor::document
