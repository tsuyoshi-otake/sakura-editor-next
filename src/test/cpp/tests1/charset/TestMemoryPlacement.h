/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/

#pragma once

#include <Windows.h>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string_view>

// Memory that an ABI-boundary test places where it needs it, shared by the encoded-EOL and the UTF-8 / CESU-8
// candidate suites. GuardedRegion puts bytes directly before a PAGE_NOACCESS page or at an odd address, so a callee
// that reads or writes one byte past what it was given faults. Arena is raw storage in which a test places
// descriptors, data and records at chosen offsets.

class GuardedRegion {
public:
	GuardedRegion()
	{
		SYSTEM_INFO systemInfo{};
		::GetSystemInfo(&systemInfo);
		m_pageSize = systemInfo.dwPageSize;
		if (m_pageSize <= 3) {
			throw std::runtime_error("Invalid system page size");
		}
		m_page = static_cast<char*>(
			::VirtualAlloc(nullptr, m_pageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
		if (m_page == nullptr) {
			throw std::runtime_error("Cannot allocate the corpus guard-page region");
		}
		DWORD oldProtection = 0;
		if (!::VirtualProtect(m_page + m_pageSize, m_pageSize, PAGE_NOACCESS, &oldProtection)) {
			::VirtualFree(m_page, 0, MEM_RELEASE);
			throw std::runtime_error("Cannot protect the corpus guard page");
		}
	}

	~GuardedRegion()
	{
		::VirtualFree(m_page, 0, MEM_RELEASE);
	}

	GuardedRegion(const GuardedRegion&) = delete;
	GuardedRegion& operator=(const GuardedRegion&) = delete;

	const char* PlaceAtEnd(std::string_view bytes)
	{
		return Place(bytes, m_pageSize - bytes.size());
	}

	const char* PlaceAtOddAddress(std::string_view bytes)
	{
		return Place(bytes, 1);
	}

private:
	const char* Place(std::string_view bytes, size_t offset)
	{
		assert(bytes.size() < m_pageSize - 1);
		if (bytes.size() >= m_pageSize - 1) {
			throw std::runtime_error("Corpus case does not fit in the guard-page region");
		}
		std::memset(m_page, 0, m_pageSize);
		char* const data = m_page + offset;
		if (!bytes.empty()) {
			std::memcpy(data, bytes.data(), bytes.size());
		}
		return data;
	}

	size_t m_pageSize;
	char* m_page;
};

// Raw, unsized storage in which a test places descriptors, data and records at chosen offsets, so every overlap is
// deliberate. Objects are copied in as bytes and only the callee reads or writes them through the returned pointers.
struct alignas(16) Arena {
	std::array<unsigned char, 512> bytes{};
};

template <class T>
T* PlaceAt(Arena& arena, std::size_t offset, const T& value)
{
	std::memcpy(arena.bytes.data() + offset, &value, sizeof(T));
	return reinterpret_cast<T*>(arena.bytes.data() + offset);
}

inline bool SameArena(const Arena& left, const Arena& right) noexcept
{
	return std::memcmp(left.bytes.data(), right.bytes.data(), left.bytes.size()) == 0;
}
