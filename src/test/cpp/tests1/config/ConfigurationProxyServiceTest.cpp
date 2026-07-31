/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include "config/BuiltinConfigurationDescriptors.h"
#include "config/CConfigurationNetworkPolicy.h"
#include "config/CConfigurationProxyService.h"
#include "config/CConfigurationService.h"

namespace {

using config::BuiltinConfigurationDescriptors;
using config::CConfigurationNetworkPolicy;
using config::CConfigurationProxyService;
using config::CConfigurationService;
using config::ConfigurationEntry;
using config::ConfigurationNetworkPolicySnapshot;
using config::ConfigurationReplaceSource;
using config::ConfigurationSource;
using config::ConfigurationTarget;
using config::ConfigurationValue;
using config::ESystemProxyResolutionOutcome;
using config::ISystemProxyResolver;
using config::SystemProxyResolution;
using platform::request::EProxyMode;
using platform::request::EProxySelectionOutcome;
using platform::request::EProxySupport;
using platform::request::IRequestCancellation;
using platform::request::ProxyRequest;
using platform::request::ProxySelection;

constexpr std::wstring_view kProfileId = L"0123456789abcdef0123456789abcdef";

ConfigurationTarget Target()
{
	return { std::wstring(kProfileId), std::nullopt, std::nullopt, std::nullopt };
}

ConfigurationSource Source()
{
	return { config::EConfigurationScope::Profile, Target(), "proxy-test", 0 };
}

class FakeResolver final : public ISystemProxyResolver {
public:
	SystemProxyResolution Resolve(
		const ProxyRequest&,
		std::optional<std::chrono::steady_clock::time_point>,
		const IRequestCancellation*
	) override
	{
		++calls;
		return result;
	}

	int calls = 0;
	SystemProxyResolution result { ESystemProxyResolutionOutcome::Unavailable, {} };
};

class Cancelled final : public IRequestCancellation {
public:
	bool IsCancellationRequested() const noexcept override { return true; }
};

struct Fixture {
	Fixture()
		: policy(service, std::wstring(kProfileId))
		, proxy(policy, resolver)
	{
	}

	void Configure(std::wstring support, std::wstring proxyUrl = L"", ConfigurationValue::Array noProxy = {})
	{
		ASSERT_EQ(config::EConfigurationOutcome::Applied, service.ReplaceSource({ Source(), {
			{ "http.proxySupport", ConfigurationValue(std::move(support)) },
			{ "http.proxy", ConfigurationValue(std::move(proxyUrl)) },
			{ "http.noProxy", ConfigurationValue(std::move(noProxy)) },
		}, "proxy-test-" + std::to_string(sourceRevision), sourceRevision }).outcome);
		++sourceRevision;
	}

	CConfigurationService service { BuiltinConfigurationDescriptors() };
	CConfigurationNetworkPolicy policy;
	FakeResolver resolver;
	CConfigurationProxyService proxy;
	std::uint64_t sourceRevision = 0;
};

ProxyRequest Request(EProxySupport support = EProxySupport::Fallback, std::wstring url = L"https://api.example.test/resource")
{
	return { std::move(url), support };
}

ConfigurationNetworkPolicySnapshot Snapshot(
	EProxySupport support = EProxySupport::Fallback,
	std::optional<std::wstring> proxyUrl = std::nullopt,
	std::vector<std::wstring> noProxy = {}
)
{
	ConfigurationNetworkPolicySnapshot snapshot;
	snapshot.proxySupport = support;
	snapshot.proxyUrl = std::move(proxyUrl);
	snapshot.noProxy = std::move(noProxy);
	return snapshot;
}

} // namespace

TEST(ConfigurationProxyService, OffIsDirectAndDoesNotCallSystemResolver)
{
	Fixture fixture;
	fixture.Configure(L"off", L"http://manual.example.test:8080");

	const auto selected = fixture.proxy.SelectProxy(Request(EProxySupport::Off), std::nullopt, nullptr);
	EXPECT_EQ(EProxySelectionOutcome::Selected, selected.outcome);
	EXPECT_EQ(EProxyMode::Direct, selected.mode);
	EXPECT_FALSE(selected.bypassed);
	EXPECT_EQ(0, fixture.resolver.calls);
}

