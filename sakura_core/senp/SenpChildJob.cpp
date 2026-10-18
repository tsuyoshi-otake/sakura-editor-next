/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "StdAfx.h"
#include "senp/SenpChildJob.h"

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

} // namespace senp
