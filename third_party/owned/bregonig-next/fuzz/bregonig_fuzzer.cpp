#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "bregexp.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
	if (data == nullptr || size < 2) {
		return 0;
	}
	const std::size_t split = data[0] % (size - 1);
	const std::size_t patternBytes = (std::min)(split, static_cast<std::size_t>(64));
	const std::size_t targetBytes = (std::min)(size - 1 - split, static_cast<std::size_t>(256));

	std::wstring pattern(1, L'/');
	for (std::size_t i = 0; i < patternBytes; ++i) {
		pattern.push_back(static_cast<wchar_t>(data[1 + i]));
	}
	pattern.push_back(L'/');

	std::wstring target;
	target.resize(targetBytes);
	for (std::size_t i = 0; i < targetBytes; ++i) {
		target[i] = static_cast<wchar_t>(data[1 + split + i]);
	}

	wchar_t msg[BREGEXP_MAX_ERROR_MESSAGE_LEN];
	std::memset(msg, 0, sizeof(msg));
	BREGEXP* rx = nullptr;
	(void)BMatch(pattern.data(), target.data(), target.data() + target.size(), &rx, msg);
	if (rx != nullptr) {
		BRegfree(rx);
	}

	std::memset(msg, 0, sizeof(msg));
	rx = nullptr;
	std::wstring subst = pattern + L"x/";
	if (subst.size() > 2) {
		(void)BSubst(subst.data(), target.data(), target.data() + target.size(), &rx, msg);
		if (rx != nullptr) {
			BRegfree(rx);
		}
	}
	return 0;
}
