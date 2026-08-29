/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "terminal/runtime/TerminalRuntimeService.h"

#include "terminal/input/SakuraTerminalInputAdapter.h"
#include "terminal/model/TerminalModel.h"
#include "terminal/runtime/TerminalCaptureExtraction.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <windows.h>

namespace terminal {
namespace {

TerminalSize NormalizeSize(TerminalSize size) noexcept
{
	size.columns = (std::max)(std::uint16_t{ 1 }, size.columns);
	size.rows = (std::max)(std::uint16_t{ 1 }, size.rows);
	return size;
}

bool IsValidOperation(const HarnessOperationId& operationId) noexcept
{
	return operationId.IsValid();
}

HarnessOperationId MakeOperationId(const std::uint64_t value) noexcept
{
	HarnessOperationId result;
	for (std::size_t index = 0; index < sizeof(value) && index < result.value.size(); ++index) {
		result.value[index] = static_cast<std::uint8_t>(value >> (index * 8));
	}
	return result;
}

TerminalRuntimeOperationCode MapTopologyCode(
	const runtime::topology::ETerminalCollectionResultCode code) noexcept
{
	using Code = runtime::topology::ETerminalCollectionResultCode;
	switch (code) {
	case Code::Succeeded: return TerminalRuntimeOperationCode::Succeeded;
	case Code::InvalidRequest: return TerminalRuntimeOperationCode::InvalidRequest;
	case Code::StaleRevision: return TerminalRuntimeOperationCode::TopologyChanged;
	case Code::TargetMissing: return TerminalRuntimeOperationCode::TargetMissing;
	case Code::NameConflict: return TerminalRuntimeOperationCode::InvalidRequest;
	case Code::ResourceExhausted: return TerminalRuntimeOperationCode::ResourceExhausted;
	case Code::IdentityExhausted: return TerminalRuntimeOperationCode::ResourceExhausted;
	case Code::Stopped: return TerminalRuntimeOperationCode::ServerStopping;
	}
	return TerminalRuntimeOperationCode::InternalError;
}

TerminalTopologyResult MapTopologyResult(
	const runtime::topology::TerminalTopologyResult& source) noexcept
{
	TerminalTopologyResult result;
	result.code = MapTopologyCode(source.code);
	result.revision = source.revision;
	result.sessionId = source.sessionId;
	result.windowId = source.windowId;
	result.paneId = source.paneId;
	result.instanceId = source.instanceId;
	return result;
}

bool IsDeadlineExpired(const std::chrono::steady_clock::time_point deadline) noexcept
{
	return deadline != std::chrono::steady_clock::time_point{}
		&& std::chrono::steady_clock::now() >= deadline;
}

struct WindowLocation final {
	runtime::topology::TerminalSessionId sessionId;
	runtime::topology::TerminalWindowSnapshot window;
};

std::optional<WindowLocation> FindWindow(
	const runtime::topology::TerminalCollectionSnapshot& snapshot,
	const TerminalWindowId id)
{
	for (const auto& session : snapshot.sessions) {
		for (const auto& window : session.windows) {
			if (window.id == id) return WindowLocation{ session.id, window };
		}
	}
	return std::nullopt;
}

std::optional<runtime::topology::TerminalPaneSnapshot> FindPaneForInstance(
	const runtime::topology::TerminalCollectionSnapshot& snapshot,
	const TerminalInstanceId id)
{
	for (const auto& session : snapshot.sessions) {
		for (const auto& window : session.windows) {
			for (const auto& pane : window.panes) {
				if (pane.instanceId == id) return pane;
			}
		}
	}
	return std::nullopt;
}

void AppendDirtyRanges(std::vector<TerminalRowRange>& ranges, const std::vector<std::size_t>& rows)
{
	for (const auto row : rows) {
		const auto coordinate = row > static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)())
			? (std::numeric_limits<std::int64_t>::max)()
			: static_cast<std::int64_t>(row);
		if (!ranges.empty() && ranges.back().last != (std::numeric_limits<std::int64_t>::max)()
			&& ranges.back().last + 1 == coordinate) {
			ranges.back().last = coordinate;
		} else {
			ranges.push_back({ coordinate, coordinate });
		}
	}
}

} // namespace

struct CTerminalRuntimeService::Impl final : std::enable_shared_from_this<Impl> {
	template<typename Id>
	struct IdHash final {
		std::size_t operator()(const Id id) const noexcept
		{
			return static_cast<std::size_t>(id.value ^ (id.value >> 32));
		}
	};

	struct CaptureState final {
		std::uint64_t screenEpoch{ 1 };
		std::uint64_t scrollbackBaseOrdinal{ 1 };
		std::uint64_t nextHistoryOrdinal{ 1 };
		std::optional<TerminalCaptureIndex> index;
	};

	struct Subscriber final {
		std::uint64_t id{};
		TerminalRuntimeEventCallback callback;
	};

	TerminalRuntimeServiceDependencies dependencies;
	TerminalRuntimeGeneration runtimeGeneration;
	mutable std::mutex mutex;
	runtime::topology::TerminalCollectionModel collection;
	std::unordered_map<TerminalInstanceId, std::shared_ptr<TerminalInstance>, IdHash<TerminalInstanceId>> instances;
	std::unordered_map<TerminalInstanceId, CaptureState, IdHash<TerminalInstanceId>> captures;
	std::vector<Subscriber> subscribers;
	std::uint64_t nextInstanceId{ 1 };
	std::uint64_t nextInstanceGeneration{ 1 };
	std::uint64_t nextSubscriberId{ 1 };
	bool closing{};

	Impl(TerminalRuntimeServiceDependencies dependenciesValue, TerminalRuntimeGeneration generation)
		: dependencies(std::move(dependenciesValue))
		, runtimeGeneration(generation.IsValid() ? generation : TerminalRuntimeGeneration{ 1 })
	{
		if (dependencies.scrollbackLimit > TerminalModel::kMaxScrollbackLines) {
			dependencies.scrollbackLimit = TerminalModel::kMaxScrollbackLines;
		}
	}

