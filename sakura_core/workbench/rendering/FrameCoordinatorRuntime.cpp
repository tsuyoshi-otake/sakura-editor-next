#include "StdAfx.h"
#include "workbench/rendering/FrameCoordinatorRuntime.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace workbench::rendering {

namespace {

using RuntimeState = EFrameCoordinatorRuntimeState;
using RuntimeStatus = EFrameCoordinatorRuntimeStatus;

[[nodiscard]] FrameCoordinatorRuntimeResult MakeResult(
	const RuntimeStatus status,
	const RuntimeState state) noexcept
{
	return FrameCoordinatorRuntimeResult{status, state};
}

[[nodiscard]] bool IsAccepting(const RuntimeState state) noexcept
{
	return state == RuntimeState::Created || state == RuntimeState::Running;
}

[[nodiscard]] bool IsTerminal(const RuntimeState state) noexcept
{
	return state == RuntimeState::Stopped || state == RuntimeState::Failed;
}

[[nodiscard]] bool IsValidFailureBoundary(
	const EFrameDeviceFailureBoundary boundary) noexcept
{
	switch (boundary) {
	case EFrameDeviceFailureBoundary::Present:
	case EFrameDeviceFailureBoundary::ResizeBuffers:
	case EFrameDeviceFailureBoundary::CompositionCommit:
		return true;
	default:
		return false;
	}
}

} // namespace

struct FrameCoordinatorRuntime::Shared {
	enum class CommandKind : std::uint8_t {
		Register,
		RegisterPresented,
		UpdatePresented,
		Close,
		Request,
		CpuComplete,
		ResetDevice,
		DeviceFailure,
		GdiCommit,
		NativePresent,
		NativeSurfaceFrame,
		Readback,
		Cadence,
	};

	struct Command {
		CommandKind kind = CommandKind::Request;
		FrameSurfaceId surfaceId = 0;
		std::uint64_t surfaceLifetimeEpoch = 0;
		std::uint64_t deviceEpoch = 0;
		EFrameDeviceFailureBoundary failureBoundary = EFrameDeviceFailureBoundary::Present;
		long failureCode = 0;
		FrameSurfaceRequest request{};
		FrameWorkTicket ticket{};
		FramePresentationSurfaceSpec presentationSpec{};
		FrameNativeSurfaceRegistration nativeRegistration{};
		FrameGdiSurfaceCommit gdiCommit{};
		FrameNativePresentationRequest nativePresentation{};
		std::shared_ptr<const FrameNativeSurfaceFrame> nativeSurfaceFrame;
		FrameReadbackObservation readback{};
		FrameCadenceInput cadence{};
	};

	explicit Shared(const FrameCoordinatorRuntimeOptions& runtimeOptions)
		: model(1)
		, options(runtimeOptions)
		, cadence(FrameCadence::Calculate(options.cadence))
		, backpressure(options.backpressure)
	{
		if (options.maxControlQueueDepth == 0) {
			options.maxControlQueueDepth = 1;
		}
		if (options.maxCpuWorkQueueDepth == 0) {
			options.maxCpuWorkQueueDepth = 1;
		}
		if (options.maxPublicationsPerTick == 0) {
			options.maxPublicationsPerTick = 1;
		}
		runtimeTelemetry.ConfigureCadence(
			cadence.effectiveRefreshRateHz,
			cadence.refreshInterval.count() > 0
				? static_cast<std::uint64_t>(cadence.refreshInterval.count())
				: 0,
			cadence.displayEpoch);
	}

	// Only OwnerMain reads/writes model.  mutex protects the command boundary,
	// callbacks, and immutable snapshots copied for callers.
	FrameCoordinatorModel model;
	FrameCoordinatorRuntimeOptions options;
	FrameCadenceResult cadence;
	FrameBackpressureController backpressure;
	mutable std::mutex mutex;
	std::condition_variable wake;
	std::deque<Command> commands;
	bool tickPending = false;
	bool closeRequested = false;
	RuntimeState state = RuntimeState::Created;
	std::thread::id ownerThreadId{};

	std::deque<FrameWorkTicket> cpuWork;
	std::shared_ptr<const FrameCommitCohort> publication;

	std::unordered_map<FrameSurfaceId, std::uint64_t> registeredLifetimes;
	std::unordered_map<FrameSurfaceId, FrameSurfaceSnapshot> surfaceSnapshots;
	std::unordered_map<FrameSurfaceId, FramePresentationSurfaceSnapshot> presentationSnapshots;
	FrameCoordinatorTelemetry telemetry{};
	FrameRuntimeTelemetry runtimeTelemetry;
	FramePresentationOwnerTelemetry presentationTelemetry{};
	FrameDeviceDomainTelemetry presentationDeviceTelemetry{};
	EFramePresentationOwnerState presentationState = EFramePresentationOwnerState::Created;
	std::uint64_t presentationDeviceEpoch = 0;
	std::uint64_t requestedTickCount = 0;
	std::uint64_t processedTickCount = 0;
	std::uint64_t replacedPublications = 0;
	std::uint64_t processedDeviceFailures = 0;
	std::uint64_t nextWindowFrameId = 1;
	bool shutdownFailed = false;
};

namespace {

//! Measures only mutex acquisition. The lock itself remains the existing
//! bounded coordinator boundary; no callback or wait is added by telemetry.
class RuntimeMutexGuard final {
public:
	explicit RuntimeMutexGuard(FrameCoordinatorRuntime::Shared& shared) noexcept
		: m_shared(shared)
		, m_startedMicroseconds(FrameRuntimeTelemetry::NowMicroseconds())
		, m_lock(shared.mutex)
	{
		const auto finished = FrameRuntimeTelemetry::NowMicroseconds();
		m_shared.runtimeTelemetry.RecordLockWaitDuration(
			finished >= m_startedMicroseconds ? finished - m_startedMicroseconds : 0);
	}

	RuntimeMutexGuard(const RuntimeMutexGuard&) = delete;
	RuntimeMutexGuard& operator=(const RuntimeMutexGuard&) = delete;

private:
	FrameCoordinatorRuntime::Shared& m_shared;
	std::uint64_t m_startedMicroseconds = 0;
	std::lock_guard<std::mutex> m_lock;
};

} // namespace

FrameCoordinatorRuntime::FrameCoordinatorRuntime(const FrameCoordinatorRuntimeOptions options)
	: m_shared(std::make_shared<Shared>(options))
	, m_ownerThread([shared = m_shared] { OwnerMain(shared); })
{
}

FrameCoordinatorRuntime::~FrameCoordinatorRuntime()
{
	// Destruction is legal only after an external owner has joined the thread.
	// Silently detaching here would bypass bounded retirement and make a UI-path
	// ownership bug look successful.
	std::lock_guard<std::mutex> joinLock(m_joinMutex);
	if (m_ownerThread.joinable()) {
		std::terminate();
	}
}

FrameCoordinatorRuntimeResult FrameCoordinatorRuntime::RegisterSurface(
	const FrameSurfaceId surfaceId,
	const std::uint64_t surfaceLifetimeEpoch) noexcept
{
	if (surfaceId == 0 || surfaceLifetimeEpoch == 0) {
		const auto state = Snapshot().state;
		return MakeResult(RuntimeStatus::Invalid, state);
	}

	try {
		const auto shared = m_shared;
		RuntimeMutexGuard lock(*shared);
		if (!IsAccepting(shared->state)) {
			return MakeResult(RuntimeStatus::Closed, shared->state);
		}
		if (shared->commands.size() >= shared->options.maxControlQueueDepth) {
			shared->runtimeTelemetry.RecordMailboxSaturation(
				EFrameTelemetryMailbox::Control, shared->commands.size(),
				shared->options.maxControlQueueDepth);
			return MakeResult(RuntimeStatus::QueueFull, shared->state);
		}
		Shared::Command command;
		command.kind = Shared::CommandKind::Register;
		command.surfaceId = surfaceId;
		command.surfaceLifetimeEpoch = surfaceLifetimeEpoch;
		shared->commands.push_back(command);
		shared->runtimeTelemetry.RecordMailboxAccepted(
			EFrameTelemetryMailbox::Control, shared->commands.size(),
			shared->options.maxControlQueueDepth);
		shared->wake.notify_one();
		return MakeResult(RuntimeStatus::Succeeded, shared->state);
	} catch (...) {
		return MakeResult(RuntimeStatus::Failed, Snapshot().state);
	}
}

FrameCoordinatorRuntimeResult FrameCoordinatorRuntime::RegisterPresentedSurface(
	const FramePresentationSurfaceSpec& spec) noexcept
{
	return RegisterPresentedSurface(FrameNativeSurfaceRegistration{
		.presentation = spec,
	});
}

FrameCoordinatorRuntimeResult FrameCoordinatorRuntime::RegisterPresentedSurface(
	const FrameNativeSurfaceRegistration& registration) noexcept
{
	if (!registration.IsValid()) {
		return MakeResult(RuntimeStatus::Invalid, Snapshot().state);
	}

	try {
		const auto shared = m_shared;
		RuntimeMutexGuard lock(*shared);
		if (!IsAccepting(shared->state)) {
			return MakeResult(RuntimeStatus::Closed, shared->state);
		}
		if (shared->commands.size() >= shared->options.maxControlQueueDepth) {
			shared->runtimeTelemetry.RecordMailboxSaturation(
				EFrameTelemetryMailbox::Control, shared->commands.size(),
				shared->options.maxControlQueueDepth);
			return MakeResult(RuntimeStatus::QueueFull, shared->state);
		}
		Shared::Command command;
		command.kind = Shared::CommandKind::RegisterPresented;
		command.surfaceId = registration.presentation.surfaceId;
		command.surfaceLifetimeEpoch = registration.presentation.surfaceLifetimeEpoch;
		command.presentationSpec = registration.presentation;
		command.nativeRegistration = registration;
		shared->commands.push_back(command);
		shared->runtimeTelemetry.RecordMailboxAccepted(
			EFrameTelemetryMailbox::Control, shared->commands.size(),
			shared->options.maxControlQueueDepth);
		shared->wake.notify_one();
		return MakeResult(RuntimeStatus::Succeeded, shared->state);
	} catch (...) {
		return MakeResult(RuntimeStatus::Failed, Snapshot().state);
	}
}

