/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "StdAfx.h"

#include "workbench/tasks/TaskExecutionService.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace workbench::tasks {
namespace {

constexpr std::size_t kMaximumOperationIdBytes = 160;
constexpr std::size_t kMaximumDiagnosticCharacters = 4'096;
constexpr std::size_t kMaximumTaskLabelCharacters = 256;
constexpr std::size_t kMaximumTaskArguments = 64;
constexpr std::size_t kMaximumTaskPayloadCharacters = 64U * 1024U;
constexpr std::uint16_t kMaximumTerminalColumns = 1'000;
constexpr std::uint16_t kMaximumTerminalRows = 1'000;

bool IsBoundedText(const std::wstring_view value, const std::size_t maximum = 4'096) noexcept
{
	return !value.empty() && value.size() <= maximum
		&& std::none_of(value.begin(), value.end(), [](const wchar_t character) { return character <= 0x1f || character == 0x7f; });
}

bool IsBoundedArgument(const std::wstring_view value) noexcept
{
	return value.size() <= 4'096
		&& std::none_of(value.begin(), value.end(), [](const wchar_t character) { return character <= 0x1f || character == 0x7f; });
}

bool IsValidStartDefinition(const TaskExecutionStartRequest& request) noexcept
{
	const auto& definition = request.definition;
	if (!definition.IsRunnable() || !IsBoundedText(definition.label, kMaximumTaskLabelCharacters)
		|| !IsBoundedText(definition.command) || definition.arguments.size() > kMaximumTaskArguments
		|| (definition.workingDirectory && !IsBoundedText(*definition.workingDirectory))
		|| request.initialSize.columns == 0 || request.initialSize.rows == 0
		|| request.initialSize.columns > kMaximumTerminalColumns || request.initialSize.rows > kMaximumTerminalRows) return false;
	std::size_t total = definition.label.size() + definition.command.size();
	if (definition.workingDirectory) total += definition.workingDirectory->size();
	for (const auto& argument : definition.arguments) {
		// Empty arguments are meaningful process/shell tokens and must survive
		// unchanged; only their size and control-character content are bounded.
		if (!IsBoundedArgument(argument) || argument.size() > kMaximumTaskPayloadCharacters - total) return false;
		total += argument.size();
	}
	return total <= kMaximumTaskPayloadCharacters;
}

std::wstring BoundedDiagnostic(std::wstring value)
{
	if (value.size() > kMaximumDiagnosticCharacters) value.resize(kMaximumDiagnosticCharacters);
	return value;
}

void AppendToken(std::string& destination, const std::string_view value)
{
	destination.append(std::to_string(value.size()));
	destination.push_back(':');
	destination.append(value);
	destination.push_back(';');
}

void AppendWideToken(std::string& destination, const std::wstring_view value)
{
	AppendToken(destination, std::string(reinterpret_cast<const char*>(value.data()), value.size() * sizeof(wchar_t)));
}

void AppendOperation(std::string& destination, const TaskExecutionOperation& operation)
{
	AppendToken(destination, operation.operationId);
	AppendToken(destination, operation.expectedRevision ? std::to_string(*operation.expectedRevision) : "-");
}

std::string StartFingerprint(const TaskExecutionStartRequest& request)
{
	std::string result("start;");
	AppendOperation(result, request.operation);
	AppendWideToken(result, request.definition.label);
	AppendToken(result, std::to_string(static_cast<unsigned int>(request.definition.executionKind)));
	AppendWideToken(result, request.definition.command);
	for (const auto& argument : request.definition.arguments) AppendWideToken(result, argument);
	AppendWideToken(result, request.definition.workingDirectory.value_or(L""));
	AppendToken(result, std::to_string(request.initialSize.columns));
	AppendToken(result, std::to_string(request.initialSize.rows));
	return result;
}

std::string MutationFingerprint(const char* verb, const TaskExecutionRunMutationRequest& request)
{
	std::string result(verb);
	result.push_back(';');
	AppendOperation(result, request.operation);
	AppendToken(result, std::to_string(request.runId));
	return result;
}

ETaskExecutionChangeKind ChangeForState(const ETaskExecutionRunState state) noexcept
{
	switch (state) {
	case ETaskExecutionRunState::Starting: return ETaskExecutionChangeKind::RunStarted;
	case ETaskExecutionRunState::Running: return ETaskExecutionChangeKind::RunRunning;
	case ETaskExecutionRunState::Cancelling: return ETaskExecutionChangeKind::RunCancelling;
	case ETaskExecutionRunState::Closing: return ETaskExecutionChangeKind::RunClosing;
	case ETaskExecutionRunState::Exited: return ETaskExecutionChangeKind::RunExited;
	case ETaskExecutionRunState::Cancelled: return ETaskExecutionChangeKind::RunCancelled;
	case ETaskExecutionRunState::Failed: return ETaskExecutionChangeKind::RunFailed;
	case ETaskExecutionRunState::Closed: return ETaskExecutionChangeKind::RunClosed;
	}
	return ETaskExecutionChangeKind::RunFailed;
}

} // namespace

struct TaskExecutionService::Impl final : std::enable_shared_from_this<Impl> {
	enum class EStopLifecycle : std::uint8_t { Active, Stopping, Stopped };
	struct Run final {
		std::uint64_t id{};
		TaskConfigurationDefinition definition;
		ETaskExecutionRunState state{ ETaskExecutionRunState::Starting };
		std::uint32_t exitCode{};
		std::wstring diagnostic;
		std::shared_ptr<ITaskExecutionSession> session;
		std::string closingOperationId;
		std::string closingFingerprint;
		ETaskExecutionOperationReason closeFailureReason{ ETaskExecutionOperationReason::None };
		bool startupInFlight{};
		bool stopCloseInitiated{};
		bool stopCloseWaited{};
	};
	struct CompletedOperation final {
		std::string fingerprint;
		TaskExecutionOperationResult result;
	};
	struct PendingNotification final {
		TaskExecutionServiceChange change;
		std::vector<TaskExecutionListener> listeners;
	};

