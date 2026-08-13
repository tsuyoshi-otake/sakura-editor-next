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
	explicit CFakeFileSystemProvider(
		bool supportsRead = true, bool supportsConditionalReplace = false, bool supportsWriteOperations = false)
		: m_supportsRead(supportsRead)
		, m_supportsConditionalReplace(supportsConditionalReplace)
		, m_supportsWriteOperations(supportsWriteOperations)
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
		if (m_supportsWriteOperations) {
			capabilities = capabilities | EFileSystemCapability::Write;
			capabilities = capabilities | EFileSystemCapability::Rename;
			capabilities = capabilities | EFileSystemCapability::Delete;
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

	FileResult<void> MakeDirectory(const platform::uri::Uri&) override
	{
		++makeDirectoryCalls;
		return FileResult<void>::Success();
	}

	FileResult<void> Rename(
		const platform::uri::Uri&, const platform::uri::Uri&, const FileRenameOptions& options) override
	{
		++renameCalls;
		lastRenameOptions = options;
		return FileResult<void>::Success();
	}

	FileResult<void> Delete(const platform::uri::Uri&, const FileDeleteOptions& options) override
	{
		++deleteCalls;
		lastDeleteOptions = options;
		return FileResult<void>::Success();
	}

	int statCalls = 0;
	int enumerateCalls = 0;
	int readCalls = 0;
	int versionedReadCalls = 0;
	int conditionalReplaceCalls = 0;
	int watchCalls = 0;
	int makeDirectoryCalls = 0;
	int renameCalls = 0;
	int deleteCalls = 0;
	FileBytes lastReplaceBytes;
	std::optional<FileConditionalReplaceOptions> lastReplaceOptions;
	std::optional<FileRenameOptions> lastRenameOptions;
	std::optional<FileDeleteOptions> lastDeleteOptions;

private:
	bool m_supportsRead;
	bool m_supportsConditionalReplace;
	bool m_supportsWriteOperations;
};

//! Provider that overrides only the pure-virtual surface, so the write-operation
//! interface defaults stay observable.
class CDefaultOnlyProvider final : public IFileSystemProvider {
public:
	FileSystemCapabilities Capabilities() const noexcept override { return {}; }
	FileResult<FileStat> Stat(const platform::uri::Uri&) override
	{
		return FileResult<FileStat>::Failure(EFileResultStatus::Failed);
	}
	FileResult<std::vector<DirectoryEntry>> Enumerate(const platform::uri::Uri&) override
	{
		return FileResult<std::vector<DirectoryEntry>>::Failure(EFileResultStatus::Failed);
	}
	FileResult<FileBytes> Read(const platform::uri::Uri&, const FileReadOptions&) override
	{
		return FileResult<FileBytes>::Failure(EFileResultStatus::Failed);
	}
	FileResult<std::unique_ptr<IFileWatch>> Watch(const platform::uri::Uri&, const FileWatchOptions&) override
	{
		return FileResult<std::unique_ptr<IFileWatch>>::Failure(EFileResultStatus::Failed);
	}
};

