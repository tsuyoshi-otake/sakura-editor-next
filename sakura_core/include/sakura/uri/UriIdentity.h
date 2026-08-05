/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

//! VS Code 互換の workbench 層で使う、Win32 UI 非依存の URI 値型。
namespace platform::uri {

//! URI の解釈に失敗した理由。値を持たない結果では必ず None 以外となる。
enum class EUriParseError : std::uint8_t {
	None,
	EmptyInput,
	MissingScheme,
	InvalidScheme,
	InvalidAuthority,
	InvalidPath,
	InvalidQuery,
	InvalidFragment,
	InvalidPercentEncoding,
	InvalidUtf8,
	InvalidWindowsPath,
};

//! Windows path への変換に失敗した理由。
enum class EUriWindowsPathError : std::uint8_t {
	None,
	NotFileUri,
	InvalidFileUri,
};

struct UriParseResult;

//! 不変な URI 値。
//!
//! 各 component は percent decode 済みの Unicode 文字列として保持する。ToString は
//! RFC 3986 の区切りを壊さないよう UTF-8 の percent encoding を使って正規化する。
//! ファイルシステム照会や realpath 解決は一切行わない。
class Uri final {
public:
	//! URI/IRI を安全に解釈する。入力の raw space、raw backslash、壊れた %xx は拒否する。
	static UriParseResult Parse(std::wstring_view text);

	//! percent decode 済み component から URI を作る。空 query/fragment も保持できる。
	static UriParseResult FromComponents(
		std::wstring scheme,
		std::wstring authority,
		std::wstring path,
		std::optional<std::wstring> query = std::nullopt,
		std::optional<std::wstring> fragment = std::nullopt,
		bool hasAuthority = false
	);

	//! 絶対 Windows drive path または UNC path から file URI を作る。I/O は行わない。
	static UriParseResult FromWindowsPath(std::wstring_view windowsPath);

	const std::wstring& Scheme() const noexcept { return m_scheme; }
	const std::wstring& Authority() const noexcept { return m_authority; }
	const std::wstring& Path() const noexcept { return m_path; }
	const std::optional<std::wstring>& Query() const noexcept { return m_query; }
	const std::optional<std::wstring>& Fragment() const noexcept { return m_fragment; }
	bool HasAuthority() const noexcept { return m_hasAuthority; }

	//! 比較可能で安全に埋め込める percent-encoded URI を返す。
	std::wstring ToString() const;

	//! file URI を Windows drive/UNC path に変換する。I/O や正規化は行わない。
	struct WindowsPathResult;
	WindowsPathResult ToWindowsPath() const;

private:
	Uri(
		std::wstring scheme,
		std::wstring authority,
		std::wstring path,
		std::optional<std::wstring> query,
		std::optional<std::wstring> fragment,
		bool hasAuthority
	) noexcept;

	std::wstring m_scheme;
	std::wstring m_authority;
	std::wstring m_path;
	std::optional<std::wstring> m_query;
	std::optional<std::wstring> m_fragment;
	bool m_hasAuthority = false;
};

//! URI の生成・parse 結果。成功時だけ value を参照する。
struct UriParseResult {
	std::optional<Uri> value;
	EUriParseError error = EUriParseError::None;

	explicit operator bool() const noexcept { return value.has_value(); }
};

//! file URI の Windows path 変換結果。成功時だけ value を参照する。
struct Uri::WindowsPathResult {
	std::optional<std::wstring> value;
	EUriWindowsPathError error = EUriWindowsPathError::None;

	explicit operator bool() const noexcept { return value.has_value(); }
};

//! non-file URI に適用する文字大小の比較規則。
//!
//! CaseSensitive は RFC の scheme 以外をそのまま比較する既定値。CaseInsensitive は
//! authority/path/query/fragment 全てを basic Unicode lower-case で
//! 比較する。file URI は常に authority と path を case-insensitive に比較し、query/
//! fragment は URI metadata として case-sensitive に保つ。
enum class ENonFileUriCasePolicy : std::uint8_t {
	CaseSensitive,
	CaseInsensitive,
};

//! URI の identity key と比較を集中させる stateless service。
class UriIdentityService final {
public:
	//! 同値判定用の安定 key を返す。key は UI 表示用・永続 format ではない。
	static std::wstring MakeComparisonKey(
		const Uri& uri,
		ENonFileUriCasePolicy nonFilePolicy = ENonFileUriCasePolicy::CaseSensitive
	);

	static bool IsEqual(
		const Uri& left,
		const Uri& right,
		ENonFileUriCasePolicy nonFilePolicy = ENonFileUriCasePolicy::CaseSensitive
	);
};

} // namespace platform::uri
