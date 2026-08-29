#include "pch.h"

#include <sakura/harnessbridge/HarnessBridgeEnvironment.h>

namespace platform::harnessbridge {
namespace {

TEST(HarnessBridgeEnvironment, RoundTripsExactEndpointTargetAndCapability)
{
	const std::wstring hash(64, L'a');
	const auto endpoint = EncodeHarnessEndpointEnvironment(hash);
	ASSERT_TRUE(endpoint);
	EXPECT_EQ(std::optional{ hash }, DecodeHarnessEndpointEnvironment(*endpoint));

	HarnessBridgeTargetDescriptor target;
	target.profileId = "default";
	target.profileGeneration = 2;
	target.editorId[0] = 1;
	target.bridgeEpoch = 3;
	target.runtimeGeneration = 4;
	target.instanceGeneration = 5;
	target.sessionId = 6;
	target.windowId = 7;
	target.paneId = 8;
	target.instanceId = 9;
	const auto encodedTarget = EncodeHarnessTargetEnvironment(target);
	ASSERT_TRUE(encodedTarget);
	EXPECT_EQ(target, DecodeHarnessTargetEnvironment(*encodedTarget));

	HarnessCapabilityCredential credential;
	credential.id.value[0] = 10;
	credential.secret[0] = 11;
	credential.grants = EHarnessGrant::Message | EHarnessGrant::ConsoleRead;
	const auto encodedCredential = EncodeHarnessCapabilityEnvironment(credential);
	ASSERT_TRUE(encodedCredential);
	const auto decodedCredential = DecodeHarnessCapabilityEnvironment(*encodedCredential);
	ASSERT_TRUE(decodedCredential);
	EXPECT_EQ(credential.id, decodedCredential->id);
	EXPECT_EQ(credential.secret, decodedCredential->secret);
	EXPECT_EQ(credential.grants, decodedCredential->grants);
}

TEST(HarnessBridgeEnvironment, RejectsMalformedAndIncompleteValues)
{
	EXPECT_FALSE(DecodeHarnessEndpointEnvironment(L"she1.not-a-hash"));
	EXPECT_FALSE(DecodeHarnessTargetEnvironment(L"sht1.A"));
	EXPECT_FALSE(DecodeHarnessCapabilityEnvironment(L"shc1.AAAA="));

	HarnessBridgeTargetDescriptor target;
	target.profileId = "default";
	EXPECT_FALSE(EncodeHarnessTargetEnvironment(target));
	HarnessCapabilityCredential credential;
	EXPECT_FALSE(EncodeHarnessCapabilityEnvironment(credential));
}

} // namespace
} // namespace platform::harnessbridge
