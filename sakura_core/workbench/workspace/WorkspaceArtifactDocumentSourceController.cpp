/*! @file
 * @brief File-backed, cancellable source adapter for workspace artifacts.
 */
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
#include <exception>
#include <limits>
#include <stdexcept>
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
	}
	return std::nullopt;
}

struct TopologyEntry final {
	Uri root;
	struct Target final {
		std::optional<Uri> member;
		bool rebuildOnRelevantChange = false;
	};
	std::vector<Target> targets;
};

bool SameTarget(const TopologyEntry::Target& entry,
	const std::optional<Uri>& member, bool rebuildOnRelevantChange) noexcept
{
	return entry.rebuildOnRelevantChange == rebuildOnRelevantChange
		&& entry.member.has_value() == member.has_value()
		&& (!entry.member || UriIdentityService::IsEqual(*entry.member, *member));
}

std::vector<TopologyEntry> BuildTopology(const WorkspaceArtifactDocumentSourceRequest& request)
{
	std::vector<TopologyEntry> topology;
	auto add = [&topology](const Uri& root, std::optional<Uri> member, bool rebuildOnRelevantChange) {
		for (auto& existing : topology) {
			if (!UriIdentityService::IsEqual(existing.root, root)) continue;
			const auto duplicate = std::any_of(existing.targets.begin(), existing.targets.end(),
				[&member, rebuildOnRelevantChange](const auto& target) {
					return SameTarget(target, member, rebuildOnRelevantChange);
				});
			if (!duplicate) existing.targets.push_back({ std::move(member), rebuildOnRelevantChange });
			return;
		}
		topology.push_back({ root, { { std::move(member), rebuildOnRelevantChange } } });
	};

	if (request.workspaceConfiguration) {
		if (auto parent = ParentDirectory(*request.workspaceConfiguration)) {
			add(*parent, request.workspaceConfiguration, false);
		}
	}
	for (const auto& folder : request.workspaceFolders) {
		auto vscode = VscodeDirectory(folder);
		if (vscode) add(folder, vscode, true);
		if (!vscode) continue;
		for (const auto kind : { EWorkspaceArtifactDocumentKind::Tasks, EWorkspaceArtifactDocumentKind::Launch }) {
			if (auto artifact = ArtifactInFolder(folder, kind)) add(*vscode, std::move(artifact), false);
		}
	}
	return topology;
}

} // namespace

struct CWorkspaceArtifactDocumentSourceController::WatchSlot final {
	struct Target final {
		std::optional<Uri> member;
		bool rebuildOnRelevantChange = false;
	};

	struct WatchHandle final {
		std::unique_ptr<platform::filesystem::IFileWatch> watch;
	};

	explicit WatchSlot(Uri rootValue) : root(std::move(rootValue)) {}
	Uri root;
	std::vector<Target> targets;
	std::shared_ptr<WatchHandle> handle;
	std::thread worker;
	std::optional<workbench::WorkerRetirementService::Reservation> workerRetirement;
};

struct CWorkspaceArtifactDocumentSourceController::SharedState final {
	SharedState(
		std::shared_ptr<platform::filesystem::IFileService> fileServiceValue,
		std::shared_ptr<CWorkspaceArtifactDocumentService> documentServiceValue)
		: fileService(std::move(fileServiceValue))
		, documentService(std::move(documentServiceValue))
	{
	}

	std::shared_ptr<platform::filesystem::IFileService> fileService;
	std::shared_ptr<CWorkspaceArtifactDocumentService> documentService;
	mutable std::mutex mutex;
	std::condition_variable pending;
	std::optional<WorkspaceArtifactDocumentSourceRequest> request;
	ReloadCallback callback;
	std::vector<std::shared_ptr<WatchSlot>> slots;
	std::thread dispatcher;
	std::optional<workbench::WorkerRetirementService::Reservation> dispatcherRetirement;
	std::uint64_t nextRevision = 1;
	bool started = false;
	bool stopping = false;
	bool rebuildRequested = false;
	bool reloadPending = false;
	bool rebuilding = false;
	std::size_t activeDocumentOperations = 0;
	std::size_t liveThreads = 0;
	bool retirementFinalized = false;
};

