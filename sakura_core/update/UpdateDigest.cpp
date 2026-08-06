/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "StdAfx.h"

#include "update/UpdateDigest.h"

#include <bcrypt.h>

namespace update {
namespace {

constexpr std::size_t kSha256HexLength = 64;

//! `BCryptHashData` takes a `ULONG` length, so a payload larger than that has to
//! be fed in pieces regardless of how it is held in memory.
constexpr std::size_t kHashChunkBytes = 64 * 1024;

[[nodiscard]] bool NtSuccess(NTSTATUS status) noexcept { return status >= 0; }

class AlgorithmProvider final {
public:
	~AlgorithmProvider() { if (m_value != nullptr) ::BCryptCloseAlgorithmProvider(m_value, 0); }
	[[nodiscard]] BCRYPT_ALG_HANDLE* Address() noexcept { return &m_value; }
	[[nodiscard]] BCRYPT_ALG_HANDLE Get() const noexcept { return m_value; }

private:
	BCRYPT_ALG_HANDLE m_value = nullptr;
};

class HashHandle final {
public:
	~HashHandle() { if (m_value != nullptr) ::BCryptDestroyHash(m_value); }
	[[nodiscard]] BCRYPT_HASH_HANDLE* Address() noexcept { return &m_value; }
	[[nodiscard]] BCRYPT_HASH_HANDLE Get() const noexcept { return m_value; }

private:
	BCRYPT_HASH_HANDLE m_value = nullptr;
};

[[nodiscard]] bool IsLowerHexDigit(wchar_t ch) noexcept
{
	return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f');
}

[[nodiscard]] std::optional<wchar_t> ToLowerHexDigit(wchar_t ch) noexcept
{
	if (ch >= L'0' && ch <= L'9') return ch;
	if (ch >= L'a' && ch <= L'f') return ch;
	if (ch >= L'A' && ch <= L'F') return static_cast<wchar_t>(ch - L'A' + L'a');
	return std::nullopt;
}

} // namespace

std::optional<std::wstring> UpdateDigest::ComputeSha256Hex(const std::vector<std::uint8_t>& bytes)
{
	AlgorithmProvider algorithm;
	if (!NtSuccess(::BCryptOpenAlgorithmProvider(algorithm.Address(), BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
		return std::nullopt;
	}

	DWORD hashLength = 0;
	DWORD resultSize = 0;
	if (!NtSuccess(::BCryptGetProperty(
			algorithm.Get(), BCRYPT_HASH_LENGTH,
			reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &resultSize, 0))
		|| hashLength == 0) {
		return std::nullopt;
	}

	HashHandle hash;
	if (!NtSuccess(::BCryptCreateHash(algorithm.Get(), hash.Address(), nullptr, 0, nullptr, 0, 0))) {
		return std::nullopt;
	}

	for (std::size_t offset = 0; offset < bytes.size(); offset += kHashChunkBytes) {
		const std::size_t length = (std::min)(kHashChunkBytes, bytes.size() - offset);
		if (!NtSuccess(::BCryptHashData(
				hash.Get(),
				const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(bytes.data() + offset)),
				static_cast<ULONG>(length), 0))) {
			return std::nullopt;
		}
	}

	std::vector<UCHAR> digest(hashLength);
	if (!NtSuccess(::BCryptFinishHash(hash.Get(), digest.data(), hashLength, 0))) return std::nullopt;

	constexpr wchar_t szHex[] = L"0123456789abcdef";
	std::wstring result;
	result.reserve(digest.size() * 2);
	for (const UCHAR byte : digest) {
		result += szHex[byte >> 4];
		result += szHex[byte & 0x0F];
	}
	return result;
}

bool DigestsMatch(std::wstring_view left, std::wstring_view right) noexcept
{
	if (left.size() != kSha256HexLength || right.size() != kSha256HexLength) return false;
	for (std::size_t index = 0; index < kSha256HexLength; ++index) {
		const auto leftDigit = ToLowerHexDigit(left[index]);
		const auto rightDigit = ToLowerHexDigit(right[index]);
		if (!leftDigit || !rightDigit) return false;
		if (!IsLowerHexDigit(*leftDigit) || *leftDigit != *rightDigit) return false;
	}
	return true;
}

} // namespace update