	mutable std::mutex mutex;
	std::condition_variable startupCondition;
	std::condition_variable stopCondition;
	std::condition_variable notificationDrainCondition;
	std::shared_ptr<ITaskExecutionSessionFactory> factory;
	TaskExecutionServiceLimits limits;
	std::map<std::uint64_t, Run> runs;
	std::map<std::string, CompletedOperation, std::less<>> completedOperations;
	std::deque<std::string> completedOperationOrder;
	std::map<TaskExecutionSubscriptionId, TaskExecutionListener> subscriptions;
	std::deque<PendingNotification> pendingNotifications;
	std::uint64_t revision{ 1 };
	std::uint64_t nextRunId{ 1 };
	std::size_t activeRunCount{};
	std::uint64_t droppedNotificationCount{};
	TaskExecutionSubscriptionId nextSubscriptionId{ 1 };
	bool drainingNotifications{};
	std::thread::id notificationDrainerThread;
	bool stopped{};
	std::size_t inFlightStartCount{};
	std::optional<std::chrono::steady_clock::time_point> stopDeadline;
	EStopLifecycle stopLifecycle{ EStopLifecycle::Active };
	std::thread::id stopOwnerThread;
	TaskExecutionOperationResult completedStopResult;
	bool dispatcherDeferredStopRequested{};
	inline static thread_local Impl* s_notificationDispatchingImpl{};

	[[nodiscard]] bool TakeDispatcherDeferredStopRequest()
	{
		std::lock_guard lock(mutex);
		if (!dispatcherDeferredStopRequested || stopLifecycle != EStopLifecycle::Active || inFlightStartCount != 0) return false;
		dispatcherDeferredStopRequested = false;
		return true;
	}

	void PublishStopResultNoexcept(TaskExecutionOperationResult& result) noexcept
	{
		// Stop publication deliberately keeps its own result diagnostic empty. The
		// per-run terminal diagnostic remains in the snapshot, while this avoids a
		// final string allocation from trapping every later Stop caller in Stopping.
		result.diagnostic.clear();
		std::lock_guard lock(mutex);
		subscriptions.clear();
		result.revision = revision;
		completedStopResult.status = result.status;
		completedStopResult.reason = result.reason;
		completedStopResult.revision = result.revision;
		completedStopResult.runId = result.runId;
		completedStopResult.errorCode = result.errorCode;
		completedStopResult.diagnostic.clear();
		stopOwnerThread = {};
		stopLifecycle = EStopLifecycle::Stopped;
		stopCondition.notify_all();
	}

	[[nodiscard]] TaskExecutionOperationResult Stop();

	void ProcessDeferredStop() noexcept
	{
		// A listener is still on the stack when a nested mutation returns early;
		// preserve the request for the outer notification owner instead of trying
		// to Stop recursively from the dispatcher.
		if (s_notificationDispatchingImpl == this) return;
		if (!TakeDispatcherDeferredStopRequest()) return;
		try { (void)Stop(); }
		catch (...) {
			// Stop publishes a terminal result after it becomes the owner. If an
			// exception happened before ownership, retain one request for a later
			// safe notification boundary instead of losing it.
			std::lock_guard lock(mutex);
			if (stopLifecycle == EStopLifecycle::Active) dispatcherDeferredStopRequested = true;
		}
	}

	explicit Impl(std::shared_ptr<ITaskExecutionSessionFactory> initialFactory, TaskExecutionServiceLimits initialLimits)
		: factory(std::move(initialFactory)), limits(std::move(initialLimits))
	{
		if (limits.maximumActiveRuns == 0) limits.maximumActiveRuns = 1;
		if (limits.maximumRetainedRuns == 0) limits.maximumRetainedRuns = 1;
		if (limits.maximumSubscriptions == 0) limits.maximumSubscriptions = 1;
		if (limits.maximumRememberedOperations == 0) limits.maximumRememberedOperations = 1;
		if (limits.maximumPendingNotifications == 0) limits.maximumPendingNotifications = 1;
		if (limits.sessionCloseTimeout < std::chrono::milliseconds::zero()) limits.sessionCloseTimeout = std::chrono::milliseconds::zero();
	}

	[[nodiscard]] TaskExecutionOperationResult Current(const ETaskExecutionOperationStatus status, const ETaskExecutionOperationReason reason) const noexcept
	{
		return { status, reason, revision, std::nullopt, 0, {} };
	}

	void RememberLocked(std::string operationId, std::string fingerprint, const TaskExecutionOperationResult& result)
	{
		if (const auto found = completedOperations.find(operationId); found != completedOperations.end()) {
			found->second = CompletedOperation { std::move(fingerprint), result };
			return;
		}
		completedOperations.emplace(operationId, CompletedOperation { std::move(fingerprint), result });
		completedOperationOrder.push_back(std::move(operationId));
		while (completedOperationOrder.size() > limits.maximumRememberedOperations) {
			const auto oldest = std::move(completedOperationOrder.front());
			completedOperationOrder.pop_front();
			completedOperations.erase(oldest);
		}
	}

