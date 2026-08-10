/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include <sakura/editor/lifecycle/EditorAppLifecycle.h>

#if __has_include("_main/editor_lifecycle/EditorAppLifecycle.cpp")
#error "sakura_editor_app_lifecycle_tests consumer can reach the provider implementation"
#endif

#include <array>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using editor::lifecycle::EEditorAppLifecycleFinalizationOutcome;
using editor::lifecycle::EEditorAppLifecyclePhase;
using editor::lifecycle::EEditorAppLifecyclePhaseOutcome;
using editor::lifecycle::EEditorAppLifecycleResultCode;
using editor::lifecycle::EEditorAppLifecycleState;
using editor::lifecycle::EditorAppLifecycle;
using editor::lifecycle::EditorAppLifecycleFinalizationResult;
using editor::lifecycle::EditorAppLifecyclePhaseDefinition;
using editor::lifecycle::EditorAppLifecyclePhaseResult;

EditorAppLifecyclePhaseDefinition Phase(EEditorAppLifecyclePhase phase, std::vector<std::string>& trace,
	EEditorAppLifecyclePhaseOutcome start = EEditorAppLifecyclePhaseOutcome::Succeeded,
	EEditorAppLifecycleFinalizationOutcome finalize = EEditorAppLifecycleFinalizationOutcome::Succeeded)
{
	return EditorAppLifecyclePhaseDefinition(
		phase,
		[&trace, start] {
			trace.emplace_back("start");
			return EditorAppLifecyclePhaseResult{ start };
		},
		[&trace, finalize] {
			trace.emplace_back("finalize");
			return EditorAppLifecycleFinalizationResult{ finalize };
		});
}

bool HasTrace(const std::vector<std::string>& actual, std::initializer_list<std::string_view> expected)
{
	if (actual.size() != expected.size()) return false;
	auto iterator = actual.begin();
	for (const auto value : expected) {
		if (*iterator++ != value) return false;
	}
	return true;
}

bool SuccessAndReverseStop()
{
	std::vector<std::string> trace;
	EditorAppLifecycle lifecycle({
		Phase(EEditorAppLifecyclePhase::ProfileResolution, trace),
		Phase(EEditorAppLifecyclePhase::PlatformServices, trace),
		Phase(EEditorAppLifecyclePhase::WorkbenchCreation, trace),
	});
	const auto started = lifecycle.Start();
	if (started.Code() != EEditorAppLifecycleResultCode::Started || started.State() != EEditorAppLifecycleState::Running) return false;
	const auto stopped = lifecycle.Stop();
	return stopped.Code() == EEditorAppLifecycleResultCode::Stopped && stopped.State() == EEditorAppLifecycleState::Stopped
		&& lifecycle.ActivePhaseCount() == 0
		&& HasTrace(trace, { "start", "start", "start", "finalize", "finalize", "finalize" });
}

bool PhaseFailureRollsBackEnteredPhases()
{
	std::vector<std::string> trace;
	EditorAppLifecycle lifecycle({
		Phase(EEditorAppLifecyclePhase::ProfileResolution, trace),
		Phase(EEditorAppLifecyclePhase::PlatformServices, trace, EEditorAppLifecyclePhaseOutcome::Failed),
		Phase(EEditorAppLifecyclePhase::WorkbenchCreation, trace),
	});
	const auto result = lifecycle.Start();
	return result.Code() == EEditorAppLifecycleResultCode::PhaseFailed
		&& result.Phase() == EEditorAppLifecyclePhase::PlatformServices
		&& result.State() == EEditorAppLifecycleState::Stopped
		&& lifecycle.ActivePhaseCount() == 0
		&& HasTrace(trace, { "start", "start", "finalize", "finalize" });
}

bool CancelledAndTimedOutStartsReachStopped()
{
	std::vector<std::string> cancelTrace;
	EditorAppLifecycle cancelled({
		Phase(EEditorAppLifecyclePhase::ProfileResolution, cancelTrace,
			EEditorAppLifecyclePhaseOutcome::Cancelled),
	});
	const auto cancelledResult = cancelled.Start();
	if (cancelledResult.Code() != EEditorAppLifecycleResultCode::Cancelled
		|| cancelledResult.State() != EEditorAppLifecycleState::Stopped
		|| !HasTrace(cancelTrace, { "start", "finalize" })) return false;

	std::vector<std::string> timeoutTrace;
	EditorAppLifecycle timedOut({
		Phase(EEditorAppLifecyclePhase::ProfileResolution, timeoutTrace,
			EEditorAppLifecyclePhaseOutcome::TimedOut),
	});
	const auto timeoutResult = timedOut.Start();
	return timeoutResult.Code() == EEditorAppLifecycleResultCode::TimedOut
		&& timeoutResult.State() == EEditorAppLifecycleState::Stopped
		&& HasTrace(timeoutTrace, { "start", "finalize" });
}

bool RepeatedStopIsIdempotent()
{
	std::vector<std::string> trace;
	EditorAppLifecycle lifecycle({ Phase(EEditorAppLifecyclePhase::ProfileResolution, trace) });
	if (lifecycle.Start().Code() != EEditorAppLifecycleResultCode::Started) return false;
	if (lifecycle.Stop().Code() != EEditorAppLifecycleResultCode::Stopped) return false;
	return lifecycle.Stop().Code() == EEditorAppLifecycleResultCode::AlreadyStopped
		&& HasTrace(trace, { "start", "finalize" });
}

