#include "polly/CodePreparation.h"
#include "polly/CodeGen/CodeGeneration.h"
#include "polly/DependenceInfo.h"
#include "polly/ScopPass.h"
#include "polly/Support/GICHelper.h"

#include "llvm/IR/PassManager.h"
#include "llvm/Support/ErrorHandling.h"

#include "isl/map.h"
#include "isl/space.h"

using namespace llvm;

namespace {

constexpr StringLiteral SystolicStrippedMarkerAttr =
    "morpher-systolic-stripped-map-marker";

isl::map scheduleMap(polly::ScopStmt &statement, StringRef range) {
  const unsigned dimensions = static_cast<unsigned>(
      statement.getDomain().dim(isl::dim::set).release());
  std::string text = "{ ";
  text += statement.getBaseName();
  text += "[";
  for (unsigned dimension = 0; dimension < dimensions; ++dimension) {
    if (dimension)
      text += ",";
    text += "i" + std::to_string(dimension);
  }
  text += "] -> [";
  text += range.str();
  text += "] }";

  isl_map *map =
      isl_map_read_from_str(statement.getIslCtx().get(), text.c_str());
  if (!map)
    report_fatal_error("Polly rejected an internally generated systolic schedule");

  isl_space *space = statement.getDomainSpace().release();
  map = isl_map_set_tuple_id(
      map, isl_dim_in, isl_space_get_tuple_id(space, isl_dim_set));
  for (isl_size parameter = 0;
       parameter < isl_space_dim(space, isl_dim_param); ++parameter) {
    isl_id *id = isl_space_get_dim_id(space, isl_dim_param, parameter);
    map = isl_map_set_dim_id(map, isl_dim_param, parameter, id);
  }
  isl_space_free(space);
  return isl::manage(map);
}

struct DistributeSystolicBetaPass
    : PassInfoMixin<DistributeSystolicBetaPass> {
  PreservedAnalyses run(polly::Scop &scop, polly::ScopAnalysisManager &manager,
                        polly::ScopStandardAnalysisResults &analyses,
                        polly::SPMUpdater &) {
    Function &function = scop.getFunction();
    if (!function.hasFnAttribute(SystolicStrippedMarkerAttr))
      return PreservedAnalyses::all();
    if (scop.getContext().dim(isl::dim::param).release() != 0)
      report_fatal_error(
          "targeted systolic distribution requires constant loop bounds");
    if (scop.getSize() != 2)
      report_fatal_error(
          "targeted systolic distribution requires exactly beta and contraction statements");

    polly::ScopStmt *beta = nullptr;
    polly::ScopStmt *contraction = nullptr;
    for (polly::ScopStmt &statement : scop) {
      const auto dimensions =
          statement.getDomain().dim(isl::dim::set).release();
      if (dimensions == 2 && !beta)
        beta = &statement;
      else if (dimensions == 3 && !contraction)
        contraction = &statement;
      else
        report_fatal_error(
            "targeted systolic distribution found an unexpected statement domain");
    }
    if (!beta || !contraction)
      report_fatal_error(
          "targeted systolic distribution could not identify beta and contraction statements");

    // Fail closed unless this is the precise source schedule we know how to
    // distribute: beta(i,j), followed by contraction(i,k,j).
    isl::map expectedBeta = scheduleMap(*beta, "i0,0,i1,0");
    isl::map expectedContraction =
        scheduleMap(*contraction, "i0,1,i1,i2");
    if (!beta->getSchedule().is_equal(expectedBeta) ||
        !contraction->getSchedule().is_equal(expectedContraction))
      report_fatal_error(
          "targeted systolic distribution requires the canonical i; beta(j); k,j source order");

    polly::Dependences::StatementToIslMapTy schedules;
    schedules[beta] = scheduleMap(*beta, "i0,i1,0,0");
    schedules[contraction] =
        scheduleMap(*contraction, "i0,i2,1,i1");
    const polly::Dependences &dependences =
        manager
            .getResult<polly::DependenceAnalysis>(scop, analyses)
            .getDependences(polly::Dependences::AL_Statement);
    if (!dependences.isValidSchedule(scop, schedules))
      report_fatal_error(
          "Polly rejected targeted systolic distribution as dependence-unsafe");

    isl::union_map schedule = isl::union_map::empty(scop.getIslCtx());
    schedule = schedule.unite(schedules[beta]);
    schedule = schedule.unite(schedules[contraction]);
    scop.setSchedule(schedule);
    scop.markAsOptimized();
    errs() << "Morpher/Polly distributed beta and canonicalized contraction "
              "order in "
           << function.getName() << "\n";
    return PreservedAnalyses::none();
  }
};

} // namespace

FunctionPassManager createMorpherSystolicPollyDistributePipeline() {
  FunctionPassManager FPM;
  FPM.addPass(polly::CodePreparationPass());
  polly::ScopPassManager SPM;
  SPM.addPass(DistributeSystolicBetaPass());
  SPM.addPass(polly::CodeGenerationPass());
  FPM.addPass(polly::createFunctionToScopPassAdaptor(std::move(SPM)));
  return FPM;
}
