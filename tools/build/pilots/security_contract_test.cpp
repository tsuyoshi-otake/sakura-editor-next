/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include <sakura/security/CurrentUserSecurityAttributes.h>

#if __has_include("platform/security/CurrentUserSecurityAttributes.h")
#error "sakura_security_tests consumer can reach the provider private header"
#endif

#include <array>
#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

using platform::security::CurrentUserSecurityAttributes;

static_assert(!std::is_copy_constructible_v<CurrentUserSecurityAttributes>);
static_assert(!std::is_copy_assignable_v<CurrentUserSecurityAttributes>);

bool InitializesProtectedCurrentUserAttributes()
{
	CurrentUserSecurityAttributes attributes;
	std::wstring diagnostic;
	if (!attributes.Initialize(diagnostic) || !diagnostic.empty()) return false;
	const auto* securityAttributes = attributes.Attributes();
	if (!attributes.IsInitialized() || securityAttributes == nullptr) return false;
	if (securityAttributes->nLength != sizeof(SECURITY_ATTRIBUTES)
		|| securityAttributes->bInheritHandle != FALSE
		|| securityAttributes->lpSecurityDescriptor == nullptr) {
		return false;
	}
	diagnostic = L"unexpected";
	if (!attributes.Initialize(diagnostic) || !diagnostic.empty()) return false;
	return attributes.Attributes() == securityAttributes;
}

struct TestCase {
	std::string_view name;
	bool (*run)();
};

constexpr std::array kTests{
	TestCase{ "InitializesProtectedCurrentUserAttributes", InitializesProtectedCurrentUserAttributes },
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

int main(int argc, char** argv)
{
	std::string_view filter = "*";
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument = argv[index];
		if (argument == "--gtest_list_tests") {
			std::cout << "CurrentUserSecurityAttributes.\n";
			for (const auto& test : kTests) std::cout << "  " << test.name << '\n';
			return 0;
		}
		constexpr std::string_view prefix = "--gtest_filter=";
		if (argument.starts_with(prefix)) filter = argument.substr(prefix.size());
	}

	int selected = 0;
	int failed = 0;
	for (const auto& test : kTests) {
		const std::string fullName = "CurrentUserSecurityAttributes." + std::string(test.name);
		if (!Matches(fullName, filter)) continue;
		++selected;
		const bool passed = test.run();
		std::cout << (passed ? "[       OK ] " : "[  FAILED  ] ") << fullName << '\n';
		if (!passed) ++failed;
	}
	std::cout << "[==========] " << selected << " tests ran; " << failed << " failed.\n";
	return failed == 0 && selected > 0 ? 0 : 1;
}
