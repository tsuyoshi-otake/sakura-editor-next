/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include <sakura/editor/document/DocumentSession.h>
#include <sakura/editor/document/LayoutProjection.h>

#if __has_include("workbench/editor/document/DocumentSession.cpp")
#error "sakura_editor_document_core_tests consumer can reach the provider implementation"
#endif

#include <array>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

using editor::document::DocumentLoadRequest;
using editor::document::DocumentLoadResult;
using editor::document::DocumentPosition;
using editor::document::DocumentSaveRequest;
using editor::document::DocumentSaveResult;
using editor::document::DocumentSession;
using editor::document::EDocumentPersistenceTerminal;
using editor::document::EDocumentSessionTerminal;
using editor::document::IDocumentPersistence;
using editor::document::LayoutProjection;
using editor::document::TextRange;

class FakePersistence final : public IDocumentPersistence {
public:
	void SetNextLoad(DocumentLoadResult result) { m_nextLoad.emplace(std::move(result)); }
	void SetNextSave(DocumentSaveResult result) { m_nextSave.emplace(std::move(result)); }
	[[nodiscard]] const std::string& SavedResource() const noexcept { return m_savedResource; }
	[[nodiscard]] const std::string& SavedContent() const noexcept { return m_savedContent; }
	[[nodiscard]] std::uint64_t SavedVersion() const noexcept { return m_savedVersion; }

	DocumentLoadResult Load(const DocumentLoadRequest&) override
	{
		return m_nextLoad ? *m_nextLoad : DocumentLoadResult();
	}
	DocumentSaveResult Save(const DocumentSaveRequest& request) override
	{
		m_savedResource = request.ResourceId();
		m_savedContent = request.Content();
		m_savedVersion = request.ContentVersion();
		return m_nextSave ? *m_nextSave : DocumentSaveResult();
	}

private:
	std::optional<DocumentLoadResult> m_nextLoad;
	std::optional<DocumentSaveResult> m_nextSave;
	std::string m_savedResource;
	std::string m_savedContent;
	std::uint64_t m_savedVersion = 0;
};

bool EditUndoRedoKeepsOneDocumentOwnerAndMonotonicVersions()
{
	DocumentSession session("alpha");
	if (session.IsDirty() || session.Document().Version() != 0) return false;
	if (session.Replace({ 5, 5 }, " beta").Terminal() != EDocumentSessionTerminal::Succeeded) return false;
	const auto editVersion = session.Document().Version();
	if (session.Document().Text() != "alpha beta" || !session.IsDirty() || !session.History().CanUndo()) return false;
	if (session.Undo().Terminal() != EDocumentSessionTerminal::Succeeded || session.Document().Text() != "alpha") return false;
	const auto undoVersion = session.Document().Version();
	if (undoVersion <= editVersion || session.History().CanUndo() || !session.History().CanRedo()) return false;
	if (session.Redo().Terminal() != EDocumentSessionTerminal::Succeeded || session.Document().Text() != "alpha beta") return false;
	return session.Document().Version() > undoVersion && session.History().CanUndo() && !session.History().CanRedo();
}

bool LogicalProjectionClampsAtDocumentEdges()
{
	constexpr std::string_view text = "one\ntwo\nthree";
	const auto position = LayoutProjection::PositionAt(text, 5);
	const auto end = LayoutProjection::PositionAt(text, 999);
	return LayoutProjection::PositionAt(text, 0).Line() == 0
		&& position.Line() == 1 && position.Column() == 1
		&& end.Line() == 2 && end.Column() == 5
		&& LayoutProjection::OffsetAt(text, { 1, 2 }) == 6
		&& LayoutProjection::OffsetAt(text, { 9, 0 }) == text.size()
		&& LayoutProjection::OffsetAt(text, { 2, 99 }) == text.size();
}