	[[nodiscard]] TerminalRuntimeOperationCode ValidateTargetLocked(
		const TerminalTargetCoordinate& target,
		const std::shared_ptr<TerminalInstance>& instance) const noexcept
	{
		if (!target.runtimeGeneration.IsValid() || target.runtimeGeneration != runtimeGeneration) {
			return TerminalRuntimeOperationCode::TargetMissing;
		}
		if (!instance || !target.instanceId.IsValid()) return TerminalRuntimeOperationCode::TargetMissing;
		if (target.instanceGeneration != 0 && target.instanceGeneration != instance->InstanceGeneration()) {
			return TerminalRuntimeOperationCode::TopologyChanged;
		}
		const auto snapshot = instance->Snapshot();
		if (target.sessionId.IsValid() && target.sessionId != snapshot.coordinate.sessionId) {
			return TerminalRuntimeOperationCode::TargetMissing;
		}
		if (target.windowId.IsValid() && target.windowId != snapshot.coordinate.windowId) {
			return TerminalRuntimeOperationCode::TargetMissing;
		}
		if (target.paneId.IsValid() && target.paneId != snapshot.coordinate.paneId) {
			return TerminalRuntimeOperationCode::TargetMissing;
		}
		return TerminalRuntimeOperationCode::Succeeded;
	}

	[[nodiscard]] std::shared_ptr<TerminalInstance> FindLocked(const TerminalInstanceId id) const
	{
		const auto found = instances.find(id);
		return found == instances.end() ? nullptr : found->second;
	}

	[[nodiscard]] std::optional<TerminalInstanceId> AllocateInstanceIdLocked()
	{
		if (nextInstanceId == 0) return std::nullopt;
		for (;;) {
			const auto candidate = TerminalInstanceId{ nextInstanceId };
			if (instances.find(candidate) == instances.end()) {
				if (nextInstanceId != (std::numeric_limits<std::uint64_t>::max)()) ++nextInstanceId;
				else nextInstanceId = 0;
				return candidate;
			}
			if (nextInstanceId == (std::numeric_limits<std::uint64_t>::max)()) {
				nextInstanceId = 0;
				return std::nullopt;
			}
			++nextInstanceId;
		}
	}

	[[nodiscard]] std::optional<std::uint64_t> AllocateInstanceGenerationLocked()
	{
		if (nextInstanceGeneration == 0) return std::nullopt;
		const auto result = nextInstanceGeneration++;
		if (nextInstanceGeneration == 0) nextInstanceGeneration = 0;
		return result;
	}

	void EnsureCollectionInstanceIdLocked() noexcept
	{
		collection.EnsureNextInstanceId(nextInstanceId);
	}

	[[nodiscard]] TerminalCreateRequest BuildLaunchRequest(
		TerminalSessionId sessionId,
		std::optional<TerminalWindowId> windowId,
		TerminalPaneId paneId,
		const HarnessOperationId& operationId,
		TerminalInstanceOrigin origin = TerminalInstanceOrigin::Interactive,
		TerminalChildEnvironmentPolicy environmentPolicy = TerminalChildEnvironmentPolicy::InteractiveWithHarnessShim,
		const std::optional<TerminalLaunchOptions>& launch = std::nullopt) const
	{
		TerminalCreateRequest request;
		request.operationId = operationId;
		request.origin = origin;
		request.environmentPolicy = environmentPolicy;
		request.sessionId = sessionId;
		request.windowId = windowId;
		request.paneId = paneId;
		if (launch) request.launch = *launch;
		else request.launch.initialSize = NormalizeSize(dependencies.defaultSize);
		if (request.launch.workingDirectory.empty()) {
			request.launch.workingDirectory = dependencies.defaultWorkingDirectory;
		}
		return request;
	}

	[[nodiscard]] TerminalCreateRequest PrepareCreateRequest(TerminalCreateRequest request) const
	{
		request.launch.initialSize = NormalizeSize(request.launch.initialSize);
		if (request.launch.executablePath.empty() && dependencies.resolveLaunch) {
			try {
				if (const auto resolved = dependencies.resolveLaunch(
					request.launch.initialSize, request.launch.workingDirectory)) {
					request.launch = *resolved;
				}
			} catch (...) {
				request.launch.executablePath.clear();
			}
		}
		request.launch.initialSize = NormalizeSize(request.launch.initialSize);
		if (request.launch.workingDirectory.empty()) {
			request.launch.workingDirectory = dependencies.defaultWorkingDirectory;
		}
		if (dependencies.decorateLaunch) {
			try {
				dependencies.decorateLaunch(request, request.launch);
			} catch (...) {
				request.launch.executablePath.clear();
			}
		}
		return request;
	}

