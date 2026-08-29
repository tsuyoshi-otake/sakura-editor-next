/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "terminal/tmux/TmuxCli.h"

namespace terminal::tmux {
namespace {

[[nodiscard]] std::string Diagnostic(std::string_view code)
{
	std::string value = "sakura-tmux: ";
	value.append(code.data(), code.size());
	value.push_back('\n');
	return value;
}

[[nodiscard]] std::string ParseDiagnostic(TmuxParseCode code, std::string_view supplied)
{
	if (!supplied.empty()) return Diagnostic(supplied);
	return Diagnostic(code == TmuxParseCode::UnsupportedSurface ? "unsupported-surface" : "invalid-usage");
}

[[nodiscard]] std::string ResultDiagnostic(TmuxCommandResultCode code, std::string_view supplied)
{
	if (!supplied.empty()) return Diagnostic(supplied);
	switch (code) {
	case TmuxCommandResultCode::InvalidUsage: return Diagnostic("invalid-usage");
	case TmuxCommandResultCode::UnsupportedSurface: return Diagnostic("unsupported-surface");
	case TmuxCommandResultCode::TargetMissing: return Diagnostic("target-missing");
	case TmuxCommandResultCode::TopologyChanged: return Diagnostic("topology-changed");
	case TmuxCommandResultCode::Denied: return Diagnostic("access-denied");
	case TmuxCommandResultCode::Unavailable: return Diagnostic("runtime-unavailable");
	case TmuxCommandResultCode::ResourceExhausted: return Diagnostic("resource-exhausted");
	case TmuxCommandResultCode::DeadlineExceeded: return Diagnostic("deadline-exceeded");
	case TmuxCommandResultCode::Ambiguous: return Diagnostic("ambiguous");
	case TmuxCommandResultCode::InternalError: return Diagnostic("internal-error");
	case TmuxCommandResultCode::Succeeded: break;
	}
	return Diagnostic("internal-error");
}

} // namespace

TmuxCliResponse TmuxCli::Run(const std::vector<std::string>& argv, ITmuxRuntimePort& runtime,
	TmuxCompatibilityProfile profile, TmuxDispatcherLimits limits) noexcept
{
	try {
		TmuxCommand command;
		const auto parse = TmuxArgumentParser::Parse(argv, command);
		if (parse.code != TmuxParseCode::Succeeded) return FromParse(parse);
		return FromDispatch(TmuxCommandDispatcher(runtime, std::move(profile), std::move(limits)).Dispatch(command));
	} catch (...) {
		return TmuxCliResponse{ 1, {}, Diagnostic("internal-error") };
	}
}

TmuxCliResponse TmuxCli::FromParse(const TmuxParseResult& parse) noexcept
{
	return TmuxCliResponse{ 1, {}, ParseDiagnostic(parse.code, parse.diagnosticCode) };
}

TmuxCliResponse TmuxCli::FromDispatch(const TmuxCommandResult& dispatch) noexcept
{
	if (dispatch.Succeeded()) return TmuxCliResponse{ 0, dispatch.stdoutText, {} };
	return TmuxCliResponse{ 1, {}, ResultDiagnostic(dispatch.code, dispatch.diagnosticCode) };
}

} // namespace terminal::tmux