bool CallbackStopIsDeferredAndOwnedByStart()
{
	std::vector<std::string> trace;
	EditorAppLifecycle* lifecycle = nullptr;
	EEditorAppLifecycleResultCode callbackResult = EEditorAppLifecycleResultCode::UnexpectedFailure;
	EditorAppLifecycle instance({
		EditorAppLifecyclePhaseDefinition(
			EEditorAppLifecyclePhase::ProfileResolution,
			[&] {
				trace.emplace_back("start");
				callbackResult = lifecycle->Stop().Code();
				return EditorAppLifecyclePhaseResult{ EEditorAppLifecyclePhaseOutcome::Succeeded };
			},
			[&] {
				trace.emplace_back("finalize");
				return EditorAppLifecycleFinalizationResult{ EEditorAppLifecycleFinalizationOutcome::Succeeded };
			}),
		Phase(EEditorAppLifecyclePhase::PlatformServices, trace),
	});
	lifecycle = &instance;
	const auto result = instance.Start();
	return callbackResult == EEditorAppLifecycleResultCode::StopDeferred
		&& result.Code() == EEditorAppLifecycleResultCode::Cancelled
		&& result.State() == EEditorAppLifecycleState::Stopped
		&& HasTrace(trace, { "start", "finalize" });
}

bool FinalizeCallbackStopDoesNotReenter()
{
	EditorAppLifecycle* lifecycle = nullptr;
	EEditorAppLifecycleResultCode callbackResult = EEditorAppLifecycleResultCode::UnexpectedFailure;
	EditorAppLifecycle instance({
		EditorAppLifecyclePhaseDefinition(
			EEditorAppLifecyclePhase::ProfileResolution,
			[] { return EditorAppLifecyclePhaseResult{ EEditorAppLifecyclePhaseOutcome::Succeeded }; },
			[&] {
				callbackResult = lifecycle->Stop().Code();
				return EditorAppLifecycleFinalizationResult{ EEditorAppLifecycleFinalizationOutcome::Succeeded };
			}),
	});
	lifecycle = &instance;
	if (instance.Start().Code() != EEditorAppLifecycleResultCode::Started) return false;
	const auto stopped = instance.Stop();
	return callbackResult == EEditorAppLifecycleResultCode::StopInProgress
		&& stopped.Code() == EEditorAppLifecycleResultCode::Stopped
		&& instance.State() == EEditorAppLifecycleState::Stopped;
}

bool ForcedShutdownFinalizesEveryOwner()
{
	std::vector<std::string> trace;
	EditorAppLifecycle lifecycle({
		Phase(EEditorAppLifecyclePhase::ProfileResolution, trace),
		Phase(EEditorAppLifecyclePhase::PlatformServices, trace,
			EEditorAppLifecyclePhaseOutcome::Succeeded, EEditorAppLifecycleFinalizationOutcome::TimedOut),
	});
	if (lifecycle.Start().Code() != EEditorAppLifecycleResultCode::Started) return false;
	const auto result = lifecycle.Stop();
	return result.Code() == EEditorAppLifecycleResultCode::ForcedShutdown && result.ForcedShutdown()
		&& result.State() == EEditorAppLifecycleState::Stopped
		&& HasTrace(trace, { "start", "start", "finalize", "finalize" });
}

class TestCase final {
public:
	constexpr TestCase(std::string_view name, bool (*run)()) noexcept
		: m_name(name)
		, m_run(run)
	{
	}

	[[nodiscard]] constexpr std::string_view Name() const noexcept { return m_name; }
	[[nodiscard]] bool Run() const { return m_run(); }

private:
	const std::string_view m_name;
	bool (*const m_run)();
};

constexpr std::array kTests{
	TestCase{"SuccessAndReverseStop", SuccessAndReverseStop},
	TestCase{"PhaseFailureRollsBackEnteredPhases", PhaseFailureRollsBackEnteredPhases},
	TestCase{"CancelledAndTimedOutStartsReachStopped", CancelledAndTimedOutStartsReachStopped},
	TestCase{"RepeatedStopIsIdempotent", RepeatedStopIsIdempotent},
	TestCase{"CallbackStopIsDeferredAndOwnedByStart", CallbackStopIsDeferredAndOwnedByStart},
	TestCase{"FinalizeCallbackStopDoesNotReenter", FinalizeCallbackStopDoesNotReenter},
	TestCase{"ForcedShutdownFinalizesEveryOwner", ForcedShutdownFinalizesEveryOwner},
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
	std::string_view filter = "EditorAppLifecycle.*";
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument = argv[index];
		if (argument == "--gtest_list_tests") {
			std::cout << "EditorAppLifecycle.\n";
			for (const auto& test : kTests) std::cout << "  " << test.Name() << '\n';
			return 0;
		}
		constexpr std::string_view prefix = "--gtest_filter=";
		if (argument.starts_with(prefix)) filter = argument.substr(prefix.size());
	}

	int selected = 0;
	int failed = 0;
	for (const auto& test : kTests) {
		const std::string fullName = "EditorAppLifecycle." + std::string(test.Name());
		if (!Matches(fullName, filter)) continue;
		++selected;
		const bool passed = test.Run();
		std::cout << (passed ? "[       OK ] " : "[  FAILED  ] ") << fullName << '\n';
		if (!passed) ++failed;
	}
	std::cout << "[==========] " << selected << " tests ran; " << failed << " failed.\n";
	return failed == 0 && selected > 0 ? 0 : 1;
}