	[[nodiscard]] TerminalCreateResult CreateOwnedInstance(
		const TerminalInstanceId id,
		TerminalCreateRequest request)
	{
		TerminalCreateResult result;
		result.instanceId = id;
		std::shared_ptr<TerminalInstance> instance;
		std::uint64_t generation{};
		try {
			std::lock_guard lock(mutex);
			if (closing || !id.IsValid() || instances.find(id) != instances.end()) {
				result.code = closing ? TerminalRuntimeOperationCode::ServerStopping
					: TerminalRuntimeOperationCode::InternalError;
				return result;
			}
			const auto generated = AllocateInstanceGenerationLocked();
			if (!generated) {
				result.code = TerminalRuntimeOperationCode::ResourceExhausted;
				return result;
			}
			generation = *generated;
			if (nextInstanceId != 0 && id.value >= nextInstanceId) {
				nextInstanceId = id.value == (std::numeric_limits<std::uint64_t>::max)()
					? 0 : id.value + 1;
			}
		} catch (...) {
			result.code = TerminalRuntimeOperationCode::InternalError;
			result.instanceId = {};
			return result;
		}
		request.instanceId = id;
		request.instanceGeneration = generation;
		try {
			request = PrepareCreateRequest(std::move(request));
		} catch (...) {
			result.code = TerminalRuntimeOperationCode::InternalError;
			result.instanceId = {};
			return result;
		}
		try {
			const auto weak = weak_from_this();
			instance = std::make_shared<TerminalInstance>(id, runtimeGeneration, generation,
				request, TerminalInstanceDependencies{ dependencies.createSession,
					dependencies.coordinateBase },
				[weak](const TerminalInstanceEvent& event) {
					if (const auto service = weak.lock()) service->Dispatch(event);
				});
			std::lock_guard lock(mutex);
			if (closing || instances.find(id) != instances.end()) {
				result.code = closing ? TerminalRuntimeOperationCode::ServerStopping
					: TerminalRuntimeOperationCode::InternalError;
				return result;
			}
			instances.emplace(id, instance);
			captures.emplace(id, CaptureState{});
		} catch (...) {
			result.code = TerminalRuntimeOperationCode::InternalError;
			result.instanceId = {};
			return result;
		}

		const auto start = instance->Start();
		result.instanceGeneration = generation;
		result.outcome = instance->Outcome();
		switch (start.status) {
		case TerminalInstanceStartStatus::Started:
			result.code = TerminalRuntimeOperationCode::Succeeded;
			break;
		case TerminalInstanceStartStatus::StartCancelled:
			result.code = TerminalRuntimeOperationCode::Cancelled;
			break;
		case TerminalInstanceStartStatus::StartFailed:
			result.code = TerminalRuntimeOperationCode::InternalError;
			break;
		case TerminalInstanceStartStatus::AlreadyStarted:
			result.code = TerminalRuntimeOperationCode::AlreadyTerminal;
			break;
		case TerminalInstanceStartStatus::Unavailable:
			result.code = TerminalRuntimeOperationCode::InternalError;
			break;
		}
		return result;
	}

	[[nodiscard]] TerminalCreateResult CreateInstance(TerminalCreateRequest request)
	{
		TerminalCreateResult result;
		if (!IsValidOperation(request.operationId)) {
			result.code = TerminalRuntimeOperationCode::InvalidRequest;
			return result;
		}
		std::optional<TerminalInstanceId> id;
		{
			std::lock_guard lock(mutex);
			if (closing) {
				result.code = TerminalRuntimeOperationCode::ServerStopping;
				return result;
			}
			id = AllocateInstanceIdLocked();
		}
		if (!id) {
			result.code = TerminalRuntimeOperationCode::ResourceExhausted;
			return result;
		}
		return CreateOwnedInstance(*id, std::move(request));
	}

	void Dispatch(const TerminalInstanceEvent& event) noexcept
	{
		std::vector<TerminalRuntimeEventCallback> callbacks;
		try {
			std::lock_guard lock(mutex);
			callbacks.reserve(subscribers.size());
			for (const auto& subscriber : subscribers) callbacks.push_back(subscriber.callback);
		} catch (...) {
			return;
		}
		for (const auto& callback : callbacks) {
			if (!callback) continue;
			try {
				callback(event);
			} catch (...) {
			}
		}
	}

	void BeginCloseAll() noexcept
	{
		std::vector<std::shared_ptr<TerminalInstance>> values;
		{
			std::lock_guard lock(mutex);
			if (closing) return;
			closing = true;
			values.reserve(instances.size());
			for (const auto& entry : instances) values.push_back(entry.second);
		}
		for (const auto& instance : values) {
			if (instance) instance->BeginClose(TerminalInstanceCloseReason::Shutdown);
		}
	}

	void BeginCloseInstance(
		const TerminalInstanceId id, const TerminalInstanceCloseReason reason) noexcept
	{
		std::shared_ptr<TerminalInstance> instance;
		{
			std::lock_guard lock(mutex);
			instance = FindLocked(id);
		}
		if (instance) instance->BeginClose(reason);
	}

	[[nodiscard]] TerminalRuntimeCloseResult WaitForCloseAll(
		const std::chrono::steady_clock::time_point deadline) noexcept
	{
		BeginCloseAll();
		std::vector<std::shared_ptr<TerminalInstance>> values;
		{
			std::lock_guard lock(mutex);
			values.reserve(instances.size());
			for (const auto& entry : instances) values.push_back(entry.second);
		}
		bool deadlineExceeded = false;
		for (const auto& instance : values) {
			if (!instance) continue;
			const auto wait = instance->WaitForClose(deadline);
			if (wait.status == TerminalInstanceCloseWaitStatus::InProgress) {
				return { TerminalRuntimeCloseWaitStatus::InProgress };
			}
			if (wait.status == TerminalInstanceCloseWaitStatus::DeadlineExceeded) deadlineExceeded = true;
		}
		return { deadlineExceeded ? TerminalRuntimeCloseWaitStatus::DeadlineExceeded
			: TerminalRuntimeCloseWaitStatus::Closed };
	}

	void RecordDrain(const TerminalInstanceId id, const TerminalInstanceDrainResult& drain)
	{
		if (!drain.found || !drain.contentRevision.IsValid()) return;
		std::lock_guard lock(mutex);
		auto instance = FindLocked(id);
		if (!instance) return;
		auto capture = captures.find(id);
		if (capture == captures.end()) return;
		auto& state = capture->second;
		if (!state.index) {
			state.index.emplace(runtimeGeneration, id, instance->InstanceGeneration(),
				state.screenEpoch, drain.contentRevision, state.scrollbackBaseOrdinal);
			return;
		}
		const auto current = state.index->CurrentCursor().revision;
		if (drain.contentRevision.value <= current.value) return;
		TerminalChangeRecord change;
		change.revision = drain.contentRevision;
		change.screenEpoch = state.screenEpoch;
		AppendDirtyRanges(change.dirtyScreenRanges, drain.dirtyRows);
		const auto appended = drain.scrollbackChange.Appended();
		if (appended != 0) {
			change.appendedHistoryBeginOrdinal = state.nextHistoryOrdinal;
			if (appended > (std::numeric_limits<std::uint64_t>::max)() - state.nextHistoryOrdinal) {
				change.appendedHistoryEndOrdinal = (std::numeric_limits<std::uint64_t>::max)();
				state.nextHistoryOrdinal = 0;
			} else {
				change.appendedHistoryEndOrdinal = state.nextHistoryOrdinal + appended - 1;
				state.nextHistoryOrdinal += appended;
			}
		}
		const auto evicted = drain.scrollbackChange.Evicted();
		if (evicted != 0) {
			const auto last = state.scrollbackBaseOrdinal + evicted - 1;
			change.evictedThroughOrdinal = last < state.scrollbackBaseOrdinal ?
				(std::numeric_limits<std::uint64_t>::max)() : last;
			state.scrollbackBaseOrdinal = last == (std::numeric_limits<std::uint64_t>::max)()
				? 0 : last + 1;
		}
		if (drain.scrollbackChange.Cleared()) change.fullInvalidation = true;
		if (state.index->Record(std::move(change)) != ETerminalCaptureIndexRecordCode::Succeeded) {
			state.index.reset();
			state.index.emplace(runtimeGeneration, id, instance->InstanceGeneration(),
				state.screenEpoch, drain.contentRevision, state.scrollbackBaseOrdinal);
		}
	}

