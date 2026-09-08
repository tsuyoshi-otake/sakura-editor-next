/*! @file */
/* Copyright (C) 2026, Sakura Editor Organization. SPDX-License-Identifier: Zlib */
#include "pch.h"
#include "senp/github/GhToolPolicy.h"

#include <algorithm>
#include <memory>

namespace {
using namespace senp::github;
using platform::process::EBoundedProcessStatus;

constexpr wchar_t kEnvironmentChildRole[] = L"SAKURA_GH_POLICY_ENV_CHILD";
constexpr wchar_t kEnvironmentSecret[] = L"SAKURA_GH_POLICY_AMBIENT_SECRET";

std::vector<std::uint8_t> Bytes(const std::string_view value)
{
	return { value.begin(), value.end() };
}

class FakePlatform final : public IGhToolPlatform {
public:
	void SetExecutable(std::optional<std::wstring> value) { m_executable = std::move(value); }
	void SetOutcome(GhProcessOutcome value) { m_outcome = std::move(value); }
	void SetThrowOnResolve(bool value) noexcept { m_throwOnResolve = value; }
	void SetThrowOnRun(bool value) noexcept { m_throwOnRun = value; }
	std::optional<std::wstring> ResolveExecutable() const override
	{
		if (m_throwOnResolve) throw std::runtime_error("resolver failure");
		return m_executable;
	}
	GhProcessOutcome Run(const GhProcessInvocation& invocation, HANDLE) const override
	{
		if (m_throwOnRun) throw std::runtime_error("runner failure");
		m_invocations->push_back(invocation);
		return m_outcome;
	}
	[[nodiscard]] const std::vector<GhProcessInvocation>& Invocations() const noexcept { return *m_invocations; }
private:
	std::optional<std::wstring> m_executable{ L"C:\\Tools\\gh.exe" };
	GhProcessOutcome m_outcome{ EBoundedProcessStatus::Succeeded, 0,
		Bytes("gh version 2.93.0 (2026-05-27)\nhttps://github.com/cli/cli/releases/tag/v2.93.0\n"), {} };
	std::shared_ptr<std::vector<GhProcessInvocation>> m_invocations{
		std::make_shared<std::vector<GhProcessInvocation>>() };
	bool m_throwOnResolve{}, m_throwOnRun{};
};

CGhToolPolicy Policy(const std::shared_ptr<FakePlatform>& platform)
{
	return { platform, L"C:\\Program Files\\Sakura Editor NEXT" };
}

GhRepositoryReadRequest Read(std::wstring owner = L"octo-org", std::wstring repository = L"octo-repo",
	std::vector<std::wstring> segments = { L"issues", L"42", L"comments" })
{
	return { L"github.com", std::move(owner), std::move(repository), std::move(segments) };
}

bool ContainsName(const std::vector<std::wstring>& values, const std::wstring_view expected)
{
	return std::ranges::any_of(values, [&](const auto& value) {
		return ::CompareStringOrdinal(value.data(), static_cast<int>(value.size()),
			expected.data(), static_cast<int>(expected.size()), TRUE) == CSTR_EQUAL;
	});
}

std::wstring CurrentExecutable()
{
	std::wstring path(32768, L'\0');
	const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
	path.resize(length);
	return path;
}

std::wstring CurrentDirectory()
{
	std::wstring path(32768, L'\0');
	const DWORD length = ::GetCurrentDirectoryW(static_cast<DWORD>(path.size()), path.data());
	path.resize(length);
	return path;
}

} // namespace

TEST(GhToolPolicyEnvironmentChild, ObservesTheSanitizedProcessEnvironment)
{
	if (::GetEnvironmentVariableW(kEnvironmentChildRole, nullptr, 0) == 0) return;
	if (::GetEnvironmentVariableW(kEnvironmentSecret, nullptr, 0) != 0) ::ExitProcess(9);
}

