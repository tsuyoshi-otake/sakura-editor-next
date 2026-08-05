/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "workbench/workspace/WorkspaceArtifactDocumentSourceController.h"

#include <sakura/serialization/JsoncDocument.h>
#include <sakura/uri/UriIdentity.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace workbench::workspace {
namespace {

using platform::filesystem::EFileResultStatus;
using platform::filesystem::EFileWatchEventType;
using platform::filesystem::FileReadOptions;
using platform::filesystem::FileWatchEvent;
using platform::filesystem::FileWatchOptions;
using platform::uri::Uri;
using platform::uri::UriIdentityService;

constexpr std::size_t kMaximumWorkspaceFolders = 64;

bool IsUsableResource(const Uri& resource) noexcept
{
	return resource.Scheme() == L"file" && !resource.Path().empty()
		&& !resource.Query() && !resource.Fragment();
}

std::optional<Uri> ParentDirectory(const Uri& resource)
{
	const auto separator = resource.Path().find_last_of(L'/');
	if (separator == std::wstring::npos || separator == 0) return std::nullopt;
	return Uri::FromComponents(resource.Scheme(), resource.Authority(), resource.Path().substr(0, separator),
		std::nullopt, std::nullopt, resource.HasAuthority()).value;
}

std::optional<Uri> ChildResource(const Uri& directory, const wchar_t* name)
{
	if (directory.Path().empty() || directory.Query() || directory.Fragment()) return std::nullopt;
	std::wstring path = directory.Path();
	if (path.back() != L'/') path.push_back(L'/');
	path += name;
	return Uri::FromComponents(directory.Scheme(), directory.Authority(), std::move(path),
		std::nullopt, std::nullopt, directory.HasAuthority()).value;
}

std::optional<Uri> VscodeDirectory(const Uri& folder) { return ChildResource(folder, L".vscode"); }

std::optional<Uri> ArtifactInFolder(const Uri& folder, EWorkspaceArtifactDocumentKind kind)
{
	auto vscode = VscodeDirectory(folder);
	if (!vscode) return std::nullopt;
	switch (kind) {
	case EWorkspaceArtifactDocumentKind::Tasks: return ChildResource(*vscode, L"tasks.json");
	case EWorkspaceArtifactDocumentKind::Launch: return ChildResource(*vscode, L"launch.json");
	case EWorkspaceArtifactDocumentKind::Extensions: return ChildResource(*vscode, L"extensions.json");
	}
	return std::nullopt;
}

} // namespace

struct CWorkspaceArtifactDocumentSourceController::WatchSlot final {
	struct Target final {
		std::optional<Uri> member;
		bool rebuildOnRelevantChange = false;
	};

	explicit WatchSlot(Uri rootValue) : root(std::move(rootValue)) {}
	Uri root;
	std::vector<Target> targets;
	std::unique_ptr<platform::filesystem::IFileWatch> watch;
	std::thread worker;
};

struct CWorkspaceArtifactDocumentSourceController::ReloadOneResult final {
	WorkspaceArtifactDocumentResult document;
	std::optional<EFileResultStatus> fileStatus;
};

CWorkspaceArtifactDocumentSourceController::CWorkspaceArtifactDocumentSourceController(
	platform::filesystem::IFileService& fileService,
	CWorkspaceArtifactDocumentService& documentService) noexcept
	: m_fileService(fileService), m_documentService(documentService)
{
}

CWorkspaceArtifactDocumentSourceController::~CWorkspaceArtifactDocumentSourceController()
{
	(void)Stop();
}

