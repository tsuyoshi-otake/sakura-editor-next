/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "update/UpdateInstallerCommandLine.h"

namespace {

using update::BuildInstallerArguments;
using update::BuildInstallerCommandLine;
using update::InstallerInvocation;
using update::QuoteInstallerArgument;
using update::kUpdateRelaunchSwitch;

InstallerInvocation Invocation()
{
	InstallerInvocation invocation;
	invocation.installerPath = LR"(C:\Users\dev\AppData\Local\sakura-editor-next\update\7300\sakura_install3-1-0-7300-x64.exe)";
	invocation.installDirectory = LR"(C:\Program Files\sakura)";
	invocation.logPath = LR"(C:\Users\dev\AppData\Local\sakura-editor-next\update\7300\install.log)";
	invocation.relaunchAfterInstall = true;
	return invocation;
}

bool Contains(const std::vector<std::wstring>& arguments, std::wstring_view value)
{
	return std::find(arguments.begin(), arguments.end(), value) != arguments.end();
}

} // namespace

TEST(UpdateInstallerCommandLine, BuildsTheSilentReinstallSwitchesInTheOrderSetupReceivesThem)
{
	const auto arguments = BuildInstallerArguments(Invocation());
	ASSERT_TRUE(arguments.has_value());
	const std::vector<std::wstring> expected{
		L"/VERYSILENT",
		L"/SUPPRESSMSGBOXES",
		L"/NORESTART",
		LR"(/DIR=C:\Program Files\sakura)",
		L"/UPDATERELAUNCH=1",
		LR"(/LOG=C:\Users\dev\AppData\Local\sakura-editor-next\update\7300\install.log)",
	};
	EXPECT_EQ(expected, *arguments);
	EXPECT_EQ(kUpdateRelaunchSwitch, (*arguments)[4]);
}

TEST(UpdateInstallerCommandLine, OmitsTheTwoSwitchesThePackageWouldRejectOrRegret)
{
	const auto arguments = BuildInstallerArguments(Invocation());
	ASSERT_TRUE(arguments.has_value());
	// `PrivilegesRequired=lowest` without an override makes Setup reject
	// `/CURRENTUSER` outright, and `/NOICONS` would delete the Start Menu
	// shortcuts the user already has instead of leaving them alone.
	EXPECT_FALSE(Contains(*arguments, L"/CURRENTUSER"));
	EXPECT_FALSE(Contains(*arguments, L"/NOICONS"));
}

TEST(UpdateInstallerCommandLine, DropsTheRelaunchSwitchAndTheLogWhenTheyAreNotWanted)
{
	auto invocation = Invocation();
	invocation.relaunchAfterInstall = false;
	invocation.logPath.clear();
	const auto arguments = BuildInstallerArguments(invocation);
	ASSERT_TRUE(arguments.has_value());
	const std::vector<std::wstring> expected{
		L"/VERYSILENT",
		L"/SUPPRESSMSGBOXES",
		L"/NORESTART",
		LR"(/DIR=C:\Program Files\sakura)",
	};
	EXPECT_EQ(expected, *arguments);
}

TEST(UpdateInstallerCommandLine, NormalizesATrailingSeparatorWithoutEatingADriveRoot)
{
	auto invocation = Invocation();
	invocation.installDirectory = LR"(C:\Program Files\sakura\)";
	const auto trimmed = BuildInstallerArguments(invocation);
	ASSERT_TRUE(trimmed.has_value());
	EXPECT_TRUE(Contains(*trimmed, LR"(/DIR=C:\Program Files\sakura)"));

	invocation.installDirectory = LR"(C:\)";
	const auto root = BuildInstallerArguments(invocation);
	ASSERT_TRUE(root.has_value());
	EXPECT_TRUE(Contains(*root, LR"(/DIR=C:\)"));
}

TEST(UpdateInstallerCommandLine, RefusesAnInvocationThatCouldReinstallSomewhereUnintended)
{
	// An empty or relative install directory means "let Setup choose", which
	// during a silent reinstall can only choose wrongly.
	auto noDirectory = Invocation();
	noDirectory.installDirectory.clear();
	EXPECT_FALSE(BuildInstallerArguments(noDirectory).has_value());

	auto relativeDirectory = Invocation();
	relativeDirectory.installDirectory = L"sakura";
	EXPECT_FALSE(BuildInstallerArguments(relativeDirectory).has_value());

	auto noInstaller = Invocation();
	noInstaller.installerPath.clear();
	EXPECT_FALSE(BuildInstallerArguments(noInstaller).has_value());

	auto relativeInstaller = Invocation();
	relativeInstaller.installerPath = L"sakura_install3-1-0-7300-x64.exe";
	EXPECT_FALSE(BuildInstallerArguments(relativeInstaller).has_value());

	auto notAnExecutable = Invocation();
	notAnExecutable.installerPath = LR"(C:\staging\sakura_install3-1-0-7300-x64.zip)";
	EXPECT_FALSE(BuildInstallerArguments(notAnExecutable).has_value());

	auto quotedInstaller = Invocation();
	quotedInstaller.installerPath = LR"(C:\staging\evil".exe)";
	EXPECT_FALSE(BuildInstallerArguments(quotedInstaller).has_value());

	auto relativeLog = Invocation();
	relativeLog.logPath = L"install.log";
	EXPECT_FALSE(BuildInstallerArguments(relativeLog).has_value());
}

