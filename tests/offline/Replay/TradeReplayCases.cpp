#include "TradeReplayFixture.hpp"

namespace {
const cxet::testing::Case cases[]{
#include "TradeReplayAdmissionCases.ipp"
#include "TradeReplayHealthCases.ipp"
#include "TradeReplayLifecycleCases.ipp"
#include "TradeReplayUniverseCases.ipp"
#include "TradeReplayStreamingCases.ipp"
#include "TradeReplayFreshnessCases.ipp"
#include "TradeReplayIdentityConversionCases.ipp"
#include "TradeReplayNativeEvidenceCases.ipp"
};
}

int main(int argc, char** argv) { return cxet::testing::runCases(argc, argv, cases); }
