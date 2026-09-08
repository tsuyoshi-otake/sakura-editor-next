/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"
#include "platform/process/BoundedProcessRunner.h"
#include "platform/process/WindowsExecutableResolver.h"
#include "workbench/scm/GitCommandRunner.h"

#include <algorithm>

namespace workbench::scm {
namespace {

bool IsCoreFsmonitorAssignment(std::wstring_view value) noexcept
{
	const auto equals = value.find(L'=');
	if (equals == std::wstring_view::npos) return false;
	const auto key = value.substr(0, equals);
	return ::CompareStringOrdinal(key.data(), static_cast<int>(key.size()),
		L"core.fsmonitor", -1, TRUE) == CSTR_EQUAL;
}

bool HasCoreFsmonitorOverride(const std::vector<std::wstring>& arguments) noexcept
{
	for (std::size_t index = 0; index < arguments.size(); ++index) {
		const auto& argument = arguments[index];
		if (argument == L"-c" || argument == L"--config" || argument == L"--config-env") {
			if (index + 1 < arguments.size() && IsCoreFsmonitorAssignment(arguments[index + 1])) return true;
			continue;
		}
		if (argument.size() > 2 && argument.starts_with(L"-c")
			&& IsCoreFsmonitorAssignment(argument.substr(2))) return true;
		if (argument.starts_with(L"--config=")
			&& IsCoreFsmonitorAssignment(argument.substr(std::wstring_view(L"--config=").size()))) return true;
		if (argument.starts_with(L"--config-env=")
			&& IsCoreFsmonitorAssignment(argument.substr(std::wstring_view(L"--config-env=").size()))) return true;
	}
	return false;
}

GitExecutionResult Terminal(EGitExecutionStatus status)
{
	GitExecutionResult result;
	result.status = status;
	return result;
}

EGitExecutionStatus MapStatus(platform::process::EBoundedProcessStatus status) noexcept
{
	switch (status) {
	case platform::process::EBoundedProcessStatus::Succeeded: return EGitExecutionStatus::Succeeded;
	case platform::process::EBoundedProcessStatus::Failed: return EGitExecutionStatus::Failed;
	case platform::process::EBoundedProcessStatus::InvalidRequest: return EGitExecutionStatus::InvalidRequest;
	case platform::process::EBoundedProcessStatus::LaunchFailed: return EGitExecutionStatus::LaunchFailed;
	case platform::process::EBoundedProcessStatus::TimedOut: return EGitExecutionStatus::TimedOut;
	case platform::process::EBoundedProcessStatus::Cancelled: return EGitExecutionStatus::Cancelled;
	case platform::process::EBoundedProcessStatus::OutputLimitExceeded: return EGitExecutionStatus::OutputLimitExceeded;
	case platform::process::EBoundedProcessStatus::ObserverRejected: return EGitExecutionStatus::LaunchFailed;
	}
	return EGitExecutionStatus::LaunchFailed;
}

} // namespace

std::wstring QuoteGitArgument(std::wstring_view value)
{
	return platform::process::QuoteWindowsArgument(value);
}

std::wstring BuildGitCommandLine(std::wstring_view executable, const std::vector<std::wstring>& arguments)
{
	return platform::process::BuildWindowsCommandLine(executable, arguments);
}

std::vector<std::wstring> BuildEffectiveGitArguments(const GitExecutionRequest& request)
{
	std::vector<std::wstring> effective;
	effective.reserve(request.arguments.size() + (request.policy == EGitRequestPolicy::PassiveRepositoryRead ? 4 : 2));
	effective.emplace_back(L"-C");
	effective.push_back(request.workingDirectory);
	if (request.policy == EGitRequestPolicy::PassiveRepositoryRead) {
		effective.emplace_back(L"-c");
		effective.emplace_back(L"core.fsmonitor=false");
	}
	effective.insert(effective.end(), request.arguments.begin(), request.arguments.end());
	return effective;
}

std::wstring ResolveGitExecutable()
{
	const auto resolved = platform::ResolveWindowsExecutable(L"git.exe");
	return resolved.value_or(std::wstring{});
}

bool IsExecutableGitRequest(const GitExecutionRequest& request) noexcept
{
	if (request.workingDirectory.empty() || request.arguments.empty()) return false;
	if (request.arguments.size() > kMaximumGitArguments
		|| request.standardInput.size() > kMaximumGitStandardInputBytes) return false;
	if (request.maximumOutputBytes == 0
		|| request.maximumOutputBytes > platform::process::kMaximumBoundedProcessOutputBytes
		|| request.timeoutMilliseconds == 0) return false;
	if (request.workingDirectory.size() > kMaximumGitArgumentLength) return false;
	if (request.policy == EGitRequestPolicy::PassiveRepositoryRead
		&& HasCoreFsmonitorOverride(request.arguments)) return false;
	return std::none_of(request.arguments.begin(), request.arguments.end(),
		[](const std::wstring& argument) { return argument.size() > kMaximumGitArgumentLength; });
}

GitExecutionResult RunGit(const GitExecutionRequest& request, HANDLE stop)
{
	if (!IsExecutableGitRequest(request)) return Terminal(EGitExecutionStatus::InvalidRequest);
	const auto executable = ResolveGitExecutable();
	if (executable.empty()) return Terminal(EGitExecutionStatus::GitUnavailable);

	platform::process::BoundedProcessRequest processRequest(
		executable, request.workingDirectory, BuildEffectiveGitArguments(request));
	processRequest.SetStandardInput(request.standardInput);
	processRequest.SetEnvironmentOverrides({
		{ L"GIT_TERMINAL_PROMPT", L"0" },
		{ L"GIT_FLUSH", L"1" },
	});
	processRequest.SetTimeoutMilliseconds(request.timeoutMilliseconds);
	processRequest.SetMaximumStandardOutputBytes(request.maximumOutputBytes);
	processRequest.SetMaximumStandardErrorBytes(kMaximumGitStandardErrorBytes);

	const auto process = platform::process::RunBoundedProcess(processRequest, stop);
	GitExecutionResult result;
	result.status = MapStatus(process.Status());
	result.exitCode = process.ExitCode();
	result.standardOutput = process.StandardOutput();
	if (!process.StandardError().empty()) {
		result.standardError.assign(
			reinterpret_cast<const char*>(process.StandardError().data()), process.StandardError().size());
	}
	return result;
}

} // namespace workbench::scm