FrameCoordinatorRuntimeResult FrameCoordinatorRuntime::UpdatePresentedSurface(
	const FrameNativeSurfaceRegistration& registration) noexcept
{
	if (!registration.IsValid()) {
		return MakeResult(RuntimeStatus::Invalid, Snapshot().state);
	}

	try {
		const auto shared = m_shared;
		RuntimeMutexGuard lock(*shared);
		if (!IsAccepting(shared->state)) {
			return MakeResult(RuntimeStatus::Closed, shared->state);
		}
		for (const auto& command : shared->commands) {
			if (command.kind == Shared::CommandKind::Close
				&& command.surfaceId == registration.presentation.surfaceId) {
				return MakeResult(RuntimeStatus::Closed, shared->state);
			}
		}
		for (auto& command : shared->commands) {
			if (command.kind == Shared::CommandKind::UpdatePresented
				&& command.surfaceId == registration.presentation.surfaceId) {
				if (registration.presentation.surfaceLifetimeEpoch
					!= command.nativeRegistration.presentation.surfaceLifetimeEpoch) {
					return registration.presentation.surfaceLifetimeEpoch
						< command.nativeRegistration.presentation.surfaceLifetimeEpoch
						? MakeResult(RuntimeStatus::Stale, shared->state)
						: MakeResult(RuntimeStatus::Invalid, shared->state);
				}
				if (registration.presentation.layoutEpoch
					< command.nativeRegistration.presentation.layoutEpoch) {
					return MakeResult(RuntimeStatus::Stale, shared->state);
				}
				command.nativeRegistration = registration;
				command.presentationSpec = registration.presentation;
				shared->wake.notify_one();
				return MakeResult(RuntimeStatus::Replaced, shared->state);
			}
		}
		if (shared->commands.size() >= shared->options.maxControlQueueDepth) {
			shared->runtimeTelemetry.RecordMailboxSaturation(
				EFrameTelemetryMailbox::Control, shared->commands.size(),
				shared->options.maxControlQueueDepth);
			return MakeResult(RuntimeStatus::QueueFull, shared->state);
		}
		Shared::Command command;
		command.kind = Shared::CommandKind::UpdatePresented;
		command.surfaceId = registration.presentation.surfaceId;
		command.surfaceLifetimeEpoch = registration.presentation.surfaceLifetimeEpoch;
		command.nativeRegistration = registration;
		command.presentationSpec = registration.presentation;
		shared->commands.push_back(std::move(command));
		shared->runtimeTelemetry.RecordMailboxAccepted(
			EFrameTelemetryMailbox::Control, shared->commands.size(),
			shared->options.maxControlQueueDepth);
		shared->wake.notify_one();
		return MakeResult(RuntimeStatus::Succeeded, shared->state);
	} catch (...) {
		return MakeResult(RuntimeStatus::Failed, Snapshot().state);
	}
}

FrameCoordinatorRuntimeResult FrameCoordinatorRuntime::CloseSurface(
	const FrameSurfaceId surfaceId,
	const std::uint64_t surfaceLifetimeEpoch) noexcept
{
	if (surfaceId == 0 || surfaceLifetimeEpoch == 0) {
		return MakeResult(RuntimeStatus::Invalid, Snapshot().state);
	}

	try {
		const auto shared = m_shared;
		std::deque<Shared::Command> retiredCommands;
		RuntimeMutexGuard lock(*shared);
		if (!IsAccepting(shared->state)) {
			return MakeResult(RuntimeStatus::Closed, shared->state);
		}

		for (auto iterator = shared->commands.begin(); iterator != shared->commands.end(); ++iterator) {
			if (iterator->kind == Shared::CommandKind::Close && iterator->surfaceId == surfaceId) {
				if (iterator->surfaceLifetimeEpoch == surfaceLifetimeEpoch) {
					return MakeResult(RuntimeStatus::Replaced, shared->state);
				}
				return MakeResult(RuntimeStatus::Stale, shared->state);
			}
		}

		// Closing a surface is terminal for queued requests on that surface.  It
		// is safe to drop them before enqueuing Close, preserving boundedness.
		for (auto iterator = shared->commands.begin(); iterator != shared->commands.end();) {
			if ((iterator->kind == Shared::CommandKind::Request
				|| iterator->kind == Shared::CommandKind::UpdatePresented
				|| iterator->kind == Shared::CommandKind::GdiCommit
				|| iterator->kind == Shared::CommandKind::NativePresent
				|| iterator->kind == Shared::CommandKind::NativeSurfaceFrame
				|| iterator->kind == Shared::CommandKind::Readback)
				&& iterator->surfaceId == surfaceId) {
				retiredCommands.push_back(std::move(*iterator));
				iterator = shared->commands.erase(iterator);
			} else {
				++iterator;
			}
		}

		if (shared->commands.size() >= shared->options.maxControlQueueDepth) {
			return MakeResult(RuntimeStatus::QueueFull, shared->state);
		}
		Shared::Command command;
		command.kind = Shared::CommandKind::Close;
		command.surfaceId = surfaceId;
		command.surfaceLifetimeEpoch = surfaceLifetimeEpoch;
		shared->commands.push_back(command);
		shared->wake.notify_one();
		return MakeResult(RuntimeStatus::Succeeded, shared->state);
	} catch (...) {
		return MakeResult(RuntimeStatus::Failed, Snapshot().state);
	}
}

FrameCoordinatorRuntimeResult FrameCoordinatorRuntime::Request(const FrameSurfaceRequest& request) noexcept
{
	if (request.surfaceId == 0 || request.surfaceLifetimeEpoch == 0 || request.requestId == 0 ||
		request.contentGeneration == 0 || request.layoutEpoch == 0 || request.deviceEpoch == 0) {
		return MakeResult(RuntimeStatus::Invalid, Snapshot().state);
	}

	try {
		const auto shared = m_shared;
	RuntimeMutexGuard lock(*shared);
		if (!IsAccepting(shared->state)) {
			return MakeResult(RuntimeStatus::Closed, shared->state);
		}
		for (const auto& command : shared->commands) {
			if (command.kind == Shared::CommandKind::Close && command.surfaceId == request.surfaceId) {
				return MakeResult(RuntimeStatus::Closed, shared->state);
			}
		}
		for (auto& command : shared->commands) {
			if (command.kind == Shared::CommandKind::Request && command.surfaceId == request.surfaceId) {
				command.request = request;
				shared->runtimeTelemetry.RecordMailboxAccepted(
					EFrameTelemetryMailbox::Control, shared->commands.size(),
					shared->options.maxControlQueueDepth, true);
				shared->runtimeTelemetry.RecordRequest(
					request.surfaceId, request.requestId);
				shared->wake.notify_one();
				return MakeResult(RuntimeStatus::Replaced, shared->state);
			}
		}
		if (shared->commands.size() >= shared->options.maxControlQueueDepth) {
			shared->runtimeTelemetry.RecordMailboxSaturation(
				EFrameTelemetryMailbox::Control, shared->commands.size(),
				shared->options.maxControlQueueDepth);
			return MakeResult(RuntimeStatus::QueueFull, shared->state);
		}
		Shared::Command command;
		command.kind = Shared::CommandKind::Request;
		command.surfaceId = request.surfaceId;
		command.request = request;
		shared->commands.push_back(command);
		shared->runtimeTelemetry.RecordMailboxAccepted(
			EFrameTelemetryMailbox::Control, shared->commands.size(),
			shared->options.maxControlQueueDepth);
		shared->runtimeTelemetry.RecordRequest(request.surfaceId, request.requestId);
		shared->wake.notify_one();
		return MakeResult(RuntimeStatus::Succeeded, shared->state);
	} catch (...) {
		return MakeResult(RuntimeStatus::Failed, Snapshot().state);
	}
}

FrameCoordinatorRuntimeResult FrameCoordinatorRuntime::SubmitCpuCompletion(
	const FrameWorkTicket& ticket) noexcept
{
	if (!ticket.IsValid()) {
		return MakeResult(RuntimeStatus::Invalid, Snapshot().state);
	}

	try {
		const auto shared = m_shared;
	RuntimeMutexGuard lock(*shared);
		if (!IsAccepting(shared->state)) {
			return MakeResult(RuntimeStatus::Closed, shared->state);
		}
		if (shared->commands.size() >= shared->options.maxControlQueueDepth) {
			shared->runtimeTelemetry.RecordMailboxSaturation(
				EFrameTelemetryMailbox::Control, shared->commands.size(),
				shared->options.maxControlQueueDepth);
			return MakeResult(RuntimeStatus::QueueFull, shared->state);
		}
		Shared::Command command;
		command.kind = Shared::CommandKind::CpuComplete;
		command.surfaceId = ticket.surfaceId;
		command.surfaceLifetimeEpoch = ticket.surfaceLifetimeEpoch;
		command.deviceEpoch = ticket.deviceEpoch;
		command.ticket = ticket;
		shared->commands.push_back(command);
		shared->runtimeTelemetry.RecordMailboxAccepted(
			EFrameTelemetryMailbox::Control, shared->commands.size(),
			shared->options.maxControlQueueDepth);
		shared->wake.notify_one();
		return MakeResult(RuntimeStatus::Succeeded, shared->state);
	} catch (...) {
		return MakeResult(RuntimeStatus::Failed, Snapshot().state);
	}
}

