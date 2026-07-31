/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "charset/charcode.h"
#include "doc/CEditDoc.h"
#include "doc/logic/CDocLine.h"
#include "env/ShareDataTestSuite.hpp"
#include "util/string_ex.h"
#include "workbench/editor/persistence/CEditDocWorkingCopyPersistenceAdapter.h"
#include "workbench/editor/persistence/WorkingCopyPersistenceCodec.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace workbench::editor::persistence {
namespace {

class CaptureContextSource final : public ICEditDocWorkingCopyCaptureContextSource {
public:
	std::optional<CEditDocWorkingCopyCaptureContext> context;

	[[nodiscard]] std::optional<CEditDocWorkingCopyCaptureContext> CurrentCaptureContext() const override
	{
		return context;
	}
};

class CEditDocWorkingCopyPersistenceAdapterTest
	: public ::testing::Test
	, public env::ShareDataTestSuite {
protected:
	static void SetUpTestSuite()
	{
		SetUpShareData();
		// CEditDoc constructs its initial layout immediately.  The production editor
		// bootstrap establishes this cache first; keep the focused native-adapter
		// fixture under the same prerequisite instead of depending on suite order.
		SelectCharWidthCache(CWM_FONT_EDIT, CWM_CACHE_SHARE);
		InitCharWidthCache(GetDllShareData().m_Common.m_sView.m_lf);
	}

	static void TearDownTestSuite()
	{
		TearDownShareData();
	}

	static std::unique_ptr<CEditDoc> NewDocument()
	{
		return std::make_unique<CEditDoc>(nullptr);
	}

	static EditorDocumentIdentity ResourceIdentity(const wchar_t* value)
	{
		auto uri = platform::uri::Uri::Parse(value);
		EXPECT_TRUE(uri);
		return uri ? EditorDocumentIdentity{ .resource = std::move(*uri.value) }
			: EditorDocumentIdentity{};
	}

	static EditorDocumentIdentity ResourceIdentityFromWindowsPath(const wchar_t* value)
	{
		auto uri = platform::uri::Uri::FromWindowsPath(value);
		EXPECT_TRUE(uri);
		return uri ? EditorDocumentIdentity{ .resource = std::move(*uri.value) }
			: EditorDocumentIdentity{};
	}

	static void SetText(CEditDoc& document, std::wstring_view first, std::wstring_view second = {})
	{
		CDocEditAgent editor(&document.m_cDocLineMgr);
		editor.AddLineStrX(first.data(), static_cast<int>(first.size()));
		if (!second.empty()) {
			editor.AddLineStrX(second.data(), static_cast<int>(second.size()));
		}
	}

	static std::string ReadUtf8(const CEditDoc& document)
	{
		std::wstring text;
		for (const CDocLine* line = document.m_cDocLineMgr.GetDocLineTop();
			line != nullptr; line = line->GetNextLine()) {
			text.append(line->GetPtr(), static_cast<std::size_t>(line->GetLengthWithEOL()));
		}
		return wcstou8s(text);
	}

	static EditorWorkingCopyRecoveryRequest RecoveryFrom(
		const EditorWorkingCopyPersistenceSnapshot& snapshot, std::uint64_t generation = 1)
	{
		WorkingCopyBackup backup{
			.scope = { "profile.test", "workspace.test" },
			.identity = snapshot.identity,
			.generation = generation,
			.contentVersion = snapshot.contentVersion,
			.encoding = snapshot.encoding,
			.eol = snapshot.eol,
			.dirty = true,
			.content = snapshot.content,
		};
		backup.checksum = CWorkingCopyPersistenceCodec::ComputeContentChecksum(backup.content);
		EditorSessionInputDescriptor input{
			.inputId = snapshot.inputId,
			.inputTypeId = snapshot.inputTypeId,
			.workingCopyIdentity = snapshot.identity,
			.stateVersion = 1,
			.backupGeneration = generation,
		};
		return { .input = std::move(input), .backup = std::move(backup) };
	}
};

TEST_F(CEditDocWorkingCopyPersistenceAdapterTest, NamedDocumentRoundTripsWithoutReadingDisk)
{
	auto sourceDocument = NewDocument();
	sourceDocument->m_cDocFile.SetFilePath(L"C:\\workspace\\未保存.txt");
	sourceDocument->m_cDocFile.SetCodeSet(CODE_UTF8, true);
	sourceDocument->m_cDocEditor.SetNewLineCode(EEolType::cr_and_lf);
	sourceDocument->m_cDocEditor.SetModified(true, false);
	SetText(*sourceDocument, L"alpha\r\n", L"さくら");

	CaptureContextSource sourceContext;
	sourceContext.context = CEditDocWorkingCopyCaptureContext{
		.inputId = "input.named",
		.inputTypeId = "workbench.editor.text",
		.documentIdentity = ResourceIdentityFromWindowsPath(sourceDocument->m_cDocFile.GetFilePath()),
		.documentRevision = 17,
	};
	CEditDocWorkingCopyPersistenceAdapter source(*sourceDocument, sourceContext);
	const auto captured = source.Capture();
	ASSERT_EQ(EEditorWorkingCopySnapshotStatus::Captured, captured.status);
	ASSERT_TRUE(captured.snapshot);
	EXPECT_TRUE(captured.snapshot->identity.canonicalResource);
	EXPECT_FALSE(captured.snapshot->identity.opaqueId);
	EXPECT_EQ(EWorkingCopyTextEncoding::Utf8WithBom, captured.snapshot->encoding);
	EXPECT_EQ(EWorkingCopyEol::CrLf, captured.snapshot->eol);
	EXPECT_EQ(17U, captured.snapshot->contentVersion);
	const auto expectedText = std::string("alpha\r\n") + wcstou8s(L"さくら");
	EXPECT_EQ(expectedText, captured.snapshot->content);

	auto targetDocument = NewDocument();
	CaptureContextSource targetContext;
	CEditDocWorkingCopyPersistenceAdapter target(*targetDocument, targetContext);
	const auto request = RecoveryFrom(*captured.snapshot);
	const auto prepared = target.Prepare(request);
	ASSERT_EQ(EEditorWorkingCopyRecoveryStatus::Prepared, prepared.status);
	ASSERT_TRUE(prepared.coreIdentity);
	ASSERT_TRUE(prepared.coreIdentity->resource);
	EXPECT_EQ(CLogicInt(0), targetDocument->m_cDocLineMgr.GetLineCount());

	static_cast<void>(target.Commit(request));
	EXPECT_EQ(L"C:\\workspace\\未保存.txt",
		std::wstring(targetDocument->m_cDocFile.GetFilePath()));
	EXPECT_EQ(CODE_UTF8, targetDocument->GetDocumentEncoding());
	EXPECT_TRUE(targetDocument->GetDocumentBomExist());
	EXPECT_EQ(EEolType::cr_and_lf, targetDocument->m_cDocEditor.GetNewLineCode().GetType());
	EXPECT_TRUE(targetDocument->m_cDocEditor.IsModified());
	EXPECT_EQ(expectedText, ReadUtf8(*targetDocument));
}

TEST_F(CEditDocWorkingCopyPersistenceAdapterTest, InvalidUtf8FailsPrepareWithoutMutatingTarget)
{
	auto targetDocument = NewDocument();
	CaptureContextSource context;
	CEditDocWorkingCopyPersistenceAdapter adapter(*targetDocument, context);
	EditorWorkingCopyPersistenceSnapshot snapshot{
		.identity = { "workbench.editor.text", std::nullopt, "untitled.invalid" },
		.inputId = "input.invalid",
		.inputTypeId = "workbench.editor.text",
		.contentVersion = 3,
		.dirty = true,
		.encoding = EWorkingCopyTextEncoding::Utf8,
		.eol = EWorkingCopyEol::Lf,
		.content = "valid",
	};
	auto request = RecoveryFrom(snapshot);
	request.backup.content = std::string("\xc3\x28", 2);
	request.backup.checksum =
		CWorkingCopyPersistenceCodec::ComputeContentChecksum(request.backup.content);

	EXPECT_EQ(EEditorWorkingCopyRecoveryStatus::Failed, adapter.Prepare(request).status);
	static_cast<void>(adapter.Commit(request));
	EXPECT_EQ(CLogicInt(0), targetDocument->m_cDocLineMgr.GetLineCount());
	EXPECT_FALSE(targetDocument->m_cDocFile.GetFilePathClass().IsValidPath());
	EXPECT_FALSE(targetDocument->m_cDocEditor.IsModified());
}

TEST_F(CEditDocWorkingCopyPersistenceAdapterTest, CapturePreservesNamedAndUntitledIdentityMetadata)
{
	CaptureContextSource context;
	context.context = CEditDocWorkingCopyCaptureContext{
		.inputId = "input.identity",
		.inputTypeId = "workbench.editor.text",
		.documentIdentity = { .opaqueId = "untitled.stable.1" },
		.documentRevision = 9,
	};

	auto untitled = NewDocument();
	untitled->m_cDocFile.SetCodeSet(CODE_UTF8, false);
	untitled->m_cDocEditor.SetNewLineCode(EEolType::line_feed);
	untitled->m_cDocEditor.SetModified(true, false);
	SetText(*untitled, L"untitled");
	CEditDocWorkingCopyPersistenceAdapter untitledAdapter(*untitled, context);
	const auto untitledCapture = untitledAdapter.Capture();
	ASSERT_EQ(EEditorWorkingCopySnapshotStatus::Captured, untitledCapture.status);
	ASSERT_TRUE(untitledCapture.snapshot);
	EXPECT_EQ(std::optional<std::string>("untitled.stable.1"),
		untitledCapture.snapshot->identity.opaqueId);
	EXPECT_FALSE(untitledCapture.snapshot->identity.canonicalResource);
	EXPECT_TRUE(untitledCapture.snapshot->dirty);

	auto named = NewDocument();
	named->m_cDocFile.SetFilePath(L"D:\\code\\dirty.txt");
	named->m_cDocFile.SetCodeSet(CODE_LATIN1, false);
	named->m_cDocEditor.SetNewLineCode(EEolType::carriage_return);
	named->m_cDocEditor.SetModified(true, false);
	SetText(*named, L"named\r");
	CaptureContextSource namedContext;
	namedContext.context = CEditDocWorkingCopyCaptureContext{
		.inputId = "input.identity",
		.inputTypeId = "workbench.editor.text",
		.documentIdentity = ResourceIdentity(L"file:///D:/core/authoritative.txt"),
		.documentRevision = 9,
	};
	CEditDocWorkingCopyPersistenceAdapter namedAdapter(*named, namedContext);
	const auto namedCapture = namedAdapter.Capture();
	ASSERT_EQ(EEditorWorkingCopySnapshotStatus::Captured, namedCapture.status);
	ASSERT_TRUE(namedCapture.snapshot);
	EXPECT_TRUE(namedCapture.snapshot->identity.canonicalResource);
	EXPECT_EQ(std::optional<std::string>("file:///D:/core/authoritative.txt"),
		namedCapture.snapshot->identity.canonicalResource);
	EXPECT_FALSE(namedCapture.snapshot->identity.opaqueId);
	EXPECT_EQ(EWorkingCopyTextEncoding::Windows1252, namedCapture.snapshot->encoding);
	EXPECT_EQ(EWorkingCopyEol::Cr, namedCapture.snapshot->eol);
	EXPECT_TRUE(namedCapture.snapshot->dirty);
}

TEST_F(CEditDocWorkingCopyPersistenceAdapterTest, NonInertTargetRejectsPrepareAndCommitCannotOverwriteIt)
{
	auto targetDocument = NewDocument();
	targetDocument->m_cDocFile.SetCodeSet(CODE_UTF8, false);
	targetDocument->m_cDocEditor.SetNewLineCode(EEolType::line_feed);
	SetText(*targetDocument, L"existing");

	CaptureContextSource context;
	CEditDocWorkingCopyPersistenceAdapter adapter(*targetDocument, context);
	EditorWorkingCopyPersistenceSnapshot snapshot{
		.identity = { "workbench.editor.text", std::nullopt, "untitled.recovered" },
		.inputId = "input.recovered",
		.inputTypeId = "workbench.editor.text",
		.contentVersion = 11,
		.dirty = true,
		.encoding = EWorkingCopyTextEncoding::Utf8,
		.eol = EWorkingCopyEol::Lf,
		.content = "recovered",
	};
	const auto request = RecoveryFrom(snapshot);

	EXPECT_EQ(EEditorWorkingCopyRecoveryStatus::Failed, adapter.Prepare(request).status);
	static_cast<void>(adapter.Commit(request));
	EXPECT_EQ("existing", ReadUtf8(*targetDocument));
	EXPECT_FALSE(targetDocument->m_cDocEditor.IsModified());

	auto changedAfterPrepare = NewDocument();
	CEditDocWorkingCopyPersistenceAdapter stagedAdapter(*changedAfterPrepare, context);
	EXPECT_EQ(EEditorWorkingCopyRecoveryStatus::Prepared, stagedAdapter.Prepare(request).status);
	SetText(*changedAfterPrepare, L"changed-after-prepare");
	static_cast<void>(stagedAdapter.Commit(request));
	EXPECT_EQ("changed-after-prepare", ReadUtf8(*changedAfterPrepare));
	EXPECT_FALSE(changedAfterPrepare->m_cDocEditor.IsModified());
}

TEST_F(CEditDocWorkingCopyPersistenceAdapterTest, AbortPreparedDiscardsTheStagedRecoveryDocument)
{
	auto targetDocument = NewDocument();
	CaptureContextSource context;
	CEditDocWorkingCopyPersistenceAdapter adapter(*targetDocument, context);
	EditorWorkingCopyPersistenceSnapshot snapshot{
		.identity = { "workbench.editor.text", std::nullopt, "untitled.abort" },
		.inputId = "input.abort",
		.inputTypeId = "workbench.editor.text",
		.contentVersion = 4,
		.dirty = true,
		.encoding = EWorkingCopyTextEncoding::Utf8,
		.eol = EWorkingCopyEol::Lf,
		.content = "must-not-commit",
	};
	const auto request = RecoveryFrom(snapshot);
	EXPECT_EQ(EEditorWorkingCopyRecoveryStatus::Prepared, adapter.Prepare(request).status);

	adapter.AbortPrepared();
	EXPECT_EQ(EEditorWorkingCopyRecoveryCommitStatus::NotPrepared, adapter.Commit(request));
	EXPECT_EQ(CLogicInt(0), targetDocument->m_cDocLineMgr.GetLineCount());
	EXPECT_FALSE(targetDocument->m_cDocFile.GetFilePathClass().IsValidPath());
	EXPECT_FALSE(targetDocument->m_cDocEditor.IsModified());
}

} // namespace
} // namespace workbench::editor::persistence
