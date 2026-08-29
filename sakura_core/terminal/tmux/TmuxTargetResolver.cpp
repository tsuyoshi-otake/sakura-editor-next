/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "terminal/tmux/TmuxTargetResolver.h"

#include <algorithm>
#include <charconv>
#include <limits>

namespace terminal::tmux {
namespace {

struct ParsedSelector final {
	char prefix{};
	std::string_view value;
	bool stable{};
};

[[nodiscard]] TmuxTargetResult Error(TmuxTargetCode code, std::string_view diagnostic) noexcept
{
	TmuxTargetResult result;
	result.code = code;
	result.diagnosticCode.assign(diagnostic.data(), diagnostic.size());
	return result;
}

[[nodiscard]] bool IsCanonicalDecimal(std::string_view value, std::uint64_t& output) noexcept
{
	if (value.empty() || (value.size() > 1 && value.front() == '0')) return false;
	for (const auto ch : value) if (ch < '0' || ch > '9') return false;
	const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
	return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

[[nodiscard]] bool ParseSelector(std::string_view text, ParsedSelector& result) noexcept
{
	if (text.empty()) return false;
	if (text.front() == '$' || text.front() == '@' || text.front() == '%') {
		result.prefix = text.front();
		result.value = text.substr(1);
		result.stable = true;
		std::uint64_t ignored{};
		return IsCanonicalDecimal(result.value, ignored) && ignored != 0;
	}
	if (text.front() == '{' || text.front() == '!' || text.front() == '^' || text.front() == '+' || text.front() == '-') {
		return false;
	}
	result.value = text;
	result.stable = false;
	return true;
}

template<typename Id>
[[nodiscard]] bool ParseId(std::string_view text, Id& output) noexcept
{
	std::uint64_t value{};
	if (!IsCanonicalDecimal(text, value) || value == 0) return false;
	output.value = value;
	return true;
}

[[nodiscard]] const TmuxSessionView* ActiveSession(const TmuxRuntimeSnapshot& snapshot) noexcept
{
	if (snapshot.activeSession) {
		for (const auto& session : snapshot.sessions) if (session.id == *snapshot.activeSession) return &session;
		return nullptr;
	}
	for (const auto& session : snapshot.sessions) if (session.active) return &session;
	return nullptr;
}

[[nodiscard]] const TmuxSessionView* FindSession(const TmuxRuntimeSnapshot& snapshot,
	const ParsedSelector& selector, TmuxTargetCode& code) noexcept
{
	const TmuxSessionView* found = nullptr;
	if (selector.prefix == '$') {
		TerminalSessionId id;
		if (!ParseId(selector.value, id)) {
			code = TmuxTargetCode::InvalidSyntax;
			return nullptr;
		}
		for (const auto& session : snapshot.sessions) if (session.id == id) {
			if (found) { code = TmuxTargetCode::Ambiguous; return nullptr; }
			found = &session;
		}
	} else {
		std::uint64_t index{};
		if (IsCanonicalDecimal(selector.value, index)) {
			for (const auto& session : snapshot.sessions) if (session.index == index) {
				if (found) { code = TmuxTargetCode::Ambiguous; return nullptr; }
				found = &session;
			}
		} else {
			for (const auto& session : snapshot.sessions) if (session.name == selector.value) {
				if (found) { code = TmuxTargetCode::Ambiguous; return nullptr; }
				found = &session;
			}
		}
	}
	code = found ? TmuxTargetCode::Succeeded : TmuxTargetCode::TargetMissing;
	return found;
}

[[nodiscard]] const TmuxWindowView* FindWindow(const TmuxSessionView& session,
	const ParsedSelector& selector, TmuxTargetCode& code) noexcept
{
	const TmuxWindowView* found = nullptr;
	if (selector.prefix == '@') {
		TerminalWindowId id;
		if (!ParseId(selector.value, id)) { code = TmuxTargetCode::InvalidSyntax; return nullptr; }
		for (const auto& window : session.windows) if (window.id == id) {
			if (found) { code = TmuxTargetCode::Ambiguous; return nullptr; }
			found = &window;
		}
	} else {
		std::uint64_t index{};
		if (IsCanonicalDecimal(selector.value, index)) {
			for (const auto& window : session.windows) if (window.index == index) {
				if (found) { code = TmuxTargetCode::Ambiguous; return nullptr; }
				found = &window;
			}
		} else {
			for (const auto& window : session.windows) if (window.name == selector.value) {
				if (found) { code = TmuxTargetCode::Ambiguous; return nullptr; }
				found = &window;
			}
		}
	}
	code = found ? TmuxTargetCode::Succeeded : TmuxTargetCode::TargetMissing;
	return found;
}

[[nodiscard]] const TmuxPaneView* FindPane(const TmuxWindowView& window,
	const ParsedSelector& selector, TmuxTargetCode& code) noexcept
{
	if (selector.prefix == '%') {
		TerminalPaneId id;
		if (!ParseId(selector.value, id)) { code = TmuxTargetCode::InvalidSyntax; return nullptr; }
		for (const auto& pane : window.panes) if (pane.id == id) {
			code = TmuxTargetCode::Succeeded;
			return &pane;
		}
		code = TmuxTargetCode::TargetMissing;
		return nullptr;
	}
	std::uint64_t index{};
	if (!IsCanonicalDecimal(selector.value, index)) {
		code = TmuxTargetCode::TargetMissing;
		return nullptr;
	}
	const TmuxPaneView* found = nullptr;
	for (const auto& pane : window.panes) if (pane.index == index) {
		if (found) { code = TmuxTargetCode::Ambiguous; return nullptr; }
		found = &pane;
	}
	code = found ? TmuxTargetCode::Succeeded : TmuxTargetCode::TargetMissing;
	return found;
}

[[nodiscard]] bool SplitTarget(std::string_view text, std::string_view& session,
	std::string_view& window, std::string_view& pane, bool& hasPane) noexcept
{
	const auto colon = text.find(':');
	if (colon == std::string_view::npos) {
		session = {};
		window = text;
		pane = {};
		hasPane = false;
		return true;
	}
	if (text.find(':', colon + 1) != std::string_view::npos) return false;
	session = text.substr(0, colon);
	const auto windowAndPane = text.substr(colon + 1);
	const auto dot = windowAndPane.find('.');
	if (dot == std::string_view::npos) {
		window = windowAndPane;
		pane = {};
		hasPane = false;
	} else {
		if (windowAndPane.find('.', dot + 1) != std::string_view::npos) return false;
		window = windowAndPane.substr(0, dot);
		pane = windowAndPane.substr(dot + 1);
		hasPane = true;
	}
	return !window.empty() && (!hasPane || !pane.empty());
}

[[nodiscard]] TmuxTargetResult MakeResult(const TmuxRuntimeSnapshot& snapshot,
	TmuxTargetKind kind, const TmuxSessionView& session, const TmuxWindowView& window,
	const TmuxPaneView* pane, bool stable) noexcept
{
	TmuxTargetResult result;
	result.code = TmuxTargetCode::Succeeded;
	result.target.kind = kind;
	result.target.revision = snapshot.revision;
	result.target.sessionId = session.id;
	result.target.windowId = window.id;
	result.target.usedStableIdentity = stable;
	if (pane) {
		result.target.paneId = pane->id;
		result.target.coordinate = pane->coordinate;
		result.target.coordinate.sessionId = session.id;
		result.target.coordinate.windowId = window.id;
		result.target.coordinate.paneId = pane->id;
		result.target.coordinate.instanceId = pane->instanceId;
	} else {
		result.target.coordinate.sessionId = session.id;
		result.target.coordinate.windowId = window.id;
	}
	return result;
}

} // namespace

TmuxTargetResult TmuxTargetResolver::Resolve(const TmuxRuntimeSnapshot& snapshot,
	const TmuxTargetKind kind, const std::optional<std::string_view> selector) noexcept
{
	try {
		if (snapshot.sessions.empty()) return Error(TmuxTargetCode::TargetMissing, "target-missing");
		const auto text = selector.value_or(std::string_view{});
		std::string_view sessionText;
		std::string_view windowText;
		std::string_view paneText;
		bool hasPane = false;
		if (selector && !SplitTarget(text, sessionText, windowText, paneText, hasPane)) {
			return Error(TmuxTargetCode::InvalidSyntax, "invalid-target");
		}
		const auto activeSession = ActiveSession(snapshot);
		const TmuxSessionView* session = nullptr;
		TmuxTargetCode code = TmuxTargetCode::Succeeded;
		bool stable = false;
		if (selector && !sessionText.empty()) {
			ParsedSelector parsed;
			if (!ParseSelector(sessionText, parsed)) return Error(TmuxTargetCode::InvalidSyntax, "invalid-session-target");
			session = FindSession(snapshot, parsed, code);
			stable = parsed.stable;
			if (!session) return Error(code, code == TmuxTargetCode::Ambiguous ? "ambiguous-target" : "target-missing");
		} else {
			session = activeSession;
			if (!session) return Error(TmuxTargetCode::TargetMissing, "target-missing");
		}

		if (kind == TmuxTargetKind::Session) {
			if (selector && (text.find(':') != std::string_view::npos || text.find('.') != std::string_view::npos)) {
				return Error(TmuxTargetCode::InvalidSyntax, "invalid-session-target");
			}
			if (!selector) stable = false;
			if (selector && sessionText.empty()) {
				ParsedSelector parsed;
				if (!ParseSelector(text, parsed)) return Error(TmuxTargetCode::InvalidSyntax, "invalid-session-target");
				session = FindSession(snapshot, parsed, code);
				stable = parsed.stable;
				if (!session) return Error(code, code == TmuxTargetCode::Ambiguous ? "ambiguous-target" : "target-missing");
			}
			TmuxTargetResult result;
			result.code = TmuxTargetCode::Succeeded;
			result.target.kind = kind;
			result.target.revision = snapshot.revision;
			result.target.sessionId = session->id;
			result.target.coordinate.sessionId = session->id;
			result.target.usedStableIdentity = stable;
			return result;
		}
		if (kind == TmuxTargetKind::Pane && sessionText.empty() && !hasPane && !text.empty() && text.front() != '@' && text.front() != '%') {
			ParsedSelector paneSelector;
			if (!ParseSelector(text, paneSelector)) return Error(TmuxTargetCode::InvalidSyntax, "invalid-pane-target");
			const auto activeWindow = std::find_if(session->windows.begin(), session->windows.end(), [](const auto& item) { return item.active; });
			if (activeWindow == session->windows.end()) return Error(TmuxTargetCode::TargetMissing, "target-missing");
			TmuxTargetCode paneCode;
			const auto pane = FindPane(*activeWindow, paneSelector, paneCode);
			if (!pane) return Error(paneCode, paneCode == TmuxTargetCode::Ambiguous ? "ambiguous-target" : "target-missing");
			return MakeResult(snapshot, kind, *session, *activeWindow, pane, false);
		}
		if (selector && sessionText.empty() && !hasPane && !text.empty() && text.front() == '$'
			&& (kind == TmuxTargetKind::Window || kind == TmuxTargetKind::Pane)) {
			ParsedSelector sessionSelector;
			if (!ParseSelector(text, sessionSelector)) return Error(TmuxTargetCode::InvalidSyntax, "invalid-session-target");
			TmuxTargetCode sessionCode;
			session = FindSession(snapshot, sessionSelector, sessionCode);
			if (!session) return Error(sessionCode, sessionCode == TmuxTargetCode::Ambiguous ? "ambiguous-target" : "target-missing");
			const auto activeWindow = std::find_if(session->windows.begin(), session->windows.end(),
				[](const auto& item) { return item.active; });
			if (activeWindow == session->windows.end()) return Error(TmuxTargetCode::TargetMissing, "target-missing");
			if (kind == TmuxTargetKind::Window) return MakeResult(snapshot, kind, *session, *activeWindow, nullptr, sessionSelector.stable);
			const auto activePane = std::find_if(activeWindow->panes.begin(), activeWindow->panes.end(),
				[](const auto& item) { return item.active; });
			if (activePane == activeWindow->panes.end()) return Error(TmuxTargetCode::TargetMissing, "target-missing");
			return MakeResult(snapshot, kind, *session, *activeWindow, &*activePane, sessionSelector.stable);
		}

		if (!selector) {
			if (!session->active || session->windows.empty()) return Error(TmuxTargetCode::TargetMissing, "target-missing");
			const auto activeWindow = std::find_if(session->windows.begin(), session->windows.end(),
				[](const auto& item) { return item.active; });
			if (activeWindow == session->windows.end()) return Error(TmuxTargetCode::TargetMissing, "target-missing");
			if (kind == TmuxTargetKind::Window) return MakeResult(snapshot, kind, *session, *activeWindow, nullptr, false);
			const auto activePane = std::find_if(activeWindow->panes.begin(), activeWindow->panes.end(),
				[](const auto& item) { return item.active; });
			if (activePane == activeWindow->panes.end()) return Error(TmuxTargetCode::TargetMissing, "target-missing");
			return MakeResult(snapshot, kind, *session, *activeWindow, &*activePane, false);
		}

		if (sessionText.empty()) session = activeSession;
		if (!session) return Error(TmuxTargetCode::TargetMissing, "target-missing");
		ParsedSelector windowSelector;
		if (kind == TmuxTargetKind::Pane && sessionText.empty() && !hasPane && !text.empty() && text.front() == '%') {
			for (const auto& candidateSession : snapshot.sessions) {
				for (const auto& candidateWindow : candidateSession.windows) {
					ParsedSelector paneSelector;
					if (!ParseSelector(text, paneSelector)) return Error(TmuxTargetCode::InvalidSyntax, "invalid-pane-target");
					TmuxTargetCode paneCode;
					const auto candidate = FindPane(candidateWindow, paneSelector, paneCode);
					if (candidate) return MakeResult(snapshot, kind, candidateSession, candidateWindow, candidate, true);
				}
			}
			return Error(TmuxTargetCode::TargetMissing, "target-missing");
		}
		if (windowText.empty()) return Error(TmuxTargetCode::InvalidSyntax, "missing-window-target");
		if (!ParseSelector(windowText, windowSelector) || windowSelector.prefix == '%' || windowSelector.prefix == '$') {
			return Error(TmuxTargetCode::InvalidSyntax, "invalid-window-target");
		}
		TmuxTargetCode windowCode;
		const auto window = FindWindow(*session, windowSelector, windowCode);
		if (!window) return Error(windowCode, windowCode == TmuxTargetCode::Ambiguous ? "ambiguous-target" : "target-missing");
		stable = stable && windowSelector.stable;
		if (kind == TmuxTargetKind::Window || !hasPane) {
			if (kind == TmuxTargetKind::Pane) {
				const auto activePane = std::find_if(window->panes.begin(), window->panes.end(), [](const auto& item) { return item.active; });
				if (activePane == window->panes.end()) return Error(TmuxTargetCode::TargetMissing, "target-missing");
				return MakeResult(snapshot, kind, *session, *window, &*activePane, stable);
			}
			return MakeResult(snapshot, kind, *session, *window, nullptr, stable);
		}
		ParsedSelector paneSelector;
		if (!ParseSelector(paneText, paneSelector) || paneSelector.prefix == '$' || paneSelector.prefix == '@') {
			return Error(TmuxTargetCode::InvalidSyntax, "invalid-pane-target");
		}
		TmuxTargetCode paneCode;
		const auto pane = FindPane(*window, paneSelector, paneCode);
		if (!pane) return Error(paneCode, paneCode == TmuxTargetCode::Ambiguous ? "ambiguous-target" : "target-missing");
		return MakeResult(snapshot, kind, *session, *window, pane, stable && paneSelector.stable);
	} catch (...) {
		return Error(TmuxTargetCode::InvalidSyntax, "internal-error");
	}
}

} // namespace terminal::tmux