	[[nodiscard]] std::optional<TaskExecutionOperationResult> ReplayLocked(const TaskExecutionOperation& operation, const std::string_view fingerprint)
	{
		const auto found = completedOperations.find(operation.operationId);
		if (found == completedOperations.end()) return std::nullopt;
		if (found->second.fingerprint != fingerprint) return Current(ETaskExecutionOperationStatus::Conflict, ETaskExecutionOperationReason::OperationIdConflict);
		auto result = found->second.result;
		result.status = ETaskExecutionOperationStatus::Replayed;
		result.revision = revision;
		return result;
	}

	void QueueNotificationLocked(const Run& run)
	{
		if (pendingNotifications.size() >= limits.maximumPendingNotifications) {
			if (droppedNotificationCount != std::numeric_limits<std::uint64_t>::max()) ++droppedNotificationCount;
			return;
		}
		try {
			PendingNotification pending { { revision, run.id, ChangeForState(run.state), run.state }, {} };
			pending.listeners.reserve(subscriptions.size());
			for (const auto& subscription : subscriptions) pending.listeners.push_back(subscription.second);
			pendingNotifications.push_back(std::move(pending));
		}
		catch (...) {
			if (droppedNotificationCount != std::numeric_limits<std::uint64_t>::max()) ++droppedNotificationCount;
		}
	}

	void PruneTerminalRunsLocked()
	{
		while (runs.size() > limits.maximumRetainedRuns) {
			auto terminal = std::find_if(runs.begin(), runs.end(), [](const auto& entry) { return IsTerminalTaskRunState(entry.second.state); });
			if (terminal == runs.end()) return;
			runs.erase(terminal);
		}
	}

	void DeliverNotifications()
	{
		{
			std::lock_guard lock(mutex);
			if (drainingNotifications || pendingNotifications.empty()) return;
			drainingNotifications = true;
			notificationDrainerThread = std::this_thread::get_id();
		}
		for (;;) {
			PendingNotification pending;
			std::vector<TaskExecutionListener> listeners;
			{
				std::lock_guard lock(mutex);
				if (pendingNotifications.empty()) {
					drainingNotifications = false;
					notificationDrainerThread = {};
					notificationDrainCondition.notify_all();
					return;
				}
				pending = std::move(pendingNotifications.front());
				pendingNotifications.pop_front();
				listeners = std::move(pending.listeners);
			}
			for (const auto& listener : listeners) {
				struct DispatchScope final {
					Impl* previous{};
					explicit DispatchScope(Impl* current) : previous(Impl::s_notificationDispatchingImpl) { Impl::s_notificationDispatchingImpl = current; }
					~DispatchScope() { Impl::s_notificationDispatchingImpl = previous; }
				} dispatchScope(this);
				try { listener(pending.change); }
				catch (...) { /* Listener faults are isolated from service progress. */ }
			}
		}
	}

	void DrainNotificationsForStop()
	{
		DeliverNotifications();
		if (s_notificationDispatchingImpl == this) return;
		std::unique_lock lock(mutex);
		if (drainingNotifications && notificationDrainerThread != std::this_thread::get_id()) {
			notificationDrainCondition.wait(lock, [this] { return !drainingNotifications; });
		}
	}

	void CompleteFromSession(const std::uint64_t runId, const TaskSessionExit exit)
	{
		{
			std::lock_guard lock(mutex);
			const auto found = runs.find(runId);
			if (found == runs.end() || IsTerminalTaskRunState(found->second.state)) return;
			const bool wasClosing = found->second.state == ETaskExecutionRunState::Closing;
			found->second.state = exit.kind == ETaskSessionExitKind::Exited ? ETaskExecutionRunState::Exited
				: exit.kind == ETaskSessionExitKind::Cancelled ? ETaskExecutionRunState::Cancelled : ETaskExecutionRunState::Failed;
			found->second.exitCode = exit.exitCode;
			found->second.diagnostic = BoundedDiagnostic(exit.diagnostic);
			if (wasClosing && exit.kind == ETaskSessionExitKind::Failed) {
				found->second.closeFailureReason = ETaskExecutionOperationReason::SessionCloseHostFailure;
			}
			found->second.session.reset();
			if (activeRunCount != 0) --activeRunCount;
			if (revision != std::numeric_limits<std::uint64_t>::max()) ++revision;
			QueueNotificationLocked(found->second);
			PruneTerminalRunsLocked();
		}
		DeliverNotifications();
		ProcessDeferredStop();
	}

	void EndStartup(const std::uint64_t runId) noexcept
	{
		{
			std::lock_guard lock(mutex);
			if (const auto found = runs.find(runId); found != runs.end()) found->second.startupInFlight = false;
			if (inFlightStartCount != 0) --inFlightStartCount;
		}
		startupCondition.notify_all();
	}

	[[nodiscard]] std::chrono::steady_clock::time_point CloseDeadline() const
	{
		std::lock_guard lock(mutex);
		return stopDeadline.value_or(std::chrono::steady_clock::now() + limits.sessionCloseTimeout);
	}

	[[nodiscard]] static TaskExecutionSessionCloseResult CloseSession(
		const std::shared_ptr<ITaskExecutionSession>& session,
		const std::chrono::steady_clock::time_point deadline)
	{
		if (!session) return TaskExecutionSessionCloseResult::HostFailure(0, L"task session was unavailable during close");
		session->BeginClose();
		return session->WaitForClose(deadline);
	}

