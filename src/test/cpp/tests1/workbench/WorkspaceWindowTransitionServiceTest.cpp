/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include <gtest/gtest.h>

#include "_main/FailedEditorProcessShutdown.h"
#include "config/WorkspaceContextTypes.h"
#include <sakura/filesystem/IFileService.h>
#include "workbench/workspace/WorkspaceEditingService.h"
#include "workbench/workspace/WorkspaceWindowTransitionService.h"
#include "workbench/workspace/WorkspaceWindowTransitionPlanner.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <deque>
#include <vector>

namespace {

using workbench::workspace::CWorkspaceWindowTransitionService;
using workbench::workspace::CWorkspaceWindowTransitionPlanner;
using workbench::workspace::CWorkspaceEditingService;
using workbench::workspace::EWorkspaceEditingOutcome;
using workbench::workspace::EWorkspaceWindowTransitionOutcome;
using workbench::workspace::IWorkspaceWindowTransitionHost;
using workbench::workspace::WorkspaceFolderEdit;
using workbench::workspace::WorkspaceFoldersEditRequest;
using workbench::workspace::WorkspaceWindowTransitionRequest;

using platform::filesystem::DirectoryEntry;
using platform::filesystem::EFileResultStatus;
using platform::filesystem::FileBytes;
using platform::filesystem::FileConditionalReplaceOptions;
using platform::filesystem::FileConditionalReplaceResult;
using platform::filesystem::FileContentSnapshot;
using platform::filesystem::FileReadOptions;
using platform::filesystem::FileResult;
using platform::filesystem::FileStat;
using platform::filesystem::FileVersionToken;
using platform::filesystem::IFileService;
using platform::filesystem::IFileWatch;
using platform::uri::Uri;

Uri WorkspaceUri(const wchar_t* text)
{
	auto parsed = Uri::Parse(text);
	EXPECT_TRUE(parsed.value.has_value());
	return *parsed.value;
}

FileVersionToken Version(std::uint8_t value)
{
	const std::uint8_t bytes[] { value };
	auto token = FileVersionToken::FromOpaqueBytes(bytes);
	EXPECT_TRUE(token.has_value());
	return *token;
}

FileResult<FileContentSnapshot> Snapshot(std::string document, std::uint8_t version = 1)
{
	return FileResult<FileContentSnapshot>::Success({ FileBytes(document.begin(), document.end()), Version(version) });
}

std::string Text(const FileBytes& bytes)
{
	return { bytes.begin(), bytes.end() };
}

//! A deliberately small versioned fake: the production editor, rather than
//! this fake, owns document rewriting and CAS choice.
class StagedWorkspaceFileService final : public IFileService {
public:
	FileResult<FileStat> Stat(const Uri&) override { return FileResult<FileStat>::Failure(EFileResultStatus::Unsupported); }
	FileResult<std::vector<DirectoryEntry>> Enumerate(const Uri&) override
	{
		return FileResult<std::vector<DirectoryEntry>>::Failure(EFileResultStatus::Unsupported);
	}
	FileResult<FileBytes> Read(const Uri&, const FileReadOptions&) override
	{
		return FileResult<FileBytes>::Failure(EFileResultStatus::Unsupported);
	}
	FileResult<FileContentSnapshot> ReadVersioned(const Uri& resource, const FileReadOptions&) override
	{
		const auto found = reads.find(resource.ToString());
		return found == reads.end()
			? FileResult<FileContentSnapshot>::Failure(EFileResultStatus::NotFound)
			: found->second;
	}
	FileConditionalReplaceResult ConditionalAtomicReplace(
		const Uri& resource, const FileBytes& bytes, const FileConditionalReplaceOptions&) override
	{
		writes[resource.ToString()] = bytes;
		return FileConditionalReplaceResult::Success(Version(2));
	}
	FileResult<std::unique_ptr<IFileWatch>> Watch(const Uri&, const platform::filesystem::FileWatchOptions&) override
	{
		return FileResult<std::unique_ptr<IFileWatch>>::Failure(EFileResultStatus::Unsupported);
	}

