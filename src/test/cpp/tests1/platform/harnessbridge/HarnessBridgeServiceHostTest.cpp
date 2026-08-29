#include "pch.h"

#include <sakura/harnessbridge/HarnessBridgeServiceHost.h>
#include <sakura/harnessbridge/HarnessBridgeSecurity.h>

namespace platform::harnessbridge {
namespace {

TEST(HarnessBridgeServiceHost, RejectsMalformedDescriptorBeforeOpeningEndpoint)
{
	CHarnessBridgeServiceHost host;
	HarnessEditorEndpointDescriptor descriptor;
	std::wstring diagnostic;
	EXPECT_EQ(EHarnessBridgeHostStartResult::InvalidDescriptor, host.Start(descriptor, diagnostic));
	EXPECT_EQ(EHarnessBridgeHostState::Stopped, host.State());
}

TEST(HarnessBridgeServiceHost, PublishesAcceptingAndStopsInReverseOrder)
{
	HarnessEditorEndpointDescriptor descriptor;
	descriptor.endpointHash = std::wstring(64, L'b');
	descriptor.profileId = L"unit-profile";
	descriptor.editorId[0] = 1;
	descriptor.bridgeId[0] = 2;
	descriptor.profileGeneration = 1;
	descriptor.bridgeEpoch = 1;
	descriptor.runtimeGeneration = 1;
	descriptor.serverPid = ::GetCurrentProcessId();
	FILETIME creation{}, exitTime{}, kernel{}, user{};
	ASSERT_TRUE(::GetProcessTimes(::GetCurrentProcess(), &creation, &exitTime, &kernel, &user));
	ULARGE_INTEGER creationValue{};
	creationValue.LowPart = creation.dwLowDateTime;
	creationValue.HighPart = creation.dwHighDateTime;
	descriptor.serverProcessCreationTime = creationValue.QuadPart;
	descriptor.pipeName = BuildHarnessPipeName(descriptor.endpointHash);
	CHarnessBridgeServiceHost host;
	std::wstring diagnostic;
	ASSERT_EQ(EHarnessBridgeHostStartResult::Started, host.Start(descriptor, diagnostic)) << diagnostic.c_str();
	EXPECT_EQ(EHarnessBridgeHostState::Accepting, host.State());
	host.Stop();
	EXPECT_EQ(EHarnessBridgeHostState::Stopped, host.State());
}

} // namespace
} // namespace platform::harnessbridge