	void FinalizeClose(const std::uint64_t runId, const TaskExecutionSessionCloseResult& closeResult)
	{
		{
			std::lock_guard lock(mutex);
			const auto found = runs.find(runId);
			if (found == runs.end() || IsTerminalTaskRunState(found->second.state)) return;
			auto& run = found->second;
			if (closeResult.kind == ETaskExecutionSessionCloseKind::Closed) {
				run.state = ETaskExecutionRunState::Closed;
				run.exitCode = 0;
				run.diagnostic.clear();
			}
			else {
				run.state = ETaskExecutionRunState::Failed;
				run.exitCode = closeResult.errorCode;
				run.diagnostic = BoundedDiagnostic(closeResult.diagnostic.empty()
					? closeResult.kind == ETaskExecutionSessionCloseKind::TimedOut
						? L"task session close timed out"
						: L"task session close failed"
					: closeResult.diagnostic);
				run.closeFailureReason = closeResult.kind == ETaskExecutionSessionCloseKind::TimedOut
					? ETaskExecutionOperationReason::SessionCloseTimedOut : ETaskExecutionOperationReason::SessionCloseHostFailure;
			}
			run.session.reset();
			if (activeRunCount != 0) --activeRunCount;
			if (revision != std::numeric_limits<std::uint64_t>::max()) ++revision;
			QueueNotificationLocked(run);
			PruneTerminalRunsLocked();
		}
		DeliverNotifications();
	}

	// The session-result contract already proves quiescence. If recording its
	// diagnostic allocates unsuccessfully, preserve that ownership guarantee with
	// an allocation-free terminal fallback rather than leaving Stop half-finished.
	void FinalizeCloseAfterQuiescence(const std::uint64_t runId, const TaskExecutionSessionCloseResult& closeResult) noexcept
	{
		try {
			FinalizeClose(runId, closeResult);
			return;
		}
		catch (...) {
			{
				std::lock_guard lock(mutex);
				const auto found = runs.find(runId);
				if (found == runs.end() || IsTerminalTaskRunState(found->second.state)) return;
				auto& run = found->second;
				run.state = closeResult.kind == ETaskExecutionSessionCloseKind::Closed ? ETaskExecutionRunState::Closed : ETaskExecutionRunState::Failed;
				run.exitCode = closeResult.errorCode;
				run.diagnostic.clear();
				run.closeFailureReason = closeResult.kind == ETaskExecutionSessionCloseKind::TimedOut
					? ETaskExecutionOperationReason::SessionCloseTimedOut : ETaskExecutionOperationReason::SessionCloseHostFailure;
				run.session.reset();
				if (activeRunCount != 0) --activeRunCount;
				if (revision != std::numeric_limits<std::uint64_t>::max()) ++revision;
				QueueNotificationLocked(run);
				PruneTerminalRunsLocked();
			}
			try { DeliverNotifications(); }
			catch (...) { /* Listener delivery is advisory after terminal ownership is recorded. */ }
		}
	}

