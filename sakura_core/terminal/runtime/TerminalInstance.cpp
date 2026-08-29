/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "terminal/runtime/TerminalInstance.h"

#include "terminal/input/SakuraTerminalInputAdapter.h"
#include "terminal/model/TerminalModel.h"
#include "terminal/parser/TerminalParser.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <mutex>
#include <utility>

namespace terminal {
namespace {

constexpr wchar_t kDefaultProcessName[] = L"PowerShell";

std::wstring InitialProcessName(const TerminalLaunchOptions& options)
{
	try {
		const std::filesystem::path executable(options.executablePath);
		const auto stem = executable.stem().wstring();
		return stem.empty() ? std::wstring(kDefaultProcessName) : stem;
	} catch (...) {
		return kDefaultProcessName;
	}
}

template<typename Callback, typename... Args>
void InvokeNoThrow(const Callback& callback, Args&&... args) noexcept
{
	if (!callback) return;
	try {
		callback(std::forward<Args>(args)...);
	} catch (...) {
		// Runtime notifications are advisory and must not unwind a session worker.
	}
}

bool CanTransition(const TerminalInstanceState from, const TerminalInstanceState to) noexcept
{
	if (from == to) return true;
	switch (from) {
	case TerminalInstanceState::Reserved:
		return to == TerminalInstanceState::Starting || to == TerminalInstanceState::Closing
			|| to == TerminalInstanceState::Terminalized;
	case TerminalInstanceState::Starting:
		return to == TerminalInstanceState::Running || to == TerminalInstanceState::Closing
			|| to == TerminalInstanceState::Terminalized;
	case TerminalInstanceState::Running:
		return to == TerminalInstanceState::Closing || to == TerminalInstanceState::Terminalized;
	case TerminalInstanceState::Closing:
		return to == TerminalInstanceState::Terminalized;
	case TerminalInstanceState::Terminalized:
		return to == TerminalInstanceState::Retired;
	case TerminalInstanceState::Retired:
		return false;
	}
	return false;
}

void AdvanceContentRevision(TerminalContentRevision& revision) noexcept
{
	if (revision.value != (std::numeric_limits<std::uint64_t>::max)()) ++revision.value;
}

TerminalInstanceOutcomeKind OutcomeForClose(
	const TerminalInstanceCloseReason reason,
	const TerminalSessionCompletionKind completion) noexcept
{
	if (completion == TerminalSessionCompletionKind::Failed) {
		return reason == TerminalInstanceCloseReason::HostLost
			? TerminalInstanceOutcomeKind::HostLost
			: TerminalInstanceOutcomeKind::Failed;
	}
	if (completion == TerminalSessionCompletionKind::Exited) return TerminalInstanceOutcomeKind::Exited;
	switch (reason) {
	case TerminalInstanceCloseReason::Cancel:
		return TerminalInstanceOutcomeKind::Cancelled;
	case TerminalInstanceCloseReason::HostLost:
		return TerminalInstanceOutcomeKind::HostLost;
	case TerminalInstanceCloseReason::Explicit:
	case TerminalInstanceCloseReason::Shutdown:
	case TerminalInstanceCloseReason::None:
		return TerminalInstanceOutcomeKind::Closed;
	}
	return TerminalInstanceOutcomeKind::Failed;
}

} // namespace

struct TerminalInstance::Impl {
	Impl(
		TerminalInstanceId instanceIdValue,
		TerminalRuntimeGeneration runtimeGenerationValue,
		const std::uint64_t instanceGenerationValue,
		TerminalCreateRequest requestValue,
		TerminalInstanceDependencies dependenciesValue,
		TerminalInstanceEventCallback eventCallbackValue)
		: coordinate()
		, origin(requestValue.origin)
		, environmentPolicy(requestValue.environmentPolicy)
		, taskRunId(requestValue.taskRunId)
		, request(std::move(requestValue))
		, dependencies(std::move(dependenciesValue))
		, eventCallback(std::move(eventCallbackValue))
		, instanceGeneration(instanceGenerationValue)
		, input(std::make_unique<SakuraTerminalInputAdapter>())
		, model(std::make_unique<TerminalModel>(request.launch.initialSize.columns, request.launch.initialSize.rows))
	{
		coordinate = dependencies.coordinateBase;
		coordinate.runtimeGeneration = runtimeGenerationValue;
		coordinate.instanceGeneration = instanceGenerationValue;
		coordinate.instanceId = instanceIdValue;
		coordinate.sessionId = request.sessionId;
		if (request.windowId) coordinate.windowId = *request.windowId;
		coordinate.paneId = request.paneId;
		processName = InitialProcessName(request.launch);
		profileLabel = processName;
		initialWorkingDirectory = request.launch.workingDirectory;
	}