	[[nodiscard]] TerminalCaptureCursor MakeCursor(const TerminalInstanceId id,
		const TerminalInstanceSnapshot& snapshot, const CaptureState& state) const noexcept
	{
		if (state.index) return state.index->CurrentCursor();
		TerminalCaptureCursor cursor;
		cursor.runtimeGeneration = runtimeGeneration;
		cursor.instanceId = id;
		cursor.instanceGeneration = snapshot.coordinate.instanceGeneration;
		cursor.screenEpoch = state.screenEpoch;
		cursor.revision = snapshot.contentRevision;
		cursor.scrollbackBaseOrdinal = state.scrollbackBaseOrdinal;
		return cursor;
	}
};

CTerminalRuntimeService::CTerminalRuntimeService(
	TerminalRuntimeServiceDependencies dependencies,
	TerminalRuntimeGeneration runtimeGeneration)
	: m_impl(std::make_shared<Impl>(std::move(dependencies), runtimeGeneration))
{
}

CTerminalRuntimeService::CTerminalRuntimeService(
	TerminalRuntimeSessionFactory createSession,
	TerminalRuntimeGeneration runtimeGeneration)
	: CTerminalRuntimeService(TerminalRuntimeServiceDependencies{ std::move(createSession) }, runtimeGeneration)
{
}

CTerminalRuntimeService::~CTerminalRuntimeService()
{
	if (!m_impl) return;
	m_impl->BeginCloseAll();
	static_cast<void>(m_impl->WaitForCloseAll(std::chrono::steady_clock::time_point::max()));
}

TerminalCreateResult CTerminalRuntimeService::CreateInstance(const TerminalCreateRequest& request)
{
	return m_impl ? m_impl->CreateInstance(request)
		: TerminalCreateResult{ TerminalRuntimeOperationCode::ServerStopping };
}

TerminalTopologyResult CTerminalRuntimeService::CreateSession(const TerminalSessionCreateRequest& request)
{
	if (!m_impl || !IsValidOperation(request.operationId)) {
		return { TerminalRuntimeOperationCode::InvalidRequest };
	}
	runtime::topology::TerminalTopologyResult topologyResult;
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->closing) return { TerminalRuntimeOperationCode::ServerStopping };
		m_impl->EnsureCollectionInstanceIdLocked();
		runtime::topology::TerminalCollectionSessionCreateRequest topologyRequest;
		topologyRequest.name = request.name;
		topologyRequest.createInitialWindow = true;
		topologyResult = m_impl->collection.CreateSession(topologyRequest);
	}
	const auto result = MapTopologyResult(topologyResult);
	if (!topologyResult.Succeeded() || !topologyResult.sessionId || !topologyResult.windowId
		|| !topologyResult.paneId || !topologyResult.instanceId) return result;
	auto createRequest = m_impl->BuildLaunchRequest(*topologyResult.sessionId, *topologyResult.windowId,
		*topologyResult.paneId, request.operationId, TerminalInstanceOrigin::Interactive,
		TerminalChildEnvironmentPolicy::InteractiveWithHarnessShim, request.launch);
	const auto create = m_impl->CreateOwnedInstance(*topologyResult.instanceId, std::move(createRequest));
	TerminalTopologyResult completed = result;
	completed.instanceId = topologyResult.instanceId;
	if (!create.Succeeded()) {
		m_impl->BeginCloseInstance(*topologyResult.instanceId, TerminalInstanceCloseReason::Explicit);
		{
			std::lock_guard lock(m_impl->mutex);
			static_cast<void>(m_impl->collection.CloseSession(*topologyResult.sessionId));
		}
		completed.code = create.code;
	}
	return completed;
}

TerminalTopologyResult CTerminalRuntimeService::CreateTerminalWindow(const TerminalWindowCreateRequest& request)
{
	if (!m_impl || !IsValidOperation(request.operationId) || !request.sessionId.IsValid()) {
		return { TerminalRuntimeOperationCode::InvalidRequest };
	}
	runtime::topology::TerminalTopologyResult topologyResult;
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->closing) return { TerminalRuntimeOperationCode::ServerStopping };
		m_impl->EnsureCollectionInstanceIdLocked();
		topologyResult = m_impl->collection.CreateTerminalWindow(request.sessionId, request.name);
	}
	const auto result = MapTopologyResult(topologyResult);
	if (!topologyResult.Succeeded() || !topologyResult.sessionId || !topologyResult.windowId
		|| !topologyResult.paneId || !topologyResult.instanceId) return result;
	auto createRequest = m_impl->BuildLaunchRequest(*topologyResult.sessionId, *topologyResult.windowId,
		*topologyResult.paneId, request.operationId, TerminalInstanceOrigin::Interactive,
		TerminalChildEnvironmentPolicy::InteractiveWithHarnessShim, request.launch);
	const auto create = m_impl->CreateOwnedInstance(*topologyResult.instanceId, std::move(createRequest));
	TerminalTopologyResult completed = result;
	completed.instanceId = topologyResult.instanceId;
	if (!create.Succeeded()) {
		m_impl->BeginCloseInstance(*topologyResult.instanceId, TerminalInstanceCloseReason::Explicit);
		{
			std::lock_guard lock(m_impl->mutex);
			static_cast<void>(m_impl->collection.CloseWindow(*topologyResult.sessionId, *topologyResult.windowId));
		}
		completed.code = create.code;
	}
	return completed;
}

