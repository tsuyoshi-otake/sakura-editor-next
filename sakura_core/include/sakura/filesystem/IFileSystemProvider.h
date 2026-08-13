/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <sakura/uri/UriIdentity.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

//! UI や OS のパス表現に依存しないファイルシステム契約。
namespace platform::filesystem {

//! ファイル操作の終端状態。Succeeded 以外では value を参照しない。
enum class EFileResultStatus : std::uint8_t {
	Succeeded,
	NotFound,
	NotDirectory,
	PermissionDenied,
	Unsupported,
	InvalidUri,
	Cancelled,
	Failed,
	//! 作成・rename の対象が既に存在する。上書きの黙認はしない。
	AlreadyExists,
};

//! 値を返すファイル操作の型付き結果。
template <class TValue>
struct FileResult {
	EFileResultStatus status = EFileResultStatus::Failed;
	std::optional<TValue> value;
	std::wstring diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept { return status == EFileResultStatus::Succeeded; }
	explicit operator bool() const noexcept { return Succeeded(); }

	[[nodiscard]] static FileResult Success(TValue result)
	{
		return { .status = EFileResultStatus::Succeeded, .value = std::move(result) };
	}

	[[nodiscard]] static FileResult Failure(EFileResultStatus failureStatus, std::wstring message = {})
	{
		return { .status = failureStatus, .diagnostic = std::move(message) };
	}
};

//! 値を持たないファイル操作の型付き結果。
template <>
struct FileResult<void> {
	EFileResultStatus status = EFileResultStatus::Failed;
	std::wstring diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept { return status == EFileResultStatus::Succeeded; }
	explicit operator bool() const noexcept { return Succeeded(); }

	[[nodiscard]] static FileResult Success()
	{
		return { .status = EFileResultStatus::Succeeded };
	}

	[[nodiscard]] static FileResult Failure(EFileResultStatus failureStatus, std::wstring message = {})
	{
		return { .status = failureStatus, .diagnostic = std::move(message) };
	}
};

//! プロバイダーが公開する操作。未実装の操作も capability として明示する。
enum class EFileSystemCapability : std::uint16_t {
	None = 0,
	Stat = 1u << 0,
	Enumerate = 1u << 1,
	Watch = 1u << 2,
	Read = 1u << 3,
	Write = 1u << 4,
	Rename = 1u << 5,
	Copy = 1u << 6,
	Delete = 1u << 7,
	AtomicReplace = 1u << 8,
};

using FileSystemCapabilities = std::uint16_t;

[[nodiscard]] constexpr FileSystemCapabilities operator|(EFileSystemCapability left, EFileSystemCapability right) noexcept
{
	return static_cast<FileSystemCapabilities>(left) | static_cast<FileSystemCapabilities>(right);
}

[[nodiscard]] constexpr FileSystemCapabilities operator|(FileSystemCapabilities left, EFileSystemCapability right) noexcept
{
	return left | static_cast<FileSystemCapabilities>(right);
}

[[nodiscard]] constexpr bool HasCapability(FileSystemCapabilities capabilities, EFileSystemCapability capability) noexcept
{
	return (capabilities & static_cast<FileSystemCapabilities>(capability)) != 0;
}

enum class EFileEntryType : std::uint8_t {
	File,
	Directory,
	SymbolicLink,
	Other,
};

//! stat のスナップショット。uri は常に問い合わせたリソースの正規 URI。
struct FileStat {
	platform::uri::Uri uri;
	EFileEntryType type = EFileEntryType::Other;
	std::uint64_t size = 0;
};

//! ディレクトリ列挙結果。uri が子リソースの identity、name は表示用の名前。
struct DirectoryEntry {
	platform::uri::Uri uri;
	std::wstring name;
	FileStat stat;
};

//! ファイル全体読み込みの明示的な上限。0 は無効で、呼び出し側は必ず上限を選ぶ。
struct FileReadOptions {
	std::size_t maximumBytes = 0;
};

//! バイナリのファイル内容。Succeeded 以外では結果値を参照しない。
using FileBytes = std::vector<std::uint8_t>;

//! Provider が生成し、利用側は比較と持ち回りだけを行う不透明なファイル版。
//!
//! token の表現は provider 固有であり、永続化や別 provider への転用をしてはならない。
//! 固定上限により、外部入力由来の token が無制限な割り当てを発生させない。
class FileVersionToken final {
public:
	static constexpr std::size_t kMaximumBytes = 64;