	TerminalInstanceId Id() const noexcept { return coordinate.instanceId; }
	TerminalRuntimeGeneration RuntimeGeneration() const noexcept { return coordinate.runtimeGeneration; }

	void Publish(
		const TerminalInstanceEventKind kind,
		std::optional<TerminalInstanceOutcome> eventOutcome = std::nullopt) noexcept
	{
		try {
			TerminalInstanceEvent event;
			TerminalInstanceEventCallback callback;
			{
				const std::lock_guard lock(stateMutex);
				if (state == TerminalInstanceState::Retired) return;
				event.kind = kind;
				event.coordinate = coordinate;
				event.state = state;
				event.sessionState = sessionStateValue;
				event.errorCode = errorCode;
				event.outcome = std::move(eventOutcome);
				callback = eventCallback;
			}
			InvokeNoThrow(callback, event);
		} catch (...) {
			// Allocation/copy failure cannot make lifecycle ownership disappear.
		}
	}

	void PublishOutcome(TerminalInstanceOutcome value) noexcept
	{
		bool publish = false;
		{
			const std::lock_guard lock(stateMutex);
			if (!outcomePublished) {
				if (!CanTransition(state, TerminalInstanceState::Terminalized)) return;
				state = TerminalInstanceState::Terminalized;
				outcome = value;
				outcomePublished = true;
				publish = true;
			}
		}
		if (!publish) return;
		Publish(TerminalInstanceEventKind::StateChanged);
		Publish(TerminalInstanceEventKind::Completed, std::move(value));
	}

	void OnOutputAvailable() noexcept
	{
		{
			const std::lock_guard lock(stateMutex);
			if (outcomePublished || state == TerminalInstanceState::Retired) return;
		}
		Publish(TerminalInstanceEventKind::OutputAvailable);
	}

	void OnSessionStateChanged(const TerminalSessionState next, const std::uint32_t nextError) noexcept
	{
		bool changed = false;
		{
			const std::lock_guard lock(stateMutex);
			if (outcomePublished || state == TerminalInstanceState::Retired) return;
			if (sessionStateValue != next || errorCode != nextError) changed = true;
			sessionStateValue = next;
			errorCode = nextError;
			switch (next) {
			case TerminalSessionState::Starting:
				if (state == TerminalInstanceState::Reserved) {
					state = TerminalInstanceState::Starting;
					changed = true;
				}
				break;
			case TerminalSessionState::Running:
				if (state == TerminalInstanceState::Starting && !closeRequested) {
					state = TerminalInstanceState::Running;
					changed = true;
				} else if (closeRequested && state == TerminalInstanceState::Starting) {
					state = TerminalInstanceState::Closing;
					changed = true;
				}
				break;
			case TerminalSessionState::Closing:
				if (state == TerminalInstanceState::Starting || state == TerminalInstanceState::Running) {
					state = TerminalInstanceState::Closing;
					changed = true;
				}
				break;
			case TerminalSessionState::Exited:
			case TerminalSessionState::Failed:
				// CTerminalSession delivers completion only after workers quiesce. Keep
				// the instance in Closing until that durable callback arrives.
				if (state == TerminalInstanceState::Starting || state == TerminalInstanceState::Running) {
					state = TerminalInstanceState::Closing;
					changed = true;
				}
				break;
			case TerminalSessionState::Idle:
				break;
			}
		}
		if (changed) Publish(TerminalInstanceEventKind::StateChanged);
	}

	void OnSessionCompleted(const TerminalSessionCompletionResult completion) noexcept
	{
		TerminalInstanceOutcome value;
		{
			const std::lock_guard lock(stateMutex);
			if (outcomePublished || state == TerminalInstanceState::Retired) return;
			sessionStateValue = completion.kind == TerminalSessionCompletionKind::Failed
				? TerminalSessionState::Failed : TerminalSessionState::Exited;
			errorCode = completion.errorCode;
			value.kind = OutcomeForClose(closeReason, completion.kind);
			if (completion.kind != TerminalSessionCompletionKind::Failed) {
				value.processExitCode = completion.exitCode;
			}
			if (completion.errorCode != 0) value.platformErrorCode = completion.errorCode;
			value.backendQuiesced = true;
			value.readerQuiesced = true;
			value.writerQuiesced = true;
			value.finalContentRevision = contentRevision;
		}
		PublishOutcome(std::move(value));
	}

