#include "QuantReplayFixture.hpp"

namespace {
const cxet::testing::Case cases[]{
#include "QuantReplayMetricsCases.ipp"
#include "QuantReplayAdmissionCases.ipp"
#include "QuantReplayAccountingCases.ipp"
#include "QuantReplayQuantityCases.ipp"
#include "QuantReplayPrivateDeliveryCases.ipp"
#include "QuantReplayEvidenceCases.ipp"
#include "QuantReplayExecutionAuthorityCases.ipp"
};
}
int main(int argc, char** argv) { return cxet::testing::runCases(argc, argv, cases); }