TEST(GhToolPolicy, ReportsMissingMalformedAndUnsupportedVersionsExplicitly)
{
	auto platform = std::make_shared<FakePlatform>();
	auto policy = Policy(platform);
	platform->SetExecutable(std::nullopt);
	EXPECT_EQ(GhToolAvailability::NotInstalled, policy.Probe(nullptr).Status());

	platform->SetExecutable(L"gh.exe");
	EXPECT_EQ(GhToolAvailability::InvalidConfiguration, policy.Probe(nullptr).Status());
	platform->SetExecutable(L"C:\\Tools\\gh.exe");
	platform->SetOutcome({ EBoundedProcessStatus::Succeeded, 0, Bytes("gh version unknown\n"), {} });
	EXPECT_EQ(GhToolAvailability::ProbeFailed, policy.Probe(nullptr).Status());
	platform->SetOutcome({ EBoundedProcessStatus::Succeeded, 0, Bytes("gh version 2.93.0 altered\n"), {} });
	EXPECT_EQ(GhToolAvailability::ProbeFailed, policy.Probe(nullptr).Status());
	platform->SetOutcome({ EBoundedProcessStatus::Succeeded, 0, Bytes("gh version 2.92.1 (old)\n"), {} });
	const auto unsupported = policy.Probe(nullptr);
	ASSERT_EQ(GhToolAvailability::UnsupportedVersion, unsupported.Status());
	ASSERT_TRUE(unsupported.Version());
	EXPECT_EQ(92U, unsupported.Version()->Minor());
	platform->SetThrowOnResolve(true);
	EXPECT_EQ(GhToolAvailability::ProbeFailed, policy.Probe(nullptr).Status());
}

TEST(GhToolPolicy, AcceptsOnlyTheMeasuredCliContractVersion)
{
	auto platform = std::make_shared<FakePlatform>();
	const auto probe = Policy(platform).Probe(nullptr);
	ASSERT_EQ(GhToolAvailability::Available, probe.Status());
	ASSERT_TRUE(probe.Version());
	EXPECT_EQ(2U, probe.Version()->Major());
	EXPECT_EQ(93U, probe.Version()->Minor());
	EXPECT_EQ(0U, probe.Version()->Patch());
	ASSERT_EQ(1U, platform->Invocations().size());
	EXPECT_EQ(std::vector<std::wstring>{ L"--version" }, platform->Invocations()[0].Arguments());
}

TEST(GhToolPolicy, BuildsOneFixedRestGetWithoutShellOrAmbientGithubSelectors)
{
	auto platform = std::make_shared<FakePlatform>();
	auto policy = Policy(platform);
	const auto probe = policy.Probe(nullptr);
	platform->SetOutcome({ EBoundedProcessStatus::Succeeded, 0, Bytes("HTTP/2.0 200 OK\r\n\r\n{}"), {} });
	const auto result = policy.ReadRepository(probe, Read(), nullptr);
	ASSERT_EQ(GhRepositoryReadStatus::Succeeded, result.Status());
	ASSERT_EQ(2U, platform->Invocations().size());
	const auto& invocation = platform->Invocations()[1];
	EXPECT_EQ(L"C:\\Tools\\gh.exe", invocation.ExecutablePath());
	EXPECT_EQ(L"C:\\Program Files\\Sakura Editor NEXT", invocation.WorkingDirectory());
	EXPECT_EQ((std::vector<std::wstring>{ L"api", L"--hostname", L"github.com", L"--method", L"GET",
		L"--include", L"--header", L"X-GitHub-Api-Version: 2022-11-28",
		L"repos/octo-org/octo-repo/issues/42/comments" }), invocation.Arguments());
	for (const auto name : { L"GH_TOKEN", L"GITHUB_TOKEN", L"GH_ENTERPRISE_TOKEN", L"GITHUB_ENTERPRISE_TOKEN",
		L"GH_HOST", L"GH_REPO", L"GH_DEBUG", L"DEBUG", L"GH_PAGER", L"PAGER", L"GH_BROWSER", L"BROWSER",
		L"GH_FORCE_TTY", L"GH_HTTP_UNIX_SOCKET", L"GH_CONFIG_DIR", L"GH_EDITOR", L"GIT_EDITOR", L"VISUAL", L"EDITOR" }) {
		EXPECT_TRUE(ContainsName(invocation.EnvironmentRemovals(), name)) << testing::PrintToString(name);
	}
	EXPECT_EQ((std::vector<std::pair<std::wstring, std::wstring>>{
		{ L"GH_PROMPT_DISABLED", L"1" }, { L"GH_NO_UPDATE_NOTIFIER", L"1" },
		{ L"GH_SPINNER_DISABLED", L"1" } }),
		invocation.EnvironmentOverrides());
	EXPECT_EQ(30000U, invocation.TimeoutMilliseconds());
	EXPECT_EQ(4U * 1024U * 1024U, invocation.MaximumOutputBytes());
	EXPECT_EQ(64U * 1024U, invocation.MaximumErrorBytes());
}

