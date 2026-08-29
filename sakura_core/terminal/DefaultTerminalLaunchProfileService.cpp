/*! @file */
#include "StdAfx.h"
#include "terminal/DefaultTerminalLaunchProfileService.h"

#include <utility>

namespace terminal {

struct CDefaultTerminalLaunchProfileService::Impl final {
	NativePowerShellLocatorProvider provider;
	PowerShellLocator locator{ provider };
	TerminalProfileCatalog catalog{ locator };
};

CDefaultTerminalLaunchProfileService::CDefaultTerminalLaunchProfileService()
	: m_impl(std::make_unique<Impl>())
{
}

CDefaultTerminalLaunchProfileService::~CDefaultTerminalLaunchProfileService() = default;

std::optional<TerminalLaunchOptions> CDefaultTerminalLaunchProfileService::Resolve(
	const TerminalSize size, const std::wstring_view workingDirectory)
{
	const auto profile = m_impl->catalog.ResolveProfile();
	if( !profile ) return std::nullopt;
	TerminalLaunchOptions options;
	options.executablePath = profile->path;
	options.arguments.emplace_back(L"-NoLogo");
	options.workingDirectory.assign(workingDirectory);
	options.initialSize = size;
	return options;
}

void CDefaultTerminalLaunchProfileService::Redetect() noexcept
{
	m_impl->catalog.Redetect();
}

std::vector<TerminalProfile> CDefaultTerminalLaunchProfileService::Profiles()
{
	return m_impl->catalog.Profiles();
}

std::optional<TerminalProfile> CDefaultTerminalLaunchProfileService::SelectedProfile()
{
	return m_impl->catalog.ResolveProfile();
}

bool CDefaultTerminalLaunchProfileService::SelectProfile(const std::wstring_view path)
{
	return m_impl->catalog.SelectProfile(path);
}

} // namespace terminal
