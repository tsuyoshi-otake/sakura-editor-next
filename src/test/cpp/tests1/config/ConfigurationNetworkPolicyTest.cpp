/*! @file */
/*
 * Copyright (C) 2026, Sakura Editor Organization
 *
 * SPDX-License-Identifier: Zlib
 */

#include "pch.h"

#include "config/BuiltinConfigurationDescriptors.h"
#include "config/CConfigurationNetworkPolicy.h"
#include "config/CConfigurationService.h"

namespace {

using config::BuiltinConfigurationDescriptors;
using config::CConfigurationNetworkPolicy;
using config::CConfigurationService;
using config::ConfigurationEntry;
using config::ConfigurationReplaceSource;
using config::ConfigurationSource;
using config::ConfigurationTarget;
using config::ConfigurationUpdate;
using config::ConfigurationValue;
using config::EConfigurationNetworkPolicyOutcome;
using config::EConfigurationOutcome;
using config::EConfigurationScope;

constexpr std::wstring_view kProfileId = L"0123456789abcdef0123456789abcdef";

ConfigurationTarget ProfileTarget()
{
	return { std::wstring(kProfileId), std::nullopt, std::nullopt, std::nullopt };
}

ConfigurationSource ProfileSource(std::string id = "settings-json")
{
	return { EConfigurationScope::Profile, ProfileTarget(), std::move(id), 0 };
}

ConfigurationTarget WorkspaceTarget()
{
	auto uri = platform::uri::Uri::Parse(L"file:///C:/network-test");
	EXPECT_TRUE(uri);
	return { std::wstring(kProfileId), std::move(*uri.value), std::nullopt, std::nullopt };
}

CConfigurationService Service()
{
	return CConfigurationService(BuiltinConfigurationDescriptors());
}

} // namespace

TEST(ConfigurationNetworkPolicy, ReturnsSafeDefaultsForOneProfile)
{
	auto service = Service();
	CConfigurationNetworkPolicy policy(service, std::wstring(kProfileId));
	const auto result = policy.Snapshot();

	ASSERT_EQ(EConfigurationNetworkPolicyOutcome::Ready, result.outcome);
	ASSERT_TRUE(result.snapshot.has_value());
	EXPECT_FALSE(result.snapshot->proxyUrl.has_value());
	EXPECT_EQ(platform::request::EProxySupport::Fallback, result.snapshot->proxySupport);
	EXPECT_TRUE(result.snapshot->noProxy.empty());
	EXPECT_TRUE(result.snapshot->proxyStrictSSL);
	EXPECT_TRUE(result.snapshot->systemCertificates);
	ASSERT_TRUE(result.snapshot->requestLimits.timeout.has_value());
	EXPECT_EQ(std::chrono::seconds(30), *result.snapshot->requestLimits.timeout);
}

// Regression test: the Default profile's selected user-data identity is the literal
// L"default", which is opaque-valid (`[A-Za-z0-9_-]`, 1..128 chars) but never
// 32-lowercase-hex canonical. The guard must accept it, not reject it as an
// unsupported profile authority target.
TEST(ConfigurationNetworkPolicy, AcceptsTheDefaultUserDataProfileIdentity)
{
	auto service = Service();
	CConfigurationNetworkPolicy policy(service, std::wstring(L"default"));
	const auto result = policy.Snapshot();

	ASSERT_EQ(EConfigurationNetworkPolicyOutcome::Ready, result.outcome);
	ASSERT_TRUE(result.snapshot.has_value());
}

TEST(ConfigurationNetworkPolicy, UsesProfileOverridesAndNeverReadsWorkspaceSource)
{
	auto service = Service();
	const auto source = ProfileSource();
	ASSERT_EQ(EConfigurationOutcome::Applied, service.ReplaceSource({ source, {
		{ "http.proxy", ConfigurationValue(L"http://proxy.example.test:8080") },
		{ "http.proxySupport", ConfigurationValue(L"override") },
		{ "http.noProxy", ConfigurationValue(ConfigurationValue::Array {
			ConfigurationValue(L"localhost"), ConfigurationValue(L"*.example.test") }) },
		{ "http.proxyStrictSSL", ConfigurationValue(false) },
		{ "http.systemCertificates", ConfigurationValue(false) },
		{ "http.timeout", ConfigurationValue(45000) },
	}, "profile-settings", 0 }).outcome);
	CConfigurationNetworkPolicy policy(service, std::wstring(kProfileId));
	const auto result = policy.Snapshot();

	ASSERT_TRUE(result);
	ASSERT_TRUE(result.snapshot->proxyUrl.has_value());
	EXPECT_EQ(L"http://proxy.example.test:8080", *result.snapshot->proxyUrl);
	EXPECT_EQ(platform::request::EProxySupport::Override, result.snapshot->proxySupport);
	ASSERT_EQ(2U, result.snapshot->noProxy.size());
	EXPECT_FALSE(result.snapshot->proxyStrictSSL);
	EXPECT_FALSE(result.snapshot->systemCertificates);
	ASSERT_TRUE(result.snapshot->requestLimits.timeout.has_value());
	EXPECT_EQ(std::chrono::seconds(45), *result.snapshot->requestLimits.timeout);

	const auto workspaceUpdate = service.Update({
		{ EConfigurationScope::Workspace, WorkspaceTarget(), "workspace-settings", 0 },
		"http.proxy", ConfigurationValue(L"http://workspace.example.test"), "workspace-write" });
	EXPECT_EQ(EConfigurationOutcome::InvalidScope, workspaceUpdate.outcome);
	EXPECT_EQ(EConfigurationNetworkPolicyOutcome::Ready, policy.Snapshot().outcome);
}