TEST(GhToolPolicy, RejectsEveryRouteThatCouldSelectFlagsShellOrAnotherRepository)
{
	auto platform = std::make_shared<FakePlatform>();
	auto policy = Policy(platform);
	const auto probe = policy.Probe(nullptr);
	const auto before = platform->Invocations().size();
	const std::vector<GhRepositoryReadRequest> invalid{
		{ L"github.com --verbose", L"owner", L"repo", { L"issues" } },
		{ L"github.com", L"owner/other", L"repo", { L"issues" } },
		{ L"github.com", L"owner", L"repo/other", { L"issues" } },
		{ L"github.com", L"owner", L"repo", { L"..", L"actions" } },
		{ L"github.com", L"owner", L"repo", { L"@payload" } },
		{ L"github.com", L"owner", L"repo", { L"issues?state=all" } },
		{ L"github.com", L"owner", L"repo", {} },
	};
	for (const auto& request : invalid) {
		EXPECT_EQ(GhRepositoryReadStatus::InvalidRequest,
			policy.ReadRepository(probe, request, nullptr).Status());
	}
	EXPECT_EQ(before, platform->Invocations().size());
}

TEST(GhToolPolicy, RequiresAnAvailableProbeAndPreservesEveryProcessTerminal)
{
	auto platform = std::make_shared<FakePlatform>();
	auto policy = Policy(platform);
	platform->SetOutcome({ EBoundedProcessStatus::Succeeded, 0, Bytes("gh version 3.0.0 (future)\n"), {} });
	const auto unsupported = policy.Probe(nullptr);
	EXPECT_EQ(GhRepositoryReadStatus::UnsupportedVersion,
		policy.ReadRepository(unsupported, Read(), nullptr).Status());

	platform->SetOutcome({ EBoundedProcessStatus::Succeeded, 0, Bytes("gh version 2.93.0 (supported)\n"), {} });
	const auto supported = policy.Probe(nullptr);
	platform->SetThrowOnRun(true);
	EXPECT_EQ(GhRepositoryReadStatus::LaunchFailed,
		policy.ReadRepository(supported, Read(), nullptr).Status());
	platform->SetThrowOnRun(false);
	for (const auto [processStatus, expected] : {
		std::pair{ EBoundedProcessStatus::Failed, GhRepositoryReadStatus::Failed },
		std::pair{ EBoundedProcessStatus::LaunchFailed, GhRepositoryReadStatus::LaunchFailed },
		std::pair{ EBoundedProcessStatus::TimedOut, GhRepositoryReadStatus::TimedOut },
		std::pair{ EBoundedProcessStatus::Cancelled, GhRepositoryReadStatus::Cancelled },
		std::pair{ EBoundedProcessStatus::OutputLimitExceeded, GhRepositoryReadStatus::OutputLimitExceeded },
	}) {
		platform->SetOutcome({ processStatus, 7, {}, Bytes("terminal") });
		const auto result = policy.ReadRepository(supported, Read(), nullptr);
		EXPECT_EQ(expected, result.Status());
		EXPECT_EQ(7, result.ExitCode());
		EXPECT_EQ(Bytes("terminal"), result.StandardError());
	}
}

TEST(GhToolPolicy, BoundedRunnerRemovesAmbientNamesCaseInsensitively)
{
	ASSERT_TRUE(::SetEnvironmentVariableW(kEnvironmentSecret, L"must-not-cross-boundary"));
	platform::process::BoundedProcessRequest request(CurrentExecutable(), CurrentDirectory(), {
		L"--gtest_filter=GhToolPolicyEnvironmentChild.ObservesTheSanitizedProcessEnvironment",
		L"--gtest_color=no",
	});
	request.SetEnvironmentOverrides({ { kEnvironmentChildRole, L"1" } });
	request.SetEnvironmentRemovals({ L"sakura_gh_policy_ambient_secret" });
	request.SetTimeoutMilliseconds(3000);
	const auto result = platform::process::RunBoundedProcess(request, nullptr);
	ASSERT_TRUE(::SetEnvironmentVariableW(kEnvironmentSecret, nullptr));
	EXPECT_EQ(EBoundedProcessStatus::Succeeded, result.Status());
	EXPECT_EQ(0, result.ExitCode());
}
