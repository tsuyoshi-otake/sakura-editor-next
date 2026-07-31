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
#include "extension/openvsx/OpenVsxProductionClient.h"

#include <chrono>

namespace {

using config::BuiltinConfigurationDescriptors;
using config::CConfigurationNetworkPolicy;
using config::CConfigurationService;
using config::ConfigurationNetworkPolicySnapshot;
using config::ConfigurationSource;
using config::ConfigurationTarget;
using config::ConfigurationValue;
using config::EConfigurationOutcome;
using config::EConfigurationScope;
using extension::openvsx::BuildOpenVsxProductionRequestPolicy;
using extension::openvsx::CreateOpenVsxProductionClient;
using extension::openvsx::EOpenVsxProductionClientOutcome;

constexpr std::wstring_view kProfileId = L"0123456789abcdef0123456789abcdef";

ConfigurationTarget ProfileTarget()
{
	return { std::wstring(kProfileId), std::nullopt, std::nullopt, std::nullopt };
}

ConfigurationSource ProfileSource()
{
	return { EConfigurationScope::Profile, ProfileTarget(), "openvsx-production-client-test", 0 };
}

} // namespace

TEST(OpenVsxProductionClient, RejectsInvalidProfileAndUnsupportedTlsConfigurationBeforeProducingAClient)
{
	CConfigurationService service(BuiltinConfigurationDescriptors());

	const auto invalidProfile = CreateOpenVsxProductionClient(service, L"not-a-canonical-profile");
	EXPECT_EQ(EOpenVsxProductionClientOutcome::InvalidProfileId, invalidProfile.outcome);
	EXPECT_FALSE(invalidProfile.client);
	EXPECT_EQ(std::string::npos, invalidProfile.diagnostic.find("not-a-canonical-profile"));

	ASSERT_EQ(EConfigurationOutcome::Applied, service.Update({ ProfileSource(), "http.proxyStrictSSL",
		ConfigurationValue(false), "unsupported-tls", 0 }).outcome);
	const auto unsupportedTls = CreateOpenVsxProductionClient(service, std::wstring(kProfileId));
	EXPECT_EQ(EOpenVsxProductionClientOutcome::ConfigurationInvalid, unsupportedTls.outcome);
	EXPECT_FALSE(unsupportedTls.client);
	EXPECT_EQ(std::string::npos, unsupportedTls.diagnostic.find("proxy"));
}

TEST(OpenVsxProductionClient, ReportsUnavailableWhenTheConfigurationServiceDoesNotProvideNetworkDescriptors)
{
	CConfigurationService service({});
	const auto result = CreateOpenVsxProductionClient(service, std::wstring(kProfileId));

	EXPECT_EQ(EOpenVsxProductionClientOutcome::ConfigurationUnavailable, result.outcome);
	EXPECT_FALSE(result.client);
}

TEST(OpenVsxProductionClient, CapturesConfigurationSnapshotAndOutlivesConfigurationObjectsWithoutNetwork)
{
	std::shared_ptr<extension::openvsx::IOpenVsxRegistryClient> client;
	{
		CConfigurationService service(BuiltinConfigurationDescriptors());
		const auto result = CreateOpenVsxProductionClient(service, std::wstring(kProfileId));
		ASSERT_EQ(EOpenVsxProductionClientOutcome::Ready, result.outcome);
		ASSERT_TRUE(result.client);
		client = result.client;
	}

	// No registry method is called: lifetime is verified without starting network work.
	EXPECT_TRUE(client);
	EXPECT_EQ(extension::openvsx::EOpenVsxRequestOutcome::NotRequested,
		client->FetchOptionalSha256(std::nullopt).status.outcome);
}

TEST(OpenVsxProductionClient, BuildsBoundedEndpointPoliciesFromTheSingleNetworkPolicySnapshot)
{
	CConfigurationService service(BuiltinConfigurationDescriptors());
	ASSERT_EQ(EConfigurationOutcome::Applied, service.ReplaceSource({ ProfileSource(), {
		{ "http.timeout", ConfigurationValue(45000) },
		{ "http.proxySupport", ConfigurationValue(L"override") },
	}, "policy-limits", 0 }).outcome);
	CConfigurationNetworkPolicy networkPolicy(service, std::wstring(kProfileId));
	const auto snapshot = networkPolicy.Snapshot();
	ASSERT_TRUE(snapshot);
	ASSERT_TRUE(snapshot.snapshot.has_value());

	const auto policy = BuildOpenVsxProductionRequestPolicy(*snapshot.snapshot);
	EXPECT_EQ(platform::request::EProxySupport::Override, policy.proxySupport);
	EXPECT_EQ(std::chrono::seconds(45), *policy.searchLimits.timeout);
	EXPECT_EQ(3u, *policy.searchLimits.maxRedirects);
	EXPECT_EQ(64u * 1024u, policy.searchLimits.maxResponseHeaderBytes);
	EXPECT_EQ(8u * 1024u * 1024u, policy.searchLimits.maxResponseBodyBytes);
	EXPECT_EQ(64u * 1024u, policy.sha256Limits.maxResponseBodyBytes);
	EXPECT_EQ(512u * 1024u * 1024u, policy.vsixLimits.maxResponseBodyBytes);
}
