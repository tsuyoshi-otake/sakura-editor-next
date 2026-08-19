/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "update/IUpdateService.h"
#include "update/UpdateInstallLocation.h"

namespace {

using update::EUpdateType;
using update::IsInnoSetupInstall;
using update::ResolveInstallTarget;
using update::UpdateFileExistsPredicate;
using update::UpdateInstallTarget;

//! A predicate that answers every question the same way, without touching the
//! real filesystem.
UpdateFileExistsPredicate Always(bool answer)
{
	return [answer](const std::filesystem::path&) { return answer; };
}

//! A predicate that answers `true` for exactly one path, so a test can assert
//! Setup targets the right directory without a stray match hiding a bug.
UpdateFileExistsPredicate OnlyFor(const std::filesystem::path& expected)
{
	return [expected](const std::filesystem::path& candidate) { return candidate == expected; };
}

//! Records every path it was asked about instead of answering from disk, so a
//! test can assert exactly which path the probe queried.
class RecordingPredicate final {
public:
	explicit RecordingPredicate(bool answer) : m_answer(answer) {}

	bool operator()(const std::filesystem::path& candidate)
	{
		m_queried.push_back(candidate);
		return m_answer;
	}

	[[nodiscard]] const std::vector<std::filesystem::path>& Queried() const { return m_queried; }

private:
	bool m_answer;
	std::vector<std::filesystem::path> m_queried;
};

} // namespace

TEST(UpdateInstallLocation, AnExecutableBesideAnUninstallerResolvesToSetupInItsOwnDirectoryEvenWithoutARecordedLocation)
{
	const std::filesystem::path executablePath = LR"(C:\Program Files\Sakura Editor NEXT\sakura.exe)";
	const auto fileExists = OnlyFor(executablePath.parent_path() / L"unins000.exe");

	const UpdateInstallTarget target =
		ResolveInstallTarget(executablePath, std::nullopt, fileExists, Always(false));

	EXPECT_EQ(EUpdateType::Setup, target.type);
	EXPECT_EQ(executablePath.parent_path().wstring(), target.installDirectory);
}

TEST(UpdateInstallLocation, TheUninstallerBesideTheExecutableWinsOverADifferentRecordedInstallLocation)
{
	const std::filesystem::path executablePath = LR"(C:\Program Files\Sakura Editor NEXT\sakura.exe)";
	const auto fileExists = OnlyFor(executablePath.parent_path() / L"unins000.exe");
	const std::optional<std::wstring> installedLocation = LR"(C:\Users\dev\AppData\Local\Programs\OldSakura)";

	const UpdateInstallTarget target =
		ResolveInstallTarget(executablePath, installedLocation, fileExists, Always(true));

	EXPECT_EQ(EUpdateType::Setup, target.type);
	EXPECT_EQ(executablePath.parent_path().wstring(), target.installDirectory);
	EXPECT_NE(*installedLocation, target.installDirectory);
}

TEST(UpdateInstallLocation, NoUninstallerBesideTheExecutablePlusARecordedLocationThatExistsResolvesToSetupThere)
{
	const std::filesystem::path executablePath = LR"(C:\dev\builds\sakura.exe)";
	const std::optional<std::wstring> installedLocation = LR"(C:\Program Files\Sakura Editor NEXT)";

	const UpdateInstallTarget target =
		ResolveInstallTarget(executablePath, installedLocation, Always(false), Always(true));

	EXPECT_EQ(EUpdateType::Setup, target.type);
	EXPECT_EQ(*installedLocation, target.installDirectory);
}

TEST(UpdateInstallLocation, TheRecordedInstallLocationKeepsItsExactFormTrailingBackslashIncluded)
{
	const std::filesystem::path executablePath = LR"(C:\dev\builds\sakura.exe)";
	const std::optional<std::wstring> installedLocation = LR"(C:\Program Files\Sakura Editor NEXT\)";

	const UpdateInstallTarget target =
		ResolveInstallTarget(executablePath, installedLocation, Always(false), Always(true));

	EXPECT_EQ(EUpdateType::Setup, target.type);
	EXPECT_EQ(*installedLocation, target.installDirectory);
	ASSERT_FALSE(target.installDirectory.empty());
	EXPECT_EQ(L'\\', target.installDirectory.back());
}

