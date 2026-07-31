/*! @file */
/*
	Copyright (C) 2026, Sakura Editor Organization

	SPDX-License-Identifier: Zlib
*/
#include "pch.h"

#include <gtest/gtest.h>

#include "agent/CLoadAgent.h"
#include "doc/CDocListener.h"

#include <stdexcept>

namespace {

class FinalLoadListener final : public CDocListener {
public:
	FinalLoadListener(CDocSubject& subject, ELoadFinalizationStatus result, bool throws = false)
		: CDocListener(&subject)
		, m_result(result)
		, m_throws(throws)
	{
	}

	ELoadFinalizationStatus OnFinalLoad(ELoadResult result) override
	{
		++calls;
		observed = result;
		if (m_throws) throw std::runtime_error("final load failure");
		return m_result;
	}

	int calls = 0;
	ELoadResult observed = LOADED_NOIMPLEMENT;

private:
	ELoadFinalizationStatus m_result;
	bool m_throws;
};

TEST(LoadFinalization, MapsEveryReadTerminalWithoutPromotingFailure)
{
	EXPECT_EQ(LOADED_OK, CLoadAgent::ToLoadResult(RESULT_COMPLETE));
	EXPECT_EQ(LOADED_LOSESOME, CLoadAgent::ToLoadResult(RESULT_LOSESOME));
	EXPECT_EQ(LOADED_FAILURE, CLoadAgent::ToLoadResult(RESULT_FAILURE));
}

TEST(LoadFinalization, CallsEveryListenerAndAggregatesFailureAndExceptions)
{
	CDocSubject subject;
	FinalLoadListener succeeds(subject, ELoadFinalizationStatus::Succeeded);
	FinalLoadListener fails(subject, ELoadFinalizationStatus::Failed);
	FinalLoadListener throws(subject, ELoadFinalizationStatus::Succeeded, true);
	FinalLoadListener afterException(subject, ELoadFinalizationStatus::Succeeded);

	EXPECT_EQ(ELoadFinalizationStatus::Failed, subject.NotifyFinalLoad(LOADED_LOSESOME));
	EXPECT_EQ(1, succeeds.calls);
	EXPECT_EQ(1, fails.calls);
	EXPECT_EQ(1, throws.calls);
	EXPECT_EQ(1, afterException.calls);
	EXPECT_EQ(LOADED_LOSESOME, afterException.observed);
}

} // namespace
