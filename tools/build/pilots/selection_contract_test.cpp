/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */
#include <sakura/editor/SelectionSession.h>

#if __has_include("workbench/editor/selection/SelectionSession.h")
#error "sakura_editor_selection_tests consumer can reach the provider private header"
#endif

#include <array>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using editor::selection::ESelectionMode;
using editor::selection::ESelectionTransition;
using editor::selection::SelectionSession;

bool StartsAndEndsWithExplicitTerminalStates()
{
	SelectionSession session;
	if (session.IsActive() || session.End() != ESelectionTransition::Noop) return false;
	if (session.Begin(ESelectionMode::Linear) != ESelectionTransition::Started) return false;
	if (!session.IsActive() || !session.IsMode(ESelectionMode::Linear)) return false;
	if (session.End() != ESelectionTransition::Ended) return false;
	return !session.IsActive() && session.End() == ESelectionTransition::Noop;
}

bool ModeRestartDoesNotLeakMultipleModes()
{
	SelectionSession session;
	if (session.Begin(ESelectionMode::Box) != ESelectionTransition::Started || !session.IsBoxSelecting()) return false;
	if (session.Begin(ESelectionMode::Word) != ESelectionTransition::Restarted) return false;
	return session.IsWordSelecting() && !session.IsBoxSelecting();
}

bool DisablingTheActiveModeFallsBackToLinear()
{
	SelectionSession session;
	if (session.Begin(ESelectionMode::Line) != ESelectionTransition::Started) return false;
	if (session.SetModeEnabled(ESelectionMode::Line, false) != ESelectionTransition::ModeChanged) return false;
	return session.IsActive() && session.Mode() == ESelectionMode::Linear && !session.IsLineSelecting();
}

bool DisablingAnInactiveOrDifferentModeIsNoop()
{
	SelectionSession session;
	if (session.SetModeEnabled(ESelectionMode::Word, false) != ESelectionTransition::Noop) return false;
	if (session.Begin(ESelectionMode::Nazo) != ESelectionTransition::Started) return false;
	return session.SetModeEnabled(ESelectionMode::Box, false) == ESelectionTransition::Noop
		&& session.IsMode(ESelectionMode::Nazo);
}

struct TestCase {
	std::string_view name;
	bool (*run)();
};

constexpr std::array kTests{
	TestCase{"StartsAndEndsWithExplicitTerminalStates", StartsAndEndsWithExplicitTerminalStates},
	TestCase{"ModeRestartDoesNotLeakMultipleModes", ModeRestartDoesNotLeakMultipleModes},
	TestCase{"DisablingTheActiveModeFallsBackToLinear", DisablingTheActiveModeFallsBackToLinear},
	TestCase{"DisablingAnInactiveOrDifferentModeIsNoop", DisablingAnInactiveOrDifferentModeIsNoop},
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
	std::string_view filter = "SelectionSession.*";
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument = argv[index];
		if (argument == "--gtest_list_tests") {
			std::cout << "SelectionSession.\n";
			for (const auto& test : kTests) std::cout << "  " << test.name << '\n';
			return 0;
		}
		constexpr std::string_view prefix = "--gtest_filter=";
		if (argument.starts_with(prefix)) filter = argument.substr(prefix.size());
	}

	int selected = 0;
	int failed = 0;
	for (const auto& test : kTests) {
		const std::string fullName = "SelectionSession." + std::string(test.name);
		if (!Matches(fullName, filter)) continue;
		++selected;
		const bool passed = test.run();
		std::cout << (passed ? "[       OK ] " : "[  FAILED  ] ") << fullName << '\n';
		if (!passed) ++failed;
	}
	std::cout << "[==========] " << selected << " tests ran; " << failed << " failed.\n";
	return failed == 0 && selected > 0 ? 0 : 1;
}