FrameCoordinatorRuntimeResult FrameCoordinatorRuntime::ResetDevice(const std::uint64_t deviceEpoch) noexcept
{
	if (deviceEpoch == 0) {
		return MakeResult(RuntimeStatus::Invalid, Snapshot().state);
	}

	try {
		const auto shared = m_shared;
	RuntimeMutexGuard lock(*shared);
		if (!IsAccepting(shared->state)) {
			return MakeResult(RuntimeStatus::Closed, shared->state);
		}
		for (auto& command : shared->commands) {
			if (command.kind == Shared::CommandKind::ResetDevice) {
				command.deviceEpoch = deviceEpoch;
				shared->wake.notify_one();
				return MakeResult(RuntimeStatus::Replaced, shared->state);
			}
		}
		if (shared->commands.size() >= shared->options.maxControlQueueDepth) {
			return MakeResult(RuntimeStatus::QueueFull, shared->state);
		}
		Shared::Command command;
		command.kind = Shared::CommandKind::ResetDevice;
		command.deviceEpoch = deviceEpoch;
		shared->commands.push_back(command);
		shared->wake.notify_one();
		return MakeResult(RuntimeStatus::Succeeded, shared->state);
	} catch (...) {
		return MakeResult(RuntimeStatus::Failed, Snapshot().state);
	}
}

FrameCoordinatorRuntimeResult FrameCoordinatorRuntime::InjectPresentationFailure(
	const EFrameDeviceFailureBoundary boundary, const long failureCode) noexcept
{
	if (!IsValidFailureBoundary(boundary)) {
		return MakeResult(RuntimeStatus::Invalid, Snapshot().state);
	}

	try {
		const auto shared = m_shared;
	RuntimeMutexGuard lock(*shared);
		if (!IsAccepting(shared->state)) {
			return MakeResult(RuntimeStatus::Closed, shared->state);
		}
		for (auto& command : shared->commands) {
			if (command.kind == Shared::CommandKind::DeviceFailure) {
				command.failureBoundary = boundary;
				command.failureCode = failureCode;
				shared->wake.notify_one();
				return MakeResult(RuntimeStatus::Replaced, shared->state);
			}
		}
		if (shared->commands.size() >= shared->options.maxControlQueueDepth) {
			return MakeResult(RuntimeStatus::QueueFull, shared->state);
		}
		Shared::Command command;
		command.kind = Shared::CommandKind::DeviceFailure;
		command.failureBoundary = boundary;
		command.failureCode = failureCode;
		shared->commands.push_back(command);
		shared->wake.notify_one();
		return MakeResult(RuntimeStatus::Succeeded, shared->state);
	} catch (...) {
		return MakeResult(RuntimeStatus::Failed, Snapshot().state);
	}
}

FrameCoordinatorRuntimeResult FrameCoordinatorRuntime::RecordGdiFallback(
	const FrameGdiSurfaceCommit& commit) noexcept
{
	if (!commit.IsValid()) {
		return MakeResult(RuntimeStatus::Invalid, Snapshot().state);
	}

	try {
		const auto shared = m_shared;
	RuntimeMutexGuard lock(*shared);
		if (!IsAccepting(shared->state)) {
			return MakeResult(RuntimeStatus::Closed, shared->state);
		}
		for (const auto& command : shared->commands) {
			if (command.kind == Shared::CommandKind::Close
				&& command.surfaceId == commit.surfaceId) {
				return MakeResult(RuntimeStatus::Closed, shared->state);
			}
		}
		for (auto& command : shared->commands) {
			if (command.kind == Shared::CommandKind::GdiCommit
				&& command.surfaceId == commit.surfaceId) {
				command.gdiCommit = commit;
				shared->wake.notify_one();
				return MakeResult(RuntimeStatus::Replaced, shared->state);
			}
		}
		if (shared->commands.size() >= shared->options.maxControlQueueDepth) {
			return MakeResult(RuntimeStatus::QueueFull, shared->state);
		}
		Shared::Command command;
		command.kind = Shared::CommandKind::GdiCommit;
		command.surfaceId = commit.surfaceId;
		command.surfaceLifetimeEpoch = commit.surfaceLifetimeEpoch;
		command.deviceEpoch = commit.deviceEpoch;
		command.gdiCommit = commit;
		shared->commands.push_back(command);
		shared->wake.notify_one();
		return MakeResult(RuntimeStatus::Succeeded, shared->state);
	} catch (...) {
		return MakeResult(RuntimeStatus::Failed, Snapshot().state);
	}
}

FrameCoordinatorRuntimeResult FrameCoordinatorRuntime::PresentSurface(
	const FrameNativePresentationRequest& request) noexcept
{
	if (!request.IsValid()) {
		return MakeResult(RuntimeStatus::Invalid, Snapshot().state);
	}

	try {
		const auto shared = m_shared;
		RuntimeMutexGuard lock(*shared);
		if (!IsAccepting(shared->state)) {
			return MakeResult(RuntimeStatus::Closed, shared->state);
		}
		if (request.displayEpoch < shared->cadence.displayEpoch) {
			return MakeResult(RuntimeStatus::Stale, shared->state);
		}
		for (const auto& command : shared->commands) {
			if (command.kind == Shared::CommandKind::Close
				&& command.surfaceId == request.surfaceId) {
				return MakeResult(RuntimeStatus::Closed, shared->state);
			}
		}
		for (auto& command : shared->commands) {
			if (command.kind == Shared::CommandKind::NativePresent
				&& command.surfaceId == request.surfaceId) {
				if (request.surfaceLifetimeEpoch < command.surfaceLifetimeEpoch
					|| request.requestId < command.nativePresentation.requestId) {
					return MakeResult(RuntimeStatus::Stale, shared->state);
				}
				command.surfaceLifetimeEpoch = request.surfaceLifetimeEpoch;
				command.nativePresentation = request;
				shared->runtimeTelemetry.RecordMailboxAccepted(
					EFrameTelemetryMailbox::Control, shared->commands.size(),
					shared->options.maxControlQueueDepth, true);
				shared->wake.notify_one();
				return MakeResult(RuntimeStatus::Replaced, shared->state);
			}
		}
		if (shared->commands.size() >= shared->options.maxControlQueueDepth) {
			shared->runtimeTelemetry.RecordMailboxSaturation(
				EFrameTelemetryMailbox::Control, shared->commands.size(),
				shared->options.maxControlQueueDepth);
			return MakeResult(RuntimeStatus::QueueFull, shared->state);
		}
		Shared::Command command;
		command.kind = Shared::CommandKind::NativePresent;
		command.surfaceId = request.surfaceId;
		command.surfaceLifetimeEpoch = request.surfaceLifetimeEpoch;
		command.deviceEpoch = request.deviceEpoch;
		command.nativePresentation = request;
		shared->commands.push_back(command);
		shared->runtimeTelemetry.RecordMailboxAccepted(
			EFrameTelemetryMailbox::Control, shared->commands.size(),
			shared->options.maxControlQueueDepth);
		shared->wake.notify_one();
		return MakeResult(RuntimeStatus::Succeeded, shared->state);
	} catch (...) {
		return MakeResult(RuntimeStatus::Failed, Snapshot().state);
	}
}

FrameCoordinatorRuntimeResult FrameCoordinatorRuntime::SubmitNativeSurfaceFrame(
	std::shared_ptr<const FrameNativeSurfaceFrame> frame) noexcept
{
	if (!frame || !frame->IsValid()) {
		return MakeResult(RuntimeStatus::Invalid, Snapshot().state);
	}

	// Releasing the replaced frame can release a large immutable pixel vector.
	// Keep that ownership outside the coordinator lock so the producer path only
	// performs bounded pointer operations under the mutex.
	std::shared_ptr<const FrameNativeSurfaceFrame> replacedFrame;
	try {
		const auto shared = m_shared;
		RuntimeMutexGuard lock(*shared);
		if (!IsAccepting(shared->state)) {
			return MakeResult(RuntimeStatus::Closed, shared->state);
		}
		const auto nativeBytes = frame->PayloadBytes();
		if (nativeBytes > shared->options.presentationOwner.maximumNativeSurfaceBytes) {
			return MakeResult(RuntimeStatus::Invalid, shared->state);
		}
		if (frame->displayEpoch < shared->cadence.displayEpoch) {
			return MakeResult(RuntimeStatus::Stale, shared->state);
		}
		for (const auto& command : shared->commands) {
			if (command.kind == Shared::CommandKind::Close
				&& command.surfaceId == frame->surfaceId) {
				return MakeResult(RuntimeStatus::Closed, shared->state);
			}
		}
		for (auto& command : shared->commands) {
			if (command.kind == Shared::CommandKind::NativeSurfaceFrame
				&& command.surfaceId == frame->surfaceId) {
				const auto& previous = command.nativeSurfaceFrame;
				if (!previous
					|| frame->surfaceLifetimeEpoch < previous->surfaceLifetimeEpoch
					|| (frame->surfaceLifetimeEpoch == previous->surfaceLifetimeEpoch
						&& frame->requestId < previous->requestId)) {
					return MakeResult(RuntimeStatus::Stale, shared->state);
				}
				command.surfaceLifetimeEpoch = frame->surfaceLifetimeEpoch;
				command.deviceEpoch = frame->deviceEpoch;
				replacedFrame = std::move(command.nativeSurfaceFrame);
				command.nativeSurfaceFrame = std::move(frame);
				shared->runtimeTelemetry.RecordMailboxAccepted(
					EFrameTelemetryMailbox::Control, shared->commands.size(),
					shared->options.maxControlQueueDepth, true);
				shared->wake.notify_one();
				return MakeResult(RuntimeStatus::Replaced, shared->state);
			}
		}
		if (shared->commands.size() >= shared->options.maxControlQueueDepth) {
			shared->runtimeTelemetry.RecordMailboxSaturation(
				EFrameTelemetryMailbox::Control, shared->commands.size(),
				shared->options.maxControlQueueDepth);
			return MakeResult(RuntimeStatus::QueueFull, shared->state);
		}
		Shared::Command command;
		command.kind = Shared::CommandKind::NativeSurfaceFrame;
		command.surfaceId = frame->surfaceId;
		command.surfaceLifetimeEpoch = frame->surfaceLifetimeEpoch;
		command.deviceEpoch = frame->deviceEpoch;
		command.nativeSurfaceFrame = std::move(frame);
		shared->commands.push_back(std::move(command));
		shared->runtimeTelemetry.RecordMailboxAccepted(
			EFrameTelemetryMailbox::Control, shared->commands.size(),
			shared->options.maxControlQueueDepth);
		shared->wake.notify_one();
		return MakeResult(RuntimeStatus::Succeeded, shared->state);
	} catch (...) {
		return MakeResult(RuntimeStatus::Failed, Snapshot().state);
	}
}

