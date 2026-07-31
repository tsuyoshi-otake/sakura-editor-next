/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include "platform/filesystem/IFileService.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace platform::filesystem {

//! Scheme と provider の明示的な対応表。scheme は ASCII 大小文字を区別しない。
class CFileSystemProviderRegistry final {
public:
	[[nodiscard]] FileResult<void> RegisterProvider(
		std::wstring_view scheme,
		std::shared_ptr<IFileSystemProvider> provider);
	[[nodiscard]] std::shared_ptr<IFileSystemProvider> FindProvider(std::wstring_view scheme) const;

private:
	mutable std::mutex m_mutex;
	std::map<std::wstring, std::shared_ptr<IFileSystemProvider>, std::less<>> m_providers;
};

//! プロバイダー登録と URI dispatch をまとめる既定のファイルサービス。
class CFileService final : public IFileService {
public:
	[[nodiscard]] FileResult<void> RegisterProvider(
		std::wstring_view scheme,
		std::shared_ptr<IFileSystemProvider> provider);

	[[nodiscard]] FileResult<FileStat> Stat(const platform::uri::Uri& resource) override;
	[[nodiscard]] FileResult<std::vector<DirectoryEntry>> Enumerate(const platform::uri::Uri& directory) override;
	[[nodiscard]] FileResult<FileBytes> Read(
		const platform::uri::Uri& resource,
		const FileReadOptions& options) override;
	[[nodiscard]] FileResult<FileContentSnapshot> ReadVersioned(
		const platform::uri::Uri& resource,
		const FileReadOptions& options) override;
	[[nodiscard]] FileConditionalReplaceResult ConditionalAtomicReplace(
		const platform::uri::Uri& resource,
		const FileBytes& bytes,
		const FileConditionalReplaceOptions& options) override;
	[[nodiscard]] FileResult<std::unique_ptr<IFileWatch>> Watch(
		const platform::uri::Uri& resource,
		const FileWatchOptions& options = {}) override;

	//! Uri::Parse / FromComponents などの失敗を明示的な InvalidUri に変換する便利 API。
	[[nodiscard]] FileResult<FileStat> Stat(const platform::uri::UriParseResult& resource);
	[[nodiscard]] FileResult<std::vector<DirectoryEntry>> Enumerate(const platform::uri::UriParseResult& directory);
	[[nodiscard]] FileResult<FileBytes> Read(
		const platform::uri::UriParseResult& resource,
		const FileReadOptions& options);
	[[nodiscard]] FileResult<FileContentSnapshot> ReadVersioned(
		const platform::uri::UriParseResult& resource,
		const FileReadOptions& options);
	[[nodiscard]] FileConditionalReplaceResult ConditionalAtomicReplace(
		const platform::uri::UriParseResult& resource,
		const FileBytes& bytes,
		const FileConditionalReplaceOptions& options);
	[[nodiscard]] FileResult<std::unique_ptr<IFileWatch>> Watch(
		const platform::uri::UriParseResult& resource,
		const FileWatchOptions& options = {});

	[[nodiscard]] const CFileSystemProviderRegistry& Providers() const noexcept { return m_providers; }

private:
	CFileSystemProviderRegistry m_providers;
};

} // namespace platform::filesystem
