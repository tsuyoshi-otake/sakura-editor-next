/*! @file
 * @brief Safe, bounded runner for the fixed GitHub CLI account query.
 */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include "StdAfx.h"

#include "platform/process/BoundedProcessRunner.h"
#include "platform/process/WindowsExecutableResolver.h"
#include "workbench/account/AccountDiscovery.h"
#include "workbench/account/GhAuthStatusEnvironment.h"

#include <sakura/security/AuthenticodeSignatureVerifier.h>

#include <string>
#include <utility>
#include <vector>

namespace workbench::account {
namespace {

//! gh's stderr is discarded; the account query needs only a small bound so a
//! runaway diagnostic stream still terminates as OutputLimitExceeded.
constexpr std::size_t kMaximumGhAuthStatusStandardErrorBytes = 64u * 1024u;

GhAuthStatusResult Terminal(EAccountCommandStatus status)
{
	GhAuthStatusResult result;
	result.status = status;
	return result;
}

//! Preserve the user's gh configuration/authentication variables, but make
//! this non-interactive JSON query deterministic. In particular, inherited
//! GH_DEBUG is suppressed because API debug output can contain request
//! metadata, while the output contract intentionally contains no diagnostics.
//! An override also replaces a same-named parent variable, so an inherited
//! GH_FORCE_TTY cannot survive.
const std::vector<std::pair<std::wstring, std::wstring>>& GhEnvironmentOverrides()
{
	static const std::vector<std::pair<std::wstring, std::wstring>> kOverrides{
		{ L"GH_FORCE_TTY", L"0" },
		{ L"GH_NO_UPDATE_NOTIFIER", L"1" },
		{ L"GH_PAGER", L"cat" },
		{ L"GH_PROMPT_DISABLED", L"1" },
		{ L"NO_COLOR", L"1" },
	};
	return kOverrides;
}

const std::vector<std::wstring>& GhEnvironmentRemovals()
{
	static const std::vector<std::wstring> kRemovals{
		L"GH_DEBUG",
	};
	return kRemovals;
}

} // namespace

std::vector<wchar_t> detail::BuildGhAuthStatusEnvironmentBlock()
{
	return platform::process::detail::BuildEnvironmentBlock(
		GhEnvironmentOverrides(), GhEnvironmentRemovals());
}

GhAuthStatusResult detail::RunGhAuthStatus(const GhAuthStatusRequest& request,
	const HANDLE stop, const detail::GhExecutableGate& gate)
{
	if (!request.IsValid()) return Terminal(EAccountCommandStatus::InvalidRequest);
	if (!gate.resolveExecutable || !gate.verifyExecutableTrust) {
		return Terminal(EAccountCommandStatus::InvalidRequest);
	}
	const auto executable = gate.resolveExecutable();
	if (!executable) return Terminal(EAccountCommandStatus::Unavailable);
	// Even though this query carries no token, the resolved binary must still
	// carry the shared GitHub-publisher Authenticode signature before it runs:
	// a spoofed gh.exe in PATH would otherwise execute under the editor's
	// account. A refused executable surfaces as Unavailable, which the account
	// source already reports as its existing not-installed state.
	if (!gate.verifyExecutableTrust(*executable)) {
		return Terminal(EAccountCommandStatus::Unavailable);
	}
	// RunBoundedProcess requires an absolute working directory; a relative one
	// would fail inside CreateProcessW anyway, so it maps to LaunchFailed.
	if (!platform::IsAbsoluteWindowsPath(request.workingDirectory)) {
		return Terminal(EAccountCommandStatus::LaunchFailed);
	}

	// The launch shares the bounded process boundary instead of a private
	// copy: the child gets only its three standard handles, an empty stdin
	// payload means the writer closes the pipe immediately (an EOF just like
	// the former NUL device handle; gh never reads stdin), stderr is captured
	// and discarded at the largest allowed bound, and a kill-on-close job owns
	// the complete child tree on timeout, cancellation or early exit.
	platform::process::BoundedProcessRequest bounded(
		*executable, request.workingDirectory, BuildGhAuthStatusArguments());
	bounded.SetEnvironmentOverrides(GhEnvironmentOverrides());
	bounded.SetEnvironmentRemovals(GhEnvironmentRemovals());
	bounded.SetTimeoutMilliseconds(request.timeoutMilliseconds);
	bounded.SetMaximumStandardOutputBytes(request.maximumOutputBytes);
	bounded.SetMaximumStandardErrorBytes(kMaximumGhAuthStatusStandardErrorBytes);
	bounded.SetSensitiveStandardOutput(true);

	auto result = platform::process::RunBoundedProcess(bounded, stop);

	GhAuthStatusResult outcome;
	switch (result.Status()) {
	case platform::process::EBoundedProcessStatus::Succeeded:
		outcome.exitCode = result.ExitCode();
		outcome.status = EAccountCommandStatus::Succeeded;
		break;
	case platform::process::EBoundedProcessStatus::Failed:
		outcome.exitCode = result.ExitCode();
		outcome.status = EAccountCommandStatus::Failed;
		break;
	case platform::process::EBoundedProcessStatus::InvalidRequest:
		outcome.status = EAccountCommandStatus::InvalidRequest;
		break;
	case platform::process::EBoundedProcessStatus::TimedOut:
		outcome.status = EAccountCommandStatus::TimedOut;
		break;
	case platform::process::EBoundedProcessStatus::Cancelled:
		outcome.status = EAccountCommandStatus::Cancelled;
		break;
	case platform::process::EBoundedProcessStatus::OutputLimitExceeded:
	case platform::process::EBoundedProcessStatus::ObserverRejected:
		// No observer is installed, so ObserverRejected is unreachable here.
		outcome.status = EAccountCommandStatus::OutputLimitExceeded;
		break;
	case platform::process::EBoundedProcessStatus::LaunchFailed:
	default:
		outcome.status = EAccountCommandStatus::LaunchFailed;
		break;
	}

	// Raw stdout is parsed and destroyed inside this boundary so token, scope,
	// and token-source members cannot escape through the result.
	auto output = result.TakeStandardOutput();
	if (!output.empty()) {
		std::string standardOutput(
			reinterpret_cast<const char*>(output.data()), output.size());
		::SecureZeroMemory(output.data(), output.size());
		const auto parsed = ParseGitHubAuthStatus(standardOutput, request.limits);
		::SecureZeroMemory(standardOutput.data(), standardOutput.size());
		outcome.parseStatus = parsed.status;
		outcome.accounts = std::move(parsed.accounts);
	}
	return outcome;
}

GhAuthStatusResult RunGhAuthStatus(const GhAuthStatusRequest& request, const HANDLE stop)
{
	// The default gate resolves gh.exe through PATH and applies the same
	// GitHub-publisher Authenticode check the SENP tool policy enforces.
	const detail::GhExecutableGate gate{
		[]() { return platform::ResolveWindowsExecutable(L"gh.exe"); },
		[](const std::wstring_view path) {
			return platform::security::VerifyGitHubCliTrust(path);
		},
	};
	return detail::RunGhAuthStatus(request, stop, gate);
}

} // namespace workbench::account