	[[nodiscard]] TaskExecutionOperationResult CompleteCloseOperationLocked(
		const TaskExecutionRunMutationRequest& request,
		const std::string_view fingerprint)
	{
		const auto found = runs.find(request.runId);
		if (found == runs.end()) return Current(ETaskExecutionOperationStatus::Rejected, ETaskExecutionOperationReason::RunNotFound);
		const auto& run = found->second;
		TaskExecutionOperationResult result { ETaskExecutionOperationStatus::Succeeded, ETaskExecutionOperationReason::None, revision, request.runId,
			run.exitCode, run.diagnostic };
		if (run.state == ETaskExecutionRunState::Failed) {
			result.status = ETaskExecutionOperationStatus::Rejected;
			result.reason = run.closeFailureReason == ETaskExecutionOperationReason::None
				? ETaskExecutionOperationReason::SessionCloseHostFailure : run.closeFailureReason;
		}
		RememberLocked(request.operation.operationId, std::string(fingerprint), result);
		return result;
	}
};

TaskExecutionService::TaskExecutionService(std::shared_ptr<ITaskExecutionSessionFactory> factory, TaskExecutionServiceLimits limits)
	: m_impl(std::make_shared<Impl>(std::move(factory), std::move(limits)))
{
}

TaskExecutionService::~TaskExecutionService()
{
	try { (void)Stop(); }
	catch (...) { /* Destruction cannot propagate a host shutdown allocation failure. */ }
}

void TaskExecutionService::ProcessDeferredStop()
{
	m_impl->ProcessDeferredStop();
}

bool TaskExecutionService::IsValidOperationId(const std::string_view value) noexcept
{
	return !value.empty() && value.size() <= kMaximumOperationIdBytes
		&& std::all_of(value.begin(), value.end(), [](const unsigned char character) {
			return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
				|| (character >= '0' && character <= '9') || character == '.' || character == '-' || character == '_' || character == ':';
		});
}

TaskExecutionOperationResult TaskExecutionService::Start(const TaskExecutionStartRequest& request)
{
	if (!IsValidOperationId(request.operation.operationId)) return { ETaskExecutionOperationStatus::Rejected, ETaskExecutionOperationReason::InvalidOperationId };
	if ((request.definition.executionKind != ETaskExecutionKind::Shell
			&& request.definition.executionKind != ETaskExecutionKind::Process)
		|| request.definition.unsupportedCapabilities != ETaskUnsupportedCapability::None) {
		return { ETaskExecutionOperationStatus::Rejected, ETaskExecutionOperationReason::UnsupportedTask };
	}
	if (!IsValidStartDefinition(request)) return { ETaskExecutionOperationStatus::Rejected, ETaskExecutionOperationReason::InvalidTask };
	const auto fingerprint = StartFingerprint(request);
	std::uint64_t runId{};
	std::shared_ptr<ITaskExecutionSessionFactory> factory;
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->stopped) return m_impl->Current(ETaskExecutionOperationStatus::Stopped, ETaskExecutionOperationReason::None);
		if (const auto replay = m_impl->ReplayLocked(request.operation, fingerprint)) return *replay;
		if (request.operation.expectedRevision && *request.operation.expectedRevision != m_impl->revision) {
			return m_impl->Current(ETaskExecutionOperationStatus::StaleRevision, ETaskExecutionOperationReason::ExpectedRevisionMismatch);
		}
		if (!m_impl->factory) return m_impl->Current(ETaskExecutionOperationStatus::Rejected, ETaskExecutionOperationReason::FactoryUnavailable);
		if (m_impl->activeRunCount >= m_impl->limits.maximumActiveRuns) {
			return m_impl->Current(ETaskExecutionOperationStatus::Rejected, ETaskExecutionOperationReason::MaximumActiveRuns);
		}
		if (m_impl->nextRunId == std::numeric_limits<std::uint64_t>::max() || m_impl->revision == std::numeric_limits<std::uint64_t>::max()) {
			return m_impl->Current(ETaskExecutionOperationStatus::Rejected, ETaskExecutionOperationReason::InvalidTask);
		}
		runId = m_impl->nextRunId++;
		Impl::Run reservedRun;
		reservedRun.id = runId;
		reservedRun.definition = request.definition;
		reservedRun.startupInFlight = true;
		m_impl->runs.emplace(runId, std::move(reservedRun));
		++m_impl->activeRunCount;
		++m_impl->inFlightStartCount;
		++m_impl->revision;
		m_impl->QueueNotificationLocked(m_impl->runs.at(runId));
		m_impl->RememberLocked(request.operation.operationId, fingerprint,
			{ ETaskExecutionOperationStatus::Started, ETaskExecutionOperationReason::None, m_impl->revision, runId });
		factory = m_impl->factory;
	}
	struct StartupFinalizer final {
		std::shared_ptr<Impl> impl;
		std::uint64_t runId{};
		bool completed{};
		void Complete() noexcept
		{
			if (!completed) {
				impl->EndStartup(runId);
				completed = true;
			}
		}
		~StartupFinalizer()
		{
			Complete();
			// Covers every exceptional Start exit as well as the normal explicit
			// finish path; ProcessDeferredStop waits for this in-flight reservation
			// to clear before it may become the Stop owner.
			impl->ProcessDeferredStop();
		}
	} startupFinalizer { m_impl, runId };
	m_impl->DeliverNotifications();
	auto finishStartup = [&] {
		startupFinalizer.Complete();
		ProcessDeferredStop();
	};
	bool stoppedBeforeCreation{};
	TaskExecutionOperationResult stoppedBeforeCreationResult;
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->stopped) {
			stoppedBeforeCreation = true;
			stoppedBeforeCreationResult = m_impl->Current(ETaskExecutionOperationStatus::Stopped, ETaskExecutionOperationReason::None);
		}
	}
	if (stoppedBeforeCreation) {
		// No factory call was made, so there is no host resource to join. This
		// Start invocation owns terminal completion for its reservation.
		m_impl->FinalizeClose(runId, TaskExecutionSessionCloseResult::Closed());
		finishStartup();
		return stoppedBeforeCreationResult;
	}

	const std::weak_ptr<Impl> weakImpl = m_impl;
	const TaskExecutionSessionCallbacks callbacks { [weakImpl, runId](TaskSessionExit exit) {
		try {
			if (const auto impl = weakImpl.lock()) impl->CompleteFromSession(runId, std::move(exit));
		}
		catch (...) { /* A host worker must never observe a model/listener fault. */ }
	} };
	std::unique_ptr<ITaskExecutionSession> created;
	try { created = factory->Create(callbacks); }
	catch (...) { created.reset(); }
	if (!created) {
		m_impl->CompleteFromSession(runId, { ETaskSessionExitKind::Failed, 0, L"task terminal factory did not create a session" });
		TaskExecutionOperationResult result;
		{
			std::lock_guard lock(m_impl->mutex);
			result = { ETaskExecutionOperationStatus::Rejected, ETaskExecutionOperationReason::FactoryUnavailable, m_impl->revision, runId };
			m_impl->RememberLocked(request.operation.operationId, fingerprint, result);
			if (const auto found = m_impl->runs.find(runId); found != m_impl->runs.end() && !found->second.closingOperationId.empty()) {
				TaskExecutionRunMutationRequest closeRequest { { found->second.closingOperationId, std::nullopt }, runId };
				(void)m_impl->CompleteCloseOperationLocked(closeRequest, found->second.closingFingerprint);
			}
		}
		finishStartup();
		return result;
	}
	const auto session = std::shared_ptr<ITaskExecutionSession>(std::move(created));
	bool cancelledBeforeStart{};
	bool closingBeforeStart{};
	bool terminalBeforeStart{};
	bool stoppedAfterCreation{};
	TaskExecutionOperationResult stoppedResult;
	{
		std::lock_guard lock(m_impl->mutex);
		stoppedAfterCreation = m_impl->stopped;
		if (stoppedAfterCreation) stoppedResult = m_impl->Current(ETaskExecutionOperationStatus::Stopped, ETaskExecutionOperationReason::None);
		else if (const auto found = m_impl->runs.find(runId); found == m_impl->runs.end() || IsTerminalTaskRunState(found->second.state)) terminalBeforeStart = true;
		else {
			found->second.session = session;
			cancelledBeforeStart = found->second.state == ETaskExecutionRunState::Cancelling;
			closingBeforeStart = found->second.state == ETaskExecutionRunState::Closing;
		}
	}
	if (stoppedAfterCreation) {
		const auto closeResult = Impl::CloseSession(session, m_impl->CloseDeadline());
		m_impl->FinalizeCloseAfterQuiescence(runId, closeResult);
		finishStartup();
		return stoppedResult;
	}
	if (terminalBeforeStart) {
		(void)Impl::CloseSession(session, m_impl->CloseDeadline());
		TaskExecutionOperationResult result;
		{
			std::lock_guard lock(m_impl->mutex);
			const auto found = m_impl->runs.find(runId);
			result = { ETaskExecutionOperationStatus::Started, ETaskExecutionOperationReason::None, m_impl->revision, runId,
				found == m_impl->runs.end() ? 0 : found->second.exitCode, found == m_impl->runs.end() ? L"task closed before session start" : found->second.diagnostic };
			m_impl->RememberLocked(request.operation.operationId, fingerprint, result);
		}
		finishStartup();
		return result;
	}
	if (cancelledBeforeStart) {
		m_impl->CompleteFromSession(runId, { ETaskSessionExitKind::Cancelled, 0, L"task cancelled before session start" });
		(void)Impl::CloseSession(session, m_impl->CloseDeadline());
		TaskExecutionOperationResult result;
		{
			std::lock_guard lock(m_impl->mutex);
			result = { ETaskExecutionOperationStatus::Started, ETaskExecutionOperationReason::None, m_impl->revision, runId };
			m_impl->RememberLocked(request.operation.operationId, fingerprint, result);
		}
		finishStartup();
		return result;
	}
	if (closingBeforeStart) {
		const auto closeResult = Impl::CloseSession(session, m_impl->CloseDeadline());
		m_impl->FinalizeCloseAfterQuiescence(runId, closeResult);
		TaskExecutionOperationResult startResult;
		{
			std::lock_guard lock(m_impl->mutex);
			const auto found = m_impl->runs.find(runId);
			if (found != m_impl->runs.end() && !found->second.closingOperationId.empty()) {
				TaskExecutionRunMutationRequest closeRequest { { found->second.closingOperationId, std::nullopt }, runId };
				(void)m_impl->CompleteCloseOperationLocked(closeRequest, found->second.closingFingerprint);
			}
			startResult = { ETaskExecutionOperationStatus::Started, ETaskExecutionOperationReason::None, m_impl->revision, runId };
			m_impl->RememberLocked(request.operation.operationId, fingerprint, startResult);
		}
		finishStartup();
		return startResult;
	}

	TaskTerminalLaunchRequest launch;
	launch.executionKind = request.definition.executionKind;
	launch.terminalLaunchOptions.workingDirectory = request.definition.workingDirectory.value_or(L"");
	launch.terminalLaunchOptions.initialSize = request.initialSize;
	if (request.definition.executionKind == ETaskExecutionKind::Process) {
		launch.terminalLaunchOptions.executablePath = request.definition.command;
		launch.terminalLaunchOptions.arguments = request.definition.arguments;
	}
	else {
		launch.shellCommand = request.definition.command;
		launch.shellArguments = request.definition.arguments;
	}
	TaskExecutionSessionStartResult startResult;
	try { startResult = session->Start(launch); }
	catch (...) { startResult = TaskExecutionSessionStartResult::Failure(0, L"task session start threw an exception"); }
	if (!startResult.succeeded) {
		m_impl->CompleteFromSession(runId, { ETaskSessionExitKind::Failed, startResult.errorCode, std::move(startResult.diagnostic) });
	}
	else {
		{
			std::lock_guard lock(m_impl->mutex);
			if (!m_impl->stopped) {
				if (const auto found = m_impl->runs.find(runId); found != m_impl->runs.end() && found->second.state == ETaskExecutionRunState::Starting) {
					found->second.state = ETaskExecutionRunState::Running;
					++m_impl->revision;
					m_impl->QueueNotificationLocked(found->second);
				}
			}
		}
		m_impl->DeliverNotifications();
	}
	TaskExecutionOperationResult result;
	{
		std::lock_guard lock(m_impl->mutex);
		result = { startResult.succeeded ? ETaskExecutionOperationStatus::Started : ETaskExecutionOperationStatus::Rejected,
			startResult.succeeded ? ETaskExecutionOperationReason::None : ETaskExecutionOperationReason::SessionStartFailed,
			m_impl->revision, runId, startResult.errorCode, BoundedDiagnostic(startResult.diagnostic) };
		m_impl->RememberLocked(request.operation.operationId, fingerprint, result);
		if (const auto found = m_impl->runs.find(runId); found != m_impl->runs.end()
			&& found->second.state == ETaskExecutionRunState::Failed && !found->second.closingOperationId.empty()) {
			TaskExecutionRunMutationRequest closeRequest { { found->second.closingOperationId, std::nullopt }, runId };
			(void)m_impl->CompleteCloseOperationLocked(closeRequest, found->second.closingFingerprint);
		}
	}
	finishStartup();
	return result;
}