TerminalTopologyResult CTerminalRuntimeService::SplitPane(const TerminalPaneSplitRequest& request)
{
	if (!m_impl || !IsValidOperation(request.operationId) || !request.paneId.IsValid()) {
		return { TerminalRuntimeOperationCode::InvalidRequest };
	}
	runtime::topology::TerminalPaneSnapshot source;
	runtime::topology::TerminalTopologyResult topologyResult;
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->closing) return { TerminalRuntimeOperationCode::ServerStopping };
		m_impl->EnsureCollectionInstanceIdLocked();
		const auto found = m_impl->collection.FindPane(request.paneId);
		if (!found) return { TerminalRuntimeOperationCode::TargetMissing,
			m_impl->collection.Revision() };
		source = *found;
		topologyResult = m_impl->collection.SplitPane(source.sessionId, source.windowId,
			request.paneId, request.orientation);
	}
	const auto result = MapTopologyResult(topologyResult);
	if (!topologyResult.Succeeded() || !topologyResult.sessionId || !topologyResult.windowId
		|| !topologyResult.paneId || !topologyResult.instanceId) return result;
	auto createRequest = m_impl->BuildLaunchRequest(*topologyResult.sessionId, *topologyResult.windowId,
		*topologyResult.paneId, request.operationId, TerminalInstanceOrigin::Interactive,
		TerminalChildEnvironmentPolicy::InteractiveWithHarnessShim, request.launch);
	const auto create = m_impl->CreateOwnedInstance(*topologyResult.instanceId, std::move(createRequest));
	TerminalTopologyResult completed = result;
	completed.instanceId = topologyResult.instanceId;
	if (!create.Succeeded()) {
		m_impl->BeginCloseInstance(*topologyResult.instanceId, TerminalInstanceCloseReason::Explicit);
		{
			std::lock_guard lock(m_impl->mutex);
			static_cast<void>(m_impl->collection.ClosePane(*topologyResult.sessionId,
				*topologyResult.windowId, *topologyResult.paneId));
		}
		completed.code = create.code;
	}
	return completed;
}

TerminalTopologyResult CTerminalRuntimeService::SelectWindow(const TerminalWindowSelectRequest& request)
{
	if (!m_impl || !IsValidOperation(request.operationId) || !request.windowId.IsValid()) {
		return { TerminalRuntimeOperationCode::InvalidRequest };
	}
	std::lock_guard lock(m_impl->mutex);
	if (m_impl->closing) return { TerminalRuntimeOperationCode::ServerStopping };
	const auto window = FindWindow(m_impl->collection.Snapshot(), request.windowId);
	if (!window) return { TerminalRuntimeOperationCode::TargetMissing, m_impl->collection.Revision() };
	return MapTopologyResult(m_impl->collection.SelectWindow(window->sessionId, request.windowId));
}

TerminalTopologyResult CTerminalRuntimeService::SelectPane(const TerminalPaneSelectRequest& request)
{
	if (!m_impl || !IsValidOperation(request.operationId) || !request.paneId.IsValid()) {
		return { TerminalRuntimeOperationCode::InvalidRequest };
	}
	std::lock_guard lock(m_impl->mutex);
	if (m_impl->closing) return { TerminalRuntimeOperationCode::ServerStopping };
	const auto pane = m_impl->collection.FindPane(request.paneId);
	if (!pane) return { TerminalRuntimeOperationCode::TargetMissing, m_impl->collection.Revision() };
	return MapTopologyResult(m_impl->collection.SelectPane(request.paneId));
}

TerminalTopologyResult CTerminalRuntimeService::ClosePane(const TerminalPaneCloseRequest& request)
{
	if (!m_impl || !IsValidOperation(request.operationId) || !request.paneId.IsValid()) {
		return { TerminalRuntimeOperationCode::InvalidRequest };
	}
	std::optional<TerminalInstanceId> instanceId;
	runtime::topology::TerminalTopologyResult topologyResult;
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->closing) return { TerminalRuntimeOperationCode::ServerStopping };
		const auto pane = m_impl->collection.FindPane(request.paneId);
		if (!pane) return { TerminalRuntimeOperationCode::TargetMissing, m_impl->collection.Revision() };
		instanceId = pane->instanceId;
		topologyResult = m_impl->collection.ClosePane(pane->sessionId, pane->windowId, pane->id);
	}
	if (topologyResult.Succeeded() && instanceId) m_impl->BeginCloseInstance(*instanceId, TerminalInstanceCloseReason::Explicit);
	return MapTopologyResult(topologyResult);
}

TerminalTopologyResult CTerminalRuntimeService::CloseWindow(const TerminalWindowCloseRequest& request)
{
	if (!m_impl || !IsValidOperation(request.operationId) || !request.windowId.IsValid()) {
		return { TerminalRuntimeOperationCode::InvalidRequest };
	}
	std::vector<TerminalInstanceId> instanceIds;
	runtime::topology::TerminalTopologyResult topologyResult;
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->closing) return { TerminalRuntimeOperationCode::ServerStopping };
		const auto window = FindWindow(m_impl->collection.Snapshot(), request.windowId);
		if (!window) return { TerminalRuntimeOperationCode::TargetMissing, m_impl->collection.Revision() };
		for (const auto& pane : window->window.panes) instanceIds.push_back(pane.instanceId);
		topologyResult = m_impl->collection.CloseWindow(window->sessionId, request.windowId);
	}
	if (topologyResult.Succeeded()) {
		for (const auto id : instanceIds) m_impl->BeginCloseInstance(id, TerminalInstanceCloseReason::Explicit);
	}
	return MapTopologyResult(topologyResult);
}