FrameCoordinatorRuntimeResult FrameCoordinatorRuntime::SubmitNativeSurfaceFrame(
	FrameNativeSurfaceFrame frame) noexcept
{
	try {
		return SubmitNativeSurfaceFrame(
			std::make_shared<const FrameNativeSurfaceFrame>(std::move(frame)));
	} catch (...) {
		return MakeResult(RuntimeStatus::Failed, Snapshot().state);
	}
}

FrameCoordinatorRuntimeResult FrameCoordinatorRuntime::RecordReadbackObservation(
	const FrameReadbackObservation& observation) noexcept
{
	if (observation.surfaceId == 0 || observation.surfaceLifetimeEpoch == 0
		|| observation.deviceEpoch == 0 || observation.displayEpoch == 0
		|| observation.layoutEpoch == 0 || observation.requestId == 0
		|| observation.width == 0 || observation.height == 0) {
		return MakeResult(RuntimeStatus::Invalid, Snapshot().state);
	}

	try {
		const auto shared = m_shared;
		RuntimeMutexGuard lock(*shared);
		if (!IsAccepting(shared->state)) {
			return MakeResult(RuntimeStatus::Closed, shared->state);
		}
		if (observation.displayEpoch < shared->cadence.displayEpoch) {
			return MakeResult(RuntimeStatus::Stale, shared->state);
		}
		for (auto& command : shared->commands) {
			if (command.kind == Shared::CommandKind::Readback
				&& command.surfaceId == observation.surfaceId) {
				if (observation.surfaceLifetimeEpoch
					< command.surfaceLifetimeEpoch
					|| observation.requestId < command.readback.requestId) {
					return MakeResult(RuntimeStatus::Stale, shared->state);
				}
				command.surfaceLifetimeEpoch = observation.surfaceLifetimeEpoch;
				command.readback = observation;
				shared->runtimeTelemetry.RecordMailboxAccepted(
					EFrameTelemetryMailbox::Control, shared->commands.size(),
					shared->options.maxControlQueueDepth, true);
				shared->wake.notify_one();
				return MakeResult(RuntimeStatus::Replaced, shared->state);
			}
		}
		if (shared->commands.size() >= shared->options.maxControlQueueDepth) {
			shared->runtimeTelemetry.RecordMailboxSaturation(
				EFrameTelemetryMailbox::Control, shared->commands.size(),
				shared->options.maxControlQueueDepth);
			return MakeResult(RuntimeStatus::QueueFull, shared->state);
		}
		Shared::Command command;
		command.kind = Shared::CommandKind::Readback;
		command.surfaceId = observation.surfaceId;
		command.surfaceLifetimeEpoch = observation.surfaceLifetimeEpoch;
		command.deviceEpoch = observation.deviceEpoch;
		command.readback = observation;
		shared->commands.push_back(command);
		shared->runtimeTelemetry.RecordMailboxAccepted(
			EFrameTelemetryMailbox::Control, shared->commands.size(),
			shared->options.maxControlQueueDepth);
		shared->wake.notify_one();
		return MakeResult(RuntimeStatus::Succeeded, shared->state);
	} catch (...) {
		return MakeResult(RuntimeStatus::Failed, Snapshot().state);
	}
}

FrameCoordinatorRuntimeResult FrameCoordinatorRuntime::UpdateCadence(
	const FrameCadenceInput& cadenceInput) noexcept
{
	if (cadenceInput.displayEpoch == 0) {
		return MakeResult(RuntimeStatus::Invalid, Snapshot().state);
	}

	try {
		const auto shared = m_shared;
		RuntimeMutexGuard lock(*shared);
		if (!IsAccepting(shared->state)) {
			return MakeResult(RuntimeStatus::Closed, shared->state);
		}
		if (cadenceInput.displayEpoch < shared->cadence.displayEpoch) {
			return MakeResult(RuntimeStatus::Stale, shared->state);
		}
		for (auto& command : shared->commands) {
			if (command.kind == Shared::CommandKind::Cadence) {
				if (cadenceInput.displayEpoch < command.cadence.displayEpoch) {
					return MakeResult(RuntimeStatus::Stale, shared->state);
				}
				command.cadence = cadenceInput;
				shared->runtimeTelemetry.RecordMailboxAccepted(
					EFrameTelemetryMailbox::Control, shared->commands.size(),
					shared->options.maxControlQueueDepth, true);
				shared->wake.notify_one();
				return MakeResult(RuntimeStatus::Replaced, shared->state);
			}
		}
		if (shared->commands.size() >= shared->options.maxControlQueueDepth) {
			shared->runtimeTelemetry.RecordMailboxSaturation(
				EFrameTelemetryMailbox::Control, shared->commands.size(),
				shared->options.maxControlQueueDepth);
			return MakeResult(RuntimeStatus::QueueFull, shared->state);
		}
		Shared::Command command;
		command.kind = Shared::CommandKind::Cadence;
		command.cadence = cadenceInput;
		shared->commands.push_back(command);
		shared->runtimeTelemetry.RecordMailboxAccepted(
			EFrameTelemetryMailbox::Control, shared->commands.size(),
			shared->options.maxControlQueueDepth);
		shared->wake.notify_one();
		return MakeResult(RuntimeStatus::Succeeded, shared->state);
	} catch (...) {
		return MakeResult(RuntimeStatus::Failed, Snapshot().state);
	}
}

FrameCoordinatorRuntimeResult FrameCoordinatorRuntime::Tick() noexcept
{
	try {
		const auto shared = m_shared;
	RuntimeMutexGuard lock(*shared);
		if (!IsAccepting(shared->state)) {
			return MakeResult(RuntimeStatus::Closed, shared->state);
		}
		if (shared->tickPending) {
			return MakeResult(RuntimeStatus::Replaced, shared->state);
		}
		shared->tickPending = true;
		++shared->requestedTickCount;
		shared->wake.notify_one();
		return MakeResult(RuntimeStatus::Succeeded, shared->state);
	} catch (...) {
		return MakeResult(RuntimeStatus::Failed, Snapshot().state);
	}
}

std::optional<FrameWorkTicket> FrameCoordinatorRuntime::TakeCpuWork() noexcept
{
	try {
		const auto shared = m_shared;
	RuntimeMutexGuard lock(*shared);
		if (shared->cpuWork.empty()) return std::nullopt;
		auto ticket = shared->cpuWork.front();
		shared->cpuWork.pop_front();
		if (IsAccepting(shared->state) && !shared->tickPending) {
			shared->tickPending = true;
			++shared->requestedTickCount;
			shared->wake.notify_one();
		}
		return ticket;
	} catch (...) {
		return std::nullopt;
	}
}

std::shared_ptr<const FrameCommitCohort> FrameCoordinatorRuntime::TakePublication() noexcept
{
	try {
		const auto shared = m_shared;
	RuntimeMutexGuard lock(*shared);
		auto publication = std::exchange(shared->publication, {});
		if (publication != nullptr && IsAccepting(shared->state)
			&& !shared->tickPending) {
			// Consuming the depth-one publication mailbox is an explicit event
			// source for retrying a previously backpressured latest request.
			// The owner remains responsible for admission; this only wakes it.
			shared->tickPending = true;
			++shared->requestedTickCount;
			shared->wake.notify_one();
		}
		return publication;
	} catch (...) {
		return {};
	}
}

FrameCoordinatorRuntimeResult FrameCoordinatorRuntime::BeginClose() noexcept
{
	// A queued native frame owns an immutable pixel vector. Swap the command
	// mailbox out so its destruction cannot release that payload while the UI
	// caller holds the runtime mutex.
	std::deque<Shared::Command> retiredCommands;
	try {
		const auto shared = m_shared;
		RuntimeMutexGuard lock(*shared);
		if (shared->state == RuntimeState::Closing) {
			return MakeResult(RuntimeStatus::Replaced, shared->state);
		}
		if (IsTerminal(shared->state)) {
			return MakeResult(RuntimeStatus::Closed, shared->state);
		}
		shared->state = RuntimeState::Closing;
		shared->closeRequested = true;
		retiredCommands.swap(shared->commands);
		shared->cpuWork.clear();
		shared->publication.reset();
		shared->tickPending = false;
		shared->wake.notify_one();
		return MakeResult(RuntimeStatus::Succeeded, shared->state);
	} catch (...) {
		return MakeResult(RuntimeStatus::Failed, Snapshot().state);
	}
}

