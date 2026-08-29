/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "terminal/tmux/TmuxArgumentParser.h"

#include <algorithm>
#include <array>
#include <limits>

namespace terminal::tmux {
namespace {

struct CommandName final {
	std::string_view name;
	TmuxCommandKind kind;
	std::string_view canonical;
};

constexpr std::array kCommandNames{
	CommandName{ "list-panes", TmuxCommandKind::ListPanes, "list-panes" },
	CommandName{ "lsp", TmuxCommandKind::ListPanes, "list-panes" },
	CommandName{ "send-keys", TmuxCommandKind::SendKeys, "send-keys" },
	CommandName{ "send", TmuxCommandKind::SendKeys, "send-keys" },
	CommandName{ "capture-pane", TmuxCommandKind::CapturePane, "capture-pane" },
	CommandName{ "capturep", TmuxCommandKind::CapturePane, "capture-pane" },
	CommandName{ "display-message", TmuxCommandKind::DisplayMessage, "display-message" },
	CommandName{ "display", TmuxCommandKind::DisplayMessage, "display-message" },
	CommandName{ "list-sessions", TmuxCommandKind::ListSessions, "list-sessions" },
	CommandName{ "ls", TmuxCommandKind::ListSessions, "list-sessions" },
	CommandName{ "list-windows", TmuxCommandKind::ListWindows, "list-windows" },
	CommandName{ "lsw", TmuxCommandKind::ListWindows, "list-windows" },
	CommandName{ "new-session", TmuxCommandKind::NewSession, "new-session" },
	CommandName{ "new", TmuxCommandKind::NewSession, "new-session" },
	CommandName{ "new-window", TmuxCommandKind::NewWindow, "new-window" },
	CommandName{ "neww", TmuxCommandKind::NewWindow, "new-window" },
	CommandName{ "split-window", TmuxCommandKind::SplitWindow, "split-window" },
	CommandName{ "splitw", TmuxCommandKind::SplitWindow, "split-window" },
	CommandName{ "select-window", TmuxCommandKind::SelectWindow, "select-window" },
	CommandName{ "selectw", TmuxCommandKind::SelectWindow, "select-window" },
	CommandName{ "select-pane", TmuxCommandKind::SelectPane, "select-pane" },
	CommandName{ "selectp", TmuxCommandKind::SelectPane, "select-pane" },
	CommandName{ "has-session", TmuxCommandKind::HasSession, "has-session" },
	CommandName{ "has", TmuxCommandKind::HasSession, "has-session" },
	CommandName{ "kill-pane", TmuxCommandKind::KillPane, "kill-pane" },
	CommandName{ "killp", TmuxCommandKind::KillPane, "kill-pane" },
	CommandName{ "kill-window", TmuxCommandKind::KillWindow, "kill-window" },
	CommandName{ "killw", TmuxCommandKind::KillWindow, "kill-window" },
	CommandName{ "kill-session", TmuxCommandKind::KillSession, "kill-session" },
	CommandName{ "wait-for", TmuxCommandKind::WaitFor, "wait-for" },
	CommandName{ "wait", TmuxCommandKind::WaitFor, "wait-for" },
};

[[nodiscard]] bool IsControlFree(std::string_view value) noexcept
{
	for (const auto ch : value) {
		const auto byte = static_cast<unsigned char>(ch);
		if (byte == 0 || byte < 0x20 || byte == 0x7f) return false;
	}
	return true;
}

[[nodiscard]] TmuxParseResult Error(TmuxParseCode code, std::string_view diagnostic) noexcept
{
	TmuxParseResult result;
	result.code = code;
	result.diagnosticCode.assign(diagnostic.data(), diagnostic.size());
	return result;
}

[[nodiscard]] bool IsOption(std::string_view token) noexcept
{
	return token.size() > 1 && token.front() == '-' && token != "-";
}

[[nodiscard]] bool ReadOptionValue(const std::vector<std::string>& argv, std::size_t& index,
	std::string_view token, std::string_view shortName, std::optional<std::string>& value) noexcept
{
	if (token == shortName) {
		if (++index >= argv.size()) return false;
		value = argv[index];
		return IsControlFree(*value);
	}
	if (token.size() > shortName.size() && token.substr(0, shortName.size()) == shortName) {
		value = std::string(token.substr(shortName.size()));
		return IsControlFree(*value);
	}
	return false;
}

[[nodiscard]] bool ReadOptionValue(const std::vector<std::string>& argv, std::size_t& index,
	std::string_view token, std::string_view shortName, std::optional<std::string>& value,
	bool& matched) noexcept
{
	if (token == shortName || (token.size() > shortName.size() && token.substr(0, shortName.size()) == shortName)) {
		matched = true;
		return ReadOptionValue(argv, index, token, shortName, value);
	}
	return true;
}

[[nodiscard]] bool SetFlag(std::string_view token, std::string_view shortName, bool& value) noexcept
{
	if (token == shortName) {
		value = true;
		return true;
	}
	return false;
}

[[nodiscard]] bool SetClusterFlags(std::string_view token, std::string_view allowed, TmuxCommand& command) noexcept
{
	if (token.size() < 3 || token.front() != '-' || token[1] == '-') return false;
	for (const auto ch : token.substr(1)) {
		if (allowed.find(ch) == std::string_view::npos) return false;
		switch (ch) {
		case 'a': command.all = true; break;
		case 'd': command.detached = true; break;
		case 'h': command.horizontal = true; break;
		case 'v': command.vertical = true; break;
		case 'j': command.joinWrapped = true; break;
		case 'J': command.joinWrapped = true; break;
		case 'l': command.literal = true; break;
		case 'p': command.print = true; break;
		case 'P': command.printResult = true; break;
		case 's': command.sessionsOnly = true; break;
		case 'S': command.signal = true; break;
		case 'L': command.lock = true; break;
		case 'U': command.unlock = true; break;
		default: return false;
		}
	}
	return true;
}

[[nodiscard]] bool IsNamedKeyCandidate(std::string_view value) noexcept
{
	static constexpr std::array names{
		"Enter", "Escape", "Tab", "BSpace", "Space", "Up", "Down", "Left", "Right",
		"Home", "End", "PageUp", "PageDown", "Insert", "Delete",
		"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
	};
	if (std::find(names.begin(), names.end(), value) != names.end()) return true;
	return value.size() == 3 && value[0] == 'C' && value[1] == '-' &&
		((value[2] >= 'a' && value[2] <= 'z') || value[2] == '@' || value[2] == '[' ||
			value[2] == '\\' || value[2] == ']' || value[2] == '^' || value[2] == '_' || value[2] == '?');
}

[[nodiscard]] TmuxParseResult ParseCommandOptions(const std::vector<std::string>& argv,
	TmuxCommand& command) noexcept
{
	const auto kind = command.kind;
	bool operandsOnly = false;
	for (std::size_t index = 1; index < argv.size(); ++index) {
		const auto& token = argv[index];
		if (!IsControlFree(token)) return Error(TmuxParseCode::InvalidUsage, "invalid-argument");
		if (operandsOnly || !IsOption(token)) {
			if (token == ";") return Error(TmuxParseCode::UnsupportedSurface, "command-list-unsupported");
			command.operands.push_back(token);
			continue;
		}
		if (token == "--") {
			operandsOnly = true;
			continue;
		}
		if (token == ";") return Error(TmuxParseCode::UnsupportedSurface, "command-list-unsupported");

		bool matched = false;
		switch (kind) {
		case TmuxCommandKind::ListPanes:
		case TmuxCommandKind::ListSessions:
		case TmuxCommandKind::ListWindows:
			if (kind == TmuxCommandKind::ListPanes && SetClusterFlags(token, "as", command)) break;
			if (kind == TmuxCommandKind::ListWindows && SetClusterFlags(token, "a", command)) break;
			if (ReadOptionValue(argv, index, token, "-F", command.format, matched) && matched) break;
			if (matched) return Error(TmuxParseCode::InvalidUsage, "missing-format");
			if (ReadOptionValue(argv, index, token, "-f", command.filter, matched) && matched) break;
			if (matched) return Error(TmuxParseCode::InvalidUsage, "missing-filter");
			if (kind != TmuxCommandKind::ListSessions
				&& ReadOptionValue(argv, index, token, "-t", command.target, matched) && matched) break;
			if (matched) return Error(TmuxParseCode::InvalidUsage, "missing-target");
			if (kind == TmuxCommandKind::ListWindows && SetFlag(token, "-a", command.all)) break;
			if (kind == TmuxCommandKind::ListPanes && SetFlag(token, "-a", command.all)) break;
			if (kind == TmuxCommandKind::ListPanes && SetFlag(token, "-s", command.sessionsOnly)) break;
			return Error(TmuxParseCode::UnsupportedSurface, "unsupported-option");

		case TmuxCommandKind::SendKeys:
			if (ReadOptionValue(argv, index, token, "-t", command.target, matched) && matched) break;
			if (matched) return Error(TmuxParseCode::InvalidUsage, "missing-target");
			if (ReadOptionValue(argv, index, token, "-N", command.length, matched) && matched) break;
			if (matched) return Error(TmuxParseCode::InvalidUsage, "missing-repeat");
			if (SetFlag(token, "-l", command.literal)) break;
			if (SetClusterFlags(token, "l", command)) break;
			return Error(TmuxParseCode::UnsupportedSurface, "unsupported-option");

		case TmuxCommandKind::CapturePane:
			if (SetClusterFlags(token, "pJ", command)) break;
			if (SetFlag(token, "-p", command.print)) break;
			if (SetFlag(token, "-J", command.joinWrapped)) break;
			if (ReadOptionValue(argv, index, token, "-t", command.target, matched) && matched) break;
			if (matched) return Error(TmuxParseCode::InvalidUsage, "missing-target");
			if (ReadOptionValue(argv, index, token, "-S", command.startLine, matched) && matched) break;
			if (matched) return Error(TmuxParseCode::InvalidUsage, "missing-start");
			if (ReadOptionValue(argv, index, token, "-E", command.endLine, matched) && matched) break;
			if (matched) return Error(TmuxParseCode::InvalidUsage, "missing-end");
			return Error(TmuxParseCode::UnsupportedSurface, "unsupported-option");

		case TmuxCommandKind::DisplayMessage:
			if (SetClusterFlags(token, "p", command)) break;
			if (SetFlag(token, "-p", command.print)) break;
			if (ReadOptionValue(argv, index, token, "-t", command.target, matched) && matched) break;
			if (matched) return Error(TmuxParseCode::InvalidUsage, "missing-target");
			if (ReadOptionValue(argv, index, token, "-F", command.format, matched) && matched) break;
			if (matched) return Error(TmuxParseCode::InvalidUsage, "missing-format");
			return Error(TmuxParseCode::UnsupportedSurface, "unsupported-option");

		case TmuxCommandKind::NewSession:
			if (SetClusterFlags(token, "d", command)) break;
			if (SetFlag(token, "-d", command.detached)) break;
			if (ReadOptionValue(argv, index, token, "-s", command.sessionName, matched) && matched) break;
			if (matched) return Error(TmuxParseCode::InvalidUsage, "missing-session-name");
			if (ReadOptionValue(argv, index, token, "-c", command.directory, matched) && matched) break;
			if (matched) return Error(TmuxParseCode::InvalidUsage, "missing-directory");
			return Error(TmuxParseCode::UnsupportedSurface, "unsupported-option");

		case TmuxCommandKind::NewWindow:
			if (SetClusterFlags(token, "dP", command)) break;
			if (SetFlag(token, "-d", command.detached)) break;
			if (SetFlag(token, "-P", command.printResult)) break;
			if (ReadOptionValue(argv, index, token, "-t", command.target, matched) && matched) break;
			if (matched) return Error(TmuxParseCode::InvalidUsage, "missing-target");
			if (ReadOptionValue(argv, index, token, "-n", command.windowName, matched) && matched) break;
			if (matched) return Error(TmuxParseCode::InvalidUsage, "missing-window-name");
			if (ReadOptionValue(argv, index, token, "-c", command.directory, matched) && matched) break;
			if (matched) return Error(TmuxParseCode::InvalidUsage, "missing-directory");
			if (ReadOptionValue(argv, index, token, "-F", command.format, matched) && matched) break;
			if (matched) return Error(TmuxParseCode::InvalidUsage, "missing-format");
			return Error(TmuxParseCode::UnsupportedSurface, "unsupported-option");

		case TmuxCommandKind::SplitWindow:
			if (SetClusterFlags(token, "hvdP", command)) break;
			if (SetFlag(token, "-h", command.horizontal)) break;
			if (SetFlag(token, "-v", command.vertical)) break;
			if (SetFlag(token, "-d", command.detached)) break;
			if (SetFlag(token, "-P", command.printResult)) break;
			if (ReadOptionValue(argv, index, token, "-t", command.target, matched) && matched) break;
			if (matched) return Error(TmuxParseCode::InvalidUsage, "missing-target");
			if (ReadOptionValue(argv, index, token, "-c", command.directory, matched) && matched) break;
			if (matched) return Error(TmuxParseCode::InvalidUsage, "missing-directory");
			if (ReadOptionValue(argv, index, token, "-l", command.length, matched) && matched) break;
			if (matched) return Error(TmuxParseCode::InvalidUsage, "missing-length");
			if (ReadOptionValue(argv, index, token, "-p", command.percentage, matched) && matched) break;
			if (matched) return Error(TmuxParseCode::InvalidUsage, "missing-percentage");
			if (ReadOptionValue(argv, index, token, "-F", command.format, matched) && matched) break;
			if (matched) return Error(TmuxParseCode::InvalidUsage, "missing-format");
			return Error(TmuxParseCode::UnsupportedSurface, "unsupported-option");

		case TmuxCommandKind::SelectWindow:
		case TmuxCommandKind::SelectPane:
		case TmuxCommandKind::HasSession:
		case TmuxCommandKind::KillPane:
		case TmuxCommandKind::KillWindow:
		case TmuxCommandKind::KillSession:
			if (ReadOptionValue(argv, index, token, "-t", command.target, matched) && matched) break;
			if (matched) return Error(TmuxParseCode::InvalidUsage, "missing-target");
			return Error(TmuxParseCode::UnsupportedSurface, "unsupported-option");

		case TmuxCommandKind::WaitFor:
			if (SetClusterFlags(token, "SLU", command)) break;
			if (SetFlag(token, "-S", command.signal)) break;
			if (SetFlag(token, "-L", command.lock)) break;
			if (SetFlag(token, "-U", command.unlock)) break;
			return Error(TmuxParseCode::UnsupportedSurface, "unsupported-option");
		case TmuxCommandKind::Version:
			return Error(TmuxParseCode::UnsupportedSurface, "unsupported-option");
		}
	}
	if (command.signal && (command.lock || command.unlock) || command.lock && command.unlock) {
		return Error(TmuxParseCode::InvalidUsage, "conflicting-wait-operation");
	}
	if (command.kind == TmuxCommandKind::CapturePane && !command.print) {
		return Error(TmuxParseCode::UnsupportedSurface, "capture-print-required");
	}
	if (command.kind == TmuxCommandKind::DisplayMessage && !command.print) {
		return Error(TmuxParseCode::UnsupportedSurface, "display-print-required");
	}
	if (command.kind == TmuxCommandKind::NewSession && !command.detached) {
		return Error(TmuxParseCode::UnsupportedSurface, "attach-unsupported");
	}
	if (command.kind == TmuxCommandKind::SplitWindow && command.horizontal && command.vertical) {
		return Error(TmuxParseCode::InvalidUsage, "conflicting-orientation");
	}
	if (command.kind == TmuxCommandKind::SendKeys && command.length) {
		const auto& value = *command.length;
		if (value.empty() || value.size() > 3 || value.find_first_not_of("0123456789") != std::string::npos) {
			return Error(TmuxParseCode::InvalidUsage, "invalid-repeat");
		}
		std::uint16_t repeat{};
		const auto parsed = std::from_chars(value.data(), value.data() + value.size(), repeat);
		if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
			return Error(TmuxParseCode::InvalidUsage, "invalid-repeat");
		}
		command.repeatCount = repeat;
	}
	if (command.kind == TmuxCommandKind::WaitFor) {
		if (command.operands.size() != 1 || command.operands.front().empty()) {
			return Error(TmuxParseCode::InvalidUsage, "missing-channel");
		}
		command.channel = command.operands.front();
		command.operands.clear();
	}
	if (command.kind == TmuxCommandKind::SendKeys && !command.literal) {
		for (const auto& operand : command.operands) {
			if (!IsNamedKeyCandidate(operand) && operand.empty()) return Error(TmuxParseCode::InvalidUsage, "empty-key");
		}
	}
	return { TmuxParseCode::Succeeded, {} };
}

} // namespace

TmuxParseResult ParseTmuxArguments(const std::vector<std::string>& argv, TmuxCommand& command) noexcept
{
	try {
		command = {};
		if (argv.empty()) return Error(TmuxParseCode::InvalidUsage, "missing-command");
		if (argv.size() == 1 && argv.front() == "-V") {
			command.kind = TmuxCommandKind::Version;
			command.canonicalName = "-V";
			return { TmuxParseCode::Succeeded, {} };
		}
		if (argv.front().empty() || argv.front().front() == '-') {
			return Error(TmuxParseCode::UnsupportedSurface, "unsupported-global-option");
		}
		const auto found = std::find_if(kCommandNames.begin(), kCommandNames.end(),
			[&argv](const auto& item) { return item.name == argv.front(); });
		if (found == kCommandNames.end()) return Error(TmuxParseCode::UnsupportedSurface, "unknown-command");
		command.kind = found->kind;
		command.canonicalName.assign(found->canonical.data(), found->canonical.size());
		return ParseCommandOptions(argv, command);
	} catch (...) {
		command = {};
		return Error(TmuxParseCode::InvalidUsage, "internal-error");
	}
}

} // namespace terminal::tmux