TEST(ConfigurationNetworkPolicy, RejectsInvalidValuesAtomicallyAndPreservesLastValidPolicy)
{
	auto service = Service();
	const auto source = ProfileSource();
	ASSERT_EQ(EConfigurationOutcome::Applied, service.ReplaceSource({ source,
		{ { "http.timeout", ConfigurationValue(40000) }, { "http.proxySupport", ConfigurationValue(L"on") } },
		"initial", 0 }).outcome);
	CConfigurationNetworkPolicy policy(service, std::wstring(kProfileId));
	ASSERT_TRUE(policy.Snapshot());
	int notifications = 0;
	auto subscription = service.Subscribe([&](const auto&) { ++notifications; });

	EXPECT_EQ(EConfigurationOutcome::InvalidValue, service.ReplaceSource({ source, {
		{ "http.timeout", ConfigurationValue(100) },
		{ "http.proxySupport", ConfigurationValue(L"unknown") },
	}, "invalid-replacement", 1 }).outcome);
	EXPECT_EQ(0, notifications);
	const auto result = policy.Snapshot();
	ASSERT_TRUE(result);
	ASSERT_TRUE(result.snapshot->requestLimits.timeout.has_value());
	EXPECT_EQ(std::chrono::seconds(40), *result.snapshot->requestLimits.timeout);
	EXPECT_EQ(platform::request::EProxySupport::On, result.snapshot->proxySupport);

	EXPECT_EQ(EConfigurationOutcome::InvalidValue, service.Update({ source, "http.proxy",
		ConfigurationValue(true), "wrong-type" }).outcome);
	EXPECT_EQ(EConfigurationOutcome::InvalidValue, service.Update({ source, "http.noProxy",
		ConfigurationValue(ConfigurationValue::Array { ConfigurationValue(7) }), "wrong-array" }).outcome);
	EXPECT_EQ(0, notifications);
}

TEST(ConfigurationNetworkPolicy, RejectsUnsafeConfiguredUrlsWithoutLeakingTheirValues)
{
	auto service = Service();
	const auto source = ProfileSource();
	CConfigurationNetworkPolicy policy(service, std::wstring(kProfileId));

	ASSERT_EQ(EConfigurationOutcome::Applied, service.Update({ source, "http.proxy",
		ConfigurationValue(L"file:///C:/private-proxy"), "unsafe-proxy" }).outcome);
	const auto proxyResult = policy.Snapshot();
	EXPECT_EQ(EConfigurationNetworkPolicyOutcome::InvalidConfiguration, proxyResult.outcome);
	EXPECT_EQ(std::string::npos, proxyResult.diagnostic.find("private-proxy"));

	ASSERT_EQ(EConfigurationOutcome::Applied, service.Update({ source, "http.proxy",
		ConfigurationValue(L"http://proxy.example.test/private-path"), "unsafe-proxy-path", 1 }).outcome);
	const auto proxyPathResult = policy.Snapshot();
	EXPECT_EQ(EConfigurationNetworkPolicyOutcome::InvalidConfiguration, proxyPathResult.outcome);
	EXPECT_EQ(std::string::npos, proxyPathResult.diagnostic.find("private-path"));

}

TEST(ConfigurationNetworkPolicy, RejectsAnInvalidUserDataProfileTarget)
{
	auto service = Service();

	// A path separator would be dangerous if silently accepted as a resource-root
	// selector, and it is outside the opaque `[A-Za-z0-9_-]` identity alphabet.
	{
		CConfigurationNetworkPolicy policy(service, L"../escape");
		const auto result = policy.Snapshot();

		EXPECT_EQ(EConfigurationNetworkPolicyOutcome::Unsupported, result.outcome);
		EXPECT_FALSE(result.snapshot.has_value());
		EXPECT_EQ(std::string::npos, result.diagnostic.find("../escape"));
	}

	// The empty id is rejected: the identity alphabet requires 1..128 characters.
	{
		CConfigurationNetworkPolicy policy(service, L"");
		const auto result = policy.Snapshot();

		EXPECT_EQ(EConfigurationNetworkPolicyOutcome::Unsupported, result.outcome);
		EXPECT_FALSE(result.snapshot.has_value());
	}

	// An id one character past the 128-character bound is rejected.
	{
		CConfigurationNetworkPolicy policy(service, std::wstring(129, L'a'));
		const auto result = policy.Snapshot();

		EXPECT_EQ(EConfigurationNetworkPolicyOutcome::Unsupported, result.outcome);
		EXPECT_FALSE(result.snapshot.has_value());
	}
}

TEST(ConfigurationNetworkPolicy, KeepsUnknownEntriesLatentAndDoesNotDefineProxyAuthorization)
{
	auto service = Service();
	const auto source = ProfileSource();
	ASSERT_EQ(EConfigurationOutcome::Applied, service.ReplaceSource({ source, {
		{ "future.networkSetting", ConfigurationValue(L"retained") },
	}, "latent", 0 }).outcome);
	EXPECT_EQ(EConfigurationOutcome::InvalidKey,
		service.GetValue("future.networkSetting", ProfileTarget()).outcome);
	EXPECT_EQ(EConfigurationOutcome::InvalidKey,
		service.GetValue("http.proxyAuthorization", ProfileTarget()).outcome);

	for (const auto& descriptor : BuiltinConfigurationDescriptors()) {
		EXPECT_NE("http.proxyAuthorization", descriptor.key);
	}
}