TaskExecutionOperationResult TaskExecutionService::Cancel(const TaskExecutionRunMutationRequest& request)
{
	if (!IsValidOperationId(request.operation.operationId)) return { ETaskExecutionOperationStatus::Rejected, ETaskExecutionOperationReason::InvalidOperationId };
	const auto fingerprint = MutationFingerprint("cancel", request);
	std::shared_ptr<ITaskExecutionSession> session;
	TaskExecutionOperationResult result;
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->stopped) return m_impl->Current(ETaskExecutionOperationStatus::Stopped, ETaskExecutionOperationReason::None);
		if (const auto replay = m_impl->ReplayLocked(request.operation, fingerprint)) return *replay;
		if (request.operation.expectedRevision && *request.operation.expectedRevision != m_impl->revision) return m_impl->Current(ETaskExecutionOperationStatus::StaleRevision, ETaskExecutionOperationReason::ExpectedRevisionMismatch);
		const auto found = m_impl->runs.find(request.runId);
		if (found == m_impl->runs.end()) return m_impl->Current(ETaskExecutionOperationStatus::Rejected, ETaskExecutionOperationReason::RunNotFound);
		if (IsTerminalTaskRunState(found->second.state)) return m_impl->Current(ETaskExecutionOperationStatus::Rejected, ETaskExecutionOperationReason::RunAlreadyTerminal);
		if (found->second.state == ETaskExecutionRunState::Closing) return m_impl->Current(ETaskExecutionOperationStatus::Rejected, ETaskExecutionOperationReason::RunClosing);
		found->second.state = ETaskExecutionRunState::Cancelling;
		++m_impl->revision;
		m_impl->QueueNotificationLocked(found->second);
		session = found->second.session;
		result = { ETaskExecutionOperationStatus::Succeeded, ETaskExecutionOperationReason::None, m_impl->revision, request.runId };
		m_impl->RememberLocked(request.operation.operationId, fingerprint, result);
	}
	m_impl->DeliverNotifications();
	if (session) { try { session->RequestCancel(); } catch (...) {} }
	ProcessDeferredStop();
	return result;
}