TerminalTopologyResult CTerminalRuntimeService::CloseSession(const TerminalSessionCloseRequest& request)
{
	if (!m_impl || !IsValidOperation(request.operationId) || !request.sessionId.IsValid()) {
		return { TerminalRuntimeOperationCode::InvalidRequest };
	}
	std::vector<TerminalInstanceId> instanceIds;
	runtime::topology::TerminalTopologyResult topologyResult;
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->closing) return { TerminalRuntimeOperationCode::ServerStopping };
		const auto session = m_impl->collection.FindSession(request.sessionId);
		if (!session) return { TerminalRuntimeOperationCode::TargetMissing, m_impl->collection.Revision() };
		for (const auto& window : session->windows) {
			for (const auto& pane : window.panes) instanceIds.push_back(pane.instanceId);
		}
		topologyResult = m_impl->collection.CloseSession(request.sessionId);
	}
	if (topologyResult.Succeeded()) {
		for (const auto id : instanceIds) m_impl->BeginCloseInstance(id, TerminalInstanceCloseReason::Explicit);
	}
	return MapTopologyResult(topologyResult);
}

TerminalInputResult CTerminalRuntimeService::QueueInputBatch(const TerminalInputBatch& batch)
{
	TerminalInputResult result;
	if (!m_impl || !batch.operationId.IsValid() || !batch.target.instanceId.IsValid()) {
		result.code = TerminalInputResultCode::InvalidInput;
		return result;
	}
	std::shared_ptr<TerminalInstance> instance;
	SakuraTerminalInputAdapter* adapter{};
	bool bracketedPaste{};
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->closing) {
			result.code = TerminalInputResultCode::BrokerStopping;
			return result;
		}
		instance = m_impl->FindLocked(batch.target.instanceId);
		const auto validation = m_impl->ValidateTargetLocked(batch.target, instance);
		if (validation != TerminalRuntimeOperationCode::Succeeded) {
			result.code = validation == TerminalRuntimeOperationCode::TopologyChanged
				? TerminalInputResultCode::StaleGeneration : TerminalInputResultCode::TargetMissing;
			return result;
		}
		adapter = instance ? instance->InputAdapter() : nullptr;
		const auto* model = instance ? instance->Model() : nullptr;
		if (model) bracketedPaste = model->Modes().bracketedPaste;
	}
	if (!instance || !adapter) {
		result.code = TerminalInputResultCode::NotRunning;
		return result;
	}
	TerminalInputBatchCommitter committer;
	result = committer.EncodeAndCommit(batch, *adapter, bracketedPaste,
		std::chrono::steady_clock::now(), [instance](const std::span<const std::uint8_t> bytes) {
			return instance->QueueInput(bytes);
		});
	result.contentRevision = instance->Snapshot().contentRevision;
	result.errorCode = instance->LastError();
	return result;
}

TerminalCaptureResult CTerminalRuntimeService::Capture(const TerminalCaptureRequest& request)
{
	TerminalCaptureResult result;
	if (!m_impl || !request.operationId.IsValid() || !request.target.instanceId.IsValid()) {
		result.code = TerminalCaptureResultCode::InvalidRequest;
		return result;
	}
	std::shared_ptr<TerminalInstance> instance;
	TerminalInstanceSnapshot snapshot;
	std::optional<TerminalCaptureDelta> delta;
	bool hasIndex{};
	TerminalCaptureCursor earliest;
	TerminalCaptureCursor next;
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->closing) {
			result.code = TerminalCaptureResultCode::NotRunning;
			return result;
		}
		instance = m_impl->FindLocked(request.target.instanceId);
		const auto validation = m_impl->ValidateTargetLocked(request.target, instance);
		if (validation != TerminalRuntimeOperationCode::Succeeded) {
			result.code = validation == TerminalRuntimeOperationCode::TopologyChanged
				? TerminalCaptureResultCode::StaleCursor : TerminalCaptureResultCode::TargetMissing;
			return result;
		}
		snapshot = instance->Snapshot();
		if (snapshot.state == TerminalInstanceState::Reserved || snapshot.state == TerminalInstanceState::Starting) {
			result.code = TerminalCaptureResultCode::NotRunning;
			return result;
		}
		auto& capture = m_impl->captures[request.target.instanceId];
		if (capture.index) {
			hasIndex = true;
			earliest = capture.index->EarliestCursor();
			next = capture.index->CurrentCursor();
			if (request.since) delta = capture.index->ChangesSince(*request.since);
		} else {
			earliest = m_impl->MakeCursor(request.target.instanceId, snapshot, capture);
			next = earliest;
		}
		result.coordinates.runtimeGeneration = m_impl->runtimeGeneration;
		result.coordinates.instanceId = request.target.instanceId;
		result.coordinates.instanceGeneration = instance->InstanceGeneration();
		result.coordinates.screenEpoch = capture.screenEpoch;
		result.coordinates.revision = snapshot.contentRevision;
		result.coordinates.scrollbackBaseOrdinal = capture.scrollbackBaseOrdinal;
		result.earliestCursor = earliest;
		result.nextCursor = next;
	}
	result.alternateScreen = snapshot.alternateScreen;
	if (request.since) {
		const auto& since = *request.since;
		if (since.version != 1 || since.runtimeGeneration != m_impl->runtimeGeneration
			|| since.instanceId != request.target.instanceId
			|| since.instanceGeneration != result.coordinates.instanceGeneration) {
			result.code = TerminalCaptureResultCode::StaleCursor;
			result.gap = true;
			result.resyncSnapshot = true;
			return result;
		}
		if (since == result.nextCursor) {
			result.code = TerminalCaptureResultCode::Succeeded;
			return result;
		}
		if (hasIndex) {
			if (!delta || delta->code == ETerminalCaptureDeltaCode::InvalidCursor
				|| delta->code == ETerminalCaptureDeltaCode::Gap) {
				result.code = TerminalCaptureResultCode::StaleCursor;
				if (delta) {
					result.gap = delta->gap;
					result.resyncSnapshot = delta->resyncSnapshot;
				}
				return result;
			}
		}
		if (!hasIndex && since.revision != result.nextCursor.revision) {
			result.code = TerminalCaptureResultCode::StaleCursor;
			result.gap = true;
			result.resyncSnapshot = true;
			return result;
		}
	}

	const auto* model = instance->Model();
	if (!model) {
		result.code = TerminalCaptureResultCode::TargetMissing;
		return result;
	}
	TerminalCaptureExtractionRequest extraction;
	extraction.startLine = request.startLine;
	extraction.endLine = request.endLine;
	if (request.since && delta && delta->code == ETerminalCaptureDeltaCode::Delta) {
		extraction.selectedRanges = delta->dirtyScreenRanges;
		if (delta->appendedHistoryBeginOrdinal != 0) {
			const auto historySize = model->ScrollbackSize();
			const auto base = result.coordinates.scrollbackBaseOrdinal;
			if (base == 0 || delta->appendedHistoryBeginOrdinal < base
				|| delta->appendedHistoryEndOrdinal < delta->appendedHistoryBeginOrdinal) {
				result.code = TerminalCaptureResultCode::StaleCursor;
				result.gap = true;
				result.resyncSnapshot = true;
				return result;
			}
			const auto firstIndex = delta->appendedHistoryBeginOrdinal - base;
			const auto lastIndex = delta->appendedHistoryEndOrdinal - base;
			if (lastIndex >= historySize
				|| historySize > static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)())) {
				result.code = TerminalCaptureResultCode::StaleCursor;
				result.gap = true;
				result.resyncSnapshot = true;
				return result;
			}
			const auto history = static_cast<std::int64_t>(historySize);
			extraction.selectedRanges->push_back({
				static_cast<std::int64_t>(firstIndex) - history,
				static_cast<std::int64_t>(lastIndex) - history });
		}
	}
	extraction.joinWrappedLines = request.joinWrappedLines;
	extraction.limits = request.limits;
	extraction.deadline = request.deadline;
	const auto extracted = ExtractTerminalCapture(*model, extraction);
	result.code = extracted.code;
	result.lines = extracted.lines;
	result.alternateScreen = extracted.alternateScreen;
	result.truncated = extracted.truncated;
	result.truncationReason = extracted.truncationReason;
	return result;
}

