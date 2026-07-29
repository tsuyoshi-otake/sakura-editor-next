/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace platform {

//! RtlGetVersion の呼び出し結果。
enum class WindowsBuildStatus : std::uint8_t {
	Success,
	NtdllUnavailable,
	RtlGetVersionUnavailable,
	RtlGetVersionFailed,
};

//! バージョン取得結果。失敗時は build を参照せず status を確認する。
struct WindowsBuildResult {
	WindowsBuildStatus status = WindowsBuildStatus::RtlGetVersionFailed;
	std::uint32_t major = 0;
	std::uint32_t minor = 0;
	std::uint32_t build = 0;
};

//! RtlGetVersion が返す値を、OS に依存しない結果へ変換する。
WindowsBuildResult EvaluateWindowsBuildQuery(
	std::int32_t ntStatus,
	std::uint32_t major,
	std::uint32_t minor,
	std::uint32_t build
) noexcept;

//! ntdll!RtlGetVersion を使って実際の Windows build を取得する。
WindowsBuildResult QueryWindowsBuild() noexcept;

//! 指定 build 以上かを確認する。取得失敗時は常に false。
bool IsBuildAtLeast(const WindowsBuildResult& result, std::uint32_t minimumBuild) noexcept;

//! Windows 11 で導入された機能を安全に使用できる build かを確認する。
bool SupportsWindows11Features(const WindowsBuildResult& result) noexcept;

//! 起動初期でも使える、リソースに依存しない固定長の診断メッセージ。
struct StartupPlatformDiagnostic {
	std::array<wchar_t, 128> text{};
};

StartupPlatformDiagnostic FormatStartupPlatformDiagnostic(const WindowsBuildResult& result) noexcept;

//! PE COFF ヘッダーに記録された target machine。
enum class PeMachine : std::uint8_t {
	Amd64,
	I386,
	Arm64,
	Unknown,
	Invalid,
	IoError,
};

//! メモリ上の PE bytes から machine を判定する。入力は変更しない。
PeMachine ParsePeMachine(std::span<const std::byte> bytes) noexcept;

//! 最小限の DOS/NT ヘッダーだけを bounded read して PE machine を判定する。
//! path は null 終端の Windows path でなければならない。
PeMachine ReadPeMachineFromFile(const wchar_t* path) noexcept;

} // namespace platform
