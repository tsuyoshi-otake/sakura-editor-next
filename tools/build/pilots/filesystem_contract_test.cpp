/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include <sakura/filesystem/FileSystemFactory.h>
#include <sakura/uri/UriIdentity.h>

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace platform::filesystem {
namespace {

#define CHECK_TRUE(expression) do { if (!(expression)) return false; } while (false)
#define CHECK_FALSE(expression) CHECK_TRUE(!(expression))
#define CHECK_EQ(expected, actual) CHECK_TRUE((expected) == (actual))
#define CHECK_NE(expected, actual) CHECK_TRUE((expected) != (actual))

std::optional<platform::uri::Uri> ParseUri(std::wstring_view text)
{
	auto parsed = platform::uri::Uri::Parse(text);
	if (!parsed) return std::nullopt;
	return std::move(*parsed.value);
}

platform::uri::Uri UriFor(const std::filesystem::path& path)
{
	auto uri = platform::uri::Uri::FromWindowsPath(path.wstring());
	if (!uri) std::abort();
	return std::move(*uri.value);
}

FileVersionToken Version(std::uint8_t marker)
{
	const std::array<std::uint8_t, 2> opaque{ 1, marker };
	auto token = FileVersionToken::FromOpaqueBytes(opaque);
	if (!token) std::abort();
	return std::move(*token);
}

class CFakeWatch final : public IFileWatch {
public:
	explicit CFakeWatch(platform::uri::Uri root)
		: m_root(std::move(root))
	{
		m_events.push_back({ .type = EFileWatchEventType::Overflow, .uri = m_root });
		m_events.push_back({ .type = EFileWatchEventType::RescanRequired, .uri = m_root });
	}

	FileResult<void> Cancel() override
	{
		if (m_cancelled) return FileResult<void>::Failure(EFileResultStatus::Cancelled);
		m_cancelled = true;
		return FileResult<void>::Success();
	}

	FileResult<FileWatchEvent> Next() override
	{
		if (!m_events.empty()) {
			auto event = std::move(m_events.front());
			m_events.pop_front();
			return FileResult<FileWatchEvent>::Success(std::move(event));
		}
		if (m_cancelled && !m_disposedDelivered) {
			m_disposedDelivered = true;
			return FileResult<FileWatchEvent>::Success({
				.type = EFileWatchEventType::Disposed, .uri = m_root,
			});
		}
		return FileResult<FileWatchEvent>::Failure(EFileResultStatus::Cancelled);
	}

private:
	platform::uri::Uri m_root;
	std::deque<FileWatchEvent> m_events;
	bool m_cancelled = false;
	bool m_disposedDelivered = false;
};

class CFakeFileSystemProvider final : public IFileSystemProvider {
public:
	explicit CFakeFileSystemProvider(bool supportsRead = true, bool supportsConditionalReplace = false)
		: m_supportsRead(supportsRead)
		, m_supportsConditionalReplace(supportsConditionalReplace)
	{
	}

	FileSystemCapabilities Capabilities() const noexcept override
	{
		FileSystemCapabilities capabilities =
			EFileSystemCapability::Stat | EFileSystemCapability::Enumerate | EFileSystemCapability::Watch;
		if (m_supportsRead) capabilities = capabilities | EFileSystemCapability::Read;
		if (m_supportsConditionalReplace) {
			capabilities = capabilities | EFileSystemCapability::Write;
			capabilities = capabilities | EFileSystemCapability::AtomicReplace;
		}
		return capabilities;
	}

	FileResult<FileStat> Stat(const platform::uri::Uri& resource) override
	{
		++statCalls;
		return FileResult<FileStat>::Success({ .uri = resource, .type = EFileEntryType::File, .size = 42 });
	}

	FileResult<std::vector<DirectoryEntry>> Enumerate(const platform::uri::Uri& directory) override
	{
		++enumerateCalls;
		auto child = platform::uri::Uri::Parse(directory.ToString() + L"/child.txt");
		if (!child) return FileResult<std::vector<DirectoryEntry>>::Failure(EFileResultStatus::Failed);
		FileStat stat{ .uri = *child.value, .type = EFileEntryType::File, .size = 7 };
		return FileResult<std::vector<DirectoryEntry>>::Success({
			{ .uri = *child.value, .name = L"child.txt", .stat = std::move(stat) },
		});
	}

	FileResult<FileBytes> Read(const platform::uri::Uri&, const FileReadOptions&) override
	{
		++readCalls;
		return FileResult<FileBytes>::Success({ 0x53, 0x00, 0x4b });
	}

	FileResult<FileContentSnapshot> ReadVersioned(const platform::uri::Uri&, const FileReadOptions&) override
	{
		++versionedReadCalls;
		return FileResult<FileContentSnapshot>::Success({
			.bytes = { 0x53, 0x00, 0x4b }, .version = Version(42),
		});
	}

	FileConditionalReplaceResult ConditionalAtomicReplace(
		const platform::uri::Uri&, const FileBytes& bytes,
		const FileConditionalReplaceOptions& options) override
	{
		++conditionalReplaceCalls;
		lastReplaceBytes = bytes;
		lastReplaceOptions = options;
		return FileConditionalReplaceResult::Success(Version(43));
	}

	FileResult<std::unique_ptr<IFileWatch>> Watch(
		const platform::uri::Uri& resource, const FileWatchOptions&) override
	{
		++watchCalls;
		return FileResult<std::unique_ptr<IFileWatch>>::Success(std::make_unique<CFakeWatch>(resource));
	}

	int statCalls = 0;
	int enumerateCalls = 0;
	int readCalls = 0;
	int versionedReadCalls = 0;
	int conditionalReplaceCalls = 0;
	int watchCalls = 0;
	FileBytes lastReplaceBytes;
	std::optional<FileConditionalReplaceOptions> lastReplaceOptions;

private:
	bool m_supportsRead;
	bool m_supportsConditionalReplace;
};

std::unique_ptr<IFileService> NewService(bool win32)
{
	auto result = win32 ? CreateWin32FileService() : CreateFileService();
	if (!result.Succeeded() || !result.value) return {};
	return std::move(*result.value);
}

bool RegistersSchemesCaseInsensitivelyAndPreservesUris()
{
	auto service = NewService(false);
	CHECK_TRUE(service);
	auto provider = std::make_shared<CFakeFileSystemProvider>();
	CHECK_EQ(EFileResultStatus::Succeeded, service->RegisterProvider(L"FiLe", provider).status);
	auto resource = ParseUri(L"FILE:///C:/Workspace/ReadMe.md");
	CHECK_TRUE(resource);
	const auto stat = service->Stat(*resource);
	CHECK_TRUE(stat && stat.value);
	CHECK_EQ(resource->ToString(), stat.value->uri.ToString());
	CHECK_EQ(1, provider->statCalls);
	const auto entries = service->Enumerate(*resource);
	CHECK_TRUE(entries && entries.value && entries.value->size() == 1);
	CHECK_EQ(L"file:///C:/Workspace/ReadMe.md/child.txt", entries.value->front().uri.ToString());
	CHECK_EQ(entries.value->front().uri.ToString(), entries.value->front().stat.uri.ToString());
	CHECK_EQ(1, provider->enumerateCalls);
	const auto read = service->Read(*resource, { .maximumBytes = 16 });
	CHECK_TRUE(read && read.value);
	CHECK_EQ((FileBytes{ 0x53, 0x00, 0x4b }), *read.value);
	CHECK_EQ(1, provider->readCalls);
	const auto versioned = service->ReadVersioned(*resource, { .maximumBytes = 16 });
	CHECK_TRUE(versioned && versioned.value);
	CHECK_EQ((FileBytes{ 0x53, 0x00, 0x4b }), versioned.value->bytes);
	CHECK_EQ(Version(42), versioned.value->version);
	CHECK_EQ(1, provider->versionedReadCalls);
	return true;
}

bool RejectsInvalidAndDuplicateRegistrationWithoutFallback()
{
	auto service = NewService(false);
	CHECK_TRUE(service);
	auto provider = std::make_shared<CFakeFileSystemProvider>();
	CHECK_EQ(EFileResultStatus::Failed, service->RegisterProvider(L"not a scheme", provider).status);
	CHECK_EQ(EFileResultStatus::Failed, service->RegisterProvider(L"null-provider", {}).status);
	CHECK_EQ(EFileResultStatus::Succeeded, service->RegisterProvider(L"file", provider).status);
	CHECK_EQ(EFileResultStatus::Failed, service->RegisterProvider(L"FILE", provider).status);
	auto virtualResource = ParseUri(L"memfs://session/virtual.txt");
	CHECK_TRUE(virtualResource);
	CHECK_EQ(EFileResultStatus::Unsupported, service->Stat(*virtualResource).status);
	CHECK_EQ(0, provider->statCalls);
	CHECK_EQ(EFileResultStatus::Unsupported, service->Read(*virtualResource, { .maximumBytes = 1 }).status);
	CHECK_EQ(EFileResultStatus::Unsupported, service->ReadVersioned(*virtualResource, { .maximumBytes = 1 }).status);
	CHECK_EQ(0, provider->readCalls);
	CHECK_EQ(0, provider->versionedReadCalls);
	auto noReadService = NewService(false);
	CHECK_TRUE(noReadService);
	auto noReadProvider = std::make_shared<CFakeFileSystemProvider>(false);
	CHECK_EQ(EFileResultStatus::Succeeded, noReadService->RegisterProvider(L"file", noReadProvider).status);
	auto resource = ParseUri(L"file:///C:/Workspace/ReadMe.md");
	CHECK_TRUE(resource);
	CHECK_EQ(EFileResultStatus::Unsupported, noReadService->Read(*resource, { .maximumBytes = 1 }).status);
	CHECK_EQ(EFileResultStatus::Unsupported, noReadService->ReadVersioned(*resource, { .maximumBytes = 1 }).status);
	CHECK_EQ(0, noReadProvider->readCalls);
	CHECK_EQ(0, noReadProvider->versionedReadCalls);
	return true;
}

bool RejectsAnUnboundedReadBeforeProviderDispatch()
{
	auto service = NewService(false);
	CHECK_TRUE(service);
	auto provider = std::make_shared<CFakeFileSystemProvider>();
	CHECK_EQ(EFileResultStatus::Succeeded, service->RegisterProvider(L"file", provider).status);
	auto resource = ParseUri(L"file:///C:/Workspace/ReadMe.md");
	CHECK_TRUE(resource);
	const auto result = service->Read(*resource, {});
	CHECK_EQ(EFileResultStatus::Failed, result.status);
	CHECK_FALSE(result.value);
	const auto versioned = service->ReadVersioned(*resource, {});
	CHECK_EQ(EFileResultStatus::Failed, versioned.status);
	CHECK_FALSE(versioned.value);
	CHECK_EQ(0, provider->readCalls);
	CHECK_EQ(0, provider->versionedReadCalls);
	return true;
}

bool DispatchesConditionalAtomicReplaceOnlyWithBothCapabilities()
{
	auto resource = ParseUri(L"file:///C:/Workspace/settings.json");
	CHECK_TRUE(resource);
	auto service = NewService(false);
	CHECK_TRUE(service);
	auto provider = std::make_shared<CFakeFileSystemProvider>(true, true);
	CHECK_EQ(EFileResultStatus::Succeeded, service->RegisterProvider(L"file", provider).status);
	const auto expected = Version(7);
	const FileBytes replacement{ 0x7b, 0x7d };
	const auto result = service->ConditionalAtomicReplace(
		*resource, replacement, FileConditionalReplaceOptions::ForCurrent(expected));
	CHECK_EQ(EFileConditionalReplaceStatus::Succeeded, result.status);
	CHECK_TRUE(result.committedVersion);
	CHECK_EQ(Version(43), *result.committedVersion);
	CHECK_EQ(1, provider->conditionalReplaceCalls);
	CHECK_EQ(replacement, provider->lastReplaceBytes);
	CHECK_TRUE(provider->lastReplaceOptions);
	CHECK_EQ(EFileConditionalReplaceExpectation::Current, provider->lastReplaceOptions->expectation);
	CHECK_EQ(expected, provider->lastReplaceOptions->expectedVersion);
	auto unsupported = NewService(false);
	CHECK_TRUE(unsupported);
	auto providerWithoutCapability = std::make_shared<CFakeFileSystemProvider>();
	CHECK_EQ(EFileResultStatus::Succeeded, unsupported->RegisterProvider(L"file", providerWithoutCapability).status);
	CHECK_EQ(EFileConditionalReplaceStatus::Unsupported,
		unsupported->ConditionalAtomicReplace(*resource, replacement, FileConditionalReplaceOptions::ForMissing()).status);
	CHECK_EQ(0, providerWithoutCapability->conditionalReplaceCalls);
	CHECK_EQ(EFileConditionalReplaceStatus::Failed,
		service->ConditionalAtomicReplace(*resource, replacement, {}).status);
	CHECK_EQ(1, provider->conditionalReplaceCalls);
	return true;
}

bool BoundsOpaqueVersionTokens()
{
	const std::array<std::uint8_t, 1> one{ 1 };
	const std::array<std::uint8_t, FileVersionToken::kMaximumBytes> maximum{};
	const std::array<std::uint8_t, FileVersionToken::kMaximumBytes + 1> tooLarge{};
	CHECK_FALSE(FileVersionToken::FromOpaqueBytes(std::span<const std::uint8_t>{}));
	CHECK_TRUE(FileVersionToken::FromOpaqueBytes(one));
	CHECK_TRUE(FileVersionToken::FromOpaqueBytes(maximum));
	CHECK_FALSE(FileVersionToken::FromOpaqueBytes(tooLarge));
	CHECK_NE(Version(1), Version(2));
	return true;
}

bool ReturnsInvalidUriAndProvidesAdvisoryWatchTerminalState()
{
	auto service = NewService(false);
	CHECK_TRUE(service);
	auto provider = std::make_shared<CFakeFileSystemProvider>();
	CHECK_EQ(EFileResultStatus::Succeeded, service->RegisterProvider(L"file", provider).status);
	auto malformed = platform::uri::Uri::Parse(L"file:///C:/bad%2");
	CHECK_FALSE(malformed);
	CHECK_EQ(EFileResultStatus::InvalidUri, service->Stat(malformed).status);
	CHECK_EQ(EFileResultStatus::InvalidUri, service->Read(malformed, { .maximumBytes = 1 }).status);
	CHECK_EQ(EFileResultStatus::InvalidUri, service->ReadVersioned(malformed, { .maximumBytes = 1 }).status);
	CHECK_EQ(EFileConditionalReplaceStatus::Failed,
		service->ConditionalAtomicReplace(malformed, {}, FileConditionalReplaceOptions::ForMissing()).status);
	auto resource = ParseUri(L"file:///C:/Workspace");
	CHECK_TRUE(resource);
	auto watched = service->Watch(*resource);
	CHECK_TRUE(watched && watched.value);
	const auto overflow = (*watched.value)->Next();
	CHECK_TRUE(overflow);
	CHECK_EQ(EFileWatchEventType::Overflow, overflow.value->type);
	CHECK_EQ(resource->ToString(), overflow.value->uri.ToString());
	const auto rescan = (*watched.value)->Next();
	CHECK_TRUE(rescan);
	CHECK_EQ(EFileWatchEventType::RescanRequired, rescan.value->type);
	CHECK_EQ(EFileResultStatus::Succeeded, (*watched.value)->Cancel().status);
	const auto disposed = (*watched.value)->Next();
	CHECK_TRUE(disposed);
	CHECK_EQ(EFileWatchEventType::Disposed, disposed.value->type);
	CHECK_EQ(resource->ToString(), disposed.value->uri.ToString());
	CHECK_EQ(EFileResultStatus::Cancelled, (*watched.value)->Next().status);
	CHECK_EQ(EFileResultStatus::Cancelled, (*watched.value)->Cancel().status);
	CHECK_EQ(1, provider->watchCalls);
	return true;
}

class CScopedTestDirectory final {
public:
	CScopedTestDirectory()
	{
		const auto nonce = std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(::GetTickCount64());
		m_path = std::filesystem::temp_directory_path() / (L"sakura-filesystem-contract-" + nonce);
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

FileBytes Bytes(std::string_view text)
{
	return FileBytes(text.begin(), text.end());
}

FileResult<FileWatchEvent> NextOfType(IFileWatch& watch, EFileWatchEventType expected)
{
	for (int attempt = 0; attempt < 16; ++attempt) {
		auto event = watch.Next();
		if (!event || event.value->type == expected) return event;
	}
	return FileResult<FileWatchEvent>::Failure(EFileResultStatus::Failed);
}

bool StatsAndEnumeratesOnlyLocalFileUris()
{
	CScopedTestDirectory directory;
	const auto file = directory.Path() / L"sample.txt";
	{ std::ofstream stream(file, std::ios::binary); stream << "sakura"; }
	auto service = NewService(true);
	CHECK_TRUE(service);
	const auto fileUri = UriFor(file);
	const auto directoryUri = UriFor(directory.Path());
	const auto stat = service->Stat(fileUri);
	CHECK_TRUE(stat && stat.value);
	CHECK_EQ(EFileEntryType::File, stat.value->type);
	CHECK_EQ(6u, stat.value->size);
	CHECK_EQ(fileUri.ToString(), stat.value->uri.ToString());
	const auto entries = service->Enumerate(directoryUri);
	CHECK_TRUE(entries && entries.value && entries.value->size() == 1);
	CHECK_EQ(L"sample.txt", entries.value->front().name);
	CHECK_EQ(fileUri.ToString(), entries.value->front().uri.ToString());
	CHECK_EQ(entries.value->front().uri.ToString(), entries.value->front().stat.uri.ToString());
	auto virtualUri = ParseUri(L"memfs://session/virtual.txt");
	CHECK_TRUE(virtualUri);
	CHECK_EQ(EFileResultStatus::Unsupported, service->Stat(*virtualUri).status);
	CHECK_EQ(EFileResultStatus::Unsupported, service->Enumerate(*virtualUri).status);
	CHECK_EQ(EFileResultStatus::Unsupported, service->Read(*virtualUri, { .maximumBytes = 1 }).status);
	CHECK_EQ(EFileResultStatus::Unsupported, service->ReadVersioned(*virtualUri, { .maximumBytes = 1 }).status);
	CHECK_EQ(EFileConditionalReplaceStatus::Unsupported,
		service->ConditionalAtomicReplace(*virtualUri, {}, FileConditionalReplaceOptions::ForMissing()).status);
	CHECK_EQ(EFileResultStatus::Unsupported, service->Watch(*virtualUri, {}).status);
	auto deviceUri = ParseUri(L"file://./pipe/sakura-test");
	CHECK_TRUE(deviceUri);
	CHECK_EQ(EFileResultStatus::InvalidUri, service->Stat(*deviceUri).status);
	CHECK_EQ(EFileResultStatus::Unsupported, service->Watch(directoryUri, { .recursive = true }).status);
	return true;
}

bool ReadsExactBinaryBytesWithinAnExplicitBound()
{
	CScopedTestDirectory directory;
	const auto file = directory.Path() / L"binary.dat";
	const std::uint8_t expected[] = { 0x53, 0x00, 0x4b, 0xff };
	{ std::ofstream stream(file, std::ios::binary); stream.write(reinterpret_cast<const char*>(expected), sizeof(expected)); }
	auto service = NewService(true);
	CHECK_TRUE(service);
	const auto fileUri = UriFor(file);
	const auto read = service->Read(fileUri, { .maximumBytes = sizeof(expected) });
	CHECK_TRUE(read && read.value);
	CHECK_EQ((FileBytes{ 0x53, 0x00, 0x4b, 0xff }), *read.value);
	const auto versioned = service->ReadVersioned(fileUri, { .maximumBytes = sizeof(expected) });
	CHECK_TRUE(versioned && versioned.value);
	CHECK_EQ((FileBytes{ 0x53, 0x00, 0x4b, 0xff }), versioned.value->bytes);
	CHECK_FALSE(versioned.value->version.Empty());
	CHECK_EQ(EFileResultStatus::Failed, service->Read(fileUri, { .maximumBytes = 0 }).status);
	CHECK_EQ(EFileResultStatus::Failed, service->Read(fileUri, { .maximumBytes = sizeof(expected) - 1 }).status);
	CHECK_EQ(EFileResultStatus::Failed, service->ReadVersioned(fileUri, { .maximumBytes = 0 }).status);
	CHECK_EQ(EFileResultStatus::Failed, service->ReadVersioned(fileUri, { .maximumBytes = sizeof(expected) - 1 }).status);
	CHECK_EQ(EFileResultStatus::NotDirectory, service->Read(UriFor(directory.Path()), { .maximumBytes = 16 }).status);
	CHECK_EQ(EFileResultStatus::NotDirectory, service->ReadVersioned(UriFor(directory.Path()), { .maximumBytes = 16 }).status);
	CHECK_EQ(EFileResultStatus::NotFound, service->Read(UriFor(directory.Path() / L"missing.dat"), { .maximumBytes = 16 }).status);
	CHECK_EQ(EFileResultStatus::NotFound, service->ReadVersioned(UriFor(directory.Path() / L"missing.dat"), { .maximumBytes = 16 }).status);
	return true;
}

bool ReplacesTheExpectedCurrentVersionAndReturnsCommittedVersion()
{
	CScopedTestDirectory directory;
	const auto file = directory.Path() / L"settings.json";
	{ std::ofstream stream(file, std::ios::binary); stream << "{\r\n  \\\"value\\\": 1\r\n}\r\n"; }
	auto service = NewService(true);
	CHECK_TRUE(service);
	const auto fileUri = UriFor(file);
	const auto initial = service->ReadVersioned(fileUri, { .maximumBytes = 4096 });
	CHECK_TRUE(initial && initial.value);
	const auto replacement = Bytes("{\r\n  \\\"value\\\": 2\r\n}\r\n");
	const auto replaced = service->ConditionalAtomicReplace(
		fileUri, replacement, FileConditionalReplaceOptions::ForCurrent(initial.value->version));
	CHECK_EQ(EFileConditionalReplaceStatus::Succeeded, replaced.status);
	CHECK_TRUE(replaced.committedVersion);
	const auto readback = service->ReadVersioned(fileUri, { .maximumBytes = 4096 });
	CHECK_TRUE(readback && readback.value);
	CHECK_EQ(replacement, readback.value->bytes);
	CHECK_EQ(*replaced.committedVersion, readback.value->version);
	CHECK_NE(initial.value->version, readback.value->version);
	return true;
}

bool RejectsAStaleCurrentVersionAndCleansItsStagingFile()
{
	CScopedTestDirectory directory;
	const auto file = directory.Path() / L"settings.json";
	{ std::ofstream stream(file, std::ios::binary); stream << "initial"; }
	auto service = NewService(true);
	CHECK_TRUE(service);
	const auto fileUri = UriFor(file);
	const auto initial = service->ReadVersioned(fileUri, { .maximumBytes = 4096 });
	CHECK_TRUE(initial && initial.value);
	{ std::ofstream stream(file, std::ios::binary | std::ios::trunc); stream << "external-change"; }
	const auto conflict = service->ConditionalAtomicReplace(
		fileUri, Bytes("our-change"), FileConditionalReplaceOptions::ForCurrent(initial.value->version));
	CHECK_EQ(EFileConditionalReplaceStatus::Conflict, conflict.status);
	CHECK_FALSE(conflict.committedVersion);
	const auto preserved = service->Read(fileUri, { .maximumBytes = 4096 });
	CHECK_TRUE(preserved && preserved.value);
	CHECK_EQ(Bytes("external-change"), *preserved.value);
	const auto entries = service->Enumerate(UriFor(directory.Path()));
	CHECK_TRUE(entries && entries.value && entries.value->size() == 1);
	CHECK_EQ(L"settings.json", entries.value->front().name);
	return true;
}

bool CreatesOnlyWhenMissingAndConflictsWhenThePathExists()
{
	CScopedTestDirectory directory;
	auto service = NewService(true);
	CHECK_TRUE(service);
	const auto file = directory.Path() / L"new-settings.json";
	const auto fileUri = UriFor(file);
	const auto replacement = Bytes("{\"created\":true}\n");
	const auto created = service->ConditionalAtomicReplace(
		fileUri, replacement, FileConditionalReplaceOptions::ForMissing());
	CHECK_EQ(EFileConditionalReplaceStatus::Succeeded, created.status);
	CHECK_TRUE(created.committedVersion);
	const auto readback = service->ReadVersioned(fileUri, { .maximumBytes = 4096 });
	CHECK_TRUE(readback && readback.value);
	CHECK_EQ(replacement, readback.value->bytes);
	CHECK_EQ(*created.committedVersion, readback.value->version);
	const auto conflict = service->ConditionalAtomicReplace(
		fileUri, Bytes("must-not-win"), FileConditionalReplaceOptions::ForMissing());
	CHECK_EQ(EFileConditionalReplaceStatus::Conflict, conflict.status);
	CHECK_FALSE(conflict.committedVersion);
	const auto preserved = service->Read(fileUri, { .maximumBytes = 4096 });
	CHECK_TRUE(preserved && preserved.value);
	CHECK_EQ(replacement, *preserved.value);
	const auto entries = service->Enumerate(UriFor(directory.Path()));
	CHECK_TRUE(entries && entries.value && entries.value->size() == 1);
	CHECK_EQ(L"new-settings.json", entries.value->front().name);
	return true;
}

bool ReportsReparsePointsAsSymbolicLinkWhenPermitted()
{
	CScopedTestDirectory directory;
	const auto target = directory.Path() / L"target.txt";
	const auto link = directory.Path() / L"link.txt";
	std::ofstream(target) << "target";
	std::error_code error;
	std::filesystem::create_symlink(target, link, error);
	if (error) return true; // Equivalent to the legacy GTest skip on restricted accounts.
	auto service = NewService(true);
	CHECK_TRUE(service);
	const auto stat = service->Stat(UriFor(link));
	CHECK_TRUE(stat && stat.value);
	CHECK_EQ(EFileEntryType::SymbolicLink, stat.value->type);
	const auto targetDirectory = directory.Path() / L"target-directory";
	const auto directoryLink = directory.Path() / L"directory-link";
	std::filesystem::create_directory(targetDirectory);
	std::filesystem::create_directory_symlink(targetDirectory, directoryLink, error);
	CHECK_FALSE(error);
	const auto directoryLinkUri = UriFor(directoryLink);
	CHECK_EQ(EFileResultStatus::Unsupported, service->Enumerate(directoryLinkUri).status);
	CHECK_EQ(EFileResultStatus::Unsupported, service->Watch(directoryLinkUri, {}).status);
	return true;
}

bool WatchesChangesAndTerminatesWithoutAWorkerLeak()
{
	CScopedTestDirectory directory;
	auto service = NewService(true);
	CHECK_TRUE(service);
	auto watched = service->Watch(UriFor(directory.Path()), {});
	CHECK_TRUE(watched && watched.value);
	IFileWatch& watch = **watched.value;
	const auto created = directory.Path() / L"created.txt";
	std::ofstream(created) << "first";
	auto createdEvent = NextOfType(watch, EFileWatchEventType::Created);
	CHECK_TRUE(createdEvent && createdEvent.value);
	CHECK_EQ(EFileWatchEventType::Created, createdEvent.value->type);
	CHECK_EQ(UriFor(created).ToString(), createdEvent.value->uri.ToString());
	std::ofstream(created, std::ios::app) << " second";
	auto changedEvent = NextOfType(watch, EFileWatchEventType::Changed);
	CHECK_TRUE(changedEvent && changedEvent.value);
	CHECK_EQ(EFileWatchEventType::Changed, changedEvent.value->type);
	const auto renamed = directory.Path() / L"renamed.txt";
	std::filesystem::rename(created, renamed);
	auto renamedEvent = NextOfType(watch, EFileWatchEventType::Renamed);
	CHECK_TRUE(renamedEvent && renamedEvent.value);
	CHECK_EQ(EFileWatchEventType::Renamed, renamedEvent.value->type);
	CHECK_TRUE(renamedEvent.value->previousUri);
	CHECK_EQ(UriFor(created).ToString(), renamedEvent.value->previousUri->ToString());
	CHECK_EQ(UriFor(renamed).ToString(), renamedEvent.value->uri.ToString());
	std::filesystem::remove(renamed);
	auto deletedEvent = NextOfType(watch, EFileWatchEventType::Deleted);
	CHECK_TRUE(deletedEvent && deletedEvent.value);
	CHECK_EQ(EFileWatchEventType::Deleted, deletedEvent.value->type);
	CHECK_EQ(EFileResultStatus::Succeeded, watch.Cancel().status);
	auto disposed = watch.Next();
	CHECK_TRUE(disposed && disposed.value);
	CHECK_EQ(EFileWatchEventType::Disposed, disposed.value->type);
	CHECK_EQ(EFileResultStatus::Cancelled, watch.Next().status);
	CHECK_EQ(EFileResultStatus::Cancelled, watch.Cancel().status);
	watched.value->reset();
	return true;
}

struct TestCase final {
	std::string_view suite;
	std::string_view name;
	bool (*run)();
};

constexpr std::array kTests{
	TestCase{ "FileService", "RegistersSchemesCaseInsensitivelyAndPreservesUris", RegistersSchemesCaseInsensitivelyAndPreservesUris },
	TestCase{ "FileService", "RejectsInvalidAndDuplicateRegistrationWithoutFallback", RejectsInvalidAndDuplicateRegistrationWithoutFallback },
	TestCase{ "FileService", "RejectsAnUnboundedReadBeforeProviderDispatch", RejectsAnUnboundedReadBeforeProviderDispatch },
	TestCase{ "FileService", "DispatchesConditionalAtomicReplaceOnlyWithBothCapabilities", DispatchesConditionalAtomicReplaceOnlyWithBothCapabilities },
	TestCase{ "FileService", "BoundsOpaqueVersionTokens", BoundsOpaqueVersionTokens },
	TestCase{ "FileService", "ReturnsInvalidUriAndProvidesAdvisoryWatchTerminalState", ReturnsInvalidUriAndProvidesAdvisoryWatchTerminalState },
	TestCase{ "Win32FileSystemProvider", "StatsAndEnumeratesOnlyLocalFileUris", StatsAndEnumeratesOnlyLocalFileUris },
	TestCase{ "Win32FileSystemProvider", "ReadsExactBinaryBytesWithinAnExplicitBound", ReadsExactBinaryBytesWithinAnExplicitBound },
	TestCase{ "Win32FileSystemProvider", "ReplacesTheExpectedCurrentVersionAndReturnsCommittedVersion", ReplacesTheExpectedCurrentVersionAndReturnsCommittedVersion },
	TestCase{ "Win32FileSystemProvider", "RejectsAStaleCurrentVersionAndCleansItsStagingFile", RejectsAStaleCurrentVersionAndCleansItsStagingFile },
	TestCase{ "Win32FileSystemProvider", "CreatesOnlyWhenMissingAndConflictsWhenThePathExists", CreatesOnlyWhenMissingAndConflictsWhenThePathExists },
	TestCase{ "Win32FileSystemProvider", "ReportsReparsePointsAsSymbolicLinkWhenPermitted", ReportsReparsePointsAsSymbolicLinkWhenPermitted },
	TestCase{ "Win32FileSystemProvider", "WatchesChangesAndTerminatesWithoutAWorkerLeak", WatchesChangesAndTerminatesWithoutAWorkerLeak },
};

bool Matches(std::string_view fullName, std::string_view filter)
{
	if (filter.empty() || filter == "*") return true;
	const auto star = filter.find('*');
	if (star == std::string_view::npos) return fullName == filter;
	const auto prefix = filter.substr(0, star);
	const auto suffix = filter.substr(star + 1);
	return fullName.starts_with(prefix) && fullName.ends_with(suffix)
		&& fullName.size() >= prefix.size() + suffix.size();
}

} // namespace
} // namespace platform::filesystem

int main(int argc, char** argv)
{
	using namespace platform::filesystem;
	std::string_view filter = "*";
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument = argv[index];
		if (argument == "--gtest_list_tests") {
			std::string_view suite;
			for (const auto& test : kTests) {
				if (test.suite != suite) {
					suite = test.suite;
					std::cout << suite << ".\n";
				}
				std::cout << "  " << test.name << '\n';
			}
			return 0;
		}
		constexpr std::string_view prefix = "--gtest_filter=";
		if (argument.starts_with(prefix)) filter = argument.substr(prefix.size());
	}

	int selected = 0;
	int failed = 0;
	for (const auto& test : kTests) {
		const std::string fullName = std::string(test.suite) + "." + std::string(test.name);
		if (!Matches(fullName, filter)) continue;
		++selected;
		const bool passed = test.run();
		std::cout << (passed ? "[       OK ] " : "[  FAILED  ] ") << fullName << '\n';
		if (!passed) ++failed;
	}
	std::cout << "[==========] " << selected << " tests ran; " << failed << " failed.\n";
	return failed == 0 && selected > 0 ? 0 : 1;
}