TerminalSnapshotResult CTerminalRuntimeService::Snapshot(const TerminalSnapshotRequest& request) const
{
	TerminalSnapshotResult result;
	if (!m_impl || !request.target.instanceId.IsValid()) {
		result.code = TerminalRuntimeOperationCode::InvalidRequest;
		return result;
	}
	std::lock_guard lock(m_impl->mutex);
	const auto instance = m_impl->FindLocked(request.target.instanceId);
	const auto validation = m_impl->ValidateTargetLocked(request.target, instance);
	if (validation != TerminalRuntimeOperationCode::Succeeded) {
		result.code = validation;
		return result;
	}
	result.code = TerminalRuntimeOperationCode::Succeeded;
	result.snapshot = instance->Snapshot();
	return result;
}

std::optional<TerminalBackendProcessIdentity> CTerminalRuntimeService::GetProcessIdentity(
	const TerminalTargetCoordinate& target) const noexcept
{
	if (!m_impl || !target.instanceId.IsValid() || target.instanceGeneration == 0) {
		return std::nullopt;
	}
	try {
		std::shared_ptr<TerminalInstance> instance;
		{
			std::lock_guard lock(m_impl->mutex);
			instance = m_impl->FindLocked(target.instanceId);
			if (m_impl->ValidateTargetLocked(target, instance)
				!= TerminalRuntimeOperationCode::Succeeded) {
				return std::nullopt;
			}
		}
		return instance ? instance->GetProcessIdentity() : std::nullopt;
	} catch (...) {
		return std::nullopt;
	}
}

bool CTerminalRuntimeService::OwnsProcess(
	const TerminalTargetCoordinate& target,
	const std::uint32_t processId,
	const std::uint64_t creationTime) const noexcept
{
	if (!m_impl || !target.instanceId.IsValid() || target.instanceGeneration == 0
		|| processId == 0 || creationTime == 0) {
		return false;
	}
	try {
		std::shared_ptr<TerminalInstance> instance;
		{
			std::lock_guard lock(m_impl->mutex);
			instance = m_impl->FindLocked(target.instanceId);
			if (m_impl->ValidateTargetLocked(target, instance)
				!= TerminalRuntimeOperationCode::Succeeded) {
				return false;
			}
		}
		return instance && instance->OwnsProcess(processId, creationTime);
	} catch (...) {
		return false;
	}
}

std::optional<TerminalBackendProcessIdentity> CTerminalRuntimeService::GetProcessIdentity(
	const TerminalInstanceId id) const noexcept
{
	if (!m_impl || !id.IsValid()) return std::nullopt;
	try {
		std::shared_ptr<TerminalInstance> instance;
		{
			std::lock_guard lock(m_impl->mutex);
			instance = m_impl->FindLocked(id);
		}
		return instance ? instance->GetProcessIdentity() : std::nullopt;
	} catch (...) {
		return std::nullopt;
	}
}

bool CTerminalRuntimeService::OwnsProcess(
	const TerminalInstanceId id,
	const std::uint32_t processId,
	const std::uint64_t creationTime) const noexcept
{
	if (!m_impl || !id.IsValid() || processId == 0 || creationTime == 0) return false;
	try {
		std::shared_ptr<TerminalInstance> instance;
		{
			std::lock_guard lock(m_impl->mutex);
			instance = m_impl->FindLocked(id);
		}
		return instance && instance->OwnsProcess(processId, creationTime);
	} catch (...) {
		return false;
	}
}

