/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include <sakura/controlipc/ControlIpcSecurity.h>
#include <sakura/controlipc/ControlPlatformEndpoint.h>

#if __has_include("platform/controlipc/ControlPlatformEndpoint.h")
#error "sakura_controlipc_endpoint_tests can reach the removed private endpoint contract"
#endif

#include <Windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using namespace platform::controlipc;

constexpr std::string_view kProfileId = "0123456789abcdef0123456789abcdef";

std::filesystem::path UniqueProfileDirectory()
{
	std::error_code error;
	auto root = std::filesystem::temp_directory_path(error);
	if (error) root = std::filesystem::current_path(error);
	return root / (L"sakura-controlipc-endpoint-contract-" + std::to_wstring(::GetCurrentProcessId()) +
		L"-" + std::to_wstring(::GetTickCount64()));
}

ControlPlatformEndpointSnapshot Snapshot(
	std::wstring_view profileHash, ControlPlatformEndpointLifecycle lifecycle, std::uint64_t generation)
{
	return {
		.controlProcessId = ::GetCurrentProcessId(),
		.generation = generation,
		.lifecycle = lifecycle,
		.profileHash = std::wstring(profileHash),
		.pipeName = BuildControlPipeName(profileHash),
		.profileId = std::string(kProfileId),
	};
}

bool PublishesAndDiscoversTheTypedEndpoint()
{
	const auto profile = UniqueProfileDirectory();
	const auto profileHash = ComputeCanonicalProfileHash(profile);
	if (profileHash.empty()) return false;

	CControlPlatformEndpoint control;
	std::wstring diagnostic;
	if (!control.CreateForControl(profile, diagnostic)) return false;
	if (control.ReadDetailed({ .requireLiveControlProcess = false }).disposition !=
		EControlPlatformEndpointDiscoveryDisposition::NotPublished) return false;

	if (!control.Publish(Snapshot(profileHash, ControlPlatformEndpointLifecycle::Starting, 1), diagnostic)) return false;
	if (control.ReadDetailed({ .requireLiveControlProcess = false }).disposition !=
		EControlPlatformEndpointDiscoveryDisposition::NotAccepting) return false;

	CControlPlatformEndpoint editor;
	const auto opened = editor.OpenForEditorDetailed(profile);
	if (opened.disposition != EControlPlatformEndpointDiscoveryDisposition::Discovered) return false;
	if (editor.ReadDetailed({ .requireLiveControlProcess = false }).disposition !=
		EControlPlatformEndpointDiscoveryDisposition::NotAccepting) return false;

	if (!control.Publish(Snapshot(profileHash, ControlPlatformEndpointLifecycle::Accepting, 7), diagnostic)) return false;
	const auto accepted = editor.ReadDetailed({ .minimumGeneration = 6, .requireLiveControlProcess = false });
	if (!accepted.IsDiscovered() || accepted.snapshot->generation != 7 ||
		accepted.snapshot->profileId != kProfileId) return false;

	editor.Close();
	return editor.ReadDetailed({ .requireLiveControlProcess = false }).disposition ==
		EControlPlatformEndpointDiscoveryDisposition::Closed;
}

bool RejectsMutatedIdentityAndStaleGenerations()
{
	const std::wstring hash(64, L'a');
	auto valid = Snapshot(hash, ControlPlatformEndpointLifecycle::Accepting, 9);
	if (!CControlPlatformEndpoint::IsSnapshotUsable(valid, hash, { .minimumGeneration = 9,
		.requireLiveControlProcess = false })) return false;
	valid.pipeName.back() = valid.pipeName.back() == L'a' ? L'b' : L'a';
	if (CControlPlatformEndpoint::IsSnapshotUsable(valid, hash, { .requireLiveControlProcess = false })) return false;
	valid = Snapshot(hash, ControlPlatformEndpointLifecycle::Accepting, 8);
	return CControlPlatformEndpoint::ClassifySnapshot(valid, hash, { .minimumGeneration = 9,
		.requireLiveControlProcess = false }) == EControlPlatformEndpointDiscoveryDisposition::DeadOrStale;
}

bool CloseIsTerminalAndIdempotent()
{
	CControlPlatformEndpoint endpoint;
	endpoint.Close();
	endpoint.Close();
	return endpoint.ReadDetailed().disposition == EControlPlatformEndpointDiscoveryDisposition::Closed;
}

class TestCase final {
public:
	constexpr TestCase(std::string_view name, bool (*run)()) noexcept : m_name(name), m_run(run) {}
	[[nodiscard]] constexpr std::string_view Name() const noexcept { return m_name; }
	[[nodiscard]] bool Run() const { return m_run(); }

private:
	std::string_view m_name;
	bool (*m_run)();
};

constexpr std::array kTests{
	TestCase{ "PublishesAndDiscoversTheTypedEndpoint", PublishesAndDiscoversTheTypedEndpoint },
	TestCase{ "RejectsMutatedIdentityAndStaleGenerations", RejectsMutatedIdentityAndStaleGenerations },
	TestCase{ "CloseIsTerminalAndIdempotent", CloseIsTerminalAndIdempotent },
};

bool Matches(std::string_view fullName, std::string_view filter)
{
	if (filter.empty() || filter == "*") return true;
	const auto star = filter.find('*');
	if (star == std::string_view::npos) return fullName == filter;
	return fullName.starts_with(filter.substr(0, star)) && fullName.ends_with(filter.substr(star + 1));
}

} // namespace

int main(int argc, char** argv)
{
	std::string_view filter = "*";
	for (int index = 1; index < argc; ++index) {
		const std::string_view argument = argv[index];
		if (argument == "--gtest_list_tests") {
			std::cout << "ControlIpcEndpointContract.\n";
			for (const auto& test : kTests) std::cout << "  " << test.Name() << '\n';
			return 0;
		}
		constexpr std::string_view prefix = "--gtest_filter=";
		if (argument.starts_with(prefix)) filter = argument.substr(prefix.size());
	}

	int selected = 0;
	int failed = 0;
	for (const auto& test : kTests) {
		const std::string fullName = "ControlIpcEndpointContract." + std::string(test.Name());
		if (!Matches(fullName, filter)) continue;
		++selected;
		const bool passed = test.Run();
		std::cout << (passed ? "[       OK ] " : "[  FAILED  ] ") << fullName << '\n';
		if (!passed) ++failed;
	}
	std::cout << "[==========] " << selected << " tests ran; " << failed << " failed.\n";
	return failed == 0 && selected > 0 ? 0 : 1;
}