	void RequestClose(const TerminalInstanceCloseReason reason) noexcept
	{
		CTerminalSession* sessionToClose = nullptr;
		bool publishState = false;
		bool publishStartCancelled = false;
		{
			const std::lock_guard lock(stateMutex);
			if (outcomePublished || state == TerminalInstanceState::Retired) return;
			closeRequested = true;
			if (closeReason == TerminalInstanceCloseReason::None || reason == TerminalInstanceCloseReason::HostLost
				|| reason == TerminalInstanceCloseReason::Shutdown) {
				closeReason = reason;
			}
			if (state == TerminalInstanceState::Reserved && !startInProgress && !startAttempted) {
				publishStartCancelled = true;
			} else if ((state == TerminalInstanceState::Starting || state == TerminalInstanceState::Running)
				&& CanTransition(state, TerminalInstanceState::Closing)) {
				state = TerminalInstanceState::Closing;
				publishState = true;
			}
			sessionToClose = session.get();
		}
		if (publishStartCancelled) {
			TerminalInstanceOutcome outcomeValue;
			outcomeValue.kind = TerminalInstanceOutcomeKind::StartCancelled;
			outcomeValue.backendQuiesced = true;
			outcomeValue.readerQuiesced = true;
			outcomeValue.writerQuiesced = true;
			{
				const std::lock_guard lock(stateMutex);
				outcomeValue.finalContentRevision = contentRevision;
			}
			PublishOutcome(std::move(outcomeValue));
			return;
		}
		if (publishState) Publish(TerminalInstanceEventKind::StateChanged);
		if (sessionToClose != nullptr) sessionToClose->BeginClose();
	}

	TerminalInstanceStartResult Start(std::optional<TerminalLaunchOptions> overrideOptions)
	{
		TerminalLaunchOptions options;
		TerminalRuntimeSessionFactory createSession;
		bool cancelBeforeFactory = false;
		{
			const std::lock_guard lock(stateMutex);
			if (state == TerminalInstanceState::Terminalized && outcome && outcome->kind == TerminalInstanceOutcomeKind::StartCancelled) {
				return { TerminalInstanceStartStatus::StartCancelled, ERROR_CANCELLED, L"Terminal instance start was cancelled." };
			}
			if (state != TerminalInstanceState::Reserved || startAttempted || startInProgress) {
				return { TerminalInstanceStartStatus::AlreadyStarted, ERROR_INVALID_STATE, L"Terminal instance has already started or closed." };
			}
			startAttempted = true;
			if (closeRequested) {
				startInProgress = false;
				cancelBeforeFactory = true;
			} else {
				startInProgress = true;
				state = TerminalInstanceState::Starting;
				options = overrideOptions ? std::move(*overrideOptions) : request.launch;
				createSession = dependencies.createSession;
			}
		}
		if (cancelBeforeFactory) {
			TerminalInstanceOutcome outcomeValue;
			outcomeValue.kind = TerminalInstanceOutcomeKind::StartCancelled;
			outcomeValue.backendQuiesced = true;
			outcomeValue.readerQuiesced = true;
			outcomeValue.writerQuiesced = true;
			{
				const std::lock_guard lock(stateMutex);
				outcomeValue.finalContentRevision = contentRevision;
			}
			PublishOutcome(std::move(outcomeValue));
			return { TerminalInstanceStartStatus::StartCancelled, ERROR_CANCELLED, L"Terminal instance start was cancelled." };
		}

		Publish(TerminalInstanceEventKind::StateChanged);
		bool cancelAfterStateNotification = false;
		{
			const std::lock_guard lock(stateMutex);
			if (closeRequested) {
				startInProgress = false;
				cancelAfterStateNotification = true;
			}
		}
		if (cancelAfterStateNotification) {
			return FinishStartFailure(ERROR_CANCELLED, L"Terminal instance start was cancelled.", true);
		}
		if (!createSession) return FinishStartFailure(ERROR_INVALID_FUNCTION, L"No terminal session factory is available.", false);

		const std::weak_ptr<Impl> weak = weakSelf;
		TerminalSessionCallbacks callbacks;
		callbacks.outputAvailable = [weak] {
			if (const auto current = weak.lock()) current->OnOutputAvailable();
		};
		callbacks.stateChanged = [weak](const TerminalSessionState stateValue, const std::uint32_t error) {
			if (const auto current = weak.lock()) current->OnSessionStateChanged(stateValue, error);
		};
		callbacks.completed = [weak](const TerminalSessionCompletionResult completion) {
			if (const auto current = weak.lock()) current->OnSessionCompleted(completion);
		};

		std::unique_ptr<CTerminalSession> created;
		try {
			created = createSession(std::move(callbacks));
		} catch (...) {
			return FinishStartFailure(ERROR_UNHANDLED_EXCEPTION, L"Terminal session factory raised an exception.", false);
		}
		if (!created) return FinishStartFailure(ERROR_NOT_ENOUGH_MEMORY, L"Terminal session factory returned no session.", false);

		CTerminalSession* sessionToStart = nullptr;
		bool cancelBeforeStart = false;
		bool alreadyOwned = false;
		{
			const std::lock_guard lock(stateMutex);
			if (session) {
				alreadyOwned = true;
			} else {
				session = std::move(created);
				sessionToStart = session.get();
				cancelBeforeStart = closeRequested;
			}
		}
		if (alreadyOwned) {
			return FinishStartFailure(ERROR_INVALID_STATE, L"Terminal instance already owns a session.", false);
		}
		if (cancelBeforeStart) {
			sessionToStart->BeginClose();
			return FinishStartFailure(ERROR_CANCELLED, L"Terminal instance start was cancelled.", true);
		}

		TerminalStartResult startResult;
		try {
			startResult = sessionToStart->Start(options);
		} catch (...) {
			startResult = TerminalStartResult::Failure(ERROR_UNHANDLED_EXCEPTION, L"Terminal session start raised an exception.");
		}
		const auto sessionState = sessionToStart->GetState();
		bool closeAfterStart = false;
		{
			const std::lock_guard lock(stateMutex);
			startInProgress = false;
			sessionStateValue = sessionState;
			closeAfterStart = closeRequested;
		}

		if (startResult.succeeded && !closeAfterStart && sessionState == TerminalSessionState::Running) {
			{
				const std::lock_guard lock(stateMutex);
				if (!outcomePublished && state == TerminalInstanceState::Starting) state = TerminalInstanceState::Running;
			}
			Publish(TerminalInstanceEventKind::StateChanged);
			return { TerminalInstanceStartStatus::Started, 0, {} };
		}

		if (closeAfterStart || startResult.errorCode == ERROR_CANCELLED
			|| sessionState == TerminalSessionState::Closing || sessionState == TerminalSessionState::Exited) {
			sessionToStart->BeginClose();
			if (sessionState == TerminalSessionState::Exited) {
				return FinishStartFailure(ERROR_CANCELLED, L"Terminal instance start was cancelled.", true);
			}
			return { TerminalInstanceStartStatus::StartCancelled,
				startResult.errorCode == 0 ? ERROR_CANCELLED : startResult.errorCode,
				startResult.diagnostic.empty() ? L"Terminal instance start was cancelled." : startResult.diagnostic };
		}

		sessionToStart->BeginClose();
		return FinishStartFailure(
			startResult.errorCode == 0 ? ERROR_GEN_FAILURE : startResult.errorCode,
			startResult.diagnostic.empty() ? L"Terminal session start failed." : startResult.diagnostic,
			false);
	}