EWorkspaceArtifactDocumentSourceStatus CWorkspaceArtifactDocumentSourceController::ValidateRequest(
	const WorkspaceArtifactDocumentSourceRequest& request) noexcept
{
	if (request.generation == 0) return EWorkspaceArtifactDocumentSourceStatus::InvalidRequest;
	if (request.workspaceFolders.size() > kMaximumWorkspaceFolders) return EWorkspaceArtifactDocumentSourceStatus::CapacityExceeded;
	if (request.workspaceConfiguration && !IsUsableResource(*request.workspaceConfiguration)) {
		return EWorkspaceArtifactDocumentSourceStatus::InvalidRequest;
	}
	for (const auto& folder : request.workspaceFolders) {
		if (!IsUsableResource(folder)) return EWorkspaceArtifactDocumentSourceStatus::InvalidRequest;
	}
	return EWorkspaceArtifactDocumentSourceStatus::Started;
}

WorkspaceArtifactDocumentSourceResult CWorkspaceArtifactDocumentSourceController::Start(
	WorkspaceArtifactDocumentSourceRequest request, ReloadCallback callback) noexcept
{
	const auto validity = ValidateRequest(request);
	if (validity != EWorkspaceArtifactDocumentSourceStatus::Started) return { validity };
	{
		std::lock_guard lock(m_mutex);
		if (m_dispatcher.joinable() && m_dispatcher.get_id() == std::this_thread::get_id()) {
			return { EWorkspaceArtifactDocumentSourceStatus::ReentrantStopDenied };
		}
	}
	std::lock_guard lifecycleLock(m_lifecycleMutex);
	const auto generation = request.generation;
	const auto stopped = StopLocked();
	if (!stopped.Succeeded() && stopped.status != EWorkspaceArtifactDocumentSourceStatus::NotStarted) return stopped;
	try {
		{
			std::lock_guard lock(m_mutex);
			m_request = std::move(request);
			m_callback = std::move(callback);
			m_stopping = false;
			m_started = true;
		}
		(void)m_documentService.BeginGeneration(generation);
		auto initial = ReloadSnapshot();
		{
			std::lock_guard lock(m_mutex);
			StartWorkersLocked();
		}
		std::thread dispatcher(&CWorkspaceArtifactDocumentSourceController::DispatchMain, this);
		{
			std::lock_guard lock(m_mutex);
			m_dispatcher = std::move(dispatcher);
		}
		initial.status = EWorkspaceArtifactDocumentSourceStatus::Started;
		return initial;
	} catch (...) {
		(void)StopLocked();
		return { EWorkspaceArtifactDocumentSourceStatus::StartFailed };
	}
}

WorkspaceArtifactDocumentSourceResult CWorkspaceArtifactDocumentSourceController::Update(
	WorkspaceArtifactDocumentSourceRequest request) noexcept
{
	const auto validity = ValidateRequest(request);
	if (validity != EWorkspaceArtifactDocumentSourceStatus::Started) return { validity };
	std::lock_guard lifecycleLock(m_lifecycleMutex);
	const auto generation = request.generation;
	{
		std::lock_guard lock(m_mutex);
		if (!m_started || m_stopping) return { EWorkspaceArtifactDocumentSourceStatus::NotStarted };
		if (!m_request || request.generation <= m_request->generation) return { EWorkspaceArtifactDocumentSourceStatus::InvalidRequest };
		m_request = std::move(request);
		m_rebuildRequested = true;
		m_reloadPending = true;
	}
	(void)m_documentService.BeginGeneration(generation);
	{
		std::lock_guard lock(m_mutex);
		if (!m_started || m_stopping) return { EWorkspaceArtifactDocumentSourceStatus::NotStarted };
		m_pending.notify_one();
	}
	return { EWorkspaceArtifactDocumentSourceStatus::Updated };
}

WorkspaceArtifactDocumentSourceResult CWorkspaceArtifactDocumentSourceController::Reload() noexcept
{
	if (!CanDispatch()) return { EWorkspaceArtifactDocumentSourceStatus::NotStarted };
	return ReloadSnapshot();
}

