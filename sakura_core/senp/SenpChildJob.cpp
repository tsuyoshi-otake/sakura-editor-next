/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "senp/SenpChildJob.h"

#include "platform/process/AllowlistedEnvironment.h"

#include <algorithm>
#include <filesystem>

namespace senp {
namespace {

//! Every UI restriction class. SENP children are console helpers created
//! with CREATE_NO_WINDOW: the hosts run Wasmtime (no USER32 handles, no
//! clipboard, no global atoms) and the package tool only writes stdout and
//! stderr, so no launcher needs any of these capabilities.
constexpr DWORD kUiRestrictionClasses = JOB_OBJECT_UILIMIT_DESKTOP
	| JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | JOB_OBJECT_UILIMIT_EXITWINDOWS
	| JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_HANDLES
	| JOB_OBJECT_UILIMIT_READCLIPBOARD | JOB_OBJECT_UILIMIT_WRITECLIPBOARD
	| JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS;

constexpr DWORD LimitFlags(const ChildJobLimits& limits) noexcept
{
	return JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION
		| JOB_OBJECT_LIMIT_ACTIVE_PROCESS
		| (limits.processMemoryBytes != 0 ? JOB_OBJECT_LIMIT_PROCESS_MEMORY : 0);
}

} // namespace

bool detail::VerifyChildJobLimits(HANDLE job, const ChildJobLimits& limits,
	QueryJobInformationFn query) noexcept
{
	if (job == nullptr || job == INVALID_HANDLE_VALUE || query == nullptr) return false;
	const DWORD flags = LimitFlags(limits);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION extended{};
	if (!query(job, JobObjectExtendedLimitInformation, &extended, sizeof(extended), nullptr)
		|| (extended.BasicLimitInformation.LimitFlags & flags) != flags
		|| extended.BasicLimitInformation.ActiveProcessLimit != 1) return false;
	if (limits.processMemoryBytes != 0 && extended.ProcessMemoryLimit != limits.processMemoryBytes) return false;
	JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
	return query(job, JobObjectBasicUIRestrictions, &ui, sizeof(ui), nullptr)
		&& ui.UIRestrictionsClass == kUiRestrictionClasses;
}

bool ConfigureChildJob(HANDLE job, const ChildJobLimits& limits) noexcept
{
	if (job == nullptr || job == INVALID_HANDLE_VALUE) return false;
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION extended{};
	extended.BasicLimitInformation.LimitFlags = LimitFlags(limits);
	extended.BasicLimitInformation.ActiveProcessLimit = 1;
	extended.ProcessMemoryLimit = limits.processMemoryBytes;
	if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &extended, sizeof(extended))) return false;
	JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
	ui.UIRestrictionsClass = kUiRestrictionClasses;
	if (!::SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof(ui))) return false;
	return detail::VerifyChildJobLimits(job, limits, &::QueryInformationJobObject);
}

detail::SenpChildLaunchConfig detail::BuildSenpChildLaunchConfig(std::wstring_view executablePath)
{
	return {
		std::filesystem::path(executablePath).parent_path().native(),
		platform::process::BuildAllowlistedEnvironmentBlock(kSenpChildEnvironmentNames),
		CREATE_UNICODE_ENVIRONMENT,
	};
}

bool detail::SetChildHandlesInheritable(std::span<const HANDLE> handles) noexcept
{
	for (const HANDLE handle : handles) {
		if (!::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) return false;
	}
	return true;
}