TaskExecutionOperationResult TaskExecutionService::Close(const TaskExecutionRunMutationRequest& request)
{
	if (!IsValidOperationId(request.operation.operationId)) return { ETaskExecutionOperationStatus::Rejected, ETaskExecutionOperationReason::InvalidOperationId };
	const auto fingerprint = MutationFingerprint("close", request);
	std::shared_ptr<ITaskExecutionSession> session;
	{
		std::lock_guard lock(m_impl->mutex);
		if (m_impl->stopped) return m_impl->Current(ETaskExecutionOperationStatus::Stopped, ETaskExecutionOperationReason::None);
		if (const auto replay = m_impl->ReplayLocked(request.operation, fingerprint)) return *replay;
		const auto found = m_impl->runs.find(request.runId);
		if (found == m_impl->runs.end()) return m_impl->Current(ETaskExecutionOperationStatus::Rejected, ETaskExecutionOperationReason::RunNotFound);
		if (found->second.state == ETaskExecutionRunState::Closing) {
			if (found->second.closingOperationId == request.operation.operationId && found->second.closingFingerprint == fingerprint) {
				return { ETaskExecutionOperationStatus::Started, ETaskExecutionOperationReason::SessionClosePending, m_impl->revision, request.runId };
			}
			return m_impl->Current(ETaskExecutionOperationStatus::Rejected, ETaskExecutionOperationReason::RunClosing);
		}
		if (request.operation.expectedRevision && *request.operation.expectedRevision != m_impl->revision) return m_impl->Current(ETaskExecutionOperationStatus::StaleRevision, ETaskExecutionOperationReason::ExpectedRevisionMismatch);
		if (IsTerminalTaskRunState(found->second.state)) return m_impl->Current(ETaskExecutionOperationStatus::Rejected, ETaskExecutionOperationReason::RunAlreadyTerminal);
		found->second.state = ETaskExecutionRunState::Closing;
		found->second.closingOperationId = request.operation.operationId;
		found->second.closingFingerprint = fingerprint;
		++m_impl->revision;
		m_impl->QueueNotificationLocked(found->second);
		session = found->second.session;
	}
	m_impl->DeliverNotifications();
	if (!session) {
		TaskExecutionOperationResult pending;
		{
			std::lock_guard lock(m_impl->mutex);
			pending = { ETaskExecutionOperationStatus::Started, ETaskExecutionOperationReason::SessionClosePending, m_impl->revision, request.runId };
		}
		ProcessDeferredStop();
		return pending;
	}
	const auto closeResult = Impl::CloseSession(session, std::chrono::steady_clock::now() + m_impl->limits.sessionCloseTimeout);
	m_impl->FinalizeCloseAfterQuiescence(request.runId, closeResult);
	TaskExecutionOperationResult result;
	{
		std::lock_guard lock(m_impl->mutex);
		result = m_impl->CompleteCloseOperationLocked(request, fingerprint);
	}
	ProcessDeferredStop();
	return result;
}