void CWorkspaceArtifactDocumentSourceController::StartWorkersLocked()
{
	if (!m_request) return;
	auto add = [this](const Uri& root, std::optional<Uri> member, bool rebuildOnRelevantChange) {
		for (const auto& existing : m_slots) {
			if (!UriIdentityService::IsEqual(existing->root, root)) continue;
			const auto duplicate = std::any_of(existing->targets.begin(), existing->targets.end(),
				[&member, rebuildOnRelevantChange](const WatchSlot::Target& target) {
					return target.rebuildOnRelevantChange == rebuildOnRelevantChange
						&& target.member.has_value() == member.has_value()
						&& (!target.member || UriIdentityService::IsEqual(*target.member, *member));
				});
			if (!duplicate) existing->targets.push_back({ std::move(member), rebuildOnRelevantChange });
			return;
		}
		auto watched = m_fileService.Watch(root, FileWatchOptions { .recursive = false });
		if (!watched.Succeeded() || !watched.value) return;
		auto slot = std::make_unique<WatchSlot>(root);
		slot->targets.push_back({ std::move(member), rebuildOnRelevantChange });
		slot->watch = std::move(*watched.value);
		m_slots.push_back(std::move(slot));
	};

	if (m_request->workspaceConfiguration) {
		if (auto parent = ParentDirectory(*m_request->workspaceConfiguration)) add(*parent, m_request->workspaceConfiguration, false);
	}
	for (const auto& folder : m_request->workspaceFolders) {
		auto vscode = VscodeDirectory(folder);
		if (vscode) add(folder, vscode, true);
		if (!vscode) continue;
		for (const auto kind : { EWorkspaceArtifactDocumentKind::Tasks, EWorkspaceArtifactDocumentKind::Launch, EWorkspaceArtifactDocumentKind::Extensions }) {
			if (auto artifact = ArtifactInFolder(folder, kind)) add(*vscode, std::move(artifact), false);
		}
	}
	for (const auto& slot : m_slots) slot->worker = std::thread(&CWorkspaceArtifactDocumentSourceController::WorkerMain, this, slot.get());
}

void CWorkspaceArtifactDocumentSourceController::CancelAndJoinWorkers() noexcept
{
	std::vector<std::unique_ptr<WatchSlot>> slots;
	{
		std::lock_guard lock(m_mutex);
		for (const auto& slot : m_slots) {
			if (slot->watch) { try { (void)slot->watch->Cancel(); } catch (...) {} }
		}
		slots.swap(m_slots);
	}
	for (const auto& slot : slots) if (slot->worker.joinable()) slot->worker.join();
}

WorkspaceArtifactDocumentSourceResult CWorkspaceArtifactDocumentSourceController::Stop() noexcept
{
	{
		std::lock_guard lock(m_mutex);
		if (m_dispatcher.joinable() && m_dispatcher.get_id() == std::this_thread::get_id()) {
			return { EWorkspaceArtifactDocumentSourceStatus::ReentrantStopDenied };
		}
	}
	std::lock_guard lifecycleLock(m_lifecycleMutex);
	return StopLocked();
}

WorkspaceArtifactDocumentSourceResult CWorkspaceArtifactDocumentSourceController::StopLocked() noexcept
{
	{
		std::unique_lock lock(m_mutex);
		if (!m_started) return { EWorkspaceArtifactDocumentSourceStatus::NotStarted };
		m_stopping = true;
		for (const auto& slot : m_slots) if (slot->watch) { try { (void)slot->watch->Cancel(); } catch (...) {} }
		m_pending.notify_all();
		// A read can complete after cancellation. Once m_stopping is set, no new
		// operation may enter this fence; wait for any already-entered Apply/Remove
		// call before reporting a terminal stopped lifecycle.
		m_documentOperationsComplete.wait(lock, [this] { return m_activeDocumentOperations == 0; });
	}
	if (m_dispatcher.joinable()) m_dispatcher.join();
	CancelAndJoinWorkers();
	std::lock_guard lock(m_mutex);
	m_started = false;
	m_callback = {};
	m_request.reset();
	m_rebuildRequested = m_reloadPending = m_rebuilding = false;
	return { EWorkspaceArtifactDocumentSourceStatus::Stopped };
}