//! Service that overrides only the pure-virtual surface, so the write-operation
//! service defaults stay observable for existing service fakes.
class CMinimalFileService final : public IFileService {
public:
	FileResult<FileStat> Stat(const platform::uri::Uri&) override
	{
		return FileResult<FileStat>::Failure(EFileResultStatus::Failed);
	}
	FileResult<std::vector<DirectoryEntry>> Enumerate(const platform::uri::Uri&) override
	{
		return FileResult<std::vector<DirectoryEntry>>::Failure(EFileResultStatus::Failed);
	}
	FileResult<FileBytes> Read(const platform::uri::Uri&, const FileReadOptions&) override
	{
		return FileResult<FileBytes>::Failure(EFileResultStatus::Failed);
	}
	FileResult<std::unique_ptr<IFileWatch>> Watch(const platform::uri::Uri&, const FileWatchOptions&) override
	{
		return FileResult<std::unique_ptr<IFileWatch>>::Failure(EFileResultStatus::Failed);
	}
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

bool GatesDirectoryRenameAndDeleteDispatchOnCapabilities()
{
	auto resource = ParseUri(L"file:///C:/Workspace/folder");
	auto target = ParseUri(L"file:///C:/Workspace/renamed");
	CHECK_TRUE(resource && target);
	auto gated = NewService(false);
	CHECK_TRUE(gated);
	auto readOnlyProvider = std::make_shared<CFakeFileSystemProvider>();
	CHECK_EQ(EFileResultStatus::Succeeded, gated->RegisterProvider(L"file", readOnlyProvider).status);
	CHECK_EQ(EFileResultStatus::Unsupported, gated->MakeDirectory(*resource).status);
	CHECK_EQ(EFileResultStatus::Unsupported, gated->Rename(*resource, *target).status);
	CHECK_EQ(EFileResultStatus::Unsupported, gated->Delete(*resource).status);
	CHECK_EQ(0, readOnlyProvider->makeDirectoryCalls);
	CHECK_EQ(0, readOnlyProvider->renameCalls);
	CHECK_EQ(0, readOnlyProvider->deleteCalls);
	auto service = NewService(false);
	CHECK_TRUE(service);
	auto provider = std::make_shared<CFakeFileSystemProvider>(true, false, true);
	auto virtualProvider = std::make_shared<CFakeFileSystemProvider>(true, false, true);
	CHECK_EQ(EFileResultStatus::Succeeded, service->RegisterProvider(L"file", provider).status);
	CHECK_EQ(EFileResultStatus::Succeeded, service->RegisterProvider(L"memfs", virtualProvider).status);
	CHECK_EQ(EFileResultStatus::Succeeded, service->MakeDirectory(*resource).status);
	CHECK_EQ(1, provider->makeDirectoryCalls);
	CHECK_EQ(EFileResultStatus::Succeeded, service->Rename(*resource, *target, { .overwrite = true }).status);
	CHECK_EQ(1, provider->renameCalls);
	CHECK_TRUE(provider->lastRenameOptions);
	CHECK_TRUE(provider->lastRenameOptions->overwrite);
	CHECK_EQ(EFileResultStatus::Succeeded,
		service->Delete(*resource, { .recursive = true, .useTrash = false }).status);
	CHECK_EQ(1, provider->deleteCalls);
	CHECK_TRUE(provider->lastDeleteOptions);
	CHECK_TRUE(provider->lastDeleteOptions->recursive);
	CHECK_FALSE(provider->lastDeleteOptions->useTrash);
	auto virtualTarget = ParseUri(L"memfs://session/renamed");
	CHECK_TRUE(virtualTarget);
	CHECK_EQ(EFileResultStatus::Unsupported, service->Rename(*resource, *virtualTarget).status);
	auto unregisteredTarget = ParseUri(L"untitled://untitled-1");
	CHECK_TRUE(unregisteredTarget);
	CHECK_EQ(EFileResultStatus::Unsupported, service->Rename(*resource, *unregisteredTarget).status);
	CHECK_EQ(1, provider->renameCalls);
	CHECK_EQ(0, virtualProvider->renameCalls);
	return true;
}

bool KeepsWriteDefaultsUnsupportedAndRejectsInvalidWriteUris()
{
	auto service = NewService(false);
	CHECK_TRUE(service);
	auto provider = std::make_shared<CFakeFileSystemProvider>(true, false, true);
	CHECK_EQ(EFileResultStatus::Succeeded, service->RegisterProvider(L"file", provider).status);
	auto malformed = platform::uri::Uri::Parse(L"file:///C:/bad%2");
	CHECK_FALSE(malformed);
	auto valid = platform::uri::Uri::Parse(L"file:///C:/Workspace/ok.txt");
	CHECK_TRUE(valid);
	CHECK_EQ(EFileResultStatus::InvalidUri, service->MakeDirectory(malformed).status);
	CHECK_EQ(EFileResultStatus::InvalidUri, service->Rename(malformed, valid).status);
	CHECK_EQ(EFileResultStatus::InvalidUri, service->Rename(valid, malformed).status);
	CHECK_EQ(EFileResultStatus::InvalidUri, service->Delete(malformed).status);
	CHECK_EQ(0, provider->makeDirectoryCalls);
	CHECK_EQ(0, provider->renameCalls);
	CHECK_EQ(0, provider->deleteCalls);
	auto resource = ParseUri(L"file:///C:/Workspace/folder");
	auto target = ParseUri(L"file:///C:/Workspace/renamed");
	CHECK_TRUE(resource && target);
	CDefaultOnlyProvider defaultProvider;
	CHECK_EQ(EFileResultStatus::Unsupported, defaultProvider.MakeDirectory(*resource).status);
	CHECK_EQ(EFileResultStatus::Unsupported, defaultProvider.Rename(*resource, *target, {}).status);
	CHECK_EQ(EFileResultStatus::Unsupported, defaultProvider.Delete(*resource, {}).status);
	CMinimalFileService minimalService;
	CHECK_EQ(EFileResultStatus::Unsupported, minimalService.MakeDirectory(*resource).status);
	CHECK_EQ(EFileResultStatus::Unsupported, minimalService.Rename(*resource, *target).status);
	CHECK_EQ(EFileResultStatus::Unsupported, minimalService.Delete(*resource).status);
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

bool CreatesDirectoriesOnlyWithinExistingParents()
{
	CScopedTestDirectory directory;
	auto service = NewService(true);
	CHECK_TRUE(service);
	const auto created = directory.Path() / L"created";
	CHECK_EQ(EFileResultStatus::Succeeded, service->MakeDirectory(UriFor(created)).status);
	const auto stat = service->Stat(UriFor(created));
	CHECK_TRUE(stat && stat.value);
	CHECK_EQ(EFileEntryType::Directory, stat.value->type);
	CHECK_EQ(EFileResultStatus::AlreadyExists, service->MakeDirectory(UriFor(created)).status);
	CHECK_EQ(EFileResultStatus::NotFound,
		service->MakeDirectory(UriFor(directory.Path() / L"missing-parent" / L"child")).status);
	const auto file = directory.Path() / L"occupied.txt";
	{ std::ofstream stream(file, std::ios::binary); stream << "occupied"; }
	CHECK_EQ(EFileResultStatus::AlreadyExists, service->MakeDirectory(UriFor(file)).status);
	return true;
}

bool RenamesOnlyWithExplicitOverwriteWithinOneProvider()
{
	CScopedTestDirectory directory;
	auto service = NewService(true);
	CHECK_TRUE(service);
	const auto source = directory.Path() / L"alpha.txt";
	const auto moved = directory.Path() / L"moved.txt";
	const auto occupied = directory.Path() / L"occupied.txt";
	{ std::ofstream stream(source, std::ios::binary); stream << "alpha"; }
	{ std::ofstream stream(occupied, std::ios::binary); stream << "beta"; }
	CHECK_EQ(EFileResultStatus::Succeeded, service->Rename(UriFor(source), UriFor(moved)).status);
	CHECK_EQ(EFileResultStatus::NotFound, service->Stat(UriFor(source)).status);
	const auto movedBytes = service->Read(UriFor(moved), { .maximumBytes = 64 });
	CHECK_TRUE(movedBytes && movedBytes.value);
	CHECK_EQ(Bytes("alpha"), *movedBytes.value);
	CHECK_EQ(EFileResultStatus::AlreadyExists, service->Rename(UriFor(moved), UriFor(occupied)).status);
	const auto preserved = service->Read(UriFor(occupied), { .maximumBytes = 64 });
	CHECK_TRUE(preserved && preserved.value);
	CHECK_EQ(Bytes("beta"), *preserved.value);
	CHECK_EQ(EFileResultStatus::Succeeded,
		service->Rename(UriFor(moved), UriFor(occupied), { .overwrite = true }).status);
	CHECK_EQ(EFileResultStatus::NotFound, service->Stat(UriFor(moved)).status);
	const auto replaced = service->Read(UriFor(occupied), { .maximumBytes = 64 });
	CHECK_TRUE(replaced && replaced.value);
	CHECK_EQ(Bytes("alpha"), *replaced.value);
	CHECK_EQ(EFileResultStatus::NotFound,
		service->Rename(UriFor(directory.Path() / L"missing.txt"), UriFor(moved)).status);
	const auto folder = directory.Path() / L"folder";
	const auto renamedFolder = directory.Path() / L"renamed-folder";
	CHECK_EQ(EFileResultStatus::Succeeded, service->MakeDirectory(UriFor(folder)).status);
	{ std::ofstream stream(folder / L"member.txt", std::ios::binary); stream << "member"; }
	CHECK_EQ(EFileResultStatus::Succeeded, service->Rename(UriFor(folder), UriFor(renamedFolder)).status);
	const auto member = service->Stat(UriFor(renamedFolder / L"member.txt"));
	CHECK_TRUE(member && member.value);
	auto virtualTarget = ParseUri(L"memfs://session/renamed.txt");
	CHECK_TRUE(virtualTarget);
	CHECK_EQ(EFileResultStatus::Unsupported, service->Rename(UriFor(occupied), *virtualTarget).status);
	return true;
}

bool DeletesPermanentlyWithExplicitRecursionAndReadOnlyForce()
{
	CScopedTestDirectory directory;
	auto service = NewService(true);
	CHECK_TRUE(service);
	const auto root = directory.Path() / L"root";
	const auto sub = root / L"sub";
	const auto inner = sub / L"inner.txt";
	const auto top = root / L"top.txt";
	CHECK_EQ(EFileResultStatus::Succeeded, service->MakeDirectory(UriFor(root)).status);
	CHECK_EQ(EFileResultStatus::Succeeded, service->MakeDirectory(UriFor(sub)).status);
	{ std::ofstream stream(inner, std::ios::binary); stream << "inner"; }
	{ std::ofstream stream(top, std::ios::binary); stream << "top"; }
	CHECK_TRUE(::SetFileAttributesW(inner.c_str(), FILE_ATTRIBUTE_READONLY));
	const FileDeleteOptions permanent{ .recursive = false, .useTrash = false };
	const FileDeleteOptions permanentRecursive{ .recursive = true, .useTrash = false };
	CHECK_EQ(EFileResultStatus::Failed, service->Delete(UriFor(root), permanent).status);
	CHECK_EQ(EFileResultStatus::Succeeded, service->Delete(UriFor(top), permanent).status);
	CHECK_EQ(EFileResultStatus::NotFound, service->Stat(UriFor(top)).status);
	CHECK_EQ(EFileResultStatus::Succeeded, service->Delete(UriFor(root), permanentRecursive).status);
	CHECK_EQ(EFileResultStatus::NotFound, service->Stat(UriFor(root)).status);
	const auto emptyFolder = directory.Path() / L"empty";
	CHECK_EQ(EFileResultStatus::Succeeded, service->MakeDirectory(UriFor(emptyFolder)).status);
	CHECK_EQ(EFileResultStatus::Succeeded, service->Delete(UriFor(emptyFolder), permanent).status);
	CHECK_EQ(EFileResultStatus::NotFound, service->Delete(UriFor(directory.Path() / L"missing"), permanent).status);
	const auto linkTarget = directory.Path() / L"link-target";
	const auto linkRoot = directory.Path() / L"link-root";
	CHECK_EQ(EFileResultStatus::Succeeded, service->MakeDirectory(UriFor(linkTarget)).status);
	{ std::ofstream stream(linkTarget / L"kept.txt", std::ios::binary); stream << "kept"; }
	CHECK_EQ(EFileResultStatus::Succeeded, service->MakeDirectory(UriFor(linkRoot)).status);
	std::error_code error;
	std::filesystem::create_directory_symlink(linkTarget, linkRoot / L"link", error);
	if (error) return true; // Equivalent to the legacy GTest skip on restricted accounts.
	CHECK_EQ(EFileResultStatus::Succeeded, service->Delete(UriFor(linkRoot), permanentRecursive).status);
	CHECK_EQ(EFileResultStatus::NotFound, service->Stat(UriFor(linkRoot)).status);
	const auto kept = service->Read(UriFor(linkTarget / L"kept.txt"), { .maximumBytes = 64 });
	CHECK_TRUE(kept && kept.value);
	CHECK_EQ(Bytes("kept"), *kept.value);
	return true;
}

bool RejectsRecycleBinDeleteBeyondMaxPathWithoutFallback()
{
	CScopedTestDirectory directory;
	auto service = NewService(true);
	CHECK_TRUE(service);
	// Grow the tree through the provider itself: the provider uses extended-length
	// paths internally, so the test does not depend on the OS long-path policy.
	auto deep = directory.Path();
	const std::wstring segment(64, L'a');
	while (deep.wstring().size() < MAX_PATH) {
		deep /= segment;
		CHECK_EQ(EFileResultStatus::Succeeded, service->MakeDirectory(UriFor(deep)).status);
	}
	const auto file = deep / L"trapped.txt";
	CHECK_TRUE(file.wstring().size() >= MAX_PATH);
	const auto created = service->ConditionalAtomicReplace(
		UriFor(file), Bytes("trapped"), FileConditionalReplaceOptions::ForMissing());
	CHECK_EQ(EFileConditionalReplaceStatus::Succeeded, created.status);
	const auto trashed = service->Delete(UriFor(file), { .recursive = false, .useTrash = true });
	CHECK_EQ(EFileResultStatus::Unsupported, trashed.status);
	// The rejection must not fall back to a silent permanent delete.
	const auto survived = service->Stat(UriFor(file));
	CHECK_TRUE(survived && survived.value);
	CHECK_EQ(EFileResultStatus::Succeeded,
		service->Delete(UriFor(file), { .recursive = false, .useTrash = false }).status);
	CHECK_EQ(EFileResultStatus::Succeeded,
		service->Delete(UriFor(directory.Path() / segment), { .recursive = true, .useTrash = false }).status);
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
	TestCase{ "FileService", "GatesDirectoryRenameAndDeleteDispatchOnCapabilities", GatesDirectoryRenameAndDeleteDispatchOnCapabilities },
	TestCase{ "FileService", "KeepsWriteDefaultsUnsupportedAndRejectsInvalidWriteUris", KeepsWriteDefaultsUnsupportedAndRejectsInvalidWriteUris },
	TestCase{ "Win32FileSystemProvider", "StatsAndEnumeratesOnlyLocalFileUris", StatsAndEnumeratesOnlyLocalFileUris },
	TestCase{ "Win32FileSystemProvider", "ReadsExactBinaryBytesWithinAnExplicitBound", ReadsExactBinaryBytesWithinAnExplicitBound },
	TestCase{ "Win32FileSystemProvider", "ReplacesTheExpectedCurrentVersionAndReturnsCommittedVersion", ReplacesTheExpectedCurrentVersionAndReturnsCommittedVersion },
	TestCase{ "Win32FileSystemProvider", "RejectsAStaleCurrentVersionAndCleansItsStagingFile", RejectsAStaleCurrentVersionAndCleansItsStagingFile },
	TestCase{ "Win32FileSystemProvider", "CreatesOnlyWhenMissingAndConflictsWhenThePathExists", CreatesOnlyWhenMissingAndConflictsWhenThePathExists },
	TestCase{ "Win32FileSystemProvider", "ReportsReparsePointsAsSymbolicLinkWhenPermitted", ReportsReparsePointsAsSymbolicLinkWhenPermitted },
	TestCase{ "Win32FileSystemProvider", "WatchesChangesAndTerminatesWithoutAWorkerLeak", WatchesChangesAndTerminatesWithoutAWorkerLeak },
	TestCase{ "Win32FileSystemProvider", "CreatesDirectoriesOnlyWithinExistingParents", CreatesDirectoriesOnlyWithinExistingParents },
	TestCase{ "Win32FileSystemProvider", "RenamesOnlyWithExplicitOverwriteWithinOneProvider", RenamesOnlyWithExplicitOverwriteWithinOneProvider },
	TestCase{ "Win32FileSystemProvider", "DeletesPermanentlyWithExplicitRecursionAndReadOnlyForce", DeletesPermanentlyWithExplicitRecursionAndReadOnlyForce },
	TestCase{ "Win32FileSystemProvider", "RejectsRecycleBinDeleteBeyondMaxPathWithoutFallback", RejectsRecycleBinDeleteBeyondMaxPathWithoutFallback },
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
