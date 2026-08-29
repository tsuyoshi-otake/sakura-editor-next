/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "terminal/tmux/TmuxCommandDispatcher.h"

#include "terminal/tmux/TmuxTargetResolver.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <utility>

namespace terminal::tmux {
namespace {

[[nodiscard]] TmuxCommandResult Result(TmuxCommandResultCode code, std::string_view diagnostic = {})
{
	TmuxCommandResult result;
	result.code = code;
	result.diagnosticCode.assign(diagnostic.data(), diagnostic.size());
	return result;
}

[[nodiscard]] TmuxCommandResult MapRuntime(const TmuxRuntimeResult& runtime)
{
	TmuxCommandResult result;
	result.sessionId = runtime.sessionId;
	result.windowId = runtime.windowId;
	result.paneId = runtime.paneId;
	switch (runtime.code) {
	case TmuxRuntimeCode::Succeeded: result.code = TmuxCommandResultCode::Succeeded; break;
	case TmuxRuntimeCode::InvalidRequest: result.code = TmuxCommandResultCode::InvalidUsage; break;
	case TmuxRuntimeCode::TargetMissing: result.code = TmuxCommandResultCode::TargetMissing; break;
	case TmuxRuntimeCode::TopologyChanged: result.code = TmuxCommandResultCode::TopologyChanged; break;
	case TmuxRuntimeCode::Denied: result.code = TmuxCommandResultCode::Denied; break;
	case TmuxRuntimeCode::ResourceExhausted:
	case TmuxRuntimeCode::IdentityExhausted: result.code = TmuxCommandResultCode::ResourceExhausted; break;
	case TmuxRuntimeCode::DeadlineExceeded: result.code = TmuxCommandResultCode::DeadlineExceeded; break;
	case TmuxRuntimeCode::Ambiguous: result.code = TmuxCommandResultCode::Ambiguous; break;
	case TmuxRuntimeCode::NotRunning:
	case TmuxRuntimeCode::Unavailable:
	case TmuxRuntimeCode::Stopped: result.code = TmuxCommandResultCode::Unavailable; break;
	case TmuxRuntimeCode::Unsupported: result.code = TmuxCommandResultCode::UnsupportedSurface; break;
	}
	return result;
}

[[nodiscard]] TmuxCommandResult MapTarget(const TmuxTargetResult& target)
{
	switch (target.code) {
	case TmuxTargetCode::Succeeded: return Result(TmuxCommandResultCode::Succeeded);
	case TmuxTargetCode::TargetMissing: return Result(TmuxCommandResultCode::TargetMissing, "target-missing");
	case TmuxTargetCode::Ambiguous: return Result(TmuxCommandResultCode::Ambiguous, "ambiguous-target");
	case TmuxTargetCode::InvalidSyntax: return Result(TmuxCommandResultCode::InvalidUsage, "invalid-target");
	}
	return Result(TmuxCommandResultCode::InternalError, "internal-error");
}

[[nodiscard]] bool ParseSigned(std::string_view value, std::int64_t& output) noexcept
{
	if (value.empty()) return false;
	const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
	return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

[[nodiscard]] bool ParseUnsigned(std::string_view value, std::uint64_t& output) noexcept
{
	if (value.empty()) return false;
	for (const auto ch : value) if (ch < '0' || ch > '9') return false;
	const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
	return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

[[nodiscard]] const TmuxSessionView* FindSession(const TmuxRuntimeSnapshot& snapshot, TerminalSessionId id) noexcept
{
	for (const auto& session : snapshot.sessions) if (session.id == id) return &session;
	return nullptr;
}

[[nodiscard]] const TmuxWindowView* FindWindow(const TmuxRuntimeSnapshot& snapshot,
	TerminalSessionId sessionId, TerminalWindowId id) noexcept
{
	const auto* session = FindSession(snapshot, sessionId);
	if (!session) return nullptr;
	for (const auto& window : session->windows) if (window.id == id) return &window;
	return nullptr;
}

[[nodiscard]] const TmuxPaneView* FindPane(const TmuxRuntimeSnapshot& snapshot,
	TerminalSessionId sessionId, TerminalWindowId windowId, TerminalPaneId id) noexcept
{
	const auto* window = FindWindow(snapshot, sessionId, windowId);
	if (!window) return nullptr;
	for (const auto& pane : window->panes) if (pane.id == id) return &pane;
	return nullptr;
}

[[nodiscard]] TmuxFormatContext Context(const TmuxRuntimeSnapshot& snapshot,
	TerminalSessionId sessionId, TerminalWindowId windowId, std::optional<TerminalPaneId> paneId = std::nullopt) noexcept
{
	TmuxFormatContext context;
	context.session = FindSession(snapshot, sessionId);
	context.window = FindWindow(snapshot, sessionId, windowId);
	if (paneId) context.pane = FindPane(snapshot, sessionId, windowId, *paneId);
	return context;
}

[[nodiscard]] bool AppendLine(std::string& output, std::string_view value, std::size_t maximum) noexcept
{
	if (value.size() > maximum || output.size() > maximum - value.size()) return false;
	output.append(value.data(), value.size());
	if (output.empty() || output.back() != '\n') {
		if (output.size() == maximum) return false;
		output.push_back('\n');
	}
	return true;
}

[[nodiscard]] bool AppendIdLine(std::string& output, char prefix, std::uint64_t value, std::size_t maximum) noexcept
{
	const auto text = std::string(1, prefix) + std::to_string(value);
	return AppendLine(output, text, maximum);
}

[[nodiscard]] TmuxCommandResult RenderFormat(std::string& output, std::string_view format,
	const TmuxFormatContext& context, const TmuxFormatLimits& limits, std::size_t maximum)
{
	const auto rendered = TmuxFormatEvaluator::Evaluate(format, context, limits);
	if (!rendered.Succeeded()) return Result(rendered.code == TmuxFormatCode::ResourceExhausted
		? TmuxCommandResultCode::ResourceExhausted : TmuxCommandResultCode::InvalidUsage, "format-error");
	if (!AppendLine(output, rendered.value, maximum)) return Result(TmuxCommandResultCode::ResourceExhausted, "output-limit");
	return Result(TmuxCommandResultCode::Succeeded);
}

[[nodiscard]] bool SameTarget(const TmuxResolvedTarget& left, const TmuxResolvedTarget& right) noexcept
{
	return left.sessionId == right.sessionId && left.windowId == right.windowId && left.paneId == right.paneId;
}

[[nodiscard]] TmuxCommandResult RevalidateTarget(ITmuxRuntimePort& runtime,
	TmuxResolvedTarget& target, TmuxTargetKind kind, const std::optional<std::string>& selector)
{
	const auto latest = runtime.Snapshot();
	if (latest.revision == target.revision) return Result(TmuxCommandResultCode::Succeeded);
	if (!target.usedStableIdentity) return Result(TmuxCommandResultCode::TopologyChanged, "topology-changed");
	const auto resolved = TmuxTargetResolver::Resolve(latest, kind,
		selector ? std::optional<std::string_view>(*selector) : std::nullopt);
	if (!resolved.Succeeded()) return MapTarget(resolved);
	if (!SameTarget(target, resolved.target)) return Result(TmuxCommandResultCode::TargetMissing, "target-missing");
	target = resolved.target;
	return Result(TmuxCommandResultCode::Succeeded);
}

} // namespace

TmuxCommandDispatcher::TmuxCommandDispatcher(ITmuxRuntimePort& runtime,
	TmuxCompatibilityProfile profile, TmuxDispatcherLimits limits) noexcept
	: m_runtime(runtime), m_profile(std::move(profile)), m_limits(std::move(limits))
{
}

TmuxCommandResult TmuxCommandDispatcher::Dispatch(const TmuxCommand& command) noexcept
{
	try {
		if (command.kind == TmuxCommandKind::Version) {
			TmuxCommandResult result = Result(TmuxCommandResultCode::Succeeded);
			result.stdoutText.assign(kTmuxCompatibilityVersion.data(), kTmuxCompatibilityVersion.size());
			result.stdoutText.push_back('\n');
			return result;
		}
		const auto snapshot = m_runtime.Snapshot();
		switch (command.kind) {
		case TmuxCommandKind::ListPanes:
		case TmuxCommandKind::ListSessions:
		case TmuxCommandKind::ListWindows: return DispatchList(command, snapshot);
		case TmuxCommandKind::SendKeys: return DispatchSend(command, snapshot);
		case TmuxCommandKind::CapturePane: return DispatchCapture(command, snapshot);
		case TmuxCommandKind::DisplayMessage: return DispatchDisplay(command, snapshot);
		case TmuxCommandKind::NewSession:
		case TmuxCommandKind::NewWindow:
		case TmuxCommandKind::SplitWindow:
		case TmuxCommandKind::SelectWindow:
		case TmuxCommandKind::SelectPane:
		case TmuxCommandKind::HasSession:
		case TmuxCommandKind::KillPane:
		case TmuxCommandKind::KillWindow:
		case TmuxCommandKind::KillSession: return DispatchTopology(command, snapshot);
		case TmuxCommandKind::WaitFor: return DispatchWait(command, snapshot);
		case TmuxCommandKind::Version: break;
		}
		return Result(TmuxCommandResultCode::InternalError, "internal-error");
	} catch (...) {
		return Result(TmuxCommandResultCode::InternalError, "internal-error");
	}
}

TmuxCommandResult TmuxCommandDispatcher::DispatchList(const TmuxCommand& command,
	const TmuxRuntimeSnapshot& snapshot)
{
	if (!command.operands.empty()) return Result(TmuxCommandResultCode::UnsupportedSurface, "command-tail-unsupported");
	std::string output;
	const auto format = command.format ? *command.format
		: command.kind == TmuxCommandKind::ListSessions ? m_profile.listSessionsFormat
		: command.kind == TmuxCommandKind::ListWindows ? m_profile.listWindowsFormat : m_profile.listPanesFormat;
	if (command.kind == TmuxCommandKind::ListSessions) {
		for (const auto& session : snapshot.sessions) {
			const TmuxFormatContext context{ &session, nullptr, nullptr };
			if (command.filter) {
				const auto filter = TmuxFormatEvaluator::EvaluateFilter(*command.filter, context, m_limits.formatLimits);
				if (!filter.Succeeded()) return Result(TmuxCommandResultCode::InvalidUsage, "filter-error");
				if (!filter.value) continue;
			}
			const auto result = RenderFormat(output, format, context, m_limits.formatLimits, m_limits.maximumOutputBytes);
			if (!result.Succeeded()) return result;
		}
	} else if (command.kind == TmuxCommandKind::ListWindows) {
		std::vector<std::pair<const TmuxSessionView*, const TmuxWindowView*>> windows;
		if (command.all) {
			for (const auto& session : snapshot.sessions) for (const auto& window : session.windows) windows.emplace_back(&session, &window);
		} else {
			const auto target = TmuxTargetResolver::Resolve(snapshot, TmuxTargetKind::Window, command.target
				? std::optional<std::string_view>(*command.target) : std::nullopt);
			if (!target.Succeeded()) return MapTarget(target);
			const auto* session = FindSession(snapshot, target.target.sessionId);
			if (!session) return Result(TmuxCommandResultCode::TargetMissing, "target-missing");
			if (command.target && command.target->find('.') != std::string::npos) return Result(TmuxCommandResultCode::InvalidUsage, "invalid-target");
			for (const auto& window : session->windows) windows.emplace_back(session, &window);
		}
		for (const auto& [session, window] : windows) {
			const TmuxFormatContext context{ session, window, nullptr };
			if (command.filter) {
				const auto filter = TmuxFormatEvaluator::EvaluateFilter(*command.filter, context, m_limits.formatLimits);
				if (!filter.Succeeded()) return Result(TmuxCommandResultCode::InvalidUsage, "filter-error");
				if (!filter.value) continue;
			}
			const auto result = RenderFormat(output, format, context, m_limits.formatLimits, m_limits.maximumOutputBytes);
			if (!result.Succeeded()) return result;
		}
	} else {
		std::vector<std::pair<const TmuxSessionView*, const TmuxWindowView*>> windows;
		if (command.all || command.sessionsOnly) {
			for (const auto& session : snapshot.sessions) for (const auto& window : session.windows) windows.emplace_back(&session, &window);
		} else {
			const auto target = TmuxTargetResolver::Resolve(snapshot, TmuxTargetKind::Window, command.target
				? std::optional<std::string_view>(*command.target) : std::nullopt);
			if (!target.Succeeded()) return MapTarget(target);
			const auto* session = FindSession(snapshot, target.target.sessionId);
			const auto* window = FindWindow(snapshot, target.target.sessionId, target.target.windowId);
			if (!session || !window) return Result(TmuxCommandResultCode::TargetMissing, "target-missing");
			windows.emplace_back(session, window);
		}
		for (const auto& [session, window] : windows) for (const auto& pane : window->panes) {
			const TmuxFormatContext context{ session, window, &pane };
			if (command.filter) {
				const auto filter = TmuxFormatEvaluator::EvaluateFilter(*command.filter, context, m_limits.formatLimits);
				if (!filter.Succeeded()) return Result(TmuxCommandResultCode::InvalidUsage, "filter-error");
				if (!filter.value) continue;
			}
			const auto result = RenderFormat(output, format, context, m_limits.formatLimits, m_limits.maximumOutputBytes);
			if (!result.Succeeded()) return result;
		}
	}
	TmuxCommandResult result = Result(TmuxCommandResultCode::Succeeded);
	result.stdoutText = std::move(output);
	return result;
}

TmuxCommandResult TmuxCommandDispatcher::DispatchSend(const TmuxCommand& command,
	const TmuxRuntimeSnapshot& snapshot)
{
	auto target = TmuxTargetResolver::Resolve(snapshot, TmuxTargetKind::Pane, command.target
		? std::optional<std::string_view>(*command.target) : std::nullopt);
	if (!target.Succeeded()) return MapTarget(target);
	auto resolvedTarget = target.target;
	const auto revalidated = RevalidateTarget(m_runtime, resolvedTarget, TmuxTargetKind::Pane, command.target);
	if (!revalidated.Succeeded()) return revalidated;
	std::uint64_t repeat = 1;
	if (command.length && (!ParseUnsigned(*command.length, repeat) || repeat == 0 || repeat > 100)) {
		return Result(TmuxCommandResultCode::InvalidUsage, "invalid-repeat");
	}
	TmuxInputBatch batch;
	batch.target = resolvedTarget;
	batch.repeatCount = static_cast<std::uint16_t>(repeat);
	batch.expectedRevision = resolvedTarget.revision;
	std::size_t inputBytes = 0;
	for (const auto& operand : command.operands) {
		if (operand.size() > m_limits.maximumInputBytes - std::min(inputBytes, m_limits.maximumInputBytes)) {
			return Result(TmuxCommandResultCode::ResourceExhausted, "input-limit");
		}
		inputBytes += operand.size();
	}
	if (inputBytes > m_limits.maximumInputBytes / repeat) return Result(TmuxCommandResultCode::ResourceExhausted, "input-limit");
	if (command.literal) {
		std::string literalText;
		literalText.reserve(inputBytes);
		for (const auto& operand : command.operands) literalText += operand;
		if (!literalText.empty()) batch.tokens.push_back(TmuxInputToken{ TmuxInputTokenKind::LiteralText, std::move(literalText) });
	} else for (const auto& operand : command.operands) {
		const bool named =
			((operand == "Enter") || (operand == "Escape") || (operand == "Tab") || (operand == "BSpace") ||
			(operand == "Space") || (operand == "Up") || (operand == "Down") || (operand == "Left") ||
			(operand == "Right") || (operand == "Home") || (operand == "End") || (operand == "PageUp") ||
			(operand == "PageDown") || (operand == "Insert") || (operand == "Delete") ||
			(operand.size() == 2 && operand[0] == 'F' && operand[1] >= '1' && operand[1] <= '9') ||
			(operand == "F10") || (operand == "F11") || (operand == "F12") ||
			(operand.size() == 3 && operand[0] == 'C' && operand[1] == '-' &&
				((operand[2] >= 'a' && operand[2] <= 'z') || operand[2] == '@' || operand[2] == '[' ||
					operand[2] == '\\' || operand[2] == ']' || operand[2] == '^' || operand[2] == '_' || operand[2] == '?')));
		batch.tokens.push_back(TmuxInputToken{ named ? TmuxInputTokenKind::NamedKey : TmuxInputTokenKind::LiteralText, operand });
	}
	return MapRuntime(m_runtime.SendKeys(batch));
}

TmuxCommandResult TmuxCommandDispatcher::DispatchCapture(const TmuxCommand& command,
	const TmuxRuntimeSnapshot& snapshot)
{
	if (!command.operands.empty()) return Result(TmuxCommandResultCode::UnsupportedSurface, "command-tail-unsupported");
	auto target = TmuxTargetResolver::Resolve(snapshot, TmuxTargetKind::Pane, command.target
		? std::optional<std::string_view>(*command.target) : std::nullopt);
	if (!target.Succeeded()) return MapTarget(target);
	auto resolvedTarget = target.target;
	const auto revalidated = RevalidateTarget(m_runtime, resolvedTarget, TmuxTargetKind::Pane, command.target);
	if (!revalidated.Succeeded()) return revalidated;
	TmuxCaptureRequest request;
	request.target = resolvedTarget;
	request.expectedRevision = resolvedTarget.revision;
	request.joinWrapped = command.joinWrapped;
	if (command.startLine) {
		if (*command.startLine == "-") request.startAtHistoryBeginning = true;
		else if (!ParseSigned(*command.startLine, request.startLine.emplace())) return Result(TmuxCommandResultCode::InvalidUsage, "invalid-start");
	}
	if (command.endLine) {
		if (*command.endLine == "-") request.endAtScreenEnd = true;
		else if (!ParseSigned(*command.endLine, request.endLine.emplace())) return Result(TmuxCommandResultCode::InvalidUsage, "invalid-end");
	}
	if (request.startLine && request.endLine && *request.startLine > *request.endLine) return Result(TmuxCommandResultCode::InvalidUsage, "invalid-range");
	const auto capture = m_runtime.CapturePane(request);
	if (capture.code != TmuxRuntimeCode::Succeeded) return MapRuntime(TmuxRuntimeResult{ capture.code, snapshot.revision });
	if (!capture.complete || capture.gap || capture.truncated) return Result(TmuxCommandResultCode::ResourceExhausted, capture.gap ? "capture-gap" : "capture-truncated");
	TmuxCommandResult result = Result(TmuxCommandResultCode::Succeeded);
	for (const auto& line : capture.lines) if (!AppendLine(result.stdoutText, line.text, m_limits.maximumOutputBytes)) {
		return Result(TmuxCommandResultCode::ResourceExhausted, "output-limit");
	}
	return result;
}

TmuxCommandResult TmuxCommandDispatcher::DispatchDisplay(const TmuxCommand& command,
	const TmuxRuntimeSnapshot& snapshot)
{
	if (command.operands.size() > 1) return Result(TmuxCommandResultCode::UnsupportedSurface, "unsupported-operand");
	auto target = TmuxTargetResolver::Resolve(snapshot, TmuxTargetKind::Pane, command.target
		? std::optional<std::string_view>(*command.target) : std::nullopt);
	if (!target.Succeeded()) return MapTarget(target);
	const auto format = command.format ? *command.format : command.operands.empty() ? m_profile.displayFormat : command.operands.front();
	const auto context = Context(snapshot, target.target.sessionId, target.target.windowId, target.target.paneId);
	TmuxCommandResult result = Result(TmuxCommandResultCode::Succeeded);
	const auto rendered = TmuxFormatEvaluator::Evaluate(format, context, m_limits.formatLimits);
	if (!rendered.Succeeded()) return Result(rendered.code == TmuxFormatCode::ResourceExhausted
		? TmuxCommandResultCode::ResourceExhausted : TmuxCommandResultCode::InvalidUsage, "format-error");
	if (!AppendLine(result.stdoutText, rendered.value, m_limits.maximumOutputBytes)) return Result(TmuxCommandResultCode::ResourceExhausted, "output-limit");
	return result;
}

TmuxCommandResult TmuxCommandDispatcher::DispatchTopology(const TmuxCommand& command,
	const TmuxRuntimeSnapshot& snapshot)
{
	if (!command.operands.empty()) return Result(TmuxCommandResultCode::UnsupportedSurface, "command-tail-unsupported");
	if (command.kind == TmuxCommandKind::HasSession && !command.target) return Result(TmuxCommandResultCode::InvalidUsage, "missing-target");
	if (command.kind == TmuxCommandKind::NewSession) {
		if (command.sessionName && command.sessionName->empty()) return Result(TmuxCommandResultCode::InvalidUsage, "invalid-session-name");
		return MapRuntime(m_runtime.CreateSession(TmuxCreateSessionRequest{
			command.sessionName.value_or(std::string{}), command.directory.value_or(std::string{}), true, snapshot.revision }));
	}
	if (command.kind == TmuxCommandKind::NewWindow) {
		auto target = TmuxTargetResolver::Resolve(snapshot, TmuxTargetKind::Window, command.target
			? std::optional<std::string_view>(*command.target) : std::nullopt);
		if (!target.Succeeded()) return MapTarget(target);
		auto resolvedTarget = target.target;
		const auto revalidated = RevalidateTarget(m_runtime, resolvedTarget, TmuxTargetKind::Window, command.target);
		if (!revalidated.Succeeded()) return revalidated;
		const auto runtime = m_runtime.CreateTerminalWindow(TmuxCreateWindowRequest{
			resolvedTarget.sessionId, command.windowName.value_or(std::string{}), command.directory.value_or(std::string{}), command.detached, resolvedTarget.revision });
		auto result = MapRuntime(runtime);
		if (result.Succeeded() && command.printResult && result.windowId) {
			if (command.format) {
				const auto after = m_runtime.Snapshot();
				const auto context = Context(after, resolvedTarget.sessionId, *result.windowId);
				const auto rendered = TmuxFormatEvaluator::Evaluate(*command.format, context, m_limits.formatLimits);
				if (!rendered.Succeeded() || !AppendLine(result.stdoutText, rendered.value, m_limits.maximumOutputBytes)) return Result(TmuxCommandResultCode::InvalidUsage, "format-error");
			} else if (!AppendIdLine(result.stdoutText, '@', result.windowId->value, m_limits.maximumOutputBytes)) return Result(TmuxCommandResultCode::ResourceExhausted, "output-limit");
		}
		return result;
	}
	if (command.kind == TmuxCommandKind::SplitWindow) {
		auto target = TmuxTargetResolver::Resolve(snapshot, TmuxTargetKind::Pane, command.target
			? std::optional<std::string_view>(*command.target) : std::nullopt);
		if (!target.Succeeded()) return MapTarget(target);
		auto resolvedTarget = target.target;
		const auto revalidated = RevalidateTarget(m_runtime, resolvedTarget, TmuxTargetKind::Pane, command.target);
		if (!revalidated.Succeeded()) return revalidated;
		if (command.length && command.percentage) return Result(TmuxCommandResultCode::InvalidUsage, "conflicting-size");
		TmuxSplitWindowRequest request{ resolvedTarget,
			command.vertical ? TerminalPaneOrientation::Vertical : TerminalPaneOrientation::Horizontal,
			command.directory, command.length, command.percentage, command.detached, resolvedTarget.revision };
		auto result = MapRuntime(m_runtime.SplitWindow(request));
		if (result.Succeeded() && command.printResult && result.paneId) {
			if (command.format) {
				const auto after = m_runtime.Snapshot();
				const auto context = Context(after, resolvedTarget.sessionId, resolvedTarget.windowId, result.paneId);
				const auto rendered = TmuxFormatEvaluator::Evaluate(*command.format, context, m_limits.formatLimits);
				if (!rendered.Succeeded() || !AppendLine(result.stdoutText, rendered.value, m_limits.maximumOutputBytes)) return Result(TmuxCommandResultCode::InvalidUsage, "format-error");
			} else if (!AppendIdLine(result.stdoutText, '%', result.paneId->value, m_limits.maximumOutputBytes)) return Result(TmuxCommandResultCode::ResourceExhausted, "output-limit");
		}
		return result;
	}

	const TmuxTargetKind targetKind = command.kind == TmuxCommandKind::SelectWindow || command.kind == TmuxCommandKind::KillWindow
		? TmuxTargetKind::Window : command.kind == TmuxCommandKind::SelectPane || command.kind == TmuxCommandKind::KillPane
		? TmuxTargetKind::Pane : TmuxTargetKind::Session;
	auto target = TmuxTargetResolver::Resolve(snapshot, targetKind, command.target
		? std::optional<std::string_view>(*command.target) : std::nullopt);
	if (!target.Succeeded()) return MapTarget(target);
	auto resolvedTarget = target.target;
	const auto revalidated = RevalidateTarget(m_runtime, resolvedTarget, targetKind, command.target);
	if (!revalidated.Succeeded()) return revalidated;
	TmuxRuntimeResult runtime;
	if (command.kind == TmuxCommandKind::SelectWindow) runtime = m_runtime.SelectWindow(TmuxSelectRequest{ resolvedTarget, resolvedTarget.revision });
	else if (command.kind == TmuxCommandKind::SelectPane) runtime = m_runtime.SelectPane(TmuxSelectRequest{ resolvedTarget, resolvedTarget.revision });
	else if (command.kind == TmuxCommandKind::KillPane) runtime = m_runtime.ClosePane(TmuxCloseRequest{ resolvedTarget, resolvedTarget.revision });
	else if (command.kind == TmuxCommandKind::KillWindow) runtime = m_runtime.CloseWindow(TmuxCloseRequest{ resolvedTarget, resolvedTarget.revision });
	else if (command.kind == TmuxCommandKind::KillSession) runtime = m_runtime.CloseSession(TmuxCloseRequest{ resolvedTarget, resolvedTarget.revision });
	else return Result(TmuxCommandResultCode::InternalError, "internal-error");
	return MapRuntime(runtime);
}

TmuxCommandResult TmuxCommandDispatcher::DispatchWait(const TmuxCommand& command,
	const TmuxRuntimeSnapshot& snapshot)
{
	if (!command.channel || command.channel->empty()) return Result(TmuxCommandResultCode::InvalidUsage, "missing-channel");
	const auto operation = command.signal ? TmuxWaitOperation::Signal : command.lock ? TmuxWaitOperation::Lock
		: command.unlock ? TmuxWaitOperation::Unlock : TmuxWaitOperation::Wait;
	TmuxWaitRequest request{ operation, *command.channel, std::chrono::steady_clock::now() + m_limits.maximumWait, snapshot.revision };
	return MapRuntime(m_runtime.WaitFor(request));
}

} // namespace terminal::tmux
