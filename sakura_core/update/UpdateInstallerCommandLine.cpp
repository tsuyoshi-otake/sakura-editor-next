/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "update/UpdateInstallerCommandLine.h"

namespace update {
namespace {

//! Nothing in this command line is ever allowed to carry a quote or a control
//! character. Both would let a crafted path end one argument and begin another,
//! and the arguments here decide where Setup installs and whether it relaunches.
bool IsQuotableArgument(std::wstring_view value) noexcept
{
	for (const wchar_t ch : value) {
		if (ch == L'"') return false;
		if (ch < 0x20) return false;
	}
	return true;
}

bool EndsWithIgnoringAsciiCase(std::wstring_view value, std::wstring_view suffix) noexcept
{
	if (value.size() < suffix.size()) return false;
	const std::wstring_view tail = value.substr(value.size() - suffix.size());
	for (std::size_t index = 0; index < suffix.size(); ++index) {
		wchar_t left = tail[index];
		wchar_t right = suffix[index];
		if (left >= L'A' && left <= L'Z') left = static_cast<wchar_t>(left - L'A' + L'a');
		if (right >= L'A' && right <= L'Z') right = static_cast<wchar_t>(right - L'A' + L'a');
		if (left != right) return false;
	}
	return true;
}

//! `C:\...` or `\\server\share\...`. A relative path would resolve against
//! whatever working directory the launcher happened to have, which is not a
//! decision an installer target may be left to.
bool IsAbsolutePath(std::wstring_view value) noexcept
{
	if (value.size() >= 3 && value[1] == L':' && (value[2] == L'\\' || value[2] == L'/')) {
		const wchar_t drive = value[0];
		return (drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z');
	}
	return value.size() >= 3 && value[0] == L'\\' && value[1] == L'\\';
}

//! Drops trailing separators so `/DIR=` never ends in a backslash.
//!
//! A path that ends in `\` has to be encoded as `"C:\dir\\"` to survive
//! `CommandLineToArgvW`, and Setup's own parser is not the one this process
//! runs. Removing the separator sidesteps the disagreement entirely; `C:\dir`
//! and `C:\dir\` name the same directory. A drive or UNC root keeps its
//! separator, because removing it would change the meaning.
std::wstring NormalizeDirectory(std::wstring_view value)
{
	std::wstring normalized(value);
	while (normalized.size() > 1 && (normalized.back() == L'\\' || normalized.back() == L'/')) {
		const wchar_t previous = normalized[normalized.size() - 2];
		if (previous == L':' || previous == L'\\' || previous == L'/') break;
		normalized.pop_back();
	}
	return normalized;
}

} // namespace

std::wstring QuoteInstallerArgument(std::wstring_view value)
{
	const bool needsQuoting = value.empty()
		|| value.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
	if (!needsQuoting) return std::wstring(value);

	std::wstring quoted;
	quoted.reserve(value.size() + 2);
	quoted.push_back(L'"');
	for (std::size_t index = 0; index < value.size(); ++index) {
		std::size_t backslashes = 0;
		while (index < value.size() && value[index] == L'\\') {
			++index;
			++backslashes;
		}
		if (index == value.size()) {
			// Backslashes that run into the closing quote must be doubled, or
			// the quote itself is escaped and the argument swallows the next one.
			quoted.append(backslashes * 2, L'\\');
			break;
		}
		if (value[index] == L'"') {
			quoted.append(backslashes * 2 + 1, L'\\');
		} else {
			quoted.append(backslashes, L'\\');
		}
		quoted.push_back(value[index]);
	}
	quoted.push_back(L'"');
	return quoted;
}

std::optional<std::vector<std::wstring>> BuildInstallerArguments(const InstallerInvocation& invocation)
{
	if (invocation.installerPath.empty() || !IsQuotableArgument(invocation.installerPath)) return std::nullopt;
	if (!IsAbsolutePath(invocation.installerPath)) return std::nullopt;
	if (!EndsWithIgnoringAsciiCase(invocation.installerPath, L".exe")) return std::nullopt;

	// An empty install directory is refused rather than defaulted: Setup would
	// then pick its own location and the user would end up with two copies.
	if (invocation.installDirectory.empty() || !IsQuotableArgument(invocation.installDirectory)) return std::nullopt;
	if (!IsAbsolutePath(invocation.installDirectory)) return std::nullopt;

	if (!IsQuotableArgument(invocation.logPath)) return std::nullopt;
	if (!invocation.logPath.empty() && !IsAbsolutePath(invocation.logPath)) return std::nullopt;

	std::vector<std::wstring> arguments;
	arguments.emplace_back(L"/VERYSILENT");
	arguments.emplace_back(L"/SUPPRESSMSGBOXES");
	arguments.emplace_back(L"/NORESTART");
	arguments.emplace_back(L"/DIR=" + NormalizeDirectory(invocation.installDirectory));
	if (invocation.relaunchAfterInstall) {
		arguments.emplace_back(kUpdateRelaunchSwitch);
	}
	if (!invocation.logPath.empty()) {
		arguments.emplace_back(L"/LOG=" + invocation.logPath);
	}
	return arguments;
}

std::optional<std::wstring> BuildInstallerCommandLine(const InstallerInvocation& invocation)
{
	const auto arguments = BuildInstallerArguments(invocation);
	if (!arguments) return std::nullopt;

	std::wstring commandLine = QuoteInstallerArgument(invocation.installerPath);
	for (const std::wstring& argument : *arguments) {
		commandLine.push_back(L' ');
		commandLine.append(QuoteInstallerArgument(argument));
	}
	return commandLine;
}

} // namespace update