bool CWorkspaceArtifactDocumentSourceController::BeginDocumentOperation() noexcept
{
	std::lock_guard lock(m_mutex);
	if (!m_started || m_stopping) return false;
	++m_activeDocumentOperations;
	return true;
}

void CWorkspaceArtifactDocumentSourceController::EndDocumentOperation() noexcept
{
	std::lock_guard lock(m_mutex);
	if (m_activeDocumentOperations != 0) --m_activeDocumentOperations;
	if (m_activeDocumentOperations == 0) m_documentOperationsComplete.notify_all();
}

bool CWorkspaceArtifactDocumentSourceController::CanDispatch() const noexcept
{
	std::lock_guard lock(m_mutex);
	return m_started && !m_stopping;
}

bool CWorkspaceArtifactDocumentSourceController::IsRelevant(
	const WatchSlot& slot, std::size_t targetIndex, const FileWatchEvent& event) const noexcept
{
	const auto& target = slot.targets[targetIndex];
	if (!target.member) return true;
	return UriIdentityService::IsEqual(event.uri, *target.member)
		|| (event.previousUri && UriIdentityService::IsEqual(*event.previousUri, *target.member));
}

void CWorkspaceArtifactDocumentSourceController::Queue(bool rebuild) noexcept
{
	std::lock_guard lock(m_mutex);
	if (m_stopping || (m_rebuilding && rebuild)) return;
	m_reloadPending = true;
	m_rebuildRequested = m_rebuildRequested || rebuild;
	m_pending.notify_one();
}

void CWorkspaceArtifactDocumentSourceController::WorkerMain(WatchSlot* slot) noexcept
{
	for (;;) {
		platform::filesystem::FileResult<FileWatchEvent> event;
		try { event = slot->watch->Next(); }
		catch (...) { Queue(true); return; }
		if (!event.Succeeded() || !event.value) { Queue(true); return; }
		const bool invalidTopology = event.value->type == EFileWatchEventType::Overflow
			|| event.value->type == EFileWatchEventType::RescanRequired || event.value->type == EFileWatchEventType::Disposed;
		if (invalidTopology) { Queue(true); return; }
		for (std::size_t index = 0; index < slot->targets.size(); ++index) {
			if (IsRelevant(*slot, index, *event.value)) Queue(slot->targets[index].rebuildOnRelevantChange);
		}
	}
}

void CWorkspaceArtifactDocumentSourceController::RebuildTopology() noexcept
{
	{
		std::lock_guard lock(m_mutex);
		if (m_stopping) return;
		m_rebuilding = true;
	}
	CancelAndJoinWorkers();
	std::lock_guard lock(m_mutex);
	if (!m_stopping) StartWorkersLocked();
	m_rebuilding = false;
}

std::uint64_t CWorkspaceArtifactDocumentSourceController::NextRevision() noexcept
{
	std::lock_guard lock(m_mutex);
	if (m_nextRevision == (std::numeric_limits<std::uint64_t>::max)()) return 0;
	return m_nextRevision++;
}

