/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "config/ConfigurationFileWatchController.h"

#include <sakura/uri/UriIdentity.h>

#include <algorithm>
#include <chrono>
#include <utility>

namespace config {
namespace {

using platform::filesystem::EFileWatchEventType;
using platform::filesystem::FileWatchEvent;
using platform::filesystem::FileWatchOptions;
using platform::uri::Uri;
using platform::uri::UriIdentityService;

std::optional<Uri> ParentDirectory(const Uri& resource)
{
	if (resource.Path().empty() || resource.Query() || resource.Fragment()) return std::nullopt;
	const auto separator = resource.Path().find_last_of(L'/');
	if (separator == std::wstring::npos || separator == 0) return std::nullopt;
	return Uri::FromComponents(resource.Scheme(), resource.Authority(), resource.Path().substr(0, separator),
		std::nullopt, std::nullopt, resource.HasAuthority()).value;
}

std::optional<Uri> VscodeDirectory(const Uri& folder)
{
	if (folder.Path().empty() || folder.Query() || folder.Fragment()) return std::nullopt;
	std::wstring path = folder.Path();
	if (path.back() != L'/') path.push_back(L'/');
	path += L".vscode";
	return Uri::FromComponents(folder.Scheme(), folder.Authority(), std::move(path),
		std::nullopt, std::nullopt, folder.HasAuthority()).value;
}

std::optional<Uri> SettingsInVscode(const Uri& folder)
{
	auto directory = VscodeDirectory(folder);
	if (!directory) return std::nullopt;
	return Uri::FromComponents(directory->Scheme(), directory->Authority(), directory->Path() + L"/settings.json",
		std::nullopt, std::nullopt, directory->HasAuthority()).value;
}

bool IsUsableResource(const Uri& resource) noexcept
{
	return resource.Scheme() == L"file" && !resource.Path().empty()
		&& !resource.Query() && !resource.Fragment();
}

constexpr std::size_t kMaximumWorkspaceFolders = 64;
constexpr std::size_t kMaximumWatchTargets = 2 + (kMaximumWorkspaceFolders * 2);

} // namespace

struct CConfigurationFileWatchController::WatchSlot final {
	struct Target final {
		EConfigurationFileWatchChange change = EConfigurationFileWatchChange::FullRescan;
		std::optional<Uri> member;
		bool rebuildOnRelevantChange = false;
	};

	explicit WatchSlot(Uri rootValue)
		: root(std::move(rootValue))
	{
	}

