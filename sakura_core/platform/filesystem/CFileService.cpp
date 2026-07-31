/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#include "StdAfx.h"
#include "platform/filesystem/CFileService.h"

#include <utility>

namespace platform::filesystem {
namespace {

[[nodiscard]] bool IsAsciiAlpha(wchar_t value) noexcept
{
	return (value >= L'a' && value <= L'z') || (value >= L'A' && value <= L'Z');
}

[[nodiscard]] bool IsValidScheme(std::wstring_view scheme) noexcept
{
	if (scheme.empty() || !IsAsciiAlpha(scheme.front())) {
		return false;
	}
	for (const wchar_t character : scheme) {
		if (!IsAsciiAlpha(character)
			&& !(character >= L'0' && character <= L'9')
			&& character != L'+' && character != L'-' && character != L'.') {
			return false;
		}
	}
	return true;
}

[[nodiscard]] std::wstring NormalizeScheme(std::wstring_view scheme)
{
	std::wstring normalized;
	normalized.reserve(scheme.size());
	for (const wchar_t character : scheme) {
		normalized.push_back(character >= L'A' && character <= L'Z'
			? static_cast<wchar_t>(character - L'A' + L'a')
			: character);
	}
	return normalized;
}

} // namespace

FileResult<void> CFileSystemProviderRegistry::RegisterProvider(
	std::wstring_view scheme,
	std::shared_ptr<IFileSystemProvider> provider)
{
	if (!IsValidScheme(scheme)) {
		return FileResult<void>::Failure(EFileResultStatus::Failed, L"filesystem provider scheme is invalid");
	}
	if (!provider) {
		return FileResult<void>::Failure(EFileResultStatus::Failed, L"filesystem provider is required");
	}

	std::scoped_lock lock(m_mutex);
	const auto [entry, inserted] = m_providers.emplace(NormalizeScheme(scheme), std::move(provider));
	(void)entry;
	if (!inserted) {
		return FileResult<void>::Failure(EFileResultStatus::Failed, L"filesystem provider scheme is already registered");
	}
	return FileResult<void>::Success();
}

std::shared_ptr<IFileSystemProvider> CFileSystemProviderRegistry::FindProvider(std::wstring_view scheme) const
{
	if (!IsValidScheme(scheme)) {
		return {};
	}

	std::scoped_lock lock(m_mutex);
	const auto entry = m_providers.find(NormalizeScheme(scheme));
	return entry == m_providers.end() ? std::shared_ptr<IFileSystemProvider>{} : entry->second;
}

FileResult<void> CFileService::RegisterProvider(
	std::wstring_view scheme,
	std::shared_ptr<IFileSystemProvider> provider)
{
	return m_providers.RegisterProvider(scheme, std::move(provider));
}

FileResult<FileStat> CFileService::Stat(const platform::uri::Uri& resource)
{
	const auto provider = m_providers.FindProvider(resource.Scheme());
	if (!provider) {
		return FileResult<FileStat>::Failure(EFileResultStatus::Unsupported, L"no filesystem provider is registered for the URI scheme");
	}
	if (!HasCapability(provider->Capabilities(), EFileSystemCapability::Stat)) {
		return FileResult<FileStat>::Failure(EFileResultStatus::Unsupported, L"filesystem provider does not support stat");
	}
	return provider->Stat(resource);
}

FileResult<std::vector<DirectoryEntry>> CFileService::Enumerate(const platform::uri::Uri& directory)
{
	const auto provider = m_providers.FindProvider(directory.Scheme());
	if (!provider) {
		return FileResult<std::vector<DirectoryEntry>>::Failure(EFileResultStatus::Unsupported, L"no filesystem provider is registered for the URI scheme");
	}
	if (!HasCapability(provider->Capabilities(), EFileSystemCapability::Enumerate)) {
		return FileResult<std::vector<DirectoryEntry>>::Failure(EFileResultStatus::Unsupported, L"filesystem provider does not support enumerate");
	}
	return provider->Enumerate(directory);
}

FileResult<FileBytes> CFileService::Read(
	const platform::uri::Uri& resource,
	const FileReadOptions& options)
{
	if (options.maximumBytes == 0) {
		return FileResult<FileBytes>::Failure(
			EFileResultStatus::Failed, L"file read requires a nonzero maximumBytes");
	}
	const auto provider = m_providers.FindProvider(resource.Scheme());
	if (!provider) {
		return FileResult<FileBytes>::Failure(EFileResultStatus::Unsupported, L"no filesystem provider is registered for the URI scheme");
	}
	if (!HasCapability(provider->Capabilities(), EFileSystemCapability::Read)) {
		return FileResult<FileBytes>::Failure(EFileResultStatus::Unsupported, L"filesystem provider does not support read");
	}
	return provider->Read(resource, options);
}

FileResult<FileContentSnapshot> CFileService::ReadVersioned(
	const platform::uri::Uri& resource,
	const FileReadOptions& options)
{
	if (options.maximumBytes == 0) {
		return FileResult<FileContentSnapshot>::Failure(
			EFileResultStatus::Failed, L"versioned file read requires a nonzero maximumBytes");
	}
	const auto provider = m_providers.FindProvider(resource.Scheme());
	if (!provider) {
		return FileResult<FileContentSnapshot>::Failure(
			EFileResultStatus::Unsupported, L"no filesystem provider is registered for the URI scheme");
	}
	if (!HasCapability(provider->Capabilities(), EFileSystemCapability::Read)) {
		return FileResult<FileContentSnapshot>::Failure(
			EFileResultStatus::Unsupported, L"filesystem provider does not support read");
	}
	return provider->ReadVersioned(resource, options);
}

FileConditionalReplaceResult CFileService::ConditionalAtomicReplace(
	const platform::uri::Uri& resource,
	const FileBytes& bytes,
	const FileConditionalReplaceOptions& options)
{
	if (options.expectation == EFileConditionalReplaceExpectation::Current
		&& options.expectedVersion.Empty()) {
		return FileConditionalReplaceResult::Failure(
			L"expected-current conditional replace requires a nonempty version token");
	}
	const auto provider = m_providers.FindProvider(resource.Scheme());
	if (!provider) {
		return FileConditionalReplaceResult::Unsupported(
			L"no filesystem provider is registered for the URI scheme");
	}
	const auto capabilities = provider->Capabilities();
	if (!HasCapability(capabilities, EFileSystemCapability::Write)
		|| !HasCapability(capabilities, EFileSystemCapability::AtomicReplace)) {
		return FileConditionalReplaceResult::Unsupported(
			L"filesystem provider does not support conditional atomic replace");
	}
	return provider->ConditionalAtomicReplace(resource, bytes, options);
}

FileResult<std::unique_ptr<IFileWatch>> CFileService::Watch(
	const platform::uri::Uri& resource,
	const FileWatchOptions& options)
{
	const auto provider = m_providers.FindProvider(resource.Scheme());
	if (!provider) {
		return FileResult<std::unique_ptr<IFileWatch>>::Failure(EFileResultStatus::Unsupported, L"no filesystem provider is registered for the URI scheme");
	}
	if (!HasCapability(provider->Capabilities(), EFileSystemCapability::Watch)) {
		return FileResult<std::unique_ptr<IFileWatch>>::Failure(EFileResultStatus::Unsupported, L"filesystem provider does not support watch");
	}
	return provider->Watch(resource, options);
}

FileResult<FileStat> CFileService::Stat(const platform::uri::UriParseResult& resource)
{
	return resource ? Stat(*resource.value)
		: FileResult<FileStat>::Failure(EFileResultStatus::InvalidUri, L"filesystem stat received an invalid URI");
}

FileResult<std::vector<DirectoryEntry>> CFileService::Enumerate(const platform::uri::UriParseResult& directory)
{
	return directory ? Enumerate(*directory.value)
		: FileResult<std::vector<DirectoryEntry>>::Failure(EFileResultStatus::InvalidUri, L"filesystem enumerate received an invalid URI");
}

FileResult<FileBytes> CFileService::Read(
	const platform::uri::UriParseResult& resource,
	const FileReadOptions& options)
{
	return resource ? Read(*resource.value, options)
		: FileResult<FileBytes>::Failure(EFileResultStatus::InvalidUri, L"filesystem read received an invalid URI");
}

FileResult<FileContentSnapshot> CFileService::ReadVersioned(
	const platform::uri::UriParseResult& resource,
	const FileReadOptions& options)
{
	return resource ? ReadVersioned(*resource.value, options)
		: FileResult<FileContentSnapshot>::Failure(
			EFileResultStatus::InvalidUri, L"filesystem versioned read received an invalid URI");
}

FileConditionalReplaceResult CFileService::ConditionalAtomicReplace(
	const platform::uri::UriParseResult& resource,
	const FileBytes& bytes,
	const FileConditionalReplaceOptions& options)
{
	return resource ? ConditionalAtomicReplace(*resource.value, bytes, options)
		: FileConditionalReplaceResult::Failure(
			L"filesystem conditional atomic replace received an invalid URI");
}

FileResult<std::unique_ptr<IFileWatch>> CFileService::Watch(
	const platform::uri::UriParseResult& resource,
	const FileWatchOptions& options)
{
	return resource ? Watch(*resource.value, options)
		: FileResult<std::unique_ptr<IFileWatch>>::Failure(EFileResultStatus::InvalidUri, L"filesystem watch received an invalid URI");
}

} // namespace platform::filesystem