	[[nodiscard]] static std::optional<FileVersionToken> FromOpaqueBytes(
		std::span<const std::uint8_t> bytes) noexcept
	{
		if (bytes.empty() || bytes.size() > kMaximumBytes) return std::nullopt;
		FileVersionToken token;
		token.m_size = bytes.size();
		for (std::size_t index = 0; index < bytes.size(); ++index) {
			token.m_bytes[index] = bytes[index];
		}
		return token;
	}

	[[nodiscard]] bool Empty() const noexcept { return m_size == 0; }
	[[nodiscard]] std::size_t Size() const noexcept { return m_size; }

	friend bool operator==(const FileVersionToken& left, const FileVersionToken& right) noexcept
	{
		if (left.m_size != right.m_size) return false;
		for (std::size_t index = 0; index < left.m_size; ++index) {
			if (left.m_bytes[index] != right.m_bytes[index]) return false;
		}
		return true;
	}

private:
	std::array<std::uint8_t, kMaximumBytes> m_bytes{};
	std::size_t m_size = 0;
};

//! 同一 native handle から得た内容と版の不可分な読み取り結果。
struct FileContentSnapshot {
	FileBytes bytes;
	FileVersionToken version;
};

enum class EFileConditionalReplaceExpectation : std::uint8_t {
	//! resource が expectedVersion と一致する場合だけ置換する。
	Current,
	//! resource が存在しない場合だけ新規作成する。
	Missing,
};

struct FileConditionalReplaceOptions {
	EFileConditionalReplaceExpectation expectation = EFileConditionalReplaceExpectation::Current;
	//! expectation == Current の場合は空でない token が必須。Missing では参照しない。
	FileVersionToken expectedVersion;

	[[nodiscard]] static FileConditionalReplaceOptions ForCurrent(FileVersionToken version) noexcept
	{
		return { .expectation = EFileConditionalReplaceExpectation::Current, .expectedVersion = std::move(version) };
	}

	[[nodiscard]] static FileConditionalReplaceOptions ForMissing() noexcept
	{
		return { .expectation = EFileConditionalReplaceExpectation::Missing };
	}
};

//! conditional atomic replace 専用の終端状態。
enum class EFileConditionalReplaceStatus : std::uint8_t {
	Succeeded,
	Conflict,
	Unsupported,
	Failed,
};

struct FileConditionalReplaceResult {
	EFileConditionalReplaceStatus status = EFileConditionalReplaceStatus::Failed;
	//! Succeeded の場合に、実際に publish したファイルの provider 版を持つ。
	std::optional<FileVersionToken> committedVersion;
	std::wstring diagnostic;

	[[nodiscard]] bool Succeeded() const noexcept
	{
		return status == EFileConditionalReplaceStatus::Succeeded;
	}
	explicit operator bool() const noexcept { return Succeeded(); }

	[[nodiscard]] static FileConditionalReplaceResult Success(
		FileVersionToken version,
		std::wstring message = {})
	{
		return {
			.status = EFileConditionalReplaceStatus::Succeeded,
			.committedVersion = std::move(version),
			.diagnostic = std::move(message),
		};
	}

	[[nodiscard]] static FileConditionalReplaceResult Conflict(std::wstring message = {})
	{
		return { .status = EFileConditionalReplaceStatus::Conflict, .diagnostic = std::move(message) };
	}

	[[nodiscard]] static FileConditionalReplaceResult Unsupported(std::wstring message = {})
	{
		return { .status = EFileConditionalReplaceStatus::Unsupported, .diagnostic = std::move(message) };
	}