CWorkspaceArtifactDocumentSourceController::ReloadOneResult CWorkspaceArtifactDocumentSourceController::ReloadOne(
	EWorkspaceArtifactDocumentKind kind, EWorkspaceArtifactDocumentSource source,
	const std::optional<Uri>& folderUri, const Uri& resource, std::uint64_t generation) noexcept
{
	bool documentOperationActive = false;
	std::optional<EFileResultStatus> failureStatus{ EFileResultStatus::Failed };
	try {
		const auto read = m_fileService.Read(resource,
			FileReadOptions { .maximumBytes = platform::serialization::CJsoncDocument::kMaximumInputBytes });
		const auto revision = NextRevision();
		if (!read.Succeeded()) failureStatus = read.status;

		// The read may have blocked while Stop cancelled the lifecycle. Claim the
		// operation immediately before each service mutation so Stop either waits
		// for that mutation or prevents it from beginning.
		if (!BeginDocumentOperation()) {
			return { { EWorkspaceArtifactDocumentStatus::Stopped, std::nullopt,
				"workspace artifact source controller is stopped" }, read.status };
		}
		documentOperationActive = true;
		if (read.Succeeded() && read.value) {
			auto result = m_documentService.Apply({ kind, source, folderUri, resource, generation, revision,
				std::string(read.value->begin(), read.value->end()) });
			EndDocumentOperation();
			documentOperationActive = false;
			return { std::move(result), std::nullopt };
		}
		if (read.status == EFileResultStatus::NotFound) {
			auto result = m_documentService.Remove({ kind, source, folderUri, resource, generation, revision });
			EndDocumentOperation();
			documentOperationActive = false;
			return { std::move(result), read.status };
		}
		EndDocumentOperation();
		documentOperationActive = false;
		return { { EWorkspaceArtifactDocumentStatus::JsoncParseFailed, std::nullopt,
			"workspace artifact file could not be read" }, read.status };
	} catch (...) {
		// A provider or service fault must release the operation fence so Stop
		// always has a terminal owner and cannot wait forever.
		if (documentOperationActive) EndDocumentOperation();
		return { { EWorkspaceArtifactDocumentStatus::JsoncParseFailed, std::nullopt,
			"workspace artifact source operation failed" }, failureStatus };
	}
}

WorkspaceArtifactDocumentSourceResult CWorkspaceArtifactDocumentSourceController::ReloadSnapshot() noexcept
{
	WorkspaceArtifactDocumentSourceRequest request;
	{
		std::lock_guard lock(m_mutex);
		if (!m_started || m_stopping || !m_request) return { EWorkspaceArtifactDocumentSourceStatus::NotStarted };
		request = *m_request;
	}
	WorkspaceArtifactDocumentSourceResult result { EWorkspaceArtifactDocumentSourceStatus::Reloaded };
	auto reload = [&](EWorkspaceArtifactDocumentKind kind, EWorkspaceArtifactDocumentSource source,
		const std::optional<Uri>& folder, const Uri& resource) {
		auto reloaded = ReloadOne(kind, source, folder, resource, request.generation);
		if (reloaded.fileStatus && *reloaded.fileStatus != EFileResultStatus::NotFound) {
			result.status = EWorkspaceArtifactDocumentSourceStatus::ReadFailed;
			result.fileStatus = reloaded.fileStatus;
		}
		result.documents.push_back(std::move(reloaded.document));
	};
	if (request.workspaceConfiguration) {
		for (const auto kind : { EWorkspaceArtifactDocumentKind::Tasks, EWorkspaceArtifactDocumentKind::Launch, EWorkspaceArtifactDocumentKind::Extensions }) {
			reload(kind, EWorkspaceArtifactDocumentSource::WorkspaceFile, std::nullopt, *request.workspaceConfiguration);
		}
	}
	for (const auto& folder : request.workspaceFolders) {
		for (const auto kind : { EWorkspaceArtifactDocumentKind::Tasks, EWorkspaceArtifactDocumentKind::Launch, EWorkspaceArtifactDocumentKind::Extensions }) {
			if (auto resource = ArtifactInFolder(folder, kind)) reload(kind, EWorkspaceArtifactDocumentSource::Folder, folder, *resource);
		}
	}
	return result;
}

void CWorkspaceArtifactDocumentSourceController::DispatchMain() noexcept
{
	for (;;) {
		bool rebuild = false;
		ReloadCallback callback;
		{
			std::unique_lock lock(m_mutex);
			m_pending.wait(lock, [this] { return m_stopping || m_rebuildRequested || m_reloadPending; });
			if (m_stopping) return;
			(void)m_pending.wait_for(lock, std::chrono::milliseconds(25), [this] { return m_stopping; });
			if (m_stopping) return;
			rebuild = m_rebuildRequested;
			m_rebuildRequested = m_reloadPending = false;
			callback = m_callback;
		}
		if (rebuild) RebuildTopology();
		auto result = ReloadSnapshot();
		if (callback && CanDispatch()) { try { callback(result); } catch (...) {} }
	}
}

} // namespace workbench::workspace
