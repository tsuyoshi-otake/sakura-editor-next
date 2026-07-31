/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "pch.h"

#include "platform/filesystem/CFileService.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <deque>
#include <memory>
#include <utility>

namespace platform::filesystem {
namespace {

platform::uri::Uri ParseUri(std::wstring_view text)
{
	auto parsed = platform::uri::Uri::Parse(text);
	assert(parsed);
	return std::move(*parsed.value);
}

FileVersionToken Version(std::uint8_t marker)
{
	const std::array<std::uint8_t, 2> opaque{ 1, marker };
	auto token = FileVersionToken::FromOpaqueBytes(opaque);
	assert(token);
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
		if (m_cancelled) {
			return FileResult<void>::Failure(EFileResultStatus::Cancelled, L"watch is already cancelled");
		}
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
			return FileResult<FileWatchEvent>::Success(FileWatchEvent{ .type = EFileWatchEventType::Disposed, .uri = m_root });
		}
		return FileResult<FileWatchEvent>::Failure(EFileResultStatus::Cancelled, L"watch has reached its terminal state");
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
		bool supportsRead = true,
		bool supportsConditionalReplace = false)
		: m_supportsRead(supportsRead)
		, m_supportsConditionalReplace(supportsConditionalReplace)
	{
	}

	FileSystemCapabilities Capabilities() const noexcept override
	{
		FileSystemCapabilities capabilities = EFileSystemCapability::Stat | EFileSystemCapability::Enumerate | EFileSystemCapability::Watch;
		if (m_supportsRead) capabilities |= static_cast<FileSystemCapabilities>(EFileSystemCapability::Read);
		if (m_supportsConditionalReplace) {
			capabilities |= static_cast<FileSystemCapabilities>(EFileSystemCapability::Write);
			capabilities |= static_cast<FileSystemCapabilities>(EFileSystemCapability::AtomicReplace);
		}
		return capabilities;
	}

	FileResult<FileStat> Stat(const platform::uri::Uri& resource) override
	{
		++statCalls;
		return FileResult<FileStat>::Success(FileStat{ .uri = resource, .type = EFileEntryType::File, .size = 42 });
	}

	FileResult<std::vector<DirectoryEntry>> Enumerate(const platform::uri::Uri& directory) override
	{
		++enumerateCalls;
		auto child = platform::uri::Uri::Parse(directory.ToString() + L"/child.txt");
		if (!child) {
			return FileResult<std::vector<DirectoryEntry>>::Failure(EFileResultStatus::Failed, L"fake child URI could not be created");
		}
		FileStat stat{ .uri = *child.value, .type = EFileEntryType::File, .size = 7 };
		return FileResult<std::vector<DirectoryEntry>>::Success(std::vector<DirectoryEntry>{
			{ .uri = *child.value, .name = L"child.txt", .stat = std::move(stat) },
		});
	}

	FileResult<FileBytes> Read(const platform::uri::Uri&, const FileReadOptions&) override
	{
		++readCalls;
		return FileResult<FileBytes>::Success({ 0x53, 0x00, 0x4b });
	}

	FileResult<FileContentSnapshot> ReadVersioned(
		const platform::uri::Uri&,
		const FileReadOptions&) override
	{
		++versionedReadCalls;
		return FileResult<FileContentSnapshot>::Success({
			.bytes = { 0x53, 0x00, 0x4b },
			.version = Version(42),
		});
	}

	FileConditionalReplaceResult ConditionalAtomicReplace(
		const platform::uri::Uri&,
		const FileBytes& bytes,
		const FileConditionalReplaceOptions& options) override
	{
		++conditionalReplaceCalls;
		lastReplaceBytes = bytes;
		lastReplaceOptions = options;
		return FileConditionalReplaceResult::Success(Version(43));
	}