FrameCoordinatorRuntimeResult FrameCoordinatorRuntime::Wait() noexcept
{
	const auto shared = m_shared;
	{
		std::lock_guard<std::mutex> joinLock(m_joinMutex);
		std::unique_lock<std::mutex> lock(shared->mutex);
		if (shared->ownerThreadId == std::this_thread::get_id()) {
			return MakeResult(RuntimeStatus::SelfWait, shared->state);
		}
		if (!shared->closeRequested && !IsTerminal(shared->state)) {
			return MakeResult(RuntimeStatus::Busy, shared->state);
		}
		shared->wake.wait(lock, [&shared] {
			return IsTerminal(shared->state);
		});
		const auto finalState = shared->state;
		lock.unlock();
		if (m_ownerThread.joinable()) {
			m_ownerThread.join();
		}
		return MakeResult(finalState == RuntimeState::Failed ? RuntimeStatus::Failed : RuntimeStatus::Succeeded, finalState);
	}
}

FrameCoordinatorRuntimeSnapshot FrameCoordinatorRuntime::Snapshot() const noexcept
{
	const auto shared = m_shared;
	RuntimeMutexGuard lock(*shared);
	return FrameCoordinatorRuntimeSnapshot{
		.state = shared->state,
		.closeRequested = shared->closeRequested,
		.ownerThreadRunning = shared->ownerThreadId != std::thread::id{},
		.controlQueueDepth = shared->commands.size(),
		.cpuWorkQueueDepth = shared->cpuWork.size(),
		.publicationPending = shared->publication != nullptr,
		.tickPending = shared->tickPending,
		.requestedTickCount = shared->requestedTickCount,
		.processedTickCount = shared->processedTickCount,
		.replacedPublications = shared->replacedPublications,
		.processedDeviceFailures = shared->processedDeviceFailures,
		.presentationState = shared->presentationState,
		.presentationDeviceEpoch = shared->presentationDeviceEpoch,
	};
}

std::optional<FrameSurfaceSnapshot> FrameCoordinatorRuntime::SurfaceSnapshot(const FrameSurfaceId surfaceId) const noexcept
{
	const auto shared = m_shared;
	RuntimeMutexGuard lock(*shared);
	const auto iterator = shared->surfaceSnapshots.find(surfaceId);
	if (iterator == shared->surfaceSnapshots.end()) {
		return std::nullopt;
	}
	return iterator->second;
}

std::optional<FramePresentationSurfaceSnapshot>
FrameCoordinatorRuntime::PresentationSurfaceSnapshot(const FrameSurfaceId surfaceId) const noexcept
{
	const auto shared = m_shared;
	RuntimeMutexGuard lock(*shared);
	const auto iterator = shared->presentationSnapshots.find(surfaceId);
	if (iterator == shared->presentationSnapshots.end()) return std::nullopt;
	return iterator->second;
}

FramePresentationOwnerTelemetry FrameCoordinatorRuntime::PresentationTelemetry() const noexcept
{
	const auto shared = m_shared;
	RuntimeMutexGuard lock(*shared);
	return shared->presentationTelemetry;
}

FrameDeviceDomainTelemetry FrameCoordinatorRuntime::PresentationDeviceTelemetry() const noexcept
{
	const auto shared = m_shared;
	RuntimeMutexGuard lock(*shared);
	return shared->presentationDeviceTelemetry;
}

FrameCoordinatorTelemetry FrameCoordinatorRuntime::Telemetry() const noexcept
{
	const auto shared = m_shared;
	RuntimeMutexGuard lock(*shared);
	return shared->telemetry;
}

FrameRuntimeTelemetrySnapshot FrameCoordinatorRuntime::RuntimeTelemetry() const noexcept
{
	return m_shared->runtimeTelemetry.Snapshot();
}

FrameCadenceResult FrameCoordinatorRuntime::Cadence() const noexcept
{
	const auto shared = m_shared;
	RuntimeMutexGuard lock(*shared);
	return shared->cadence;
}

FrameBackpressureSnapshot FrameCoordinatorRuntime::Backpressure() const noexcept
{
	const auto shared = m_shared;
	RuntimeMutexGuard lock(*shared);
	return shared->backpressure.Snapshot();
}