	TerminalInstanceStartResult FinishStartFailure(
		const std::uint32_t failureCode,
		std::wstring diagnostic,
		const bool cancelled)
	{
		{
			const std::lock_guard lock(stateMutex);
			startInProgress = false;
			sessionStateValue = cancelled ? sessionStateValue : TerminalSessionState::Failed;
			errorCode = failureCode;
		}
		TerminalInstanceOutcome outcomeValue;
		outcomeValue.kind = cancelled ? TerminalInstanceOutcomeKind::StartCancelled : TerminalInstanceOutcomeKind::StartFailed;
		if (failureCode != 0) outcomeValue.platformErrorCode = failureCode;
		outcomeValue.backendQuiesced = true;
		outcomeValue.readerQuiesced = true;
		outcomeValue.writerQuiesced = true;
		{
			const std::lock_guard lock(stateMutex);
			outcomeValue.finalContentRevision = contentRevision;
		}
		PublishOutcome(std::move(outcomeValue));
		return { cancelled ? TerminalInstanceStartStatus::StartCancelled : TerminalInstanceStartStatus::StartFailed,
			failureCode, std::move(diagnostic) };
	}

	TerminalQueueInputResult QueueProtocolInput(std::span<const std::uint8_t> bytes) noexcept
	{
		if (bytes.empty()) return TerminalQueueInputResult::Accepted;
		bool publishRejected = false;
		TerminalQueueInputResult result = TerminalQueueInputResult::NotRunning;
		{
			const std::lock_guard lock(stateMutex);
			if (!session || outcomePublished || sessionStateValue != TerminalSessionState::Running) return result;
			if (pendingProtocolInput.empty()) {
				result = session->QueueInput(bytes, TerminalInputSource::Protocol);
				if (result != TerminalQueueInputResult::QueueFull) return result;
			}
			if (bytes.size() > CTerminalSession::kInputLimitBytes - pendingProtocolInput.size()) {
				protocolInputRejected = true;
				errorCode = ERROR_BUFFER_OVERFLOW;
				publishRejected = true;
				result = TerminalQueueInputResult::QueueFull;
			} else {
				try {
					pendingProtocolInput.insert(pendingProtocolInput.end(), bytes.begin(), bytes.end());
				} catch (...) {
					protocolInputRejected = true;
					errorCode = ERROR_NOT_ENOUGH_MEMORY;
					publishRejected = true;
					result = TerminalQueueInputResult::QueueFull;
				}
			}
		}
		if (publishRejected) Publish(TerminalInstanceEventKind::StateChanged);
		return result;
	}