struct CWorkspaceArtifactDocumentSourceController::ReloadOneResult final {
	WorkspaceArtifactDocumentResult document;
	std::optional<EFileResultStatus> fileStatus;
};

CWorkspaceArtifactDocumentSourceController::CWorkspaceArtifactDocumentSourceController(
	std::shared_ptr<platform::filesystem::IFileService> fileService,
	std::shared_ptr<CWorkspaceArtifactDocumentService> documentService) noexcept
	: m_fileService(std::move(fileService))
	, m_documentService(std::move(documentService))
	, m_state(std::make_shared<SharedState>(m_fileService, m_documentService))
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
		const auto state = m_state;
		std::lock_guard lock(state->mutex);
		if (state->dispatcher.joinable() && state->dispatcher.get_id() == std::this_thread::get_id()) {
			return { EWorkspaceArtifactDocumentSourceStatus::ReentrantStopDenied };
		}
	}
	std::lock_guard lifecycleLock(m_lifecycleMutex);
	const auto stopped = StopLocked();
	if (!stopped.Succeeded() && stopped.status != EWorkspaceArtifactDocumentSourceStatus::NotStarted) return stopped;

	const auto state = std::make_shared<SharedState>(m_fileService, m_documentService);
	m_state = state;
	auto dispatcherRetirement = workbench::WorkerRetirementService::Instance().TryReserve();
	if (!dispatcherRetirement) return { EWorkspaceArtifactDocumentSourceStatus::RetirementUnavailable };
	{
		std::lock_guard lock(state->mutex);
		state->request = std::move(request);
		state->callback = std::move(callback);
		state->dispatcherRetirement.emplace(std::move(*dispatcherRetirement));
		state->started = true;
	}

	try {
		if (!state->fileService || !state->documentService) throw std::runtime_error("workspace artifact source dependencies are unavailable");
		const auto generation = state->request->generation;
		const auto generationResult = state->documentService->BeginGeneration(generation);
		if (generationResult.status == EWorkspaceArtifactDocumentStatus::Stopped) {
			throw std::runtime_error("workspace artifact document service is stopped");
		}
		auto initial = ReloadSnapshot(state);
		{
			std::lock_guard lock(state->mutex);
			if (!StartWorkersLocked(state)) throw std::runtime_error("workspace artifact watch worker creation failed");
			state->retirementFinalized = false;
			++state->liveThreads;
			try {
				state->dispatcher = std::thread([state] { DispatchMain(state); });
			} catch (...) {
				--state->liveThreads;
				throw;
			}
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
	const auto state = m_state;
	const auto generation = request.generation;
	{
		std::lock_guard lock(state->mutex);
		if (!state->started || state->stopping) return { EWorkspaceArtifactDocumentSourceStatus::NotStarted };
		if (!state->request || request.generation <= state->request->generation) return { EWorkspaceArtifactDocumentSourceStatus::InvalidRequest };
		state->request = std::move(request);
		state->rebuildRequested = true;
		state->reloadPending = true;
	}
	if (!state->documentService || state->documentService->BeginGeneration(generation).status == EWorkspaceArtifactDocumentStatus::Stopped) {
		return { EWorkspaceArtifactDocumentSourceStatus::NotStarted };
	}
	{
		std::lock_guard lock(state->mutex);
		if (!state->started || state->stopping) return { EWorkspaceArtifactDocumentSourceStatus::NotStarted };
		state->pending.notify_one();
	}
	return { EWorkspaceArtifactDocumentSourceStatus::Updated };
}

WorkspaceArtifactDocumentSourceResult CWorkspaceArtifactDocumentSourceController::Reload() noexcept
{
	const auto state = m_state;
	if (!CanDispatch(state)) return { EWorkspaceArtifactDocumentSourceStatus::NotStarted };
	return ReloadSnapshot(state);
}

bool CWorkspaceArtifactDocumentSourceController::StartWorkersLocked(const std::shared_ptr<SharedState>& state)
{
	if (!state->request || !state->fileService) return false;
	for (const auto& entry : BuildTopology(*state->request)) {
		const auto duplicate = std::any_of(state->slots.begin(), state->slots.end(),
			[&entry](const auto& slot) { return UriIdentityService::IsEqual(slot->root, entry.root); });
		if (duplicate) continue;
		auto retirement = workbench::WorkerRetirementService::Instance().TryReserve();
		if (!retirement) continue; // Watch support is explicitly best effort.
		platform::filesystem::FileResult<std::unique_ptr<platform::filesystem::IFileWatch>> watched;
		try {
			watched = state->fileService->Watch(entry.root, FileWatchOptions { .recursive = false });
		} catch (...) {
			continue;
		}
		if (!watched.Succeeded() || !watched.value) continue;
		auto slot = std::make_shared<WatchSlot>(entry.root);
		for (const auto& target : entry.targets) {
			slot->targets.push_back({ target.member, target.rebuildOnRelevantChange });
		}
		slot->handle = std::make_shared<WatchSlot::WatchHandle>();
		slot->handle->watch = std::move(*watched.value);
		slot->workerRetirement.emplace(std::move(*retirement));
		state->slots.push_back(std::move(slot));
	}

	for (const auto& slot : state->slots) {
		if (!slot->workerRetirement || slot->worker.joinable()) continue;
		++state->liveThreads;
		try {
			slot->worker = std::thread([state, slot] { WorkerMain(state, slot); });
		} catch (...) {
			--state->liveThreads;
			if (slot->handle && slot->handle->watch) {
				try { (void)slot->handle->watch->Cancel(); } catch (...) {}
			}
			slot->workerRetirement.reset();
			return false;
		}
	}
	return true;
}

void CWorkspaceArtifactDocumentSourceController::CancelAndRetireWorkers(
	const std::shared_ptr<SharedState>& state) noexcept
{
	std::vector<std::shared_ptr<WatchSlot>> slots;
	{
		std::lock_guard lock(state->mutex);
		slots.swap(state->slots);
	}
	for (const auto& slot : slots) {
		if (slot->handle && slot->handle->watch) {
			try { (void)slot->handle->watch->Cancel(); } catch (...) {}
		}
	}
	for (const auto& slot : slots) {
		if (!slot->worker.joinable()) {
			slot->workerRetirement.reset();
			continue;
		}
		if (!slot->workerRetirement) std::terminate();
		const auto status = workbench::WorkerRetirementService::Instance().Retire(
			std::move(slot->worker), std::move(*slot->workerRetirement), state);
		if (status != workbench::WorkerRetirementStatus::Retired) std::terminate();
		slot->workerRetirement.reset();
	}
}

WorkspaceArtifactDocumentSourceResult CWorkspaceArtifactDocumentSourceController::Stop() noexcept
{
	{
		const auto state = m_state;
		std::lock_guard lock(state->mutex);
		if (state->dispatcher.joinable() && state->dispatcher.get_id() == std::this_thread::get_id()) {
			return { EWorkspaceArtifactDocumentSourceStatus::ReentrantStopDenied };
		}
	}
	std::lock_guard lifecycleLock(m_lifecycleMutex);
	return StopLocked();
}

WorkspaceArtifactDocumentSourceResult CWorkspaceArtifactDocumentSourceController::StopLocked() noexcept
{
	const auto state = m_state;
	std::thread dispatcher;
	std::optional<workbench::WorkerRetirementService::Reservation> dispatcherRetirement;
	{
		std::lock_guard lock(state->mutex);
		if (!state->started) {
			return { EWorkspaceArtifactDocumentSourceStatus::NotStarted, std::nullopt, {},
				false, state->retirementFinalized };
		}
		state->stopping = true;
		state->pending.notify_all();
		dispatcher = std::move(state->dispatcher);
		dispatcherRetirement = std::move(state->dispatcherRetirement);
		state->started = false;
		state->callback = {};
		state->request.reset();
		state->rebuildRequested = state->reloadPending = state->rebuilding = false;
	}

	CancelAndRetireWorkers(state);
	if (dispatcher.joinable()) {
		if (!dispatcherRetirement) std::terminate();
		const auto status = workbench::WorkerRetirementService::Instance().Retire(
			std::move(dispatcher), std::move(*dispatcherRetirement), state);
		if (status != workbench::WorkerRetirementStatus::Retired) std::terminate();
		dispatcherRetirement.reset();
	} else {
		dispatcherRetirement.reset();
	}

	WorkspaceArtifactDocumentSourceResult result;
	result.status = EWorkspaceArtifactDocumentSourceStatus::Stopped;
	{
		std::lock_guard lock(state->mutex);
		state->retirementFinalized = state->liveThreads == 0;
		result.retirementFinalized = state->retirementFinalized;
		result.retirementPending = !result.retirementFinalized;
	}
	return result;
}

bool CWorkspaceArtifactDocumentSourceController::IsRetirementFinalized() const noexcept
{
	const auto state = m_state;
	std::lock_guard lock(state->mutex);
	return state->retirementFinalized;
}

void CWorkspaceArtifactDocumentSourceController::MarkThreadFinished(
	const std::shared_ptr<SharedState>& state) noexcept
{
	std::lock_guard lock(state->mutex);
	if (state->liveThreads != 0) --state->liveThreads;
	if (state->stopping && state->liveThreads == 0) state->retirementFinalized = true;
}

bool CWorkspaceArtifactDocumentSourceController::BeginDocumentOperation(
	const std::shared_ptr<SharedState>& state) noexcept
{
	std::lock_guard lock(state->mutex);
	if (!state->started || state->stopping) return false;
	++state->activeDocumentOperations;
	return true;
}

void CWorkspaceArtifactDocumentSourceController::EndDocumentOperation(
	const std::shared_ptr<SharedState>& state) noexcept
{
	std::lock_guard lock(state->mutex);
	if (state->activeDocumentOperations != 0) --state->activeDocumentOperations;
}

bool CWorkspaceArtifactDocumentSourceController::CanDispatch(
	const std::shared_ptr<SharedState>& state) noexcept
{
	std::lock_guard lock(state->mutex);
	return state->started && !state->stopping;
}

bool CWorkspaceArtifactDocumentSourceController::IsRelevant(
	const WatchSlot& slot, std::size_t targetIndex, const FileWatchEvent& event) noexcept
{
	const auto& target = slot.targets[targetIndex];
	if (!target.member) return true;
	return UriIdentityService::IsEqual(event.uri, *target.member)
		|| (event.previousUri && UriIdentityService::IsEqual(*event.previousUri, *target.member));
}

void CWorkspaceArtifactDocumentSourceController::Queue(
	const std::shared_ptr<SharedState>& state, bool rebuild) noexcept
{
	std::lock_guard lock(state->mutex);
	if (state->stopping || (state->rebuilding && rebuild)) return;
	state->reloadPending = true;
	state->rebuildRequested = state->rebuildRequested || rebuild;
	state->pending.notify_one();
}

void CWorkspaceArtifactDocumentSourceController::WorkerMain(
	const std::shared_ptr<SharedState>& state,
	const std::shared_ptr<WatchSlot>& slot) noexcept
{
	struct Finalizer final {
		std::shared_ptr<SharedState> state;
		~Finalizer() { CWorkspaceArtifactDocumentSourceController::MarkThreadFinished(state); }
	} finalizer { state };

	for (;;) {
		std::shared_ptr<WatchSlot::WatchHandle> handle;
		{
			std::lock_guard lock(state->mutex);
			if (state->stopping) return;
			handle = slot->handle;
		}
		if (!handle || !handle->watch) return;

		platform::filesystem::FileResult<FileWatchEvent> event;
		try { event = handle->watch->Next(); }
		catch (...) {
			{
				std::lock_guard lock(state->mutex);
				if (state->stopping || slot->handle != handle) continue;
			}
			Queue(state, true);
			return;
		}
		if (!event.Succeeded() || !event.value) {
			{
				std::lock_guard lock(state->mutex);
				if (state->stopping || slot->handle != handle) continue;
			}
			Queue(state, true);
			return;
		}
		const bool invalidTopology = event.value->type == EFileWatchEventType::Overflow
			|| event.value->type == EFileWatchEventType::RescanRequired || event.value->type == EFileWatchEventType::Disposed;
		if (invalidTopology) {
			{
				std::lock_guard lock(state->mutex);
				if (state->stopping || slot->handle != handle) continue;
			}
			Queue(state, true);
			return;
		}

		bool queueReload = false;
		bool queueRebuild = false;
		{
			std::lock_guard lock(state->mutex);
			if (state->stopping || slot->handle != handle) continue;
			for (std::size_t index = 0; index < slot->targets.size(); ++index) {
				if (!IsRelevant(*slot, index, *event.value)) continue;
				queueReload = true;
				queueRebuild = queueRebuild || slot->targets[index].rebuildOnRelevantChange;
			}
		}
		if (queueReload) Queue(state, queueRebuild);
	}
}

void CWorkspaceArtifactDocumentSourceController::RebuildTopology(
	const std::shared_ptr<SharedState>& state) noexcept
{
	WorkspaceArtifactDocumentSourceRequest request;
	{
		std::lock_guard lock(state->mutex);
		if (state->stopping || !state->request || state->rebuilding) return;
		state->rebuilding = true;
		request = *state->request;
	}

	const auto desired = BuildTopology(request);
	struct Prepared final {
		TopologyEntry entry;
		std::shared_ptr<WatchSlot::WatchHandle> handle;
		std::optional<workbench::WorkerRetirementService::Reservation> retirement;
	};
	std::vector<Prepared> prepared;
	for (const auto& entry : desired) {
		bool exists = false;
		bool needsReplacement = false;
		{
			std::lock_guard lock(state->mutex);
			for (const auto& slot : state->slots) {
				if (!UriIdentityService::IsEqual(slot->root, entry.root)) continue;
				exists = true;
				needsReplacement = slot->targets.size() != entry.targets.size();
				if (!needsReplacement) {
					for (const auto& target : entry.targets) {
						const auto targetFound = std::any_of(slot->targets.begin(), slot->targets.end(),
							[&target](const auto& existing) {
								return existing.rebuildOnRelevantChange == target.rebuildOnRelevantChange
									&& existing.member.has_value() == target.member.has_value()
									&& (!existing.member || UriIdentityService::IsEqual(*existing.member, *target.member));
							});
						if (!targetFound) { needsReplacement = true; break; }
					}
				}
				break;
			}
		}
		if (exists && !needsReplacement) continue;
		std::optional<workbench::WorkerRetirementService::Reservation> retirement;
		if (!exists) {
			retirement = workbench::WorkerRetirementService::Instance().TryReserve();
			if (!retirement) continue;
		}
		platform::filesystem::FileResult<std::unique_ptr<platform::filesystem::IFileWatch>> watched;
		try {
			watched = state->fileService->Watch(entry.root, FileWatchOptions { .recursive = false });
		} catch (...) {
			continue;
		}
		if (!watched.Succeeded() || !watched.value) continue;
		auto handle = std::make_shared<WatchSlot::WatchHandle>();
		handle->watch = std::move(*watched.value);
		prepared.push_back({ entry, std::move(handle), std::move(retirement) });
	}

	std::vector<std::shared_ptr<WatchSlot>> removed;
	std::vector<std::shared_ptr<WatchSlot::WatchHandle>> replaced;
	std::vector<std::shared_ptr<WatchSlot::WatchHandle>> preparedToCancel;
	{
		std::lock_guard lock(state->mutex);
		if (state->stopping) {
			for (auto& item : prepared) preparedToCancel.push_back(std::move(item.handle));
			state->rebuilding = false;
		} else {
			for (auto iterator = state->slots.begin(); iterator != state->slots.end();) {
				const auto found = std::find_if(desired.begin(), desired.end(), [&iterator](const auto& entry) {
					return UriIdentityService::IsEqual((*iterator)->root, entry.root);
				});
				if (found == desired.end()) {
					removed.push_back(std::move(*iterator));
					iterator = state->slots.erase(iterator);
					continue;
				}
				++iterator;
			}
			for (auto& item : prepared) {
				const auto existing = std::find_if(state->slots.begin(), state->slots.end(), [&item](const auto& slot) {
					return UriIdentityService::IsEqual(slot->root, item.entry.root);
				});
				if (existing != state->slots.end()) {
					replaced.push_back((*existing)->handle);
					(*existing)->targets.clear();
					for (const auto& target : item.entry.targets) {
						(*existing)->targets.push_back({ target.member, target.rebuildOnRelevantChange });
					}
					(*existing)->handle = std::move(item.handle);
					item.retirement.reset();
					continue;
				}
				if (!item.retirement || !item.handle) continue;
				auto slot = std::make_shared<WatchSlot>(item.entry.root);
				for (const auto& target : item.entry.targets) {
					slot->targets.push_back({ target.member, target.rebuildOnRelevantChange });
				}
				slot->handle = std::move(item.handle);
				slot->workerRetirement = std::move(item.retirement);
				++state->liveThreads;
				try {
					slot->worker = std::thread([state, slot] { WorkerMain(state, slot); });
					state->slots.push_back(std::move(slot));
				} catch (...) {
					--state->liveThreads;
					preparedToCancel.push_back(std::move(slot->handle));
					slot->workerRetirement.reset();
				}
			}
			state->rebuilding = false;
		}
	}
	for (const auto& handle : replaced) {
		if (handle && handle->watch) { try { (void)handle->watch->Cancel(); } catch (...) {} }
	}
	for (const auto& handle : preparedToCancel) {
		if (handle && handle->watch) { try { (void)handle->watch->Cancel(); } catch (...) {} }
	}
	for (const auto& slot : removed) {
		if (slot->handle && slot->handle->watch) { try { (void)slot->handle->watch->Cancel(); } catch (...) {} }
	}
	for (const auto& slot : removed) {
		if (!slot->worker.joinable()) {
			slot->workerRetirement.reset();
			continue;
		}
		if (!slot->workerRetirement) std::terminate();
		const auto status = workbench::WorkerRetirementService::Instance().Retire(
			std::move(slot->worker), std::move(*slot->workerRetirement), state);
		if (status != workbench::WorkerRetirementStatus::Retired) std::terminate();
		slot->workerRetirement.reset();
	}
}

std::uint64_t CWorkspaceArtifactDocumentSourceController::NextRevision(
	const std::shared_ptr<SharedState>& state) noexcept
{
	std::lock_guard lock(state->mutex);
	if (state->nextRevision == (std::numeric_limits<std::uint64_t>::max)()) return 0;
	return state->nextRevision++;
}

CWorkspaceArtifactDocumentSourceController::ReloadOneResult
CWorkspaceArtifactDocumentSourceController::ReloadOne(
	const std::shared_ptr<SharedState>& state,
	EWorkspaceArtifactDocumentKind kind, EWorkspaceArtifactDocumentSource source,
	const std::optional<Uri>& folderUri, const Uri& resource, std::uint64_t generation) noexcept
{
	bool documentOperationActive = false;
	std::optional<EFileResultStatus> failureStatus{ EFileResultStatus::Failed };
	try {
		const auto read = state->fileService->Read(resource,
			FileReadOptions { .maximumBytes = platform::serialization::CJsoncDocument::kMaximumInputBytes });
		const auto revision = NextRevision(state);
		if (!read.Succeeded()) failureStatus = read.status;
		if (!BeginDocumentOperation(state)) {
			return { { EWorkspaceArtifactDocumentStatus::Stopped, std::nullopt,
				"workspace artifact source controller is stopped" }, read.status };
		}
		documentOperationActive = true;
		if (read.Succeeded() && read.value) {
			auto result = state->documentService->Apply({ kind, source, folderUri, resource, generation, revision,
				std::string(read.value->begin(), read.value->end()) });
			EndDocumentOperation(state);
			documentOperationActive = false;
			return { std::move(result), std::nullopt };
		}
		if (read.status == EFileResultStatus::NotFound) {
			auto result = state->documentService->Remove({ kind, source, folderUri, resource, generation, revision });
			EndDocumentOperation(state);
			documentOperationActive = false;
			return { std::move(result), read.status };
		}
		EndDocumentOperation(state);
		documentOperationActive = false;
		return { { EWorkspaceArtifactDocumentStatus::JsoncParseFailed, std::nullopt,
			"workspace artifact file could not be read" }, read.status };
	} catch (...) {
		if (documentOperationActive) EndDocumentOperation(state);
		return { { EWorkspaceArtifactDocumentStatus::JsoncParseFailed, std::nullopt,
			"workspace artifact source operation failed" }, failureStatus };
	}
}

WorkspaceArtifactDocumentSourceResult CWorkspaceArtifactDocumentSourceController::ReloadSnapshot(
	const std::shared_ptr<SharedState>& state) noexcept
{
	WorkspaceArtifactDocumentSourceRequest request;
	{
		std::lock_guard lock(state->mutex);
		if (!state->started || state->stopping || !state->request) return { EWorkspaceArtifactDocumentSourceStatus::NotStarted };
		request = *state->request;
	}
	WorkspaceArtifactDocumentSourceResult result { EWorkspaceArtifactDocumentSourceStatus::Reloaded };
	auto reload = [&](EWorkspaceArtifactDocumentKind kind, EWorkspaceArtifactDocumentSource source,
		const std::optional<Uri>& folder, const Uri& resource) {
		auto reloaded = ReloadOne(state, kind, source, folder, resource, request.generation);
		if (reloaded.fileStatus && *reloaded.fileStatus != EFileResultStatus::NotFound) {
			result.status = EWorkspaceArtifactDocumentSourceStatus::ReadFailed;
			result.fileStatus = reloaded.fileStatus;
		}
		result.documents.push_back(std::move(reloaded.document));
	};
	if (request.workspaceConfiguration) {
		for (const auto kind : { EWorkspaceArtifactDocumentKind::Tasks, EWorkspaceArtifactDocumentKind::Launch }) {
			reload(kind, EWorkspaceArtifactDocumentSource::WorkspaceFile, std::nullopt, *request.workspaceConfiguration);
		}
	}
	for (const auto& folder : request.workspaceFolders) {
		for (const auto kind : { EWorkspaceArtifactDocumentKind::Tasks, EWorkspaceArtifactDocumentKind::Launch }) {
			if (auto resource = ArtifactInFolder(folder, kind)) reload(kind, EWorkspaceArtifactDocumentSource::Folder, folder, *resource);
		}
	}
	return result;
}

void CWorkspaceArtifactDocumentSourceController::DispatchMain(
	const std::shared_ptr<SharedState>& state) noexcept
{
	struct Finalizer final {
		std::shared_ptr<SharedState> state;
		~Finalizer() { CWorkspaceArtifactDocumentSourceController::MarkThreadFinished(state); }
	} finalizer { state };

	for (;;) {
		bool rebuild = false;
		ReloadCallback callback;
		{
			std::unique_lock lock(state->mutex);
			state->pending.wait(lock, [state] { return state->stopping || state->rebuildRequested || state->reloadPending; });
			if (state->stopping) return;
			(void)state->pending.wait_for(lock, std::chrono::milliseconds(25), [state] { return state->stopping; });
			if (state->stopping) return;
			rebuild = state->rebuildRequested;
			state->rebuildRequested = state->reloadPending = false;
			callback = state->callback;
		}
		if (rebuild) RebuildTopology(state);
		auto result = ReloadSnapshot(state);
		bool invoke = false;
		{
			std::lock_guard lock(state->mutex);
			if (state->started && !state->stopping && callback) invoke = true;
		}
		if (invoke) {
			try { callback(result); } catch (...) {}
		}
	}
}

} // namespace workbench::workspace