bool SaveFailureCancellationAndVersionConflictKeepDirtyContent()
{
	DocumentSession session;
	FakePersistence persistence;
	persistence.SetNextLoad({ EDocumentPersistenceTerminal::Succeeded, true, "draft" });
	if (session.Load("untitled:1", persistence).Terminal() != EDocumentSessionTerminal::Succeeded) return false;
	if (session.Replace({ 5, 5 }, "!").Terminal() != EDocumentSessionTerminal::Succeeded) return false;
	const auto version = session.Document().Version();
	persistence.SetNextSave({ EDocumentPersistenceTerminal::Cancelled, 0 });
	if (session.Save(persistence).Terminal() != EDocumentSessionTerminal::Cancelled || !session.IsDirty()) return false;
	persistence.SetNextSave({ EDocumentPersistenceTerminal::Failed, 0 });
	if (session.Save(persistence).Terminal() != EDocumentSessionTerminal::Failed || !session.IsDirty()) return false;
	persistence.SetNextSave({ EDocumentPersistenceTerminal::Succeeded, version + 1 });
	if (session.Save(persistence).Terminal() != EDocumentSessionTerminal::Conflict || !session.IsDirty()) return false;
	return persistence.SavedResource() == "untitled:1" && persistence.SavedContent() == "draft!"
		&& persistence.SavedVersion() == version;
}

bool SaveAndLoadCommitOnlyAtSuccessfulTerminal()
{
	DocumentSession session("before");
	FakePersistence persistence;
	persistence.SetNextLoad({ EDocumentPersistenceTerminal::Cancelled, false, {} });
	if (session.Load("file:///cancelled", persistence).Terminal() != EDocumentSessionTerminal::Cancelled) return false;
	if (session.Document().Text() != "before" || session.ResourceId() != "") return false;
	persistence.SetNextLoad({ EDocumentPersistenceTerminal::Succeeded, false, "partial" });
	if (session.Load("file:///partial", persistence).Terminal() != EDocumentSessionTerminal::RolledBack) return false;
	if (session.Document().Text() != "before" || session.ResourceId() != "") return false;
	persistence.SetNextLoad({ EDocumentPersistenceTerminal::Succeeded, true, "loaded" });
	if (session.Load("file:///loaded", persistence).Terminal() != EDocumentSessionTerminal::Succeeded) return false;
	if (session.Document().Text() != "loaded" || session.IsDirty() || session.History().CanUndo()) return false;
	if (session.Replace({ 6, 6 }, " text").Terminal() != EDocumentSessionTerminal::Succeeded) return false;
	persistence.SetNextSave({ EDocumentPersistenceTerminal::Succeeded, session.Document().Version() });
	return session.Save(persistence).Terminal() == EDocumentSessionTerminal::Succeeded && !session.IsDirty()
		&& session.Save(persistence).Terminal() == EDocumentSessionTerminal::NoChange;
}

bool DocumentTransactionCharacterizesOpenEditSelectionDeleteUndoRedoSaveAndReopen()
{
	FakePersistence persistence;
	persistence.SetNextLoad({ EDocumentPersistenceTerminal::Succeeded, true, "first\nsecond" });
	std::string savedContent;
	{
		DocumentSession session;
		if (session.Load("untitled:characterization", persistence).Terminal()
			!= EDocumentSessionTerminal::Succeeded) return false;

		// Cursor and selection are presentation-owned.  Establish the same logical
		// offsets through the document-core projection before applying text changes.
		const auto caret = LayoutProjection::OffsetAt(session.Document().Text(), { 0, 5 });
		if (caret != 5 || session.Replace({ caret, caret }, "!").Terminal()
			!= EDocumentSessionTerminal::Succeeded) return false;
		const auto selectionStart = LayoutProjection::OffsetAt(session.Document().Text(), { 1, 1 });
		const auto selectionEnd = LayoutProjection::OffsetAt(session.Document().Text(), { 1, 3 });
		if (session.Replace({ selectionStart, selectionEnd }, "").Terminal()
			!= EDocumentSessionTerminal::Succeeded) return false;
		if (session.Document().Text() != "first!\nsond" || !session.IsDirty()) return false;
		if (session.Undo().Terminal() != EDocumentSessionTerminal::Succeeded
			|| session.Document().Text() != "first!\nsecond") return false;
		if (session.Redo().Terminal() != EDocumentSessionTerminal::Succeeded
			|| session.Document().Text() != "first!\nsond") return false;

		persistence.SetNextSave({ EDocumentPersistenceTerminal::Succeeded, session.Document().Version() });
		if (session.Save(persistence).Terminal() != EDocumentSessionTerminal::Succeeded || session.IsDirty()) return false;
		savedContent = persistence.SavedContent();
	}

	// Reopening is a new aggregate; it must reproduce the saved logical bytes
	// without importing stale undo or presentation state from the closed one.
	persistence.SetNextLoad({ EDocumentPersistenceTerminal::Succeeded, true, savedContent });
	DocumentSession reopened;
	return savedContent == "first!\nsond"
		&& reopened.Load("untitled:characterization", persistence).Terminal() == EDocumentSessionTerminal::Succeeded
		&& reopened.Document().Text() == savedContent && !reopened.IsDirty()
		&& !reopened.History().CanUndo() && !reopened.History().CanRedo();
}