	[[nodiscard]] static FileConditionalReplaceResult Failure(std::wstring message = {})
	{
		return { .status = EFileConditionalReplaceStatus::Failed, .diagnostic = std::move(message) };
	}
};

//! rename の明示的なオプション。overwrite=false は既存 target を AlreadyExists で拒否する。
struct FileRenameOptions {
	bool overwrite = false;
};

//! delete の明示的なオプション。
//!
//! useTrash の既定 true は VS Code の files.enableTrash 既定と一致させる。
//! recursive=false は空でないディレクトリを削除しない。
struct FileDeleteOptions {
	bool recursive = false;
	bool useTrash = true;
};

struct FileWatchOptions {
	bool recursive = false;
};

//! Watch は advisory。Overflow / RescanRequired 後の完全な再走査は呼び出し側が所有する。
enum class EFileWatchEventType : std::uint8_t {
	Created,
	Changed,
	Deleted,
	Renamed,
	Overflow,
	RescanRequired,
	Disposed,
};

struct FileWatchEvent {
	EFileWatchEventType type = EFileWatchEventType::Changed;
	//! 変更対象、または Overflow / RescanRequired / Disposed 時は watch root の URI。
	platform::uri::Uri uri;
	//! Rename の変更前 URI。その他の event では値を持たない。
	std::optional<platform::uri::Uri> previousUri;
};

//! 1つの watch の排他的な取り消し所有権。
//!
//! Watch を受け取った呼び出し側だけが Cancel を呼ぶ。Cancel は terminal Disposed
//! event を1件予約し、Next はその event を返した後で Cancelled を返す。
class IFileWatch {
public:
	virtual ~IFileWatch() = default;

	[[nodiscard]] virtual FileResult<void> Cancel() = 0;
	[[nodiscard]] virtual FileResult<FileWatchEvent> Next() = 0;
};

//! URI scheme ごとの非同期化可能なファイルシステム実装。
//!
//! stat / enumerate / read / watch、version-aware conditional publish、および
//! directory 作成 / rename / delete の契約。Copy capability は将来の操作との
//! 互換性を先に確保する宣言である。
class IFileSystemProvider {
public:
	virtual ~IFileSystemProvider() = default;

	[[nodiscard]] virtual FileSystemCapabilities Capabilities() const noexcept = 0;
	[[nodiscard]] virtual FileResult<FileStat> Stat(const platform::uri::Uri& resource) = 0;
	[[nodiscard]] virtual FileResult<std::vector<DirectoryEntry>> Enumerate(const platform::uri::Uri& directory) = 0;
	[[nodiscard]] virtual FileResult<FileBytes> Read(
		const platform::uri::Uri& resource,
		const FileReadOptions& options) = 0;
	//! 既存 provider の source compatibility のため、未実装時は明示的な Unsupported。
	[[nodiscard]] virtual FileResult<FileContentSnapshot> ReadVersioned(
		const platform::uri::Uri&,
		const FileReadOptions&)
	{
		return FileResult<FileContentSnapshot>::Failure(
			EFileResultStatus::Unsupported, L"filesystem provider does not support versioned reads");
	}
	//! expected-current / expected-missing の条件付き atomic publish。
	[[nodiscard]] virtual FileConditionalReplaceResult ConditionalAtomicReplace(
		const platform::uri::Uri&,
		const FileBytes&,
		const FileConditionalReplaceOptions&)
	{
		return FileConditionalReplaceResult::Unsupported(
			L"filesystem provider does not support conditional atomic replace");
	}
	[[nodiscard]] virtual FileResult<std::unique_ptr<IFileWatch>> Watch(
		const platform::uri::Uri& resource,
		const FileWatchOptions& options) = 0;
	//! 新規ディレクトリの作成。Write capability に属する。既存は AlreadyExists。
	//! Win32 の CreateDirectory マクロと衝突しない名前を用いる。
	[[nodiscard]] virtual FileResult<void> MakeDirectory(const platform::uri::Uri&)
	{
		return FileResult<void>::Failure(
			EFileResultStatus::Unsupported, L"filesystem provider does not support directory creation");
	}
	//! 同一 provider 内の rename/move。Rename capability に属する。
	[[nodiscard]] virtual FileResult<void> Rename(
		const platform::uri::Uri& /*source*/,
		const platform::uri::Uri& /*target*/,
		const FileRenameOptions&)
	{
		return FileResult<void>::Failure(
			EFileResultStatus::Unsupported, L"filesystem provider does not support rename");
	}
	//! 削除。Delete capability に属し、useTrash はネイティブのごみ箱へ移す。
	[[nodiscard]] virtual FileResult<void> Delete(
		const platform::uri::Uri&,
		const FileDeleteOptions&)
	{
		return FileResult<void>::Failure(
			EFileResultStatus::Unsupported, L"filesystem provider does not support delete");
	}
};

} // namespace platform::filesystem