TEST(ConfigurationProxyService, ExplicitOffDoesNotRequireNetworkPolicyDescriptors)
{
	CConfigurationService service({});
	CConfigurationNetworkPolicy policy(service, std::wstring(kProfileId));
	FakeResolver resolver;
	CConfigurationProxyService proxy(policy, resolver);

	const auto selected = proxy.SelectProxy(Request(EProxySupport::Off), std::nullopt, nullptr);

	EXPECT_EQ(EProxySelectionOutcome::Selected, selected.outcome);
	EXPECT_EQ(EProxyMode::Direct, selected.mode);
	EXPECT_EQ(0, resolver.calls);
}

TEST(ConfigurationProxyService, OnAndOverrideUseManualBeforeSystem)
{
	for (const auto support : { EProxySupport::On, EProxySupport::Override }) {
		Fixture fixture;
		fixture.Configure(L"fallback", L"http://manual.example.test:8080");
		fixture.resolver.result = { ESystemProxyResolutionOutcome::Selected,
			{ EProxyMode::Direct, std::nullopt, false, EProxySelectionOutcome::Selected } };

		const auto selected = fixture.proxy.SelectProxy(Request(support), std::nullopt, nullptr);
		EXPECT_EQ(EProxySelectionOutcome::Selected, selected.outcome);
		EXPECT_EQ(EProxyMode::Manual, selected.mode);
		ASSERT_TRUE(selected.proxyUrl.has_value());
		EXPECT_EQ(L"http://manual.example.test:8080", *selected.proxyUrl);
		EXPECT_EQ(0, fixture.resolver.calls);
	}
}

TEST(ConfigurationProxyService, FallbackUsesSystemThenManualOnlyWhenUnavailable)
{
	Fixture fixture;
	fixture.Configure(L"fallback", L"http://manual.example.test:8080");
	fixture.resolver.result = { ESystemProxyResolutionOutcome::Selected,
		{ EProxyMode::Direct, std::nullopt, false, EProxySelectionOutcome::Selected } };
	EXPECT_EQ(EProxyMode::Direct, fixture.proxy.SelectProxy(Request(), std::nullopt, nullptr).mode);
	EXPECT_EQ(1, fixture.resolver.calls);

	fixture.resolver.result = { ESystemProxyResolutionOutcome::Unavailable, {} };
	const auto fallback = fixture.proxy.SelectProxy(Request(), std::nullopt, nullptr);
	EXPECT_EQ(EProxyMode::Manual, fallback.mode);
	EXPECT_EQ(2, fixture.resolver.calls);
}

TEST(ConfigurationProxyService, AmbientFallbackInheritsTheProfileProxySupportMode)
{
	Fixture fixture;
	fixture.Configure(L"override", L"http://manual.example.test:8080");
	fixture.resolver.result = { ESystemProxyResolutionOutcome::Selected,
		{ EProxyMode::Direct, std::nullopt, false, EProxySelectionOutcome::Selected } };

	const auto selected = fixture.proxy.SelectProxy(Request(EProxySupport::Fallback), std::nullopt, nullptr);
	EXPECT_EQ(EProxyMode::Manual, selected.mode);
	EXPECT_EQ(0, fixture.resolver.calls);
}

TEST(ConfigurationProxyService, RequiresSystemWhenNoManualPolicyExists)
{
	Fixture fixture;
	fixture.Configure(L"on");
	fixture.resolver.result = { ESystemProxyResolutionOutcome::Unavailable, {} };

	for (const auto support : { EProxySupport::On, EProxySupport::Override, EProxySupport::Fallback }) {
		const auto selected = fixture.proxy.SelectProxy(Request(support), std::nullopt, nullptr);
		EXPECT_EQ(EProxySelectionOutcome::UnsupportedPolicy, selected.outcome);
	}
	EXPECT_EQ(3, fixture.resolver.calls);
}