TEST(UpdateInstallerCommandLine, AcceptsAUncInstallationAndACaseInsensitiveExecutableSuffix)
{
	auto unc = Invocation();
	unc.installerPath = LR"(\\build\share\sakura_install3-1-0-7300-x64.EXE)";
	unc.installDirectory = LR"(\\build\share\sakura)";
	unc.logPath = LR"(\\build\share\install.log)";
	const auto arguments = BuildInstallerArguments(unc);
	ASSERT_TRUE(arguments.has_value());
	EXPECT_TRUE(Contains(*arguments, LR"(/DIR=\\build\share\sakura)"));

	auto forwardSlashes = Invocation();
	forwardSlashes.installerPath = L"C:/staging/sakura_install3-1-0-7300-x64.exe";
	forwardSlashes.installDirectory = L"C:/Program Files/sakura";
	EXPECT_TRUE(BuildInstallerArguments(forwardSlashes).has_value());
}

/*
	MSVC's traditional preprocessor (the default without /Zc:preprocessor) does not understand
	raw string literals: it re-lexes them as ordinary strings while collecting macro arguments,
	so a raw string containing a double quote splits into garbage tokens inside EXPECT_EQ. Every
	expectation below therefore names its literal outside the macro. Keep them out of the macro
	arguments; moving one back inline breaks the build rather than just the assertion.
*/
TEST(UpdateInstallerCommandLine, QuotesEachArgumentSoCommandLineToArgvReproducesItExactly)
{
	constexpr const wchar_t* kEmptyQuoted = LR"("")";
	constexpr const wchar_t* kSpaceQuoted = LR"("has space")";
	constexpr const wchar_t* kTrailingBackslashQuoted = LR"("C:\Program Files\sakura\\")";
	constexpr const wchar_t* kTrailingBackslash = LR"(C:\Program Files\sakura\)";
	constexpr const wchar_t* kEmbeddedQuoteQuoted = LR"("a \\\"b")";
	constexpr const wchar_t* kEmbeddedQuote = LR"(a \"b)";

	EXPECT_EQ(L"plain", QuoteInstallerArgument(L"plain"));
	EXPECT_EQ(kEmptyQuoted, QuoteInstallerArgument(L""));
	EXPECT_EQ(kSpaceQuoted, QuoteInstallerArgument(L"has space"));
	EXPECT_EQ(L"\"has\ttab\"", QuoteInstallerArgument(L"has\ttab"));
	// The backslash run before the closing quote must be doubled, or the quote
	// that ends a directory argument would be escaped by the path's own separator.
	EXPECT_EQ(kTrailingBackslashQuoted, QuoteInstallerArgument(kTrailingBackslash));
	EXPECT_EQ(kEmbeddedQuoteQuoted, QuoteInstallerArgument(kEmbeddedQuote));
}

TEST(UpdateInstallerCommandLine, PutsTheInstallerPathFirstOnTheCreateProcessCommandLine)
{
	constexpr const wchar_t* kExpectedCommandLine =
		LR"(C:\Users\dev\AppData\Local\sakura-editor-next\update\7300\sakura_install3-1-0-7300-x64.exe)"
		LR"( /VERYSILENT /SUPPRESSMSGBOXES /NORESTART "/DIR=C:\Program Files\sakura" /UPDATERELAUNCH=1)"
		LR"( /LOG=C:\Users\dev\AppData\Local\sakura-editor-next\update\7300\install.log)";
	constexpr const wchar_t* kExpectedSpacedCommandLine =
		LR"("C:\Users\Dev User\update\sakura_install3-1-0-7300-x64.exe")"
		LR"( /VERYSILENT /SUPPRESSMSGBOXES /NORESTART "/DIR=C:\Program Files\sakura" /UPDATERELAUNCH=1)";

	const auto commandLine = BuildInstallerCommandLine(Invocation());
	ASSERT_TRUE(commandLine.has_value());
	EXPECT_EQ(kExpectedCommandLine, *commandLine);

	// A profile path containing a space becomes one quoted argv[0], not two arguments.
	auto spaced = Invocation();
	spaced.installerPath = LR"(C:\Users\Dev User\update\sakura_install3-1-0-7300-x64.exe)";
	spaced.logPath.clear();
	const auto spacedCommandLine = BuildInstallerCommandLine(spaced);
	ASSERT_TRUE(spacedCommandLine.has_value());
	EXPECT_EQ(kExpectedSpacedCommandLine, *spacedCommandLine);

	auto refused = Invocation();
	refused.installDirectory.clear();
	EXPECT_FALSE(BuildInstallerCommandLine(refused).has_value());
}