namespace {

void RefreshTelemetryAndSnapshot(
	const std::shared_ptr<FrameCoordinatorRuntime::Shared>& shared,
	const FrameSurfaceId surfaceId)
{
	const auto snapshot = shared->model.SurfaceSnapshot(surfaceId);
	const auto telemetry = shared->model.Telemetry();
	RuntimeMutexGuard lock(*shared);
	if (snapshot.has_value()) {
		shared->surfaceSnapshots[surfaceId] = *snapshot;
	}
	shared->telemetry = telemetry;
}

void RefreshAllSnapshots(const std::shared_ptr<FrameCoordinatorRuntime::Shared>& shared)
{
	std::vector<FrameSurfaceId> surfaceIds;
	{
	RuntimeMutexGuard lock(*shared);
		surfaceIds.reserve(shared->surfaceSnapshots.size() + shared->registeredLifetimes.size());
		for (const auto& entry : shared->surfaceSnapshots) {
			surfaceIds.push_back(entry.first);
		}
		for (const auto& entry : shared->registeredLifetimes) {
			if (std::find(surfaceIds.begin(), surfaceIds.end(), entry.first) == surfaceIds.end()) {
				surfaceIds.push_back(entry.first);
			}
		}
	}
	for (const auto surfaceId : surfaceIds) {
		RefreshTelemetryAndSnapshot(shared, surfaceId);
	}
}

void RefreshPresentationSnapshot(
	const std::shared_ptr<FrameCoordinatorRuntime::Shared>& shared,
	const FramePresentationOwner& owner,
	const FrameSurfaceId surfaceId)
{
	const auto snapshot = owner.SurfaceRegistry().Snapshot(surfaceId);
	const auto telemetry = owner.Telemetry();
	const auto deviceTelemetry = owner.DeviceTelemetry();
	const auto state = owner.State();
	const auto deviceEpoch = owner.DeviceEpoch();
	RuntimeMutexGuard lock(*shared);
	if (snapshot.has_value()) shared->presentationSnapshots[surfaceId] = *snapshot;
	shared->presentationTelemetry = telemetry;
	shared->presentationDeviceTelemetry = deviceTelemetry;
	shared->presentationState = state;
	shared->presentationDeviceEpoch = deviceEpoch;
}

void ReprojectPresentationSurfaces(
	const std::shared_ptr<FrameCoordinatorRuntime::Shared>& shared,
	FramePresentationOwner& owner,
	const std::uint64_t deviceEpoch)
{
	std::vector<std::pair<FrameSurfaceId, std::uint64_t>> registered;
	{
	RuntimeMutexGuard lock(*shared);
		registered.reserve(shared->registeredLifetimes.size());
		for (const auto& entry : shared->registeredLifetimes) {
			registered.push_back(entry);
		}
	}

	const bool softwareOnly = owner.State() == EFramePresentationOwnerState::SoftwareOnly;
	for (const auto& entry : registered) {
		(void)owner.SurfaceRegistry().ReprojectDevice(
			entry.first, entry.second, deviceEpoch, softwareOnly);
		RefreshPresentationSnapshot(shared, owner, entry.first);
	}
}

void RequestOwnerFailure(const std::shared_ptr<FrameCoordinatorRuntime::Shared>& shared) noexcept
{
	RuntimeMutexGuard lock(*shared);
	shared->shutdownFailed = true;
	if (!IsTerminal(shared->state)) shared->state = RuntimeState::Closing;
	shared->closeRequested = true;
	shared->commands.clear();
	shared->cpuWork.clear();
	shared->publication.reset();
	shared->tickPending = false;
	shared->wake.notify_all();
}

void CompleteCpuWork(
	const std::shared_ptr<FrameCoordinatorRuntime::Shared>& shared,
	const FrameWorkTicket& ticket)
{
	const auto cpuResult = shared->model.CompleteCpu(ticket);
	if (cpuResult.status == EFrameOperationStatus::Succeeded) {
		const auto queueResult = shared->model.QueueGpu(ticket);
		if (queueResult.status == EFrameOperationStatus::Succeeded) {
			const auto beginResult = shared->model.BeginGpu(ticket);
			if (beginResult.status == EFrameOperationStatus::Succeeded) {
				(void)shared->model.CompleteGpu(ticket);
			} else if (beginResult.phase == EFrameSurfacePhase::Withdrawn) {
				(void)shared->model.RetireWithdrawn(ticket);
			}
		} else if (queueResult.phase == EFrameSurfacePhase::Withdrawn) {
			(void)shared->model.RetireWithdrawn(ticket);
		}
	} else if (cpuResult.phase == EFrameSurfacePhase::Withdrawn) {
		(void)shared->model.RetireWithdrawn(ticket);
	}
	RefreshTelemetryAndSnapshot(shared, ticket.surfaceId);
}

void CompleteNativeAdmission(
	const std::shared_ptr<FrameCoordinatorRuntime::Shared>& shared,
	const FrameNativePresentationRequest& request) noexcept
{
	RuntimeMutexGuard lock(*shared);
	(void)shared->backpressure.Complete(request.surfaceId, request.requestId);
}

void ApplyNativePresentation(
	const std::shared_ptr<FrameCoordinatorRuntime::Shared>& shared,
	FramePresentationOwner& presentationOwner,
	const FrameNativePresentationRequest& request,
	bool& shouldTick)
{
	const auto snapshot = presentationOwner.SurfaceRegistry().Snapshot(request.surfaceId);
	std::uint64_t currentDisplayEpoch = 0;
	{
		RuntimeMutexGuard lock(*shared);
		currentDisplayEpoch = shared->cadence.displayEpoch;
	}
	if (!snapshot.has_value()
		|| snapshot->surfaceLifetimeEpoch != request.surfaceLifetimeEpoch
		|| snapshot->deviceEpoch != request.deviceEpoch
		|| request.layoutEpoch < snapshot->layoutEpoch
		|| request.requestId <= snapshot->lastPresentedRequestId
		|| (currentDisplayEpoch != 0
			&& request.displayEpoch < currentDisplayEpoch)) {
		shared->runtimeTelemetry.RecordSkippedPresentation(request.surfaceId, false);
		RefreshPresentationSnapshot(shared, presentationOwner, request.surfaceId);
		return;
	}

	FrameBackpressureResult admission;
	{
		RuntimeMutexGuard lock(*shared);
		admission = shared->backpressure.Submit(
			request.surfaceId, request.requestId);
	}
	const bool retryingPendingRequest = admission.status
		== EFrameBackpressureStatus::SkippedSaturated
		&& admission.requestId == request.requestId;
	if (!admission.Accepted() && !retryingPendingRequest) {
		// This is bounded owner-path admission pressure, not a CPU publication
		// mailbox signal and not evidence that Present1 was attempted.
		shared->runtimeTelemetry.RecordSkippedPresentation(request.surfaceId, true);
		RefreshPresentationSnapshot(shared, presentationOwner, request.surfaceId);
		return;
	}

	const auto presentResult = presentationOwner.Present();
	const auto& ownerTelemetry = presentationOwner.Telemetry();
	const auto duration = ownerTelemetry.lastPresentDurationMicroseconds;
	const auto resultCode = static_cast<std::int64_t>(presentResult.hresult);
	switch (presentResult.status) {
	case EFramePresentationOwnerStatus::Succeeded: {
		CompleteNativeAdmission(shared, request);
		(void)presentationOwner.SurfaceRegistry().MarkPresented(
			request.surfaceId, request.surfaceLifetimeEpoch, request.deviceEpoch,
			request.layoutEpoch, request.requestId);
		shared->runtimeTelemetry.RecordNativePresent(
			EFrameNativePresentOutcome::Presented, resultCode, duration,
			request.surfaceId, request.requestId);
		RefreshPresentationSnapshot(shared, presentationOwner, request.surfaceId);
		break;
	}
	case EFramePresentationOwnerStatus::SkippedBackpressure:
		// Keep the admission pending. A later explicit display event may retry
		// this surface; no timer, wait, or all-surface barrier is introduced.
		(void)presentationOwner.SurfaceRegistry().MarkBackpressure(
			request.surfaceId, request.surfaceLifetimeEpoch);
		shared->runtimeTelemetry.RecordSkippedPresentation(request.surfaceId, true);
		shared->runtimeTelemetry.RecordNativePresent(
			EFrameNativePresentOutcome::Backpressured, resultCode, duration,
			request.surfaceId, request.requestId);
		RefreshPresentationSnapshot(shared, presentationOwner, request.surfaceId);
		break;
	case EFramePresentationOwnerStatus::NotReady:
		CompleteNativeAdmission(shared, request);
		shared->runtimeTelemetry.RecordSkippedPresentation(request.surfaceId, false);
		shared->runtimeTelemetry.RecordNativePresent(
			EFrameNativePresentOutcome::NotReady, resultCode, duration,
			request.surfaceId, request.requestId);
		if (presentationOwner.State() == EFramePresentationOwnerState::SoftwareOnly) {
			(void)presentationOwner.SurfaceRegistry().MarkSoftwareOnly(
				request.surfaceId, request.surfaceLifetimeEpoch,
				presentationOwner.DeviceEpoch());
		}
		RefreshPresentationSnapshot(shared, presentationOwner, request.surfaceId);
		break;
	case EFramePresentationOwnerStatus::DeviceLost: {
		CompleteNativeAdmission(shared, request);
		shared->runtimeTelemetry.RecordNativePresent(
			EFrameNativePresentOutcome::DeviceLost, resultCode, duration,
			request.surfaceId, request.requestId);
		const auto recoveredDeviceEpoch = presentationOwner.DeviceEpoch();
		const auto resetResult = shared->model.ResetDevice(recoveredDeviceEpoch);
		if (resetResult.status != EFrameOperationStatus::Succeeded) {
			RequestOwnerFailure(shared);
			break;
		}
		{
			RuntimeMutexGuard lock(*shared);
			shared->nextWindowFrameId = 1;
			++shared->processedDeviceFailures;
		}
		ReprojectPresentationSurfaces(shared, presentationOwner,
			recoveredDeviceEpoch);
		RefreshAllSnapshots(shared);
		shouldTick = true;
		break;
	}
	case EFramePresentationOwnerStatus::Failed:
	case EFramePresentationOwnerStatus::Closed:
	case EFramePresentationOwnerStatus::WrongThread:
	case EFramePresentationOwnerStatus::Invalid:
	case EFramePresentationOwnerStatus::Backoff:
	default:
		CompleteNativeAdmission(shared, request);
		shared->runtimeTelemetry.RecordNativePresent(
			EFrameNativePresentOutcome::Failed, resultCode, duration,
			request.surfaceId, request.requestId);
		(void)presentationOwner.SurfaceRegistry().MarkFailed(
			request.surfaceId, request.surfaceLifetimeEpoch);
		RefreshPresentationSnapshot(shared, presentationOwner, request.surfaceId);
		RequestOwnerFailure(shared);
		break;
	}
}

void ApplyNativeSurfaceFrame(
	const std::shared_ptr<FrameCoordinatorRuntime::Shared>& shared,
	FramePresentationOwner& presentationOwner,
	const FrameNativeSurfaceFrame& frame,
	bool& shouldTick)
{
	const auto snapshot = presentationOwner.SurfaceRegistry().Snapshot(
		frame.surfaceId);
	std::uint64_t currentDisplayEpoch = 0;
	{
		RuntimeMutexGuard lock(*shared);
		currentDisplayEpoch = shared->cadence.displayEpoch;
	}
	if (!snapshot.has_value()
		|| snapshot->surfaceLifetimeEpoch != frame.surfaceLifetimeEpoch
		|| snapshot->deviceEpoch != frame.deviceEpoch
		|| frame.layoutEpoch < snapshot->layoutEpoch
		|| frame.requestId <= snapshot->lastPresentedRequestId
		|| (currentDisplayEpoch != 0 && frame.displayEpoch < currentDisplayEpoch)) {
		shared->runtimeTelemetry.RecordSkippedPresentation(frame.surfaceId, false);
		RefreshPresentationSnapshot(shared, presentationOwner, frame.surfaceId);
		return;
	}

	const auto presentResult = presentationOwner.PresentNativeSurface(frame);
	const auto& ownerTelemetry = presentationOwner.Telemetry();
	const auto duration = ownerTelemetry.lastPresentDurationMicroseconds;
	const auto resultCode = static_cast<std::int64_t>(presentResult.hresult);
	switch (presentResult.status) {
	case EFramePresentationOwnerStatus::Succeeded:
		(void)presentationOwner.SurfaceRegistry().MarkPresented(
			frame.surfaceId, frame.surfaceLifetimeEpoch, frame.deviceEpoch,
			frame.layoutEpoch, frame.requestId);
		shared->runtimeTelemetry.RecordNativePresent(
			EFrameNativePresentOutcome::Presented, resultCode, duration,
			frame.surfaceId, frame.requestId);
		RefreshPresentationSnapshot(shared, presentationOwner, frame.surfaceId);
		break;
	case EFramePresentationOwnerStatus::SkippedBackpressure:
		(void)presentationOwner.SurfaceRegistry().MarkBackpressure(
			frame.surfaceId, frame.surfaceLifetimeEpoch);
		shared->runtimeTelemetry.RecordSkippedPresentation(frame.surfaceId, true);
		shared->runtimeTelemetry.RecordNativePresent(
			EFrameNativePresentOutcome::Backpressured, resultCode, duration,
			frame.surfaceId, frame.requestId);
		RefreshPresentationSnapshot(shared, presentationOwner, frame.surfaceId);
		break;
	case EFramePresentationOwnerStatus::NotReady:
		shared->runtimeTelemetry.RecordSkippedPresentation(frame.surfaceId, false);
		shared->runtimeTelemetry.RecordNativePresent(
			EFrameNativePresentOutcome::NotReady, resultCode, duration,
			frame.surfaceId, frame.requestId);
		if (presentationOwner.State() == EFramePresentationOwnerState::SoftwareOnly) {
			(void)presentationOwner.SurfaceRegistry().MarkSoftwareOnly(
				frame.surfaceId, frame.surfaceLifetimeEpoch,
				presentationOwner.DeviceEpoch());
		}
		RefreshPresentationSnapshot(shared, presentationOwner, frame.surfaceId);
		break;
	case EFramePresentationOwnerStatus::DeviceLost: {
		shared->runtimeTelemetry.RecordNativePresent(
			EFrameNativePresentOutcome::DeviceLost, resultCode, duration,
			frame.surfaceId, frame.requestId);
		const auto recoveredDeviceEpoch = presentationOwner.DeviceEpoch();
		const auto resetResult = shared->model.ResetDevice(recoveredDeviceEpoch);
		if (resetResult.status != EFrameOperationStatus::Succeeded) {
			RequestOwnerFailure(shared);
			break;
		}
		{
			RuntimeMutexGuard lock(*shared);
			shared->nextWindowFrameId = 1;
			++shared->processedDeviceFailures;
		}
		ReprojectPresentationSurfaces(shared, presentationOwner,
			recoveredDeviceEpoch);
		RefreshAllSnapshots(shared);
		shouldTick = true;
		break;
	}
	case EFramePresentationOwnerStatus::Failed:
	case EFramePresentationOwnerStatus::Closed:
	case EFramePresentationOwnerStatus::WrongThread:
	case EFramePresentationOwnerStatus::Invalid:
	case EFramePresentationOwnerStatus::Backoff:
	default:
		shared->runtimeTelemetry.RecordNativePresent(
			EFrameNativePresentOutcome::Failed, resultCode, duration,
			frame.surfaceId, frame.requestId);
		(void)presentationOwner.SurfaceRegistry().MarkFailed(
			frame.surfaceId, frame.surfaceLifetimeEpoch);
		RefreshPresentationSnapshot(shared, presentationOwner, frame.surfaceId);
		RequestOwnerFailure(shared);
		break;
	}
}

void ApplyReadbackObservation(
	const std::shared_ptr<FrameCoordinatorRuntime::Shared>& shared,
	FramePresentationOwner& presentationOwner,
	const FrameReadbackObservation& observation)
{
	std::uint64_t currentDisplayEpoch = 0;
	{
		RuntimeMutexGuard lock(*shared);
		currentDisplayEpoch = shared->cadence.displayEpoch;
	}
	if (currentDisplayEpoch != 0
		&& observation.displayEpoch < currentDisplayEpoch) {
		return;
	}
	const auto result = presentationOwner.ObserveReadback(observation);
	if (result.status == EFramePresentationOwnerStatus::Succeeded) {
		shared->runtimeTelemetry.RecordReadbackObservation(observation);
	}
	RefreshPresentationSnapshot(shared, presentationOwner, observation.surfaceId);
}

void ApplyCadence(
	const std::shared_ptr<FrameCoordinatorRuntime::Shared>& shared,
	const FrameCadenceInput& input,
	bool& shouldTick)
{
	const auto calculated = FrameCadence::Calculate(input);
	{
		RuntimeMutexGuard lock(*shared);
		if (input.displayEpoch == 0
			|| (shared->cadence.displayEpoch != 0
				&& input.displayEpoch < shared->cadence.displayEpoch)) {
			return;
		}
		shared->cadence = calculated;
	}
	shared->runtimeTelemetry.ConfigureCadence(
		shared->cadence.effectiveRefreshRateHz,
		shared->cadence.refreshInterval.count() > 0
			? static_cast<std::uint64_t>(shared->cadence.refreshInterval.count())
			: 0,
		shared->cadence.displayEpoch);
	// A cadence event is itself the explicit wake source. It never schedules a
	// timer and does not wait for any other surface.
	shouldTick = true;
}

void ApplyCommand(
	const std::shared_ptr<FrameCoordinatorRuntime::Shared>& shared,
	FramePresentationOwner& presentationOwner,
	const FrameCoordinatorRuntime::Shared::Command& command,
	bool& shouldTick,
	std::uint64_t& commitLayoutEpoch)
{
	using CommandKind = FrameCoordinatorRuntime::Shared::CommandKind;
	using OperationStatus = EFrameOperationStatus;
	FrameOperationResult result;
	switch (command.kind) {
	case CommandKind::Register:
		result = shared->model.RegisterSurface(command.surfaceId, command.surfaceLifetimeEpoch);
		if (result.Accepted()) {
	RuntimeMutexGuard lock(*shared);
			shared->registeredLifetimes[command.surfaceId] = command.surfaceLifetimeEpoch;
			const auto admission = shared->backpressure.RegisterSurface(
				command.surfaceId, false);
			if (!admission.Accepted()) {
				shared->runtimeTelemetry.RecordMailboxSaturation(
					EFrameTelemetryMailbox::Surface,
					kFrameBackpressureSurfaceSlotCount,
					kFrameBackpressureSurfaceSlotCount);
			}
		}
		RefreshTelemetryAndSnapshot(shared, command.surfaceId);
		shouldTick = true;
		break;
	case CommandKind::RegisterPresented: {
		result = shared->model.RegisterSurface(command.surfaceId,
			command.surfaceLifetimeEpoch);
		const auto presentationResult = presentationOwner.RegisterNativeSurface(
			command.nativeRegistration);
		if (result.Accepted() && presentationResult.Accepted()) {
	RuntimeMutexGuard lock(*shared);
			shared->registeredLifetimes[command.surfaceId] = command.surfaceLifetimeEpoch;
			const auto admission = shared->backpressure.RegisterSurface(
				command.surfaceId, false);
			if (!admission.Accepted()) {
				shared->runtimeTelemetry.RecordMailboxSaturation(
					EFrameTelemetryMailbox::Surface,
					kFrameBackpressureSurfaceSlotCount,
					kFrameBackpressureSurfaceSlotCount);
			}
		} else if (result.Accepted()) {
			(void)shared->model.CloseSurface(command.surfaceId,
				command.surfaceLifetimeEpoch);
	RuntimeMutexGuard lock(*shared);
			shared->shutdownFailed = true;
		}
		RefreshTelemetryAndSnapshot(shared, command.surfaceId);
		RefreshPresentationSnapshot(shared, presentationOwner, command.surfaceId);
		break;
	}
	case CommandKind::UpdatePresented: {
		const auto presentationResult = presentationOwner.UpdateNativeSurface(
			command.nativeRegistration);
		if (presentationResult.Accepted()) {
			(void)presentationOwner.SurfaceRegistry().Resize(
				command.surfaceId,
				command.surfaceLifetimeEpoch,
				command.nativeRegistration.presentation.width,
				command.nativeRegistration.presentation.height);
		}
		RefreshPresentationSnapshot(shared, presentationOwner, command.surfaceId);
		break;
	}
	case CommandKind::Close:
		result = shared->model.CloseSurface(command.surfaceId, command.surfaceLifetimeEpoch);
		(void)presentationOwner.CloseNativeSurface(
			command.surfaceId, command.surfaceLifetimeEpoch);
		if (result.status == OperationStatus::Succeeded) {
	RuntimeMutexGuard lock(*shared);
			shared->registeredLifetimes.erase(command.surfaceId);
			(void)shared->backpressure.CloseSurface(command.surfaceId);
		}
		RefreshTelemetryAndSnapshot(shared, command.surfaceId);
		RefreshPresentationSnapshot(shared, presentationOwner, command.surfaceId);
		break;
	case CommandKind::Request:
		result = shared->model.Request(command.request);
		if (result.Accepted()) {
			RuntimeMutexGuard lock(*shared);
			// Interactive work is the Editor-like lane. Promotion is monotonic
			// and remains bounded even when the request later gets superseded.
			(void)shared->backpressure.RegisterSurface(
				command.request.surfaceId,
				command.request.workClass == EFrameWorkClass::Interactive);
		}
		RefreshTelemetryAndSnapshot(shared, command.request.surfaceId);
		shouldTick = true;
		commitLayoutEpoch = command.request.layoutEpoch;
		break;
	case CommandKind::CpuComplete:
		CompleteCpuWork(shared, command.ticket);
		shouldTick = true;
		commitLayoutEpoch = command.ticket.layoutEpoch;
		break;
	case CommandKind::ResetDevice:
		result = shared->model.ResetDevice(command.deviceEpoch);
		if (result.status == OperationStatus::Succeeded) {
	RuntimeMutexGuard lock(*shared);
			shared->nextWindowFrameId = 1;
		}
		RefreshAllSnapshots(shared);
		shouldTick = true;
		break;
	case CommandKind::DeviceFailure: {
		const auto presentationResult = presentationOwner.InjectDeviceLoss(
			command.failureBoundary, command.failureCode);
		if (presentationResult.status == EFramePresentationOwnerStatus::DeviceLost) {
			const auto recoveredDeviceEpoch = presentationOwner.DeviceEpoch();
			result = shared->model.ResetDevice(recoveredDeviceEpoch);
			if (result.status != OperationStatus::Succeeded) {
				RequestOwnerFailure(shared);
				break;
			}
			{
	RuntimeMutexGuard lock(*shared);
				shared->nextWindowFrameId = 1;
				++shared->processedDeviceFailures;
			}
			ReprojectPresentationSurfaces(shared, presentationOwner,
				recoveredDeviceEpoch);
			RefreshAllSnapshots(shared);
			shouldTick = true;
		} else {
			RefreshPresentationSnapshot(shared, presentationOwner, command.surfaceId);
			if (presentationResult.status == EFramePresentationOwnerStatus::Failed
				|| presentationResult.status == EFramePresentationOwnerStatus::Closed) {
				RequestOwnerFailure(shared);
			}
		}
		break;
	}
	case CommandKind::GdiCommit: {
		const auto& commit = command.gdiCommit;
		const auto resizeResult = presentationOwner.SurfaceRegistry().Resize(
			commit.surfaceId, commit.surfaceLifetimeEpoch,
			commit.width, commit.height);
		if (resizeResult.Accepted()) {
			(void)presentationOwner.SurfaceRegistry().MarkGdiFallback(
				commit.surfaceId, commit.surfaceLifetimeEpoch,
				commit.deviceEpoch, commit.layoutEpoch,
				commit.requestId, commit.visible);
		}
		RefreshPresentationSnapshot(shared, presentationOwner, commit.surfaceId);
		break;
	}
	case CommandKind::NativePresent:
		ApplyNativePresentation(shared, presentationOwner,
			command.nativePresentation, shouldTick);
		break;
	case CommandKind::NativeSurfaceFrame:
		if (command.nativeSurfaceFrame != nullptr) {
			ApplyNativeSurfaceFrame(shared, presentationOwner,
				*command.nativeSurfaceFrame, shouldTick);
		}
		break;
	case CommandKind::Readback:
		ApplyReadbackObservation(shared, presentationOwner, command.readback);
		break;
	case CommandKind::Cadence:
		ApplyCadence(shared, command.cadence, shouldTick);
		break;
	default:
		// Keep the switch total if a new command kind is added without an
		// implementation here.  Failure is terminal and observable.
		RequestOwnerFailure(shared);
		break;
	}
}

void CloseRegisteredSurfaces(
	const std::shared_ptr<FrameCoordinatorRuntime::Shared>& shared,
	FramePresentationOwner* presentationOwner) noexcept
{
	for (;;) {
		std::pair<FrameSurfaceId, std::uint64_t> entry{};
		{
	RuntimeMutexGuard lock(*shared);
			shared->commands.clear();
			shared->cpuWork.clear();
			shared->publication.reset();
			shared->tickPending = false;
			if (shared->registeredLifetimes.empty()) break;
			const auto iterator = shared->registeredLifetimes.begin();
			entry = *iterator;
			shared->registeredLifetimes.erase(iterator);
			(void)shared->backpressure.CloseSurface(entry.first);
		}

		try {
			if (presentationOwner != nullptr) {
				(void)presentationOwner->CloseNativeSurface(entry.first, entry.second);
			}
			const auto result = shared->model.CloseSurface(entry.first, entry.second);
			if (result.status == EFrameOperationStatus::Busy) {
				const auto finalResult = shared->model.FinalizeCloseSurface(entry.first, entry.second);
				if (finalResult.status != EFrameOperationStatus::Succeeded) {
	RuntimeMutexGuard lock(*shared);
					shared->shutdownFailed = true;
				}
			} else if (result.status != EFrameOperationStatus::Succeeded
				&& result.status != EFrameOperationStatus::UnknownSurface) {
	RuntimeMutexGuard lock(*shared);
				shared->shutdownFailed = true;
			}
			RefreshTelemetryAndSnapshot(shared, entry.first);
			if (presentationOwner != nullptr) {
				RefreshPresentationSnapshot(shared, *presentationOwner, entry.first);
			}
		} catch (...) {
	RuntimeMutexGuard lock(*shared);
			shared->shutdownFailed = true;
		}
	}
}

void ProcessTick(
	const std::shared_ptr<FrameCoordinatorRuntime::Shared>& shared,
	std::uint64_t commitLayoutEpoch)
{
	std::optional<FrameWorkTicket> ticket;
	{
	RuntimeMutexGuard lock(*shared);
		if (shared->cpuWork.size() < shared->options.maxCpuWorkQueueDepth) {
			ticket = shared->model.TakeNextCpuWork();
			if (ticket.has_value()) {
				shared->cpuWork.push_back(*ticket);
				shared->runtimeTelemetry.RecordMailboxAccepted(
					EFrameTelemetryMailbox::CpuWork, shared->cpuWork.size(),
					shared->options.maxCpuWorkQueueDepth);
			}
		} else {
			shared->runtimeTelemetry.RecordMailboxSaturation(
				EFrameTelemetryMailbox::CpuWork, shared->cpuWork.size(),
				shared->options.maxCpuWorkQueueDepth);
		}
	}
	if (ticket.has_value()) {
		bool closeRequested = false;
		{
	RuntimeMutexGuard lock(*shared);
			closeRequested = shared->closeRequested;
		}
		if (closeRequested) {
			{
	RuntimeMutexGuard lock(*shared);
				if (!shared->cpuWork.empty() && shared->cpuWork.back() == *ticket) shared->cpuWork.pop_back();
			}
			// CloseSurface makes an in-flight CPU ticket terminal.  Completion is
			// intentionally skipped after the close fence.
			const auto closeResult = shared->model.CloseSurface(ticket->surfaceId, ticket->surfaceLifetimeEpoch);
			if (closeResult.status == EFrameOperationStatus::Busy) {
				shared->shutdownFailed = true;
			}
			RefreshTelemetryAndSnapshot(shared, ticket->surfaceId);
			return;
		}

		RefreshTelemetryAndSnapshot(shared, ticket->surfaceId);
	}

	bool closeRequested = false;
	{
	RuntimeMutexGuard lock(*shared);
		closeRequested = shared->closeRequested;
	}
	if (closeRequested) {
		return;
	}

	std::uint64_t windowFrameId = 0;
	{
	RuntimeMutexGuard lock(*shared);
		if (shared->nextWindowFrameId == 0 || shared->nextWindowFrameId == std::numeric_limits<std::uint64_t>::max()) {
			shared->shutdownFailed = true;
			shared->state = RuntimeState::Closing;
			shared->closeRequested = true;
			shared->commands.clear();
			shared->cpuWork.clear();
			shared->publication.reset();
			shared->tickPending = false;
			shared->wake.notify_all();
			return;
		}
		windowFrameId = shared->nextWindowFrameId++;
	}
	if (ticket.has_value()) commitLayoutEpoch = ticket->layoutEpoch;
	const auto cohort = shared->model.AssembleCommit(
		windowFrameId,
		commitLayoutEpoch,
		shared->options.maxPublicationsPerTick);
	if (cohort.publications.empty()) {
		RefreshAllSnapshots(shared);
		return;
	}

	// CPU publication is deliberately independent from compositor readiness.
	// Complete the logical cohort and publish its immutable plan even when a
	// native surface is later skipped by Present1(DXGI_PRESENT_DO_NOT_WAIT).
	// Native adapters report that boundary through PresentSurface; no CPU pull
	// mailbox operation is allowed to consume a swap-chain admission slot.
	const auto commitResult = shared->model.CompleteCommit(cohort);
	RefreshAllSnapshots(shared);
	if (commitResult.status != EFrameOperationStatus::Succeeded) {
		return;
	}

	try {
		auto immutable = std::make_shared<const FrameCommitCohort>(cohort);
	RuntimeMutexGuard lock(*shared);
		const bool replaced = shared->publication != nullptr;
		if (replaced) ++shared->replacedPublications;
		shared->publication = std::move(immutable);
		shared->runtimeTelemetry.RecordMailboxAccepted(
			EFrameTelemetryMailbox::Publication, 1, 1, replaced);
		for (const auto& publication : cohort.publications) {
			shared->runtimeTelemetry.RecordPublication(
				publication.ticket.surfaceId, publication.ticket.requestId);
		}
	} catch (...) {
		RequestOwnerFailure(shared);
	}
}

} // namespace