TEST(ConfigurationProxyService, MatchesNoProxyVariantsBeforeAnyResolver)
{
	Fixture fixture;
	fixture.Configure(L"fallback", L"http://manual.example.test:8080", {
		ConfigurationValue(L"localhost"),
		ConfigurationValue(L".example.test"),
		ConfigurationValue(L"*.wild.test"),
		ConfigurationValue(L"api.port.test:8443"),
		ConfigurationValue(L"127.0.0.1"),
		ConfigurationValue(L"[::1]") });

	for (const auto& url : { L"https://localhost/", L"https://example.test/", L"https://child.example.test/",
		L"https://a.wild.test/", L"https://api.port.test:8443/", L"http://127.0.0.1/", L"http://[::1]/" }) {
		const auto selected = fixture.proxy.SelectProxy(Request(EProxySupport::Fallback, url), std::nullopt, nullptr);
		EXPECT_EQ(EProxyMode::Direct, selected.mode);
		EXPECT_TRUE(selected.bypassed);
	}
	EXPECT_EQ(0, fixture.resolver.calls);
}

TEST(ConfigurationProxyService, GlobalNoProxyBypassesEverything)
{
	Fixture fixture;
	fixture.Configure(L"fallback", L"http://manual.example.test:8080", { ConfigurationValue(L"*") });
	const auto selected = fixture.proxy.SelectProxy(Request(), std::nullopt, nullptr);
	EXPECT_EQ(EProxyMode::Direct, selected.mode);
	EXPECT_TRUE(selected.bypassed);
	EXPECT_EQ(0, fixture.resolver.calls);
}

TEST(ConfigurationProxyService, CancellationDeadlineAndInvalidPatternsReturnOneUnsupportedTerminalResult)
{
	Fixture fixture;
	fixture.Configure(L"fallback", L"http://manual.example.test:8080");
	Cancelled cancelled;
	EXPECT_EQ(EProxySelectionOutcome::UnsupportedPolicy,
		fixture.proxy.SelectProxy(Request(), std::nullopt, &cancelled).outcome);
	EXPECT_EQ(EProxySelectionOutcome::UnsupportedPolicy,
		fixture.proxy.SelectProxy(Request(), std::chrono::steady_clock::now() - std::chrono::milliseconds(1), nullptr).outcome);
	EXPECT_EQ(0, fixture.resolver.calls);

	fixture.Configure(L"fallback", L"http://manual.example.test:8080", { ConfigurationValue(L"private.example.test/path") });
	const auto malformed = fixture.proxy.SelectProxy(Request(), std::nullopt, nullptr);
	EXPECT_EQ(EProxySelectionOutcome::UnsupportedPolicy, malformed.outcome);
	EXPECT_FALSE(malformed.proxyUrl.has_value());
	EXPECT_EQ(0, fixture.resolver.calls);
}

TEST(ConfigurationProxyService, DoesNotExposeEndpointValuesInTerminalResult)
{
	Fixture fixture;
	fixture.Configure(L"fallback", L"http://manual.example.test:8080", { ConfigurationValue(L"bad.example.test/path") });
	const auto result = fixture.proxy.SelectProxy(
		Request(EProxySupport::Fallback, L"https://secret.example.test/path"), std::nullopt, nullptr);

	EXPECT_EQ(EProxySelectionOutcome::UnsupportedPolicy, result.outcome);
	EXPECT_FALSE(result.proxyUrl.has_value());
	EXPECT_FALSE(result.bypassed);
}

TEST(ConfigurationProxyService, DetachedSnapshotRemainsUsableAfterConfigurationPolicyIsReleased)
{
	FakeResolver resolver;
	std::optional<CConfigurationProxyService> detached;
	{
		CConfigurationService service { BuiltinConfigurationDescriptors() };
		CConfigurationNetworkPolicy policy(service, std::wstring(kProfileId));
		ASSERT_EQ(config::EConfigurationOutcome::Applied, service.ReplaceSource({ Source(), {
			{ "http.proxySupport", ConfigurationValue(L"on") },
			{ "http.proxy", ConfigurationValue(L"http://manual.example.test:8080") },
			{ "http.noProxy", ConfigurationValue(ConfigurationValue::Array {}) },
		}, "detached-policy", 0 }).outcome);
		auto policySnapshot = policy.Snapshot();
		ASSERT_TRUE(policySnapshot);
		ASSERT_TRUE(policySnapshot.snapshot.has_value());
		detached.emplace(std::move(*policySnapshot.snapshot), resolver);
	}

	const auto selected = detached->SelectProxy(Request(), std::nullopt, nullptr);
	EXPECT_EQ(EProxySelectionOutcome::Selected, selected.outcome);
	EXPECT_EQ(EProxyMode::Manual, selected.mode);
	ASSERT_TRUE(selected.proxyUrl.has_value());
	EXPECT_EQ(L"http://manual.example.test:8080", *selected.proxyUrl);
	EXPECT_EQ(0, resolver.calls);
}