	std::map<std::wstring, FileResult<FileContentSnapshot>, std::less<>> reads;
	std::map<std::wstring, FileBytes, std::less<>> writes;
};

class TransitionHost final : public IWorkspaceWindowTransitionHost {
public:
	EWorkspaceWindowTransitionOutcome PrepareReplacement() override
	{
		order.emplace_back("prepare");
		return prepare;
	}
	EWorkspaceWindowTransitionOutcome LaunchAndWaitForReady() override
	{
		order.emplace_back("launch");
		return launch;
	}
	EWorkspaceWindowTransitionOutcome CloseCurrentWindowOnce() override
	{
		order.emplace_back("close");
		return close;
	}
	EWorkspaceWindowTransitionOutcome DeleteStagedTarget() override
	{
		order.emplace_back("cleanup");
		if (cleanup == EWorkspaceWindowTransitionOutcome::Succeeded && cleanupAction) cleanupAction();
		return cleanup;
	}

	EWorkspaceWindowTransitionOutcome prepare = EWorkspaceWindowTransitionOutcome::Succeeded;
	EWorkspaceWindowTransitionOutcome launch = EWorkspaceWindowTransitionOutcome::Succeeded;
	EWorkspaceWindowTransitionOutcome close = EWorkspaceWindowTransitionOutcome::Succeeded;
	EWorkspaceWindowTransitionOutcome cleanup = EWorkspaceWindowTransitionOutcome::Succeeded;
	std::function<void()> cleanupAction;
	std::vector<std::string> order;
};

std::deque<DWORD> g_shutdownWaitResults;
BOOL g_shutdownTerminateResult = TRUE;

DWORD WINAPI InjectedShutdownWait(HANDLE, DWORD)
{
	if (g_shutdownWaitResults.empty()) return WAIT_FAILED;
	const DWORD result = g_shutdownWaitResults.front();
	g_shutdownWaitResults.pop_front();
	return result;
}

BOOL WINAPI InjectedShutdownTerminate(HANDLE, UINT)
{
	return g_shutdownTerminateResult;
}

CFailedEditorProcessShutdown::Operations InjectedShutdownOperations()
{
	return { InjectedShutdownWait, InjectedShutdownTerminate };
}

TEST(WorkspaceWindowTransitionService, ReplacementPreflightIsTerminalAndCleansManagedTargets)
{
	TransitionHost host;
	host.prepare = EWorkspaceWindowTransitionOutcome::Cancelled;
	auto result = CWorkspaceWindowTransitionService::Execute(
		{ .replaceCurrentWindow = true, .deleteStagedTargetOnFailure = true }, host);
	EXPECT_EQ(EWorkspaceWindowTransitionOutcome::Cancelled, result);
	EXPECT_EQ((std::vector<std::string>{ "prepare", "cleanup" }), host.order);

	host = {};
	host.prepare = EWorkspaceWindowTransitionOutcome::Failed;
	result = CWorkspaceWindowTransitionService::Execute(
		{ .replaceCurrentWindow = true, .deleteStagedTargetOnFailure = true }, host);
	EXPECT_EQ(EWorkspaceWindowTransitionOutcome::Failed, result);
	EXPECT_EQ((std::vector<std::string>{ "prepare", "cleanup" }), host.order);

	host = {};
	host.prepare = EWorkspaceWindowTransitionOutcome::Cancelled;
	host.cleanup = EWorkspaceWindowTransitionOutcome::Failed;
	result = CWorkspaceWindowTransitionService::Execute(
		{ .replaceCurrentWindow = true, .deleteStagedTargetOnFailure = true }, host);
	EXPECT_EQ(EWorkspaceWindowTransitionOutcome::Failed, result);
	EXPECT_EQ((std::vector<std::string>{ "prepare", "cleanup" }), host.order);
}

TEST(WorkspaceWindowTransitionService, ReplacementLaunchFailureNeverClosesOldWindowAndCleansManagedTarget)
{
	TransitionHost host;
	host.launch = EWorkspaceWindowTransitionOutcome::Failed;
	auto result = CWorkspaceWindowTransitionService::Execute(
		{ .replaceCurrentWindow = true, .deleteStagedTargetOnFailure = true }, host);
	EXPECT_EQ(EWorkspaceWindowTransitionOutcome::Failed, result);
	EXPECT_EQ((std::vector<std::string>{ "prepare", "launch", "cleanup" }), host.order);

	host = {};
	host.launch = EWorkspaceWindowTransitionOutcome::Failed; // bounded ready timeout maps to Failed.
	host.cleanup = EWorkspaceWindowTransitionOutcome::Failed;
	result = CWorkspaceWindowTransitionService::Execute(
		{ .replaceCurrentWindow = true, .deleteStagedTargetOnFailure = true }, host);
	EXPECT_EQ(EWorkspaceWindowTransitionOutcome::Failed, result);
	EXPECT_EQ((std::vector<std::string>{ "prepare", "launch", "cleanup" }), host.order);

	host = {};
	host.launch = EWorkspaceWindowTransitionOutcome::Failed;
	result = CWorkspaceWindowTransitionService::Execute(
		{ .replaceCurrentWindow = true, .deleteStagedTargetOnFailure = false }, host);
	EXPECT_EQ(EWorkspaceWindowTransitionOutcome::Failed, result);
	EXPECT_EQ((std::vector<std::string>{ "prepare", "launch" }), host.order);
}

TEST(WorkspaceWindowTransitionService, ReplacementClosesExactlyOnceOnlyAfterReady)
{
	TransitionHost host;
	auto result = CWorkspaceWindowTransitionService::Execute(
		{ .replaceCurrentWindow = true, .deleteStagedTargetOnFailure = true }, host);
	EXPECT_EQ(EWorkspaceWindowTransitionOutcome::Succeeded, result);
	EXPECT_EQ((std::vector<std::string>{ "prepare", "launch", "close" }), host.order);

	host = {};
	host.close = EWorkspaceWindowTransitionOutcome::Failed;
	result = CWorkspaceWindowTransitionService::Execute(
		{ .replaceCurrentWindow = true, .deleteStagedTargetOnFailure = true }, host);
	EXPECT_EQ(EWorkspaceWindowTransitionOutcome::Failed, result);
	EXPECT_EQ((std::vector<std::string>{ "prepare", "launch", "close" }), host.order);
}

TEST(WorkspaceWindowTransitionService, DuplicateNeverPreflightsOrClosesTheCurrentWindow)
{
	TransitionHost host;
	auto result = CWorkspaceWindowTransitionService::Execute(
		{ .replaceCurrentWindow = false, .deleteStagedTargetOnFailure = true }, host);
	EXPECT_EQ(EWorkspaceWindowTransitionOutcome::Succeeded, result);
	EXPECT_EQ((std::vector<std::string>{ "launch" }), host.order);

	host = {};
	host.launch = EWorkspaceWindowTransitionOutcome::Failed;
	result = CWorkspaceWindowTransitionService::Execute(
		{ .replaceCurrentWindow = false, .deleteStagedTargetOnFailure = true }, host);
	EXPECT_EQ(EWorkspaceWindowTransitionOutcome::Failed, result);
	EXPECT_EQ((std::vector<std::string>{ "launch", "cleanup" }), host.order);
}

TEST(WorkspaceWindowTransitionService, PlannerCoversEmptyFolderAndWorkspaceFlows)
{
	const auto target = WorkspaceUri(L"file:///C:/Temp/untitled.code-workspace");
	const auto root = WorkspaceUri(L"file:///C:/Project");
	const auto added = WorkspaceUri(L"file:///C:/Added");
	const auto configuration = WorkspaceUri(L"file:///C:/Project/project.code-workspace");

	config::WorkspaceContextSnapshot empty;
	const auto emptyAdd = CWorkspaceWindowTransitionPlanner::BuildWorkspaceDocumentEdit(
		empty, target, { { added, L"Added" } });
	EXPECT_EQ(target.ToString(), emptyAdd.source.ToString());
	EXPECT_EQ(target.ToString(), emptyAdd.target.ToString());
	ASSERT_EQ(1U, emptyAdd.folders.size());
	EXPECT_EQ(added.ToString(), emptyAdd.folders.front().uri.ToString());

	config::WorkspaceContextSnapshot folder;
	folder.kind = config::EWorkspaceKind::Folder;
	folder.folders.push_back({ root, L"Project" });
	const auto folderAdd = CWorkspaceWindowTransitionPlanner::BuildWorkspaceDocumentEdit(
		folder, target, { { added, std::nullopt } });
	EXPECT_EQ(target.ToString(), folderAdd.source.ToString());
	ASSERT_EQ(2U, folderAdd.folders.size());
	EXPECT_EQ(root.ToString(), folderAdd.folders[0].uri.ToString());
	EXPECT_EQ(added.ToString(), folderAdd.folders[1].uri.ToString());

	config::WorkspaceContextSnapshot workspace;
	workspace.kind = config::EWorkspaceKind::Workspace;
	workspace.workspaceConfigUri = configuration;
	workspace.folders.push_back({ root, L"Project" });
	const auto workspaceCopy = CWorkspaceWindowTransitionPlanner::BuildWorkspaceDocumentEdit(workspace, target);
	EXPECT_EQ(configuration.ToString(), workspaceCopy.source.ToString());
	EXPECT_EQ(target.ToString(), workspaceCopy.target.ToString());
	ASSERT_EQ(1U, workspaceCopy.folders.size());
	EXPECT_EQ(root.ToString(), workspaceCopy.folders.front().uri.ToString());
	EXPECT_FALSE(workspaceCopy.folders.front().label.has_value());
	const auto workspaceInPlace = CWorkspaceWindowTransitionPlanner::BuildWorkspaceDocumentEdit(
		workspace, configuration, { { added, std::nullopt } });
	EXPECT_EQ(configuration.ToString(), workspaceInPlace.source.ToString());
	EXPECT_EQ(configuration.ToString(), workspaceInPlace.target.ToString());
	ASSERT_EQ(2U, workspaceInPlace.folders.size());

	EXPECT_EQ((WorkspaceWindowTransitionRequest{ true, true }).replaceCurrentWindow,
		CWorkspaceWindowTransitionPlanner::ManagedReplacement().replaceCurrentWindow);
	EXPECT_TRUE(CWorkspaceWindowTransitionPlanner::ManagedReplacement().deleteStagedTargetOnFailure);
	EXPECT_FALSE(CWorkspaceWindowTransitionPlanner::SaveAsReplacement().deleteStagedTargetOnFailure);
	EXPECT_FALSE(CWorkspaceWindowTransitionPlanner::ManagedDuplicate().replaceCurrentWindow);
	EXPECT_TRUE(CWorkspaceWindowTransitionPlanner::ManagedDuplicate().deleteStagedTargetOnFailure);
	EXPECT_TRUE(CWorkspaceWindowTransitionPlanner::CloseToEmpty().replaceCurrentWindow);
}

TEST(WorkspaceWindowTransitionService, FailedAddHandoffLeavesWorkspaceSourceBytesUntouchedAndCleansStagedTarget)
{
	const auto source = WorkspaceUri(L"file:///C:/Project/project.code-workspace");
	const auto target = WorkspaceUri(L"file:///C:/Temp/untitled.code-workspace");
	const auto root = WorkspaceUri(L"file:///C:/Project");
	const auto added = WorkspaceUri(L"file:///C:/Added");
	const std::string sourceDocument = "{\r\n  // retained source\r\n  \"folders\": [ { \"path\": \".\" } ],\r\n  \"settings\": { \"editor.tabSize\": 2 },\r\n}\r\n";

	StagedWorkspaceFileService files;
	files.reads.emplace(source.ToString(), Snapshot(sourceDocument));
	CWorkspaceEditingService editor(files);
	const auto edited = editor.ReplaceFolders({ source, target, { { root, L"Project" }, { added, L"Added" } } });
	ASSERT_EQ(EWorkspaceEditingOutcome::Succeeded, edited.outcome);
	ASSERT_EQ(1U, files.writes.count(target.ToString()));
	EXPECT_EQ(sourceDocument, Text(files.reads.at(source.ToString()).value->bytes));

	TransitionHost host;
	host.launch = EWorkspaceWindowTransitionOutcome::Failed; // launch/ready timeout has the same terminal ownership.
	host.cleanupAction = [&files, &target]() { files.writes.erase(target.ToString()); };
	const auto transition = CWorkspaceWindowTransitionService::Execute(
		CWorkspaceWindowTransitionPlanner::ManagedReplacement(), host);
	EXPECT_EQ(EWorkspaceWindowTransitionOutcome::Failed, transition);
	EXPECT_EQ((std::vector<std::string>{ "prepare", "launch", "cleanup" }), host.order);
	EXPECT_EQ(0U, files.writes.count(target.ToString()));
	EXPECT_EQ(sourceDocument, Text(files.reads.at(source.ToString()).value->bytes));
}

TEST(FailedEditorProcessShutdown, JoinsSuspendedSuccessorBeforeReturning)
{
	WCHAR executable[MAX_PATH + 1]{};
	ASSERT_NE(0U, ::GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))));
	std::wstring commandLine = L"\"";
	commandLine += executable;
	commandLine += L"\" --gtest_filter=FailedEditorProcessShutdown.NoSuchTest";
	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process{};
	ASSERT_NE(FALSE, ::CreateProcessW(executable, commandLine.data(), nullptr, nullptr, FALSE,
		CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
	::CloseHandle(process.hThread);

	ASSERT_EQ(EFailedEditorProcessShutdownResult::Stopped,
		CFailedEditorProcessShutdown::TerminateAndWait(process.hProcess, ERROR_TIMEOUT));
	DWORD exitCode = 0;
	ASSERT_NE(FALSE, ::GetExitCodeProcess(process.hProcess, &exitCode));
	EXPECT_EQ(static_cast<DWORD>(ERROR_TIMEOUT), exitCode);
	::CloseHandle(process.hProcess);
}

TEST(FailedEditorProcessShutdown, ReportsUnresolvedWhenTerminationAndRecheckCannotProveExit)
{
	g_shutdownWaitResults = { WAIT_FAILED, WAIT_TIMEOUT };
	g_shutdownTerminateResult = FALSE;
	EXPECT_EQ(EFailedEditorProcessShutdownResult::OwnershipUnresolved,
		CFailedEditorProcessShutdown::TerminateAndWait(reinterpret_cast<HANDLE>(1), ERROR_TIMEOUT,
			InjectedShutdownOperations()));
	EXPECT_TRUE(g_shutdownWaitResults.empty());
}

TEST(FailedEditorProcessShutdown, ReportsUnresolvedWhenFinalJoinFails)
{
	g_shutdownWaitResults = { WAIT_TIMEOUT, WAIT_FAILED };
	g_shutdownTerminateResult = TRUE;
	EXPECT_EQ(EFailedEditorProcessShutdownResult::OwnershipUnresolved,
		CFailedEditorProcessShutdown::TerminateAndWait(reinterpret_cast<HANDLE>(1), ERROR_TIMEOUT,
			InjectedShutdownOperations()));
	EXPECT_TRUE(g_shutdownWaitResults.empty());
}

TEST(FailedEditorProcessShutdown, TerminateFailureStillSucceedsWhenRecheckProvesExit)
{
	g_shutdownWaitResults = { WAIT_TIMEOUT, WAIT_OBJECT_0 };
	g_shutdownTerminateResult = FALSE;
	EXPECT_EQ(EFailedEditorProcessShutdownResult::Stopped,
		CFailedEditorProcessShutdown::TerminateAndWait(reinterpret_cast<HANDLE>(1), ERROR_TIMEOUT,
			InjectedShutdownOperations()));
	EXPECT_TRUE(g_shutdownWaitResults.empty());
}

} // namespace