TaskExecutionOperationResult TaskExecutionService::Impl::Stop()
{
	TaskExecutionOperationResult result;
	std::chrono::steady_clock::time_point deadline;
	bool unownedSessionFailure{};
	{
		std::unique_lock lock(mutex);
		if (s_notificationDispatchingImpl == this) {
			if (stopLifecycle == EStopLifecycle::Active) dispatcherDeferredStopRequested = true;
			return Current(ETaskExecutionOperationStatus::Deferred, ETaskExecutionOperationReason::StopInProgress);
		}
		if (stopLifecycle == EStopLifecycle::Stopped) return completedStopResult;
		if (stopLifecycle == EStopLifecycle::Stopping) {
			if (stopOwnerThread == std::this_thread::get_id()) {
				return Current(ETaskExecutionOperationStatus::Deferred, ETaskExecutionOperationReason::StopInProgress);
			}
			stopCondition.wait(lock, [this] { return stopLifecycle == EStopLifecycle::Stopped; });
			return completedStopResult;
		}
		stopLifecycle = EStopLifecycle::Stopping;
		stopOwnerThread = std::this_thread::get_id();
		stopped = true;
		deadline = std::chrono::steady_clock::now() + limits.sessionCloseTimeout;
		stopDeadline = deadline;
		for (auto& [id, run] : runs) {
			if (IsTerminalTaskRunState(run.state)) continue;
			run.state = ETaskExecutionRunState::Closing;
			if (revision != std::numeric_limits<std::uint64_t>::max()) ++revision;
			QueueNotificationLocked(run);
			if (!run.session && !run.startupInFlight) {
				run.state = ETaskExecutionRunState::Failed;
				run.diagnostic.clear();
				run.closeFailureReason = ETaskExecutionOperationReason::SessionCloseHostFailure;
				if (activeRunCount != 0) --activeRunCount;
				if (revision != std::numeric_limits<std::uint64_t>::max()) ++revision;
				QueueNotificationLocked(run);
				unownedSessionFailure = true;
			}
		}
		factory.reset();
		completedOperations.clear();
		completedOperationOrder.clear();
		PruneTerminalRunsLocked();
		result = { ETaskExecutionOperationStatus::Succeeded, ETaskExecutionOperationReason::None, revision };
	}
	struct StopPublicationScope final {
		Impl& impl;
		TaskExecutionOperationResult& result;
		bool active{ true };
		~StopPublicationScope() { if (active) impl.PublishStopResultNoexcept(result); }
		void Publish() noexcept
		{
			if (!active) return;
			impl.PublishStopResultNoexcept(result);
			active = false;
		}
	} publication { *this, result };

	try {
		DeliverNotifications();
		ETaskExecutionOperationReason failureReason = unownedSessionFailure
			? ETaskExecutionOperationReason::SessionCloseHostFailure : ETaskExecutionOperationReason::None;
		std::uint32_t failureCode{};
		// Phase 1: mark and invoke every published session before waiting on any one.
		for (;;) {
		std::uint64_t runId{};
		std::shared_ptr<ITaskExecutionSession> session;
		{
			std::lock_guard lock(mutex);
			const auto found = std::find_if(runs.begin(), runs.end(), [](const auto& entry) {
				return !IsTerminalTaskRunState(entry.second.state) && entry.second.session && !entry.second.stopCloseInitiated;
			});
			if (found == runs.end()) break;
			runId = found->first;
			found->second.stopCloseInitiated = true;
			session = found->second.session;
		}
		session->BeginClose();
		}
		// Phase 2: every wait receives the same absolute deadline, bounding total stop latency.
		for (;;) {
		std::uint64_t runId{};
		std::shared_ptr<ITaskExecutionSession> session;
		{
			std::lock_guard lock(mutex);
			const auto found = std::find_if(runs.begin(), runs.end(), [](const auto& entry) {
				return !IsTerminalTaskRunState(entry.second.state) && entry.second.session
					&& entry.second.stopCloseInitiated && !entry.second.stopCloseWaited;
			});
			if (found == runs.end()) break;
			runId = found->first;
			found->second.stopCloseWaited = true;
			session = found->second.session;
		}
		const auto closeResult = session->WaitForClose(deadline);
		FinalizeCloseAfterQuiescence(runId, closeResult);
		if (closeResult.kind != ETaskExecutionSessionCloseKind::Closed && failureReason == ETaskExecutionOperationReason::None) {
			failureReason = closeResult.kind == ETaskExecutionSessionCloseKind::TimedOut
				? ETaskExecutionOperationReason::SessionCloseTimedOut : ETaskExecutionOperationReason::SessionCloseHostFailure;
			failureCode = closeResult.errorCode;
		}
		}
		bool startupFinished{};
		{
		std::unique_lock lock(mutex);
		startupFinished = startupCondition.wait_until(lock, deadline, [this] { return inFlightStartCount == 0; });
		}
		if (!startupFinished && failureReason == ETaskExecutionOperationReason::None) {
			failureReason = ETaskExecutionOperationReason::SessionCloseTimedOut;
		}
		DrainNotificationsForStop();
		if (failureReason != ETaskExecutionOperationReason::None) {
			result.status = ETaskExecutionOperationStatus::Rejected;
			result.reason = failureReason;
			result.errorCode = failureCode;
		}
	}
	catch (...) {
		// Publication remains owned by this scope even if an implementation detail
		// (for example a condition-variable failure) aborts the shutdown walk.
		result = { ETaskExecutionOperationStatus::Rejected, ETaskExecutionOperationReason::SessionCloseHostFailure };
	}
	publication.Publish();
	return result;
}

TaskExecutionOperationResult TaskExecutionService::Stop()
{
	return m_impl->Stop();
}

TaskExecutionServiceSnapshot TaskExecutionService::Snapshot() const
{
	std::lock_guard lock(m_impl->mutex);
	TaskExecutionServiceSnapshot snapshot { m_impl->revision, m_impl->stopped, m_impl->droppedNotificationCount, {} };
	snapshot.runs.reserve(m_impl->runs.size());
	for (const auto& [id, run] : m_impl->runs) snapshot.runs.push_back({ id, run.definition.label, run.definition.executionKind, run.state, run.exitCode, run.diagnostic });
	return snapshot;
}

std::optional<TaskExecutionSubscriptionId> TaskExecutionService::Subscribe(TaskExecutionListener listener)
{
	if (!listener) return std::nullopt;
	std::lock_guard lock(m_impl->mutex);
	if (m_impl->stopped || m_impl->subscriptions.size() >= m_impl->limits.maximumSubscriptions || m_impl->nextSubscriptionId == std::numeric_limits<TaskExecutionSubscriptionId>::max()) return std::nullopt;
	const auto id = m_impl->nextSubscriptionId++;
	m_impl->subscriptions.emplace(id, std::move(listener));
	return id;
}

void TaskExecutionService::Unsubscribe(const TaskExecutionSubscriptionId subscriptionId) noexcept
{
	std::lock_guard lock(m_impl->mutex);
	m_impl->subscriptions.erase(subscriptionId);
}

} // namespace workbench::tasks