	Uri root;
	std::vector<Target> targets;
	std::unique_ptr<platform::filesystem::IFileWatch> watch;
	std::thread worker;
};

CConfigurationFileWatchController::CConfigurationFileWatchController(platform::filesystem::IFileService& fileService) noexcept
	: m_fileService(fileService)
{
}

CConfigurationFileWatchController::~CConfigurationFileWatchController()
{
	(void)Stop();
}

EConfigurationFileWatchStatus CConfigurationFileWatchController::ValidateRequest(const ConfigurationFileWatchRequest& request) noexcept
{
	if (!IsUsableResource(request.profileSettings)) return EConfigurationFileWatchStatus::InvalidRequest;
	if (request.workspaceFolders.size() > kMaximumWorkspaceFolders) return EConfigurationFileWatchStatus::CapacityExceeded;
	if (request.workspaceConfiguration && !IsUsableResource(*request.workspaceConfiguration)) {
		return EConfigurationFileWatchStatus::InvalidRequest;
	}
	for (const auto& folder : request.workspaceFolders) {
		if (!IsUsableResource(folder)) return EConfigurationFileWatchStatus::InvalidRequest;
	}
	return EConfigurationFileWatchStatus::Started;
}

ConfigurationFileWatchResult CConfigurationFileWatchController::Start(ConfigurationFileWatchRequest request, ChangeCallback callback) noexcept
{
	const auto validity = ValidateRequest(request);
	if (validity != EConfigurationFileWatchStatus::Started) return { validity };
	// A callback cannot replace the dispatcher whose stack it is executing on.
	// Check before taking the lifecycle lock so an external Stop that is joining
	// this dispatcher cannot deadlock with a reentrant Start.
	if (IsDispatcherThread()) return { EConfigurationFileWatchStatus::ReentrantStopDenied };
	std::lock_guard lifecycleLock(m_lifecycleMutex);
	const auto stopped = StopLocked();
	if (!stopped.Succeeded() && stopped.status != EConfigurationFileWatchStatus::NotStarted) return stopped;
	try {
		{
			std::lock_guard lock(m_mutex);
			m_request = std::move(request);
			m_callback = std::move(callback);
			m_stopping = false;
			m_started = true;
			StartWorkersLocked();
		}
		m_dispatcher = std::thread(&CConfigurationFileWatchController::DispatchMain, this);
		return { EConfigurationFileWatchStatus::Started };
	} catch (...) {
		(void)StopLocked();
		return { EConfigurationFileWatchStatus::StartFailed };
	}
}

ConfigurationFileWatchResult CConfigurationFileWatchController::Update(ConfigurationFileWatchRequest request) noexcept
{
	const auto validity = ValidateRequest(request);
	if (validity != EConfigurationFileWatchStatus::Started) return { validity };
	std::lock_guard lock(m_mutex);
	if (!m_started || m_stopping) return { EConfigurationFileWatchStatus::NotStarted };
	m_request = std::move(request);
	m_rebuildRequested = true;
	m_pending.notify_one();
	return { EConfigurationFileWatchStatus::Updated };
}

void CConfigurationFileWatchController::StartWorkersLocked()
{
	if (!m_request) return;
	const auto& request = *m_request;
	auto add = [this](EConfigurationFileWatchChange change, const Uri& root, std::optional<Uri> member, bool rebuildOnRelevantChange = false) {
		for (const auto& existing : m_slots) {
			if (!UriIdentityService::IsEqual(existing->root, root)) continue;
			const auto duplicate = std::any_of(existing->targets.begin(), existing->targets.end(),
				[change, &member, rebuildOnRelevantChange](const WatchSlot::Target& target) {
					return target.change == change && target.rebuildOnRelevantChange == rebuildOnRelevantChange
						&& target.member.has_value() == member.has_value()
						&& (!target.member || UriIdentityService::IsEqual(*target.member, *member));
				});
			if (!duplicate) existing->targets.push_back({ change, std::move(member), rebuildOnRelevantChange });
			return;
		}
		auto watched = m_fileService.Watch(root, FileWatchOptions { .recursive = false });
		if (!watched.Succeeded() || !watched.value) return;
		auto slot = std::make_unique<WatchSlot>(root);
		slot->targets.push_back({ change, std::move(member), rebuildOnRelevantChange });
		slot->watch = std::move(*watched.value);
		m_slots.push_back(std::move(slot));
	};

	if (auto profileParent = ParentDirectory(request.profileSettings)) {
		add(EConfigurationFileWatchChange::ProfileSettings, *profileParent, request.profileSettings);
	}
	if (request.workspaceConfiguration) {
		if (auto workspaceParent = ParentDirectory(*request.workspaceConfiguration)) {
			add(EConfigurationFileWatchChange::WorkspaceConfiguration, *workspaceParent, request.workspaceConfiguration);
		}
	}
	for (const auto& folder : request.workspaceFolders) {
		const auto vscode = VscodeDirectory(folder);
		const auto settings = SettingsInVscode(folder);
		if (vscode) add(EConfigurationFileWatchChange::WorkspaceFolderSettings, folder, vscode, true);
		if (vscode && settings) add(EConfigurationFileWatchChange::WorkspaceFolderSettings, *vscode, settings);
	}
	for (const auto& slot : m_slots) {
		slot->worker = std::thread(&CConfigurationFileWatchController::WorkerMain, this, slot.get());
	}
}

void CConfigurationFileWatchController::CancelAndJoinWorkers() noexcept
{
	std::vector<std::unique_ptr<WatchSlot>> slots;
	{
		std::lock_guard lock(m_mutex);
		for (const auto& slot : m_slots) {
			if (slot->watch) {
				try { (void)slot->watch->Cancel(); }
				catch (...) { }
			}
		}
		slots.swap(m_slots);
	}
	for (const auto& slot : slots) {
		if (slot->worker.joinable()) slot->worker.join();
	}
}

ConfigurationFileWatchResult CConfigurationFileWatchController::Stop() noexcept
{
	// Perform the self-join check before waiting for the lifecycle owner. An
	// external Stop may hold that lock while joining this callback thread.
	if (IsDispatcherThread()) return { EConfigurationFileWatchStatus::ReentrantStopDenied };
	std::lock_guard lifecycleLock(m_lifecycleMutex);
	return StopLocked();
}

ConfigurationFileWatchResult CConfigurationFileWatchController::StopLocked() noexcept
{
	{
		std::lock_guard lock(m_mutex);
		if (!m_started) return { EConfigurationFileWatchStatus::NotStarted };
		m_stopping = true;
		for (const auto& slot : m_slots) {
			if (slot->watch) {
				try { (void)slot->watch->Cancel(); }
				catch (...) { }
			}
		}
		m_pending.notify_all();
	}
	if (m_dispatcher.joinable()) m_dispatcher.join();
	CancelAndJoinWorkers();
	std::lock_guard lock(m_mutex);
	m_started = false;
	m_callback = {};
	m_dispatcherThreadId = {};
	m_profilePending = m_folderPending = m_workspacePending = m_rebuildRequested = m_rebuilding = false;
	return { EConfigurationFileWatchStatus::Stopped };
}

bool CConfigurationFileWatchController::CanDispatch() const noexcept
{
	std::lock_guard lock(m_mutex);
	return m_started && !m_stopping;
}

bool CConfigurationFileWatchController::IsDispatcherThread() const noexcept
{
	std::lock_guard lock(m_mutex);
	return m_dispatcherThreadId == std::this_thread::get_id();
}

bool CConfigurationFileWatchController::IsRelevant(
	const WatchSlot& slot, std::size_t targetIndex, const FileWatchEvent& event) const noexcept
{
	const auto& target = slot.targets[targetIndex];
	if (!target.member) return true;
	return UriIdentityService::IsEqual(event.uri, *target.member)
		|| (event.previousUri && UriIdentityService::IsEqual(*event.previousUri, *target.member));
}

void CConfigurationFileWatchController::Queue(EConfigurationFileWatchChange change, bool rebuild) noexcept
{
	std::lock_guard lock(m_mutex);
	if (m_stopping || (m_rebuilding && change == EConfigurationFileWatchChange::FullRescan)) return;
	switch (change) {
	case EConfigurationFileWatchChange::ProfileSettings: m_profilePending = true; break;
	case EConfigurationFileWatchChange::WorkspaceFolderSettings: m_folderPending = true; break;
	case EConfigurationFileWatchChange::WorkspaceConfiguration: m_workspacePending = true; break;
	case EConfigurationFileWatchChange::FullRescan: m_profilePending = m_folderPending = m_workspacePending = true; break;
	}
	m_rebuildRequested = m_rebuildRequested || rebuild;
	m_pending.notify_one();
}

void CConfigurationFileWatchController::WorkerMain(WatchSlot* slot) noexcept
{
	for (;;) {
		platform::filesystem::FileResult<FileWatchEvent> event;
		try {
			event = slot->watch->Next();
		} catch (...) {
			Queue(EConfigurationFileWatchChange::FullRescan, true);
			return;
		}
		if (!event.Succeeded() || !event.value) {
			Queue(EConfigurationFileWatchChange::FullRescan, !event.Succeeded());
			return;
		}
		const bool topologyInvalid = event.value->type == EFileWatchEventType::Overflow
			|| event.value->type == EFileWatchEventType::RescanRequired
			|| event.value->type == EFileWatchEventType::Disposed;
		if (topologyInvalid) {
			Queue(EConfigurationFileWatchChange::FullRescan, true);
			return;
		}
		for (std::size_t index = 0; index < slot->targets.size(); ++index) {
			if (IsRelevant(*slot, index, *event.value)) {
				Queue(slot->targets[index].change, slot->targets[index].rebuildOnRelevantChange);
			}
		}
	}
}

void CConfigurationFileWatchController::RebuildTopology() noexcept
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

void CConfigurationFileWatchController::DispatchMain() noexcept
{
	{
		std::lock_guard lock(m_mutex);
		m_dispatcherThreadId = std::this_thread::get_id();
	}
	for (;;) {
		bool profile = false;
		bool folders = false;
		bool workspace = false;
		bool rebuild = false;
		ChangeCallback callback;
		{
			std::unique_lock lock(m_mutex);
			m_pending.wait(lock, [this] {
				return m_stopping || m_rebuildRequested || m_profilePending || m_folderPending || m_workspacePending;
			});
			if (m_stopping) return;
			// A short bounded debounce collapses atomic-save event bursts without
			// creating an unbounded refresh queue.
			(void)m_pending.wait_for(lock, std::chrono::milliseconds(25), [this] { return m_stopping; });
			if (m_stopping) return;
			profile = m_profilePending;
			folders = m_folderPending;
			workspace = m_workspacePending;
			rebuild = m_rebuildRequested;
			m_profilePending = m_folderPending = m_workspacePending = m_rebuildRequested = false;
			callback = m_callback;
		}
		if (rebuild) {
			RebuildTopology();
			if (callback && CanDispatch()) {
				try { callback(EConfigurationFileWatchChange::FullRescan); }
				catch (...) { }
			}
			continue;
		}
		if (callback && profile && CanDispatch()) {
			try { callback(EConfigurationFileWatchChange::ProfileSettings); }
			catch (...) { }
		}
		if (callback && folders && CanDispatch()) {
			try { callback(EConfigurationFileWatchChange::WorkspaceFolderSettings); }
			catch (...) { }
		}
		if (callback && workspace && CanDispatch()) {
			try { callback(EConfigurationFileWatchChange::WorkspaceConfiguration); }
			catch (...) { }
		}
	}
}

} // namespace config