	TerminalQueueInputResult FlushPendingProtocolInput() noexcept
	{
		bool publishState = false;
		TerminalQueueInputResult result = TerminalQueueInputResult::Accepted;
		{
			const std::lock_guard lock(stateMutex);
			if (pendingProtocolInput.empty()) return result;
			if (!session || outcomePublished) {
				pendingProtocolInput.clear();
				protocolInputRejected = true;
				errorCode = ERROR_OPERATION_ABORTED;
				return TerminalQueueInputResult::NotRunning;
			}
			result = session->QueueInput(pendingProtocolInput, TerminalInputSource::Protocol);
			if (result == TerminalQueueInputResult::Accepted) {
				pendingProtocolInput.clear();
				if (protocolInputRejected || errorCode == ERROR_BUFFER_OVERFLOW) publishState = true;
				protocolInputRejected = false;
				if (errorCode == ERROR_BUFFER_OVERFLOW) errorCode = 0;
			} else if (result == TerminalQueueInputResult::NotRunning) {
				pendingProtocolInput.clear();
				protocolInputRejected = true;
				errorCode = ERROR_OPERATION_ABORTED;
				publishState = true;
			}
		}
		if (publishState) Publish(TerminalInstanceEventKind::StateChanged);
		return result;
	}

	TerminalInstanceDrainResult DrainOutput()
	{
		TerminalInstanceDrainResult result;
		CTerminalSession* sessionValue = nullptr;
		{
			const std::lock_guard lock(stateMutex);
			if (!session || outcomePublished) return result;
			sessionValue = session.get();
			result.sessionRunning = sessionStateValue == TerminalSessionState::Running;
		}
		result.found = true;
		static_cast<void>(FlushPendingProtocolInput());
		const auto bytes = sessionValue->DrainOutput();
		result.bytesDrained = bytes.size();
		bool parserFailed = false;
		std::wstring beforeTitle;
		std::uint64_t beforeSynchronizedCommit = 0;
		{
			const std::lock_guard lock(modelMutex);
			beforeTitle = model->Title();
			beforeSynchronizedCommit = model->SynchronizedOutputCommitGeneration();
			if (!bytes.empty()) {
				try {
					parser->Feed(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
				} catch (...) {
					parserFailed = true;
				}
			}
			result.synchronizedOutputCommitted =
				model->SynchronizedOutputCommitGeneration() != beforeSynchronizedCommit;
			const auto currentTitle = model->Title();
			if (currentTitle != beforeTitle) {
				result.sequenceChanged = true;
				beforeTitle = currentTitle;
			}
			result.scrollbackChange = model->ConsumeScrollbackChange();
			result.dirtyRows = model->ConsumeDirtyRows();
		}
		if (result.sequenceChanged) {
			const std::lock_guard lock(stateMutex);
			sequenceTitle = beforeTitle;
		}
		static_cast<void>(FlushPendingProtocolInput());
		{
			const std::lock_guard lock(stateMutex);
			result.protocolInputPending = !pendingProtocolInput.empty();
			result.protocolInputRejected = protocolInputRejected;
			if (!result.dirtyRows.empty() || result.scrollbackChange.Changed() || result.synchronizedOutputCommitted) {
				AdvanceContentRevision(contentRevision);
			}
			result.contentRevision = contentRevision;
		}
		if (parserFailed) {
			RequestClose(TerminalInstanceCloseReason::HostLost);
		}
		const auto snapshot = Snapshot();
		sessionValue->RecordModelDiagnostic({
			.bytesDrained = result.bytesDrained,
			.scrollbackAppended = result.scrollbackChange.Appended(),
			.scrollbackEvicted = result.scrollbackChange.Evicted(),
			.scrollbackRows = snapshot.scrollbackSize,
			.scrollbackLimit = snapshot.scrollbackLimit,
			.dirtyRows = result.dirtyRows.size(),
			.columns = snapshot.columns,
			.rows = snapshot.rows,
			.scrollbackCleared = result.scrollbackChange.Cleared(),
			.protocolInputPending = result.protocolInputPending,
			.protocolInputRejected = result.protocolInputRejected,
			.synchronizedOutputCommitted = result.synchronizedOutputCommitted,
			.alternateScreen = snapshot.alternateScreen,
		});
		return result;
	}

	TerminalInstanceResizeResult Resize(const TerminalSize size)
	{
		if (size.columns == 0 || size.rows == 0) return { false, ERROR_INVALID_PARAMETER };
		CTerminalSession* sessionValue = nullptr;
		{
			const std::lock_guard lock(stateMutex);
			if (!session || outcomePublished || sessionStateValue != TerminalSessionState::Running) {
				return { false, ERROR_INVALID_STATE };
			}
			sessionValue = session.get();
		}
		{
			const std::lock_guard lock(modelMutex);
			model->Resize(size.columns, size.rows);
		}
		if (!sessionValue->RequestResize(size)) return { false, ERROR_INVALID_STATE };
		{
			const std::lock_guard lock(stateMutex);
			AdvanceContentRevision(contentRevision);
		}
		return { true, 0 };
	}

	TerminalInstanceSnapshot Snapshot() const
	{
		TerminalInstanceSnapshot result;
		{
			const std::lock_guard lock(stateMutex);
			result.coordinate = coordinate;
			result.origin = origin;
			result.environmentPolicy = environmentPolicy;
			result.taskRunId = taskRunId;
			result.state = state;
			result.sessionState = sessionStateValue;
			result.errorCode = errorCode;
			result.contentRevision = contentRevision;
			result.processName = processName;
			result.profileLabel = profileLabel;
			result.sequenceTitle = sequenceTitle;
			result.initialWorkingDirectory = initialWorkingDirectory;
			result.outcome = outcome;
		}
		{
			const std::lock_guard lock(modelMutex);
			result.columns = model->Columns();
			result.rows = model->RowCount();
			result.scrollbackSize = model->ScrollbackSize();
			result.scrollbackLimit = model->ScrollbackLimit();
			result.alternateScreen = model->IsAlternateScreen();
		}
		return result;
	}

	TerminalInstanceCloseWaitResult WaitForClose(const std::chrono::steady_clock::time_point deadline) noexcept
	{
		RequestClose(TerminalInstanceCloseReason::Explicit);
		CTerminalSession* sessionValue = nullptr;
		{
			const std::lock_guard lock(stateMutex);
			if (outcomePublished) return { TerminalInstanceCloseWaitStatus::Closed, outcome };
			if (startInProgress) return { TerminalInstanceCloseWaitStatus::InProgress, outcome };
			if (!session) return { TerminalInstanceCloseWaitStatus::InProgress, outcome };
			sessionValue = session.get();
		}
		const auto waited = sessionValue->WaitForClose(deadline);
		if (waited.status == TerminalSessionCloseWaitStatus::InProgress) {
			return { TerminalInstanceCloseWaitStatus::InProgress, Outcome() };
		}
		if (!HasOutcome()) {
			TerminalInstanceOutcome value;
			const auto sessionState = sessionValue->GetState();
			{
				const std::lock_guard lock(stateMutex);
				value.kind = waited.status == TerminalSessionCloseWaitStatus::DeadlineExceeded
					&& closeReason == TerminalInstanceCloseReason::Shutdown
					? TerminalInstanceOutcomeKind::ShutdownDeadlineExceeded
					: sessionState == TerminalSessionState::Failed
					? TerminalInstanceOutcomeKind::Failed
					: closeReason == TerminalInstanceCloseReason::Cancel
					? TerminalInstanceOutcomeKind::Cancelled
					: closeReason == TerminalInstanceCloseReason::HostLost
					? TerminalInstanceOutcomeKind::HostLost
					: TerminalInstanceOutcomeKind::Closed;
				value.backendQuiesced = true;
				value.readerQuiesced = true;
				value.writerQuiesced = true;
				value.finalContentRevision = contentRevision;
			}
			PublishOutcome(std::move(value));
		}
		const auto status = waited.status == TerminalSessionCloseWaitStatus::DeadlineExceeded
			? TerminalInstanceCloseWaitStatus::DeadlineExceeded : TerminalInstanceCloseWaitStatus::Closed;
		return { status, Outcome() };
	}

	bool HasOutcome() const noexcept
	{
		const std::lock_guard lock(stateMutex);
		return outcomePublished;
	}

	std::optional<TerminalInstanceOutcome> Outcome() const
	{
		const std::lock_guard lock(stateMutex);
		return outcome;
	}

	std::optional<TerminalBackendProcessIdentity> GetProcessIdentity() const noexcept
	{
		CTerminalSession* sessionValue = nullptr;
		{
			const std::lock_guard lock(stateMutex);
			if (!session || outcomePublished
				|| sessionStateValue == TerminalSessionState::Idle
				|| sessionStateValue == TerminalSessionState::Exited
				|| sessionStateValue == TerminalSessionState::Failed) {
				return std::nullopt;
			}
			sessionValue = session.get();
		}
		try {
			return sessionValue->GetProcessIdentity();
		} catch (...) {
			return std::nullopt;
		}
	}

	bool OwnsProcess(
		const std::uint32_t processId, const std::uint64_t creationTime) const noexcept
	{
		if (processId == 0 || creationTime == 0) return false;
		CTerminalSession* sessionValue = nullptr;
		{
			const std::lock_guard lock(stateMutex);
			if (!session || outcomePublished
				|| sessionStateValue == TerminalSessionState::Idle
				|| sessionStateValue == TerminalSessionState::Exited
				|| sessionStateValue == TerminalSessionState::Failed) {
				return false;
			}
			sessionValue = session.get();
		}
		try {
			return sessionValue->OwnsProcess(processId, creationTime);
		} catch (...) {
			return false;
		}
	}

	std::weak_ptr<Impl> weakSelf;
	mutable std::mutex stateMutex;
	mutable std::mutex modelMutex;
	TerminalTargetCoordinate coordinate;
	TerminalInstanceOrigin origin;
	TerminalChildEnvironmentPolicy environmentPolicy;
	std::optional<std::string> taskRunId;
	TerminalCreateRequest request;
	TerminalInstanceDependencies dependencies;
	TerminalInstanceEventCallback eventCallback;
	std::uint64_t instanceGeneration{};
	TerminalInstanceState state{ TerminalInstanceState::Reserved };
	TerminalSessionState sessionStateValue{ TerminalSessionState::Idle };
	TerminalInstanceCloseReason closeReason{ TerminalInstanceCloseReason::None };
	std::uint32_t errorCode{};
	TerminalContentRevision contentRevision;
	std::wstring processName{ kDefaultProcessName };
	std::wstring profileLabel{ kDefaultProcessName };
	std::wstring sequenceTitle;
	std::wstring initialWorkingDirectory;
	std::unique_ptr<SakuraTerminalInputAdapter> input;
	std::unique_ptr<TerminalModel> model;
	std::unique_ptr<TerminalParser> parser;
	std::unique_ptr<CTerminalSession> session;
	std::vector<std::uint8_t> pendingProtocolInput;
	std::optional<TerminalInstanceOutcome> outcome;
	bool protocolInputRejected{};
	bool startAttempted{};
	bool startInProgress{};
	bool closeRequested{};
	bool outcomePublished{};
};

TerminalInstance::TerminalInstance(
	TerminalInstanceId instanceId,
	TerminalRuntimeGeneration runtimeGeneration,
	const std::uint64_t instanceGeneration,
	TerminalCreateRequest request,
	TerminalInstanceDependencies dependencies,
	TerminalInstanceEventCallback eventCallback)
	: m_impl(std::make_shared<Impl>(instanceId, runtimeGeneration, instanceGeneration,
		std::move(request), std::move(dependencies), std::move(eventCallback)))
{
	std::weak_ptr<Impl> weak = m_impl;
	m_impl->weakSelf = weak;
	m_impl->parser = std::make_unique<TerminalParser>(*m_impl->model, m_impl->input.get(),
		[weak](const std::string_view response) {
			if (const auto current = weak.lock()) {
				static_cast<void>(current->QueueProtocolInput(std::span<const std::uint8_t>(
					reinterpret_cast<const std::uint8_t*>(response.data()), response.size())));
			}
		});
}

TerminalInstance::~TerminalInstance()
{
	if (m_impl) m_impl->RequestClose(TerminalInstanceCloseReason::Explicit);
}

TerminalInstanceStartResult TerminalInstance::Start()
{
	return m_impl ? m_impl->Start(std::nullopt)
		: TerminalInstanceStartResult{ TerminalInstanceStartStatus::Unavailable, ERROR_OPERATION_ABORTED, L"Terminal instance is unavailable." };
}

TerminalInstanceStartResult TerminalInstance::Start(const TerminalLaunchOptions& options)
{
	return m_impl ? m_impl->Start(options)
		: TerminalInstanceStartResult{ TerminalInstanceStartStatus::Unavailable, ERROR_OPERATION_ABORTED, L"Terminal instance is unavailable." };
}

void TerminalInstance::BeginClose(const TerminalInstanceCloseReason reason) noexcept
{
	if (m_impl) m_impl->RequestClose(reason);
}

void TerminalInstance::RequestCancel() noexcept
{
	BeginClose(TerminalInstanceCloseReason::Cancel);
}

void TerminalInstance::NotifyHostLost() noexcept
{
	BeginClose(TerminalInstanceCloseReason::HostLost);
}

TerminalInstanceCloseWaitResult TerminalInstance::WaitForClose(
	const std::chrono::steady_clock::time_point absoluteDeadline) noexcept
{
	return m_impl ? m_impl->WaitForClose(absoluteDeadline)
		: TerminalInstanceCloseWaitResult{ TerminalInstanceCloseWaitStatus::Unavailable, std::nullopt };
}

void TerminalInstance::Close() noexcept
{
	BeginClose(TerminalInstanceCloseReason::Explicit);
	static_cast<void>(WaitForClose(std::chrono::steady_clock::time_point::max()));
}

TerminalInstanceId TerminalInstance::Id() const noexcept
{
	return m_impl ? m_impl->Id() : TerminalInstanceId{};
}

TerminalRuntimeGeneration TerminalInstance::RuntimeGeneration() const noexcept
{
	return m_impl ? m_impl->RuntimeGeneration() : TerminalRuntimeGeneration{};
}

std::uint64_t TerminalInstance::InstanceGeneration() const noexcept
{
	if (!m_impl) return 0;
	return m_impl->instanceGeneration;
}

TerminalInstanceOrigin TerminalInstance::Origin() const noexcept
{
	if (!m_impl) return TerminalInstanceOrigin::Interactive;
	return m_impl->origin;
}

TerminalChildEnvironmentPolicy TerminalInstance::EnvironmentPolicy() const noexcept
{
	if (!m_impl) return TerminalChildEnvironmentPolicy::InteractiveWithHarnessShim;
	return m_impl->environmentPolicy;
}

TerminalInstanceState TerminalInstance::State() const noexcept
{
	if (!m_impl) return TerminalInstanceState::Retired;
	const std::lock_guard lock(m_impl->stateMutex);
	return m_impl->state;
}

TerminalSessionState TerminalInstance::SessionState() const noexcept
{
	if (!m_impl) return TerminalSessionState::Failed;
	const std::lock_guard lock(m_impl->stateMutex);
	return m_impl->sessionStateValue;
}

std::uint32_t TerminalInstance::LastError() const noexcept
{
	if (!m_impl) return ERROR_OPERATION_ABORTED;
	const std::lock_guard lock(m_impl->stateMutex);
	return m_impl->errorCode;
}

std::optional<TerminalInstanceOutcome> TerminalInstance::Outcome() const
{
	return m_impl ? m_impl->Outcome() : std::nullopt;
}

TerminalInstanceSnapshot TerminalInstance::Snapshot() const
{
	return m_impl ? m_impl->Snapshot() : TerminalInstanceSnapshot{};
}

std::optional<TerminalBackendProcessIdentity> TerminalInstance::GetProcessIdentity() const noexcept
{
	return m_impl ? m_impl->GetProcessIdentity() : std::nullopt;
}

bool TerminalInstance::OwnsProcess(
	const std::uint32_t processId, const std::uint64_t creationTime) const noexcept
{
	return m_impl && m_impl->OwnsProcess(processId, creationTime);
}

const TerminalModel* TerminalInstance::Model() const noexcept
{
	return m_impl ? m_impl->model.get() : nullptr;
}

TerminalModel* TerminalInstance::Model() noexcept
{
	return const_cast<TerminalModel*>(std::as_const(*this).Model());
}

const SakuraTerminalInputAdapter* TerminalInstance::InputAdapter() const noexcept
{
	return m_impl ? m_impl->input.get() : nullptr;
}

SakuraTerminalInputAdapter* TerminalInstance::InputAdapter() noexcept
{
	return const_cast<SakuraTerminalInputAdapter*>(std::as_const(*this).InputAdapter());
}

TerminalInstanceDrainResult TerminalInstance::DrainOutput()
{
	return m_impl ? m_impl->DrainOutput() : TerminalInstanceDrainResult{};
}

TerminalQueueInputResult TerminalInstance::QueueInput(
	const std::span<const std::uint8_t> bytes,
	const TerminalInputSource source)
{
	if (!m_impl) return TerminalQueueInputResult::NotRunning;
	CTerminalSession* session = nullptr;
	{
		const std::lock_guard lock(m_impl->stateMutex);
		if (!m_impl->session || m_impl->outcomePublished) return TerminalQueueInputResult::NotRunning;
		session = m_impl->session.get();
	}
	return session->QueueInput(bytes, source);
}

TerminalQueueInputResult TerminalInstance::FlushPendingProtocolInput()
{
	return m_impl ? m_impl->FlushPendingProtocolInput() : TerminalQueueInputResult::NotRunning;
}

bool TerminalInstance::HasPendingProtocolInput() const noexcept
{
	if (!m_impl) return false;
	const std::lock_guard lock(m_impl->stateMutex);
	return !m_impl->pendingProtocolInput.empty();
}

bool TerminalInstance::ProtocolInputRejected() const noexcept
{
	if (!m_impl) return false;
	const std::lock_guard lock(m_impl->stateMutex);
	return m_impl->protocolInputRejected;
}

TerminalInstanceResizeResult TerminalInstance::Resize(const TerminalSize size)
{
	return m_impl ? m_impl->Resize(size) : TerminalInstanceResizeResult{ false, ERROR_OPERATION_ABORTED };
}

void TerminalInstance::RecordViewportDiagnostic(const TerminalViewportDiagnosticSnapshot& snapshot) noexcept
{
	if (!m_impl) return;
	CTerminalSession* session = nullptr;
	{
		const std::lock_guard lock(m_impl->stateMutex);
		if (m_impl->session) session = m_impl->session.get();
	}
	if (session) session->RecordViewportDiagnostic(snapshot);
}

} // namespace terminal