	FileResult<std::unique_ptr<IFileWatch>> Watch(
		const platform::uri::Uri& resource,
		const FileWatchOptions&) override
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

TEST(FileService, RegistersSchemesCaseInsensitivelyAndPreservesUris)
{
	CFileService service;
	auto provider = std::make_shared<CFakeFileSystemProvider>();
	ASSERT_EQ(EFileResultStatus::Succeeded, service.RegisterProvider(L"FiLe", provider).status);

	const auto resource = ParseUri(L"FILE:///C:/Workspace/ReadMe.md");
	const auto stat = service.Stat(resource);
	ASSERT_TRUE(stat);
	ASSERT_TRUE(stat.value);
	EXPECT_EQ(resource.ToString(), stat.value->uri.ToString());
	EXPECT_EQ(1, provider->statCalls);

	const auto entries = service.Enumerate(resource);
	ASSERT_TRUE(entries);
	ASSERT_TRUE(entries.value);
	ASSERT_EQ(1u, entries.value->size());
	EXPECT_EQ(L"file:///C:/Workspace/ReadMe.md/child.txt", entries.value->front().uri.ToString());
	EXPECT_EQ(entries.value->front().uri.ToString(), entries.value->front().stat.uri.ToString());
	EXPECT_EQ(1, provider->enumerateCalls);

	const auto read = service.Read(resource, { .maximumBytes = 16 });
	ASSERT_TRUE(read);
	ASSERT_TRUE(read.value);
	EXPECT_EQ((FileBytes{ 0x53, 0x00, 0x4b }), *read.value);
	EXPECT_EQ(1, provider->readCalls);

	const auto versioned = service.ReadVersioned(resource, { .maximumBytes = 16 });
	ASSERT_TRUE(versioned);
	ASSERT_TRUE(versioned.value);
	EXPECT_EQ((FileBytes{ 0x53, 0x00, 0x4b }), versioned.value->bytes);
	EXPECT_EQ(Version(42), versioned.value->version);
	EXPECT_EQ(1, provider->versionedReadCalls);
}

TEST(FileService, RejectsInvalidAndDuplicateRegistrationWithoutFallback)
{
	CFileService service;
	auto provider = std::make_shared<CFakeFileSystemProvider>();
	EXPECT_EQ(EFileResultStatus::Failed, service.RegisterProvider(L"not a scheme", provider).status);
	EXPECT_EQ(EFileResultStatus::Failed, service.RegisterProvider(L"null-provider", {}).status);
	ASSERT_EQ(EFileResultStatus::Succeeded, service.RegisterProvider(L"file", provider).status);
	EXPECT_EQ(EFileResultStatus::Failed, service.RegisterProvider(L"FILE", provider).status);

	const auto virtualResource = ParseUri(L"memfs://session/virtual.txt");
	const auto unknown = service.Stat(virtualResource);
	EXPECT_EQ(EFileResultStatus::Unsupported, unknown.status);
	EXPECT_EQ(0, provider->statCalls);
	EXPECT_EQ(EFileResultStatus::Unsupported, service.Read(virtualResource, { .maximumBytes = 1 }).status);
	EXPECT_EQ(EFileResultStatus::Unsupported, service.ReadVersioned(virtualResource, { .maximumBytes = 1 }).status);
	EXPECT_EQ(0, provider->readCalls);
	EXPECT_EQ(0, provider->versionedReadCalls);

	CFileService noReadService;
	auto noReadProvider = std::make_shared<CFakeFileSystemProvider>(false);
	ASSERT_EQ(EFileResultStatus::Succeeded, noReadService.RegisterProvider(L"file", noReadProvider).status);
	EXPECT_EQ(EFileResultStatus::Unsupported, noReadService.Read(ParseUri(L"file:///C:/Workspace/ReadMe.md"), { .maximumBytes = 1 }).status);
	EXPECT_EQ(EFileResultStatus::Unsupported, noReadService.ReadVersioned(ParseUri(L"file:///C:/Workspace/ReadMe.md"), { .maximumBytes = 1 }).status);
	EXPECT_EQ(0, noReadProvider->readCalls);
	EXPECT_EQ(0, noReadProvider->versionedReadCalls);
}

TEST(FileService, RejectsAnUnboundedReadBeforeProviderDispatch)
{
	CFileService service;
	auto provider = std::make_shared<CFakeFileSystemProvider>();
	ASSERT_EQ(EFileResultStatus::Succeeded, service.RegisterProvider(L"file", provider).status);

	const auto result = service.Read(ParseUri(L"file:///C:/Workspace/ReadMe.md"), {});
	EXPECT_EQ(EFileResultStatus::Failed, result.status);
	EXPECT_FALSE(result.value);
	const auto versioned = service.ReadVersioned(ParseUri(L"file:///C:/Workspace/ReadMe.md"), {});
	EXPECT_EQ(EFileResultStatus::Failed, versioned.status);
	EXPECT_FALSE(versioned.value);
	EXPECT_EQ(0, provider->readCalls);
	EXPECT_EQ(0, provider->versionedReadCalls);
}

TEST(FileService, DispatchesConditionalAtomicReplaceOnlyWithBothCapabilities)
{
	const auto resource = ParseUri(L"file:///C:/Workspace/settings.json");
	CFileService service;
	auto provider = std::make_shared<CFakeFileSystemProvider>(true, true);
	ASSERT_EQ(EFileResultStatus::Succeeded, service.RegisterProvider(L"file", provider).status);

	const auto expected = Version(7);
	const FileBytes replacement{ 0x7b, 0x7d };
	const auto result = service.ConditionalAtomicReplace(
		resource,
		replacement,
		FileConditionalReplaceOptions::ForCurrent(expected));
	ASSERT_EQ(EFileConditionalReplaceStatus::Succeeded, result.status);
	ASSERT_TRUE(result.committedVersion);
	EXPECT_EQ(Version(43), *result.committedVersion);
	EXPECT_EQ(1, provider->conditionalReplaceCalls);
	EXPECT_EQ(replacement, provider->lastReplaceBytes);
	ASSERT_TRUE(provider->lastReplaceOptions);
	EXPECT_EQ(EFileConditionalReplaceExpectation::Current, provider->lastReplaceOptions->expectation);
	EXPECT_EQ(expected, provider->lastReplaceOptions->expectedVersion);

	CFileService unsupported;
	auto providerWithoutCapability = std::make_shared<CFakeFileSystemProvider>();
	ASSERT_EQ(EFileResultStatus::Succeeded, unsupported.RegisterProvider(L"file", providerWithoutCapability).status);
	EXPECT_EQ(
		EFileConditionalReplaceStatus::Unsupported,
		unsupported.ConditionalAtomicReplace(
			resource, replacement, FileConditionalReplaceOptions::ForMissing()).status);
	EXPECT_EQ(0, providerWithoutCapability->conditionalReplaceCalls);

	EXPECT_EQ(
		EFileConditionalReplaceStatus::Failed,
		service.ConditionalAtomicReplace(
			resource, replacement, FileConditionalReplaceOptions{}).status);
	EXPECT_EQ(1, provider->conditionalReplaceCalls);
}

TEST(FileService, BoundsOpaqueVersionTokens)
{
	const std::array<std::uint8_t, 1> one{ 1 };
	const std::array<std::uint8_t, FileVersionToken::kMaximumBytes> maximum{};
	const std::array<std::uint8_t, FileVersionToken::kMaximumBytes + 1> tooLarge{};
	EXPECT_FALSE(FileVersionToken::FromOpaqueBytes(std::span<const std::uint8_t>{}));
	EXPECT_TRUE(FileVersionToken::FromOpaqueBytes(one));
	EXPECT_TRUE(FileVersionToken::FromOpaqueBytes(maximum));
	EXPECT_FALSE(FileVersionToken::FromOpaqueBytes(tooLarge));
	EXPECT_NE(Version(1), Version(2));
}

TEST(FileService, ReturnsInvalidUriAndProvidesAdvisoryWatchTerminalState)
{
	CFileService service;
	auto provider = std::make_shared<CFakeFileSystemProvider>();
	ASSERT_EQ(EFileResultStatus::Succeeded, service.RegisterProvider(L"file", provider).status);

	const auto malformed = platform::uri::Uri::Parse(L"file:///C:/bad%2");
	ASSERT_FALSE(malformed);
	EXPECT_EQ(EFileResultStatus::InvalidUri, service.Stat(malformed).status);
	EXPECT_EQ(EFileResultStatus::InvalidUri, service.Read(malformed, { .maximumBytes = 1 }).status);
	EXPECT_EQ(EFileResultStatus::InvalidUri, service.ReadVersioned(malformed, { .maximumBytes = 1 }).status);
	EXPECT_EQ(
		EFileConditionalReplaceStatus::Failed,
		service.ConditionalAtomicReplace(
			malformed, {}, FileConditionalReplaceOptions::ForMissing()).status);

	const auto resource = ParseUri(L"file:///C:/Workspace");
	auto watched = service.Watch(resource);
	ASSERT_TRUE(watched);
	ASSERT_TRUE(watched.value);

	const auto overflow = (*watched.value)->Next();
	ASSERT_TRUE(overflow);
	EXPECT_EQ(EFileWatchEventType::Overflow, overflow.value->type);
	EXPECT_EQ(resource.ToString(), overflow.value->uri.ToString());
	const auto rescan = (*watched.value)->Next();
	ASSERT_TRUE(rescan);
	EXPECT_EQ(EFileWatchEventType::RescanRequired, rescan.value->type);

	EXPECT_EQ(EFileResultStatus::Succeeded, (*watched.value)->Cancel().status);
	const auto disposed = (*watched.value)->Next();
	ASSERT_TRUE(disposed);
	EXPECT_EQ(EFileWatchEventType::Disposed, disposed.value->type);
	EXPECT_EQ(resource.ToString(), disposed.value->uri.ToString());
	EXPECT_EQ(EFileResultStatus::Cancelled, (*watched.value)->Next().status);
	EXPECT_EQ(EFileResultStatus::Cancelled, (*watched.value)->Cancel().status);
	EXPECT_EQ(1, provider->watchCalls);
}

} // namespace
} // namespace platform::filesystem