std::optional<PROCESS_INFORMATION> detail::LaunchSenpChildProcess(
	const std::wstring_view executablePath, std::wstring& commandLine,
	const SenpChildStdHandles& stdHandles, const DWORD creationFlags,
	const HANDLE job) noexcept
{
	try {
		if (executablePath.empty() || executablePath.size() >= 32768
			|| commandLine.empty() || commandLine.size() >= 32768) return std::nullopt;
		// Copy the path: CreateProcessW's lpApplicationName must be
		// NUL-terminated while a wstring_view need not be.
		const std::wstring executable(executablePath);
		auto config = BuildSenpChildLaunchConfig(executablePath);

		// Collect the distinct non-null child-side handles and make exactly
		// those inheritable. The guard restores the exact prior flag state
		// when this call returns, so a handle that stays open is not left
		// ambient-inheritable for unrelated children.
		std::vector<HANDLE> listed;
		for (const HANDLE handle :
			{ stdHandles.input, stdHandles.output, stdHandles.error }) {
			if (handle != nullptr && handle != INVALID_HANDLE_VALUE
				&& std::ranges::find(listed, handle) == listed.end()) {
				listed.push_back(handle);
			}
		}
		std::vector<HANDLE> raised;
		struct InheritWindow final {
			~InheritWindow()
			{
				for (const HANDLE handle : lifted) {
					(void)::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
				}
			}
			const std::vector<HANDLE>& lifted;
		} window{ raised };
		for (const HANDLE handle : listed) {
			DWORD flags = 0;
			if (!::GetHandleInformation(handle, &flags)) return std::nullopt;
			if ((flags & HANDLE_FLAG_INHERIT) == 0) raised.push_back(handle);
		}
		if (!SetChildHandlesInheritable(listed)) return std::nullopt;

		// PROC_THREAD_ATTRIBUTE_HANDLE_LIST admits exactly the listed handles;
		// PROC_THREAD_ATTRIBUTE_JOB_LIST joins the job at creation, so no
		// suspend/resume window exists. No list at all keeps the classic
		// STARTUPINFO layout (bInheritHandles FALSE, no extended flag).
		const DWORD attributeCount = (listed.empty() ? 0u : 1u)
			+ (job != nullptr && job != INVALID_HANDLE_VALUE ? 1u : 0u);
		SIZE_T attributeBytes = 0;
		LPPROC_THREAD_ATTRIBUTE_LIST attributes = nullptr;
		std::vector<std::uint8_t> attributeStorage;
		if (attributeCount != 0) {
			(void)::InitializeProcThreadAttributeList(nullptr, attributeCount, 0, &attributeBytes);
			if (attributeBytes == 0) return std::nullopt;
			attributeStorage.assign(attributeBytes, std::uint8_t{});
			attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
			if (!::InitializeProcThreadAttributeList(attributes, attributeCount, 0, &attributeBytes)) {
				return std::nullopt;
			}
		}
		struct AttributeListGuard final {
			const LPPROC_THREAD_ATTRIBUTE_LIST value;
			~AttributeListGuard() { if (value != nullptr) ::DeleteProcThreadAttributeList(value); }
		} attributeGuard{ attributes };
		if (!listed.empty()
			&& !::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
				listed.data(), static_cast<SIZE_T>(listed.size() * sizeof(HANDLE)),
				nullptr, nullptr)) return std::nullopt;
		if (job != nullptr && job != INVALID_HANDLE_VALUE) {
			HANDLE jobHandle = job;
			if (!::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
					&jobHandle, sizeof(jobHandle), nullptr, nullptr)) return std::nullopt;
		}

		STARTUPINFOEXW startup{};
		startup.StartupInfo.cb = static_cast<DWORD>(
			attributes != nullptr ? sizeof(startup) : sizeof(STARTUPINFOW));
		startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		startup.StartupInfo.hStdInput = stdHandles.input;
		startup.StartupInfo.hStdOutput = stdHandles.output;
		startup.StartupInfo.hStdError = stdHandles.error;
		startup.lpAttributeList = attributes;
		const DWORD flags = creationFlags | config.environmentFlags
			| (attributes != nullptr ? DWORD{ EXTENDED_STARTUPINFO_PRESENT } : 0u);
		PROCESS_INFORMATION process{};
		if (!::CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr,
				listed.empty() ? FALSE : TRUE, flags, config.environment.data(),
				config.workingDirectory.c_str(), &startup.StartupInfo, &process)) {
			return std::nullopt;
		}
		return process;
	} catch (const std::exception&) {
		// Only allocation inside this body can throw; the handle-list launch
		// itself fails via return values, so a narrowed catch is sufficient.
		return std::nullopt;
	}
}

} // namespace senp