TEST(ConfigurationProxyService, DetachedSnapshotHonorsNoProxyBeforeManualOrSystemResolution)
{
	FakeResolver resolver;
	resolver.result = { ESystemProxyResolutionOutcome::Selected,
		{ EProxyMode::Manual, L"http://system.example.test:8080", false, EProxySelectionOutcome::Selected } };
	CConfigurationProxyService proxy(Snapshot(EProxySupport::Fallback,
		L"http://manual.example.test:8080", { L".example.test" }), resolver);

	const auto selected = proxy.SelectProxy(Request(EProxySupport::Fallback, L"https://internal.example.test/path"), std::nullopt, nullptr);
	EXPECT_EQ(EProxySelectionOutcome::Selected, selected.outcome);
	EXPECT_EQ(EProxyMode::Direct, selected.mode);
	EXPECT_TRUE(selected.bypassed);
	EXPECT_EQ(0, resolver.calls);
}

TEST(ConfigurationProxyService, DetachedSnapshotSelectsManualOrSystemUsingTheExistingModeRules)
{
	FakeResolver resolver;
	resolver.result = { ESystemProxyResolutionOutcome::Selected,
		{ EProxyMode::Manual, L"http://system.example.test:8080", false, EProxySelectionOutcome::Selected } };
	CConfigurationProxyService manual(Snapshot(EProxySupport::On, L"http://manual.example.test:8080"), resolver);
	const auto manualSelected = manual.SelectProxy(Request(), std::nullopt, nullptr);
	EXPECT_EQ(EProxyMode::Manual, manualSelected.mode);
	ASSERT_TRUE(manualSelected.proxyUrl.has_value());
	EXPECT_EQ(L"http://manual.example.test:8080", *manualSelected.proxyUrl);
	EXPECT_EQ(0, resolver.calls);

	CConfigurationProxyService system(Snapshot(EProxySupport::Fallback), resolver);
	const auto systemSelected = system.SelectProxy(Request(), std::nullopt, nullptr);
	EXPECT_EQ(EProxySelectionOutcome::Selected, systemSelected.outcome);
	EXPECT_EQ(EProxyMode::Manual, systemSelected.mode);
	ASSERT_TRUE(systemSelected.proxyUrl.has_value());
	EXPECT_EQ(L"http://system.example.test:8080", *systemSelected.proxyUrl);
	EXPECT_EQ(1, resolver.calls);
}

TEST(ConfigurationProxyService, DetachedSnapshotPreservesOffCancellationAndDeadlineTerminalBehavior)
{
	FakeResolver resolver;
	CConfigurationProxyService proxy(Snapshot(EProxySupport::Fallback, L"http://manual.example.test:8080"), resolver);
	CConfigurationProxyService policyOff(Snapshot(EProxySupport::Off), resolver);
	Cancelled cancelled;

	EXPECT_EQ(EProxySelectionOutcome::Selected,
		proxy.SelectProxy(Request(EProxySupport::Off), std::nullopt, &cancelled).outcome);
	EXPECT_EQ(EProxyMode::Direct,
		proxy.SelectProxy(Request(EProxySupport::Off), std::chrono::steady_clock::now() - std::chrono::milliseconds(1), nullptr).mode);
	EXPECT_EQ(EProxyMode::Direct, policyOff.SelectProxy(Request(), std::nullopt, nullptr).mode);
	EXPECT_EQ(EProxySelectionOutcome::UnsupportedPolicy,
		proxy.SelectProxy(Request(), std::nullopt, &cancelled).outcome);
	EXPECT_EQ(EProxySelectionOutcome::UnsupportedPolicy,
		proxy.SelectProxy(Request(), std::chrono::steady_clock::now() - std::chrono::milliseconds(1), nullptr).outcome);
	EXPECT_EQ(0, resolver.calls);
}