void FrameCoordinatorRuntime::OwnerMain(const std::shared_ptr<Shared>& shared) noexcept
{
	std::unique_ptr<FramePresentationOwner> presentationOwner;
	try {
		presentationOwner = std::make_unique<FramePresentationOwner>(
			shared->options.presentationOwner);
		const auto presentationResult = presentationOwner->Initialize();
		{
	RuntimeMutexGuard lock(*shared);
			shared->presentationState = presentationOwner->State();
			shared->presentationDeviceEpoch = presentationOwner->DeviceEpoch();
			shared->presentationTelemetry = presentationOwner->Telemetry();
			shared->presentationDeviceTelemetry = presentationOwner->DeviceTelemetry();
			// A failed native domain does not kill the coordinator. Registered
			// surfaces can remain explicitly GDI-authoritative.
			(void)presentationResult;
			shared->ownerThreadId = std::this_thread::get_id();
			if (shared->state == RuntimeState::Created) {
				shared->state = RuntimeState::Running;
			}
			shared->wake.notify_all();
		}

		for (;;) {
			std::deque<Shared::Command> commands;
			bool explicitTick = false;
			{
				std::unique_lock<std::mutex> lock(shared->mutex);
				shared->wake.wait(lock, [&shared] {
					return shared->closeRequested || !shared->commands.empty() || shared->tickPending;
				});
				if (shared->closeRequested) {
					shared->commands.clear();
					shared->tickPending = false;
				} else {
					commands.swap(shared->commands);
					explicitTick = shared->tickPending;
					shared->tickPending = false;
				}
			}

			bool closeRequested = false;
			{
	RuntimeMutexGuard lock(*shared);
				closeRequested = shared->closeRequested;
			}
			if (closeRequested) {
				CloseRegisteredSurfaces(shared, presentationOwner.get());
				break;
			}

			bool shouldTick = explicitTick;
			std::uint64_t commitLayoutEpoch = 0;
			for (const auto& command : commands) {
				FrameUiHandlerTimingScope handlerTiming(shared->runtimeTelemetry);
				ApplyCommand(shared, *presentationOwner, command, shouldTick, commitLayoutEpoch);
				{
	RuntimeMutexGuard lock(*shared);
					closeRequested = shared->closeRequested;
				}
				if (closeRequested) {
					break;
				}
			}
			if (closeRequested) {
				CloseRegisteredSurfaces(shared, presentationOwner.get());
				break;
			}
			if (shouldTick) {
				ProcessTick(shared, commitLayoutEpoch);
	RuntimeMutexGuard lock(*shared);
				++shared->processedTickCount;
			}
		}
	} catch (...) {
		RequestOwnerFailure(shared);
	}

	// This is the common no-throw finalizer for normal close and every owner
	// failure path.  Surface snapshots reach Closed before the runtime itself
	// becomes terminal, so callers never observe an abandoned intermediate state.
	CloseRegisteredSurfaces(shared, presentationOwner.get());
	if (presentationOwner != nullptr) {
		(void)presentationOwner->BeginClose();
		(void)presentationOwner->Close();
	}

	{
	RuntimeMutexGuard lock(*shared);
		shared->ownerThreadId = std::thread::id{};
		if (presentationOwner != nullptr) {
			shared->presentationState = presentationOwner->State();
			shared->presentationDeviceEpoch = presentationOwner->DeviceEpoch();
			shared->presentationTelemetry = presentationOwner->Telemetry();
			shared->presentationDeviceTelemetry = presentationOwner->DeviceTelemetry();
		} else {
			shared->presentationState = EFramePresentationOwnerState::Failed;
		}
		shared->state = shared->shutdownFailed ? RuntimeState::Failed : RuntimeState::Stopped;
		shared->closeRequested = true;
		shared->commands.clear();
		shared->cpuWork.clear();
		shared->publication.reset();
		shared->tickPending = false;
		shared->wake.notify_all();
	}
}

} // namespace workbench::rendering