TEST(UpdateInstallLocation, NoUninstallerAndNoRecordedLocationResolvesToArchiveWithAnEmptyInstallDirectory)
{
	const std::filesystem::path executablePath = LR"(C:\dev\builds\sakura.exe)";

	const UpdateInstallTarget target =
		ResolveInstallTarget(executablePath, std::nullopt, Always(false), Always(false));

	EXPECT_EQ(EUpdateType::Archive, target.type);
	EXPECT_TRUE(target.installDirectory.empty());
}

TEST(UpdateInstallLocation, NoUninstallerAndARecordedLocationWhoseDirectoryNoLongerExistsResolvesToArchive)
{
	const std::filesystem::path executablePath = LR"(C:\dev\builds\sakura.exe)";
	const std::optional<std::wstring> installedLocation = LR"(C:\Program Files\Sakura Editor NEXT)";

	const UpdateInstallTarget target =
		ResolveInstallTarget(executablePath, installedLocation, Always(false), Always(false));

	EXPECT_EQ(EUpdateType::Archive, target.type);
}

TEST(UpdateInstallLocation, AnEmptyRecordedInstallLocationStringResolvesToArchive)
{
	const std::filesystem::path executablePath = LR"(C:\dev\builds\sakura.exe)";
	const std::optional<std::wstring> installedLocation = std::wstring();

	const UpdateInstallTarget target =
		ResolveInstallTarget(executablePath, installedLocation, Always(false), Always(true));

	EXPECT_EQ(EUpdateType::Archive, target.type);
	EXPECT_TRUE(target.installDirectory.empty());
}

TEST(UpdateInstallLocation, AnEmptyExecutablePathResolvesToArchiveAndIsNeverSeenAsAnInnoSetupInstall)
{
	const std::filesystem::path executablePath;

	EXPECT_FALSE(IsInnoSetupInstall(executablePath, Always(true)));

	const UpdateInstallTarget target =
		ResolveInstallTarget(executablePath, std::nullopt, Always(true), Always(true));

	EXPECT_EQ(EUpdateType::Archive, target.type);
	EXPECT_TRUE(target.installDirectory.empty());
}

TEST(UpdateInstallLocation, AnEmptyPredicateIsTreatedAsNoAndToleratedWithoutCrashing)
{
	const std::filesystem::path executablePath = LR"(C:\dev\builds\sakura.exe)";
	const UpdateFileExistsPredicate emptyPredicate;

	EXPECT_FALSE(IsInnoSetupInstall(executablePath, emptyPredicate));

	const UpdateInstallTarget resolvedWithoutLocation =
		ResolveInstallTarget(executablePath, std::nullopt, emptyPredicate, emptyPredicate);
	EXPECT_EQ(EUpdateType::Archive, resolvedWithoutLocation.type);

	const std::optional<std::wstring> installedLocation = LR"(C:\Program Files\Sakura Editor NEXT)";
	const UpdateInstallTarget resolvedWithLocation =
		ResolveInstallTarget(executablePath, installedLocation, emptyPredicate, emptyPredicate);
	EXPECT_EQ(EUpdateType::Archive, resolvedWithLocation.type);
}

TEST(UpdateInstallLocation, TheUninstallerProbeQueriesExactlyTheExecutableDirectoryJoinedWithUnins000Exe)
{
	const std::filesystem::path executablePath = LR"(C:\Program Files\Sakura Editor NEXT\sakura.exe)";
	RecordingPredicate recorder(false);

	const bool isInnoSetupInstall = IsInnoSetupInstall(
		executablePath, [&recorder](const std::filesystem::path& candidate) { return recorder(candidate); });

	EXPECT_FALSE(isInnoSetupInstall);
	ASSERT_EQ(1u, recorder.Queried().size());
	EXPECT_EQ(
		LR"(C:\Program Files\Sakura Editor NEXT\unins000.exe)", recorder.Queried().front().wstring());
}