TerminalResizeResult CTerminalRuntimeService::Resize(const TerminalResizeRequest& request)
{
	TerminalResizeResult result;
	if (!m_impl || !request.operationId.IsValid() || !request.target.instanceId.IsValid()
		|| request.size.columns == 0 || request.size.rows == 0) {
		result.code = TerminalRuntimeOperationCode::InvalidRequest;
		return result;
	}
	if (IsDeadlineExpired(request.deadline)) {
		result.code = TerminalRuntimeOperationCode::DeadlineExceeded;
		return result;
	}
	std::shared_ptr<TerminalInstance> instance;
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->closing) {
			result.code = TerminalRuntimeOperationCode::ServerStopping;
			return result;
		}
		instance = m_impl->FindLocked(request.target.instanceId);
		const auto validation = m_impl->ValidateTargetLocked(request.target, instance);
		if (validation != TerminalRuntimeOperationCode::Succeeded) {
			result.code = validation;
			return result;
		}
	}
	result.code = instance->Resize(request.size).succeeded
		? TerminalRuntimeOperationCode::Succeeded : TerminalRuntimeOperationCode::NotRunning;
	return result;
}

TerminalInstanceDrainResult CTerminalRuntimeService::DrainOutput(const TerminalInstanceId id)
{
	if (!m_impl || !id.IsValid()) return {};
	std::shared_ptr<TerminalInstance> instance;
	{
		std::lock_guard lock(m_impl->mutex);
		instance = m_impl->FindLocked(id);
	}
	if (!instance) return {};
	const auto result = instance->DrainOutput();
	m_impl->RecordDrain(id, result);
	return result;
}

TerminalSubscription CTerminalRuntimeService::Subscribe(TerminalRuntimeEventCallback callback)
{
	if (!m_impl || !callback) return {};
	std::uint64_t id{};
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->closing || m_impl->nextSubscriberId == 0) return {};
		id = m_impl->nextSubscriberId++;
		m_impl->subscribers.push_back({ id, std::move(callback) });
	}
	const std::weak_ptr<Impl> weak = m_impl;
	return TerminalSubscription([weak, id] {
		if (const auto impl = weak.lock()) {
			std::lock_guard lock(impl->mutex);
			const auto found = std::remove_if(impl->subscribers.begin(), impl->subscribers.end(),
				[id](const Impl::Subscriber& subscriber) { return subscriber.id == id; });
			impl->subscribers.erase(found, impl->subscribers.end());
		}
	});
}

void CTerminalRuntimeService::BeginClose() noexcept
{
	if (m_impl) m_impl->BeginCloseAll();
}

TerminalRuntimeCloseResult CTerminalRuntimeService::WaitForClose(
	const std::chrono::steady_clock::time_point absoluteDeadline) noexcept
{
	return m_impl ? m_impl->WaitForCloseAll(absoluteDeadline)
		: TerminalRuntimeCloseResult{ TerminalRuntimeCloseWaitStatus::Unavailable };
}

TerminalInstance* CTerminalRuntimeService::Instance(const TerminalInstanceId id) noexcept
{
	if (!m_impl) return nullptr;
	std::lock_guard lock(m_impl->mutex);
	const auto instance = m_impl->FindLocked(id);
	return instance.get();
}

const TerminalInstance* CTerminalRuntimeService::Instance(const TerminalInstanceId id) const noexcept
{
	if (!m_impl) return nullptr;
	std::lock_guard lock(m_impl->mutex);
	const auto instance = m_impl->FindLocked(id);
	return instance.get();
}

void CTerminalRuntimeService::BeginCloseInstance(
	const TerminalInstanceId id, const TerminalInstanceCloseReason reason) noexcept
{
	if (!m_impl) return;
	std::shared_ptr<TerminalInstance> instance;
	{
		std::lock_guard lock(m_impl->mutex);
		instance = m_impl->FindLocked(id);
	}
	if (instance) instance->BeginClose(reason);
}

TerminalInstanceCloseWaitResult CTerminalRuntimeService::WaitForInstanceClose(
	const TerminalInstanceId id, const std::chrono::steady_clock::time_point deadline) noexcept
{
	if (!m_impl) return { TerminalInstanceCloseWaitStatus::Unavailable, std::nullopt };
	std::shared_ptr<TerminalInstance> instance;
	{
		std::lock_guard lock(m_impl->mutex);
		instance = m_impl->FindLocked(id);
	}
	return instance ? instance->WaitForClose(deadline)
		: TerminalInstanceCloseWaitResult{ TerminalInstanceCloseWaitStatus::Unavailable, std::nullopt };
}

TerminalModel* CTerminalRuntimeService::Model(const TerminalInstanceId id) noexcept
{
	const auto* self = std::as_const(*this).Instance(id);
	return self ? const_cast<TerminalModel*>(self->Model()) : nullptr;
}

const TerminalModel* CTerminalRuntimeService::Model(const TerminalInstanceId id) const noexcept
{
	const auto* instance = Instance(id);
	return instance ? instance->Model() : nullptr;
}

SakuraTerminalInputAdapter* CTerminalRuntimeService::InputAdapter(const TerminalInstanceId id) noexcept
{
	const auto* self = std::as_const(*this).Instance(id);
	return self ? const_cast<SakuraTerminalInputAdapter*>(self->InputAdapter()) : nullptr;
}

const SakuraTerminalInputAdapter* CTerminalRuntimeService::InputAdapter(const TerminalInstanceId id) const noexcept
{
	const auto* instance = Instance(id);
	return instance ? instance->InputAdapter() : nullptr;
}

TerminalRuntimeGeneration CTerminalRuntimeService::RuntimeGeneration() const noexcept
{
	return m_impl ? m_impl->runtimeGeneration : TerminalRuntimeGeneration{};
}

std::optional<runtime::topology::TerminalCollectionSnapshot> CTerminalRuntimeService::CollectionSnapshot() const
{
	if (!m_impl) return std::nullopt;
	std::lock_guard lock(m_impl->mutex);
	return m_impl->collection.Snapshot();
}

} // namespace terminal
