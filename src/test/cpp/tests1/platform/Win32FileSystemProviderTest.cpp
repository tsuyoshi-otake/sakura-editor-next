/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include "platform/filesystem/CWin32FileSystemProvider.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

namespace platform::filesystem {
namespace {

class CScopedTestDirectory final {
public:
	CScopedTestDirectory()
	{
		const auto nonce = std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(::GetTickCount64());
		m_path = std::filesystem::temp_directory_path() / (L"sakura-win32-filesystem-provider-" + nonce);
		std::filesystem::create_directories(m_path);
	}

	~CScopedTestDirectory()
	{
		std::error_code ignored;
		std::filesystem::remove_all(m_path, ignored);
	}

	const std::filesystem::path& Path() const noexcept { return m_path; }

private:
	std::filesystem::path m_path;
};

platform::uri::Uri UriFor(const std::filesystem::path& path)
{
	auto uri = platform::uri::Uri::FromWindowsPath(path.wstring());
	EXPECT_TRUE(uri);
	return std::move(*uri.value);
}

FileBytes Bytes(std::string_view text)
{
	return FileBytes(text.begin(), text.end());
}

FileResult<FileWatchEvent> NextWithin(IFileWatch& watch)
{
	// The provider's production Next is intentionally blocking.  Every action in
	// these tests is performed before Next, so a failure is a useful test timeout
	// rather than a polling loop.
	return watch.Next();
}

FileResult<FileWatchEvent> NextOfType(IFileWatch& watch, EFileWatchEventType expected)
{
	// Writes commonly produce more than one advisory Changed notification.  The
	// ordering contract is only meaningful for a rename pair, so consume the
	// bounded burst until the action under test is observed.
	for (int attempt = 0; attempt < 16; ++attempt) {
		auto event = NextWithin(watch);
		if (!event || event.value->type == expected) return event;
	}
	return FileResult<FileWatchEvent>::Failure(EFileResultStatus::Failed, L"expected watch notification was not observed in its bounded event burst");
}

TEST(Win32FileSystemProvider, StatsAndEnumeratesOnlyLocalFileUris)
{
	CScopedTestDirectory directory;
	const auto file = directory.Path() / L"sample.txt";
	{
		std::ofstream stream(file, std::ios::binary);
		stream << "sakura";
	}
	CWin32FileSystemProvider provider;
	const auto fileUri = UriFor(file);
	const auto directoryUri = UriFor(directory.Path());

	const auto stat = provider.Stat(fileUri);
	ASSERT_TRUE(stat);
	ASSERT_TRUE(stat.value);
	EXPECT_EQ(EFileEntryType::File, stat.value->type);
	EXPECT_EQ(6u, stat.value->size);
	EXPECT_EQ(fileUri.ToString(), stat.value->uri.ToString());

	const auto entries = provider.Enumerate(directoryUri);
	ASSERT_TRUE(entries);
	ASSERT_TRUE(entries.value);
	ASSERT_EQ(1u, entries.value->size());
	EXPECT_EQ(L"sample.txt", entries.value->front().name);
	EXPECT_EQ(fileUri.ToString(), entries.value->front().uri.ToString());
	EXPECT_EQ(entries.value->front().uri.ToString(), entries.value->front().stat.uri.ToString());

	auto parsedVirtual = platform::uri::Uri::Parse(L"memfs://session/virtual.txt");
	ASSERT_TRUE(parsedVirtual);
	const auto virtualUri = std::move(*parsedVirtual.value);
	EXPECT_EQ(EFileResultStatus::InvalidUri, provider.Stat(virtualUri).status);
	EXPECT_EQ(EFileResultStatus::InvalidUri, provider.Enumerate(virtualUri).status);
	EXPECT_EQ(EFileResultStatus::InvalidUri, provider.Read(virtualUri, { .maximumBytes = 1 }).status);
	EXPECT_EQ(EFileResultStatus::InvalidUri, provider.ReadVersioned(virtualUri, { .maximumBytes = 1 }).status);
	EXPECT_EQ(
		EFileConditionalReplaceStatus::Failed,
		provider.ConditionalAtomicReplace(
			virtualUri, {}, FileConditionalReplaceOptions::ForMissing()).status);
	EXPECT_EQ(EFileResultStatus::InvalidUri, provider.Watch(virtualUri, {}).status);
	auto parsedDevice = platform::uri::Uri::Parse(L"file://./pipe/sakura-test");
	ASSERT_TRUE(parsedDevice);
	EXPECT_EQ(EFileResultStatus::InvalidUri, provider.Stat(*parsedDevice.value).status);
	EXPECT_EQ(EFileResultStatus::Unsupported, provider.Watch(directoryUri, { .recursive = true }).status);
	EXPECT_TRUE(HasCapability(provider.Capabilities(), EFileSystemCapability::Read));
	EXPECT_TRUE(HasCapability(provider.Capabilities(), EFileSystemCapability::Write));
	EXPECT_TRUE(HasCapability(provider.Capabilities(), EFileSystemCapability::AtomicReplace));
}

TEST(Win32FileSystemProvider, ReadsExactBinaryBytesWithinAnExplicitBound)
{
	CScopedTestDirectory directory;
	const auto file = directory.Path() / L"binary.dat";
	const std::uint8_t expected[] = { 0x53, 0x00, 0x4b, 0xff };
	{
		std::ofstream stream(file, std::ios::binary);
		stream.write(reinterpret_cast<const char*>(expected), sizeof(expected));
	}
	CWin32FileSystemProvider provider;
	const auto fileUri = UriFor(file);

	const auto read = provider.Read(fileUri, { .maximumBytes = sizeof(expected) });
	ASSERT_TRUE(read);
	ASSERT_TRUE(read.value);
	EXPECT_EQ((FileBytes{ 0x53, 0x00, 0x4b, 0xff }), *read.value);

	const auto versioned = provider.ReadVersioned(fileUri, { .maximumBytes = sizeof(expected) });
	ASSERT_TRUE(versioned);
	ASSERT_TRUE(versioned.value);
	EXPECT_EQ((FileBytes{ 0x53, 0x00, 0x4b, 0xff }), versioned.value->bytes);
	EXPECT_FALSE(versioned.value->version.Empty());

	EXPECT_EQ(EFileResultStatus::Failed, provider.Read(fileUri, { .maximumBytes = 0 }).status);
	EXPECT_EQ(EFileResultStatus::Failed, provider.Read(fileUri, { .maximumBytes = sizeof(expected) - 1 }).status);
	EXPECT_EQ(EFileResultStatus::Failed, provider.ReadVersioned(fileUri, { .maximumBytes = 0 }).status);
	EXPECT_EQ(EFileResultStatus::Failed, provider.ReadVersioned(fileUri, { .maximumBytes = sizeof(expected) - 1 }).status);
	EXPECT_EQ(EFileResultStatus::NotDirectory, provider.Read(UriFor(directory.Path()), { .maximumBytes = 16 }).status);
	EXPECT_EQ(EFileResultStatus::NotDirectory, provider.ReadVersioned(UriFor(directory.Path()), { .maximumBytes = 16 }).status);
	EXPECT_EQ(EFileResultStatus::NotFound, provider.Read(UriFor(directory.Path() / L"missing.dat"), { .maximumBytes = 16 }).status);
	EXPECT_EQ(EFileResultStatus::NotFound, provider.ReadVersioned(UriFor(directory.Path() / L"missing.dat"), { .maximumBytes = 16 }).status);
}

TEST(Win32FileSystemProvider, ReplacesTheExpectedCurrentVersionAndReturnsCommittedVersion)
{
	CScopedTestDirectory directory;
	const auto file = directory.Path() / L"settings.json";
	{
		std::ofstream stream(file, std::ios::binary);
		stream << "{\r\n  // preserved by the caller\r\n  \"value\": 1\r\n}\r\n";
	}
	CWin32FileSystemProvider provider;
	const auto fileUri = UriFor(file);
	const auto initial = provider.ReadVersioned(fileUri, { .maximumBytes = 4096 });
	ASSERT_TRUE(initial);
	ASSERT_TRUE(initial.value);

	const auto replacement = Bytes("{\r\n  \"value\": 2\r\n}\r\n");
	const auto replaced = provider.ConditionalAtomicReplace(
		fileUri,
		replacement,
		FileConditionalReplaceOptions::ForCurrent(initial.value->version));
	ASSERT_EQ(EFileConditionalReplaceStatus::Succeeded, replaced.status)
		<< std::string(replaced.diagnostic.begin(), replaced.diagnostic.end());
	ASSERT_TRUE(replaced.committedVersion);

	const auto readback = provider.ReadVersioned(fileUri, { .maximumBytes = 4096 });
	ASSERT_TRUE(readback);
	ASSERT_TRUE(readback.value);
	EXPECT_EQ(replacement, readback.value->bytes);
	EXPECT_EQ(*replaced.committedVersion, readback.value->version);
	EXPECT_NE(initial.value->version, readback.value->version);
}

TEST(Win32FileSystemProvider, RejectsAStaleCurrentVersionAndCleansItsStagingFile)
{
	CScopedTestDirectory directory;
	const auto file = directory.Path() / L"settings.json";
	{
		std::ofstream stream(file, std::ios::binary);
		stream << "initial";
	}
	CWin32FileSystemProvider provider;
	const auto fileUri = UriFor(file);
	const auto initial = provider.ReadVersioned(fileUri, { .maximumBytes = 4096 });
	ASSERT_TRUE(initial);
	ASSERT_TRUE(initial.value);

	{
		std::ofstream stream(file, std::ios::binary | std::ios::trunc);
		stream << "external-change";
	}
	const auto conflict = provider.ConditionalAtomicReplace(
		fileUri,
		Bytes("our-change"),
		FileConditionalReplaceOptions::ForCurrent(initial.value->version));
	EXPECT_EQ(EFileConditionalReplaceStatus::Conflict, conflict.status);
	EXPECT_FALSE(conflict.committedVersion);

	const auto preserved = provider.Read(fileUri, { .maximumBytes = 4096 });
	ASSERT_TRUE(preserved);
	ASSERT_TRUE(preserved.value);
	EXPECT_EQ(Bytes("external-change"), *preserved.value);
	const auto entries = provider.Enumerate(UriFor(directory.Path()));
	ASSERT_TRUE(entries);
	ASSERT_TRUE(entries.value);
	ASSERT_EQ(1u, entries.value->size());
	EXPECT_EQ(L"settings.json", entries.value->front().name);
}

TEST(Win32FileSystemProvider, CreatesOnlyWhenMissingAndConflictsWhenThePathExists)
{
	CScopedTestDirectory directory;
	CWin32FileSystemProvider provider;
	const auto file = directory.Path() / L"new-settings.json";
	const auto fileUri = UriFor(file);
	const auto replacement = Bytes("{\"created\":true}\n");

	const auto created = provider.ConditionalAtomicReplace(
		fileUri, replacement, FileConditionalReplaceOptions::ForMissing());
	ASSERT_EQ(EFileConditionalReplaceStatus::Succeeded, created.status);
	ASSERT_TRUE(created.committedVersion);
	const auto readback = provider.ReadVersioned(fileUri, { .maximumBytes = 4096 });
	ASSERT_TRUE(readback);
	ASSERT_TRUE(readback.value);
	EXPECT_EQ(replacement, readback.value->bytes);
	EXPECT_EQ(*created.committedVersion, readback.value->version);

	const auto conflict = provider.ConditionalAtomicReplace(
		fileUri, Bytes("must-not-win"), FileConditionalReplaceOptions::ForMissing());
	EXPECT_EQ(EFileConditionalReplaceStatus::Conflict, conflict.status);
	EXPECT_FALSE(conflict.committedVersion);
	const auto preserved = provider.Read(fileUri, { .maximumBytes = 4096 });
	ASSERT_TRUE(preserved);
	ASSERT_TRUE(preserved.value);
	EXPECT_EQ(replacement, *preserved.value);

	const auto entries = provider.Enumerate(UriFor(directory.Path()));
	ASSERT_TRUE(entries);
	ASSERT_TRUE(entries.value);
	ASSERT_EQ(1u, entries.value->size());
	EXPECT_EQ(L"new-settings.json", entries.value->front().name);
}

TEST(Win32FileSystemProvider, ReportsReparsePointsAsSymbolicLinkWhenPermitted)
{
	CScopedTestDirectory directory;
	const auto target = directory.Path() / L"target.txt";
	const auto link = directory.Path() / L"link.txt";
	std::ofstream(target) << "target";
	std::error_code error;
	std::filesystem::create_symlink(target, link, error);
	if (error) {
		GTEST_SKIP() << "creating a symbolic link is not permitted for this test account";
	}

	CWin32FileSystemProvider provider;
	const auto stat = provider.Stat(UriFor(link));
	ASSERT_TRUE(stat);
	ASSERT_TRUE(stat.value);
	EXPECT_EQ(EFileEntryType::SymbolicLink, stat.value->type);

	const auto targetDirectory = directory.Path() / L"target-directory";
	const auto directoryLink = directory.Path() / L"directory-link";
	std::filesystem::create_directory(targetDirectory);
	std::filesystem::create_directory_symlink(targetDirectory, directoryLink, error);
	ASSERT_FALSE(error) << "file symlink creation succeeded but directory symlink creation failed";
	const auto directoryLinkUri = UriFor(directoryLink);
	EXPECT_EQ(EFileResultStatus::Unsupported, provider.Enumerate(directoryLinkUri).status);
	EXPECT_EQ(EFileResultStatus::Unsupported, provider.Watch(directoryLinkUri, {}).status);
}

TEST(Win32FileSystemProvider, WatchesChangesAndTerminatesWithoutAWorkerLeak)
{
	CScopedTestDirectory directory;
	CWin32FileSystemProvider provider;
	auto watched = provider.Watch(UriFor(directory.Path()), {});
	ASSERT_TRUE(watched);
	ASSERT_TRUE(watched.value);
	IFileWatch& watch = **watched.value;

	const auto created = directory.Path() / L"created.txt";
	std::ofstream(created) << "first";
	auto createdEvent = NextOfType(watch, EFileWatchEventType::Created);
	ASSERT_TRUE(createdEvent);
	EXPECT_EQ(EFileWatchEventType::Created, createdEvent.value->type);
	EXPECT_EQ(UriFor(created).ToString(), createdEvent.value->uri.ToString());

	std::ofstream(created, std::ios::app) << " second";
	auto changedEvent = NextOfType(watch, EFileWatchEventType::Changed);
	ASSERT_TRUE(changedEvent);
	EXPECT_EQ(EFileWatchEventType::Changed, changedEvent.value->type);

	const auto renamed = directory.Path() / L"renamed.txt";
	std::filesystem::rename(created, renamed);
	auto renamedEvent = NextOfType(watch, EFileWatchEventType::Renamed);
	ASSERT_TRUE(renamedEvent);
	EXPECT_EQ(EFileWatchEventType::Renamed, renamedEvent.value->type);
	ASSERT_TRUE(renamedEvent.value->previousUri);
	EXPECT_EQ(UriFor(created).ToString(), renamedEvent.value->previousUri->ToString());
	EXPECT_EQ(UriFor(renamed).ToString(), renamedEvent.value->uri.ToString());

	std::filesystem::remove(renamed);
	auto deletedEvent = NextOfType(watch, EFileWatchEventType::Deleted);
	ASSERT_TRUE(deletedEvent);
	EXPECT_EQ(EFileWatchEventType::Deleted, deletedEvent.value->type);

	EXPECT_EQ(EFileResultStatus::Succeeded, watch.Cancel().status);
	auto disposed = watch.Next();
	ASSERT_TRUE(disposed);
	EXPECT_EQ(EFileWatchEventType::Disposed, disposed.value->type);
	EXPECT_EQ(EFileResultStatus::Cancelled, watch.Next().status);
	EXPECT_EQ(EFileResultStatus::Cancelled, watch.Cancel().status);
	// Cancel joins the worker and closes its directory handle synchronously.
	// Dropping the exclusive owner here must not leave a provider worker alive.
	watched.value->reset();
}

} // namespace
} // namespace platform::filesystem