bool InvalidEditsAndMissingResourceAreExplicitTerminals()
{
	DocumentSession session("text");
	FakePersistence persistence;
	return session.Replace({ 4, 3 }, "x").Terminal() == EDocumentSessionTerminal::InvalidRequest
		&& session.Save(persistence).Terminal() == EDocumentSessionTerminal::InvalidRequest
		&& session.Load("", persistence).Terminal() == EDocumentSessionTerminal::InvalidRequest
		&& session.Undo().Terminal() == EDocumentSessionTerminal::NoChange;
}

class TestCase final {
public:
	constexpr TestCase(std::string_view name, bool (*run)()) noexcept : m_name(name), m_run(run) {}
	[[nodiscard]] constexpr std::string_view Name() const noexcept { return m_name; }
	[[nodiscard]] bool Run() const { return m_run(); }

private:
	const std::string_view m_name;
	bool (*const m_run)();
};

constexpr std::array kTests{
	TestCase{"EditUndoRedoKeepsOneDocumentOwnerAndMonotonicVersions", EditUndoRedoKeepsOneDocumentOwnerAndMonotonicVersions},
	TestCase{"LogicalProjectionClampsAtDocumentEdges", LogicalProjectionClampsAtDocumentEdges},
	TestCase{"SaveFailureCancellationAndVersionConflictKeepDirtyContent", SaveFailureCancellationAndVersionConflictKeepDirtyContent},
	TestCase{"SaveAndLoadCommitOnlyAtSuccessfulTerminal", SaveAndLoadCommitOnlyAtSuccessfulTerminal},
	TestCase{"DocumentTransactionCharacterizesOpenEditSelectionDeleteUndoRedoSaveAndReopen", DocumentTransactionCharacterizesOpenEditSelectionDeleteUndoRedoSaveAndReopen},
	TestCase{"InvalidEditsAndMissingResourceAreExplicitTerminals", InvalidEditsAndMissingResourceAreExplicitTerminals},
};

bool Matches(std::string_view fullName, std::string_view filter)
{
	if (filter.empty() || filter == "*") return true;
	const auto star = filter.find('*');
	if (star == std::string_view::npos) return fullName == filter;
	const auto prefix = filter.substr(0, star);
	const auto suffix = filter.substr(star + 1);
	return fullName.starts_with(prefix) && fullName.ends_with(suffix)
		&& fullName.size() >= prefix.size() + suffix.size();
}

} // namespace

int main(int argc, char** argv)
{
	std::string_view filter = "DocumentCore.*";
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument = argv[index];
		if (argument == "--gtest_list_tests") {
			std::cout << "DocumentCore.\n";
			for (const auto& test : kTests) std::cout << "  " << test.Name() << '\n';
			return 0;
		}
		constexpr std::string_view prefix = "--gtest_filter=";
		if (argument.starts_with(prefix)) filter = argument.substr(prefix.size());
	}

	int selected = 0;
	int failed = 0;
	for (const auto& test : kTests) {
		const std::string fullName = "DocumentCore." + std::string(test.Name());
		if (!Matches(fullName, filter)) continue;
		++selected;
		const bool passed = test.Run();
		std::cout << (passed ? "[       OK ] " : "[  FAILED  ] ") << fullName << '\n';
		if (!passed) ++failed;
	}
	std::cout << "[==========] " << selected << " tests ran; " << failed << " failed.\n";
	return failed == 0 && selected > 0 ? 0 : 1;
}
