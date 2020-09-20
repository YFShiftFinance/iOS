//===--- CSSolver.cpp - Constraint Solver ---------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements the constraint solver used in the type checker.
//
//===----------------------------------------------------------------------===//
#include "CSStep.h"
#include "ConstraintGraph.h"
#include "ConstraintSystem.h"
#include "SolutionResult.h"
#include "TypeCheckType.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/TypeWalker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <memory>
#include <tuple>

using namespace swift;
using namespace constraints;

//===----------------------------------------------------------------------===//
// Constraint solver statistics
//===----------------------------------------------------------------------===//
#define DEBUG_TYPE "Constraint solver overall"
#define JOIN2(X,Y) X##Y
STATISTIC(NumSolutionAttempts, "# of solution attempts");
STATISTIC(TotalNumTypeVariables, "# of type variables created");

#define CS_STATISTIC(Name, Description) \
  STATISTIC(Overall##Name, Description);
#include "ConstraintSolverStats.def"

#undef DEBUG_TYPE
#define DEBUG_TYPE "Constraint solver largest system"
#define CS_STATISTIC(Name, Description) \
  STATISTIC(Largest##Name, Description);
#include "ConstraintSolverStats.def"
STATISTIC(LargestSolutionAttemptNumber, "# of the largest solution attempt");

TypeVariableType *ConstraintSystem::createTypeVariable(
                                     ConstraintLocator *locator,
                                     unsigned options) {
  ++TotalNumTypeVariables;
  auto tv = TypeVariableType::getNew(getASTContext(), assignTypeVariableID(),
                                     locator, options);
  addTypeVariable(tv);
  return tv;
}

Solution ConstraintSystem::finalize() {
  assert(solverState);

  // Create the solution.
  Solution solution(*this, CurrentScore);

  // Update the best score we've seen so far.
  auto &ctx = getASTContext();
  assert(ctx.TypeCheckerOpts.DisableConstraintSolverPerformanceHacks ||
         !solverState->BestScore || CurrentScore <= *solverState->BestScore);

  if (!solverState->BestScore || CurrentScore <= *solverState->BestScore) {
    solverState->BestScore = CurrentScore;
  }

  for (auto tv : getTypeVariables()) {
    if (getFixedType(tv))
      continue;

    switch (solverState->AllowFreeTypeVariables) {
    case FreeTypeVariableBinding::Disallow:
      llvm_unreachable("Solver left free type variables");

    case FreeTypeVariableBinding::Allow:
      break;
        
    case FreeTypeVariableBinding::UnresolvedType:
      assignFixedType(tv, ctx.TheUnresolvedType);
      break;
    }
  }

  // For each of the type variables, get its fixed type.
  for (auto tv : getTypeVariables()) {
    solution.typeBindings[tv] = simplifyType(tv)->reconstituteSugar(false);
  }

  // Copy over the resolved overloads.
  solution.overloadChoices.insert(ResolvedOverloads.begin(),
                                  ResolvedOverloads.end());

  // For each of the constraint restrictions, record it with simplified,
  // canonical types.
  if (solverState) {
    for (auto &restriction : ConstraintRestrictions) {
      using std::get;
      CanType first = simplifyType(get<0>(restriction))->getCanonicalType();
      CanType second = simplifyType(get<1>(restriction))->getCanonicalType();
      solution.ConstraintRestrictions[{first, second}] = get<2>(restriction);
    }
  }

  // For each of the fixes, record it as an operation on the affected
  // expression.
  unsigned firstFixIndex = 0;
  if (solverState && solverState->PartialSolutionScope) {
    firstFixIndex = solverState->PartialSolutionScope->numFixes;
  }
  solution.Fixes.append(Fixes.begin() + firstFixIndex, Fixes.end());

  // Remember all the disjunction choices we made.
  for (auto &choice : DisjunctionChoices) {
    // We shouldn't ever register disjunction choices multiple times,
    // but saving and re-applying solutions can cause us to get
    // multiple entries.  We should use an optimized PartialSolution
    // structure for that use case, which would optimize a lot of
    // stuff here.
    assert(!solution.DisjunctionChoices.count(choice.first) ||
           solution.DisjunctionChoices[choice.first] == choice.second);
    solution.DisjunctionChoices.insert(choice);
  }

  // Remember all of the trailing closure matching choices we made.
  for (auto &trailingClosureMatch : trailingClosureMatchingChoices) {
    auto inserted = solution.trailingClosureMatchingChoices.insert(
        trailingClosureMatch);
    assert((inserted.second ||
            inserted.first->second == trailingClosureMatch.second));
    (void)inserted;
  }

  // Remember the opened types.
  for (const auto &opened : OpenedTypes) {
    // We shouldn't ever register opened types multiple times,
    // but saving and re-applying solutions can cause us to get
    // multiple entries.  We should use an optimized PartialSolution
    // structure for that use case, which would optimize a lot of
    // stuff here.
#if false
    assert((solution.OpenedTypes.count(opened.first) == 0 ||
            solution.OpenedTypes[opened.first] == opened.second)
            && "Already recorded");
#endif
    solution.OpenedTypes.insert(opened);
  }

  // Remember the opened existential types.
  for (const auto &openedExistential : OpenedExistentialTypes) {
    assert(solution.OpenedExistentialTypes.count(openedExistential.first) == 0||
           solution.OpenedExistentialTypes[openedExistential.first]
             == openedExistential.second &&
           "Already recorded");
    solution.OpenedExistentialTypes.insert(openedExistential);
  }

  // Remember the defaulted type variables.
  solution.DefaultedConstraints.insert(DefaultedConstraints.begin(),
                                       DefaultedConstraints.end());

  for (auto &nodeType : NodeTypes) {
    solution.nodeTypes.insert(nodeType);
  }

  // Remember contextual types.
  solution.contextualTypes.assign(
      contextualTypes.begin(), contextualTypes.end());

  solution.solutionApplicationTargets = solutionApplicationTargets;
  solution.caseLabelItems = caseLabelItems;

  for (auto &e : CheckedConformances)
    solution.Conformances.push_back({e.first, e.second});

  for (const auto &transformed : functionBuilderTransformed) {
    solution.functionBuilderTransformed.insert(transformed);
  }

  return solution;
}

void ConstraintSystem::applySolution(const Solution &solution) {
  // Update the score.
  CurrentScore += solution.getFixedScore();

  // Assign fixed types to the type variables solved by this solution.
  for (auto binding : solution.typeBindings) {
    // If we haven't seen this type variable before, record it now.
    addTypeVariable(binding.first);

    // If we don't already have a fixed type for this type variable,
    // assign the fixed type from the solution.
    if (!getFixedType(binding.first) && !binding.second->hasTypeVariable())
      assignFixedType(binding.first, binding.second, /*updateState=*/false);
  }

  // Register overload choices.
  // FIXME: Copy these directly into some kind of partial solution?
  for (auto overload : solution.overloadChoices)
    ResolvedOverloads.insert(overload);

  // Register constraint restrictions.
  // FIXME: Copy these directly into some kind of partial solution?
  for (auto restriction : solution.ConstraintRestrictions) {
    ConstraintRestrictions.push_back(
        std::make_tuple(restriction.first.first, restriction.first.second,
                        restriction.second));
  }

  // Register the solution's disjunction choices.
  for (auto &choice : solution.DisjunctionChoices) {
    DisjunctionChoices.push_back(choice);
  }

  // Remember all of the trailing closure matching choices we made.
  for (auto &trailingClosureMatch : solution.trailingClosureMatchingChoices) {
    trailingClosureMatchingChoices.push_back(trailingClosureMatch);
  }

  // Register the solution's opened types.
  for (const auto &opened : solution.OpenedTypes) {
    OpenedTypes.push_back(opened);
  }

  // Register the solution's opened existential types.
  for (const auto &openedExistential : solution.OpenedExistentialTypes) {
    OpenedExistentialTypes.push_back(openedExistential);
  }

  // Register the defaulted type variables.
  DefaultedConstraints.insert(DefaultedConstraints.end(),
                              solution.DefaultedConstraints.begin(),
                              solution.DefaultedConstraints.end());

  // Add the node types back.
  for (auto &nodeType : solution.nodeTypes) {
    setType(nodeType.first, nodeType.second);
  }

  // Add the contextual types.
  for (const auto &contextualType : solution.contextualTypes) {
    if (!getContextualTypeInfo(contextualType.first)) {
      setContextualType(contextualType.first, contextualType.second.typeLoc,
                        contextualType.second.purpose);
    }
  }

  // Register the statement condition targets.
  for (const auto &target : solution.solutionApplicationTargets) {
    if (!getSolutionApplicationTarget(target.first))
      setSolutionApplicationTarget(target.first, target.second);
  }

  // Register the statement condition targets.
  for (const auto &info : solution.caseLabelItems) {
    if (!getCaseLabelItemInfo(info.first))
      setCaseLabelItemInfo(info.first, info.second);
  }

  // Register the conformances checked along the way to arrive to solution.
  for (auto &conformance : solution.Conformances)
    CheckedConformances.push_back(conformance);

  for (const auto &transformed : solution.functionBuilderTransformed) {
    functionBuilderTransformed.push_back(transformed);
  }
    
  // Register any fixes produced along this path.
  Fixes.append(solution.Fixes.begin(), solution.Fixes.end());
}

/// Restore the type variable bindings to what they were before
/// we attempted to solve this constraint system.
void ConstraintSystem::restoreTypeVariableBindings(unsigned numBindings) {
  auto &savedBindings = *getSavedBindings();
  std::for_each(savedBindings.rbegin(), savedBindings.rbegin() + numBindings,
                [](SavedTypeVariableBinding &saved) {
                  saved.restore();
                });
  savedBindings.erase(savedBindings.end() - numBindings,
                      savedBindings.end());
}

bool ConstraintSystem::simplify(bool ContinueAfterFailures) {
  // While we have a constraint in the worklist, process it.
  while (!ActiveConstraints.empty()) {
    // Grab the next constraint from the worklist.
    auto *constraint = &ActiveConstraints.front();
    deactivateConstraint(constraint);

    // Simplify this constraint.
    switch (simplifyConstraint(*constraint)) {
    case SolutionKind::Error:
      retireFailedConstraint(constraint);
      break;

    case SolutionKind::Solved:
      if (solverState)
        ++solverState->NumSimplifiedConstraints;
      retireConstraint(constraint);
      break;

    case SolutionKind::Unsolved:
      if (solverState)
        ++solverState->NumUnsimplifiedConstraints;
      break;
    }

    // Check whether a constraint failed. If so, we're done.
    if (failedConstraint && !ContinueAfterFailures) {
      return true;
    }

    // If the current score is worse than the best score we've seen so far,
    // there's no point in continuing. So don't.
    if (worseThanBestSolution()) {
      return true;
    }
  }

  return false;
}

namespace {

template<typename T>
void truncate(std::vector<T> &vec, unsigned newSize) {
  assert(newSize <= vec.size() && "Not a truncation!");
  vec.erase(vec.begin() + newSize, vec.end());
}

/// Truncate the given small vector to the given new size.
template<typename T>
void truncate(SmallVectorImpl<T> &vec, unsigned newSize) {
  assert(newSize <= vec.size() && "Not a truncation!");
  vec.erase(vec.begin() + newSize, vec.end());
}

template<typename T, unsigned N>
void truncate(llvm::SmallSetVector<T, N> &vec, unsigned newSize) {
  assert(newSize <= vec.size() && "Not a truncation!");
  for (unsigned i = 0, n = vec.size() - newSize; i != n; ++i)
    vec.pop_back();
}

template <typename K, typename V>
void truncate(llvm::MapVector<K, V> &map, unsigned newSize) {
  assert(newSize <= map.size() && "Not a truncation!");
  for (unsigned i = 0, n = map.size() - newSize; i != n; ++i)
    map.pop_back();
}

template <typename K, typename V, unsigned N>
void truncate(llvm::SmallMapVector<K, V, N> &map, unsigned newSize) {
  assert(newSize <= map.size() && "Not a truncation!");
  for (unsigned i = 0, n = map.size() - newSize; i != n; ++i)
    map.pop_back();
}

} // end anonymous namespace

ConstraintSystem::SolverState::SolverState(
    ConstraintSystem &cs, FreeTypeVariableBinding allowFreeTypeVariables)
    : CS(cs), AllowFreeTypeVariables(allowFreeTypeVariables) {
  assert(!CS.solverState &&
         "Constraint system should not already have solver state!");
  CS.solverState = this;

  ++NumSolutionAttempts;
  SolutionAttempt = NumSolutionAttempts;

  // Record active constraints for re-activation at the end of lifetime.
  for (auto &constraint : cs.ActiveConstraints)
    activeConstraints.push_back(&constraint);

  // If we're supposed to debug a specific constraint solver attempt,
  // turn on debugging now.
  ASTContext &ctx = CS.getASTContext();
  const auto &tyOpts = ctx.TypeCheckerOpts;
  if (tyOpts.DebugConstraintSolverAttempt &&
      tyOpts.DebugConstraintSolverAttempt == SolutionAttempt) {
    CS.Options |= ConstraintSystemFlags::DebugConstraints;
    llvm::errs() << "---Constraint system #" << SolutionAttempt << "---\n";
    CS.print(llvm::errs());
  }
}

ConstraintSystem::SolverState::~SolverState() {
  assert((CS.solverState == this) &&
         "Expected constraint system to have this solver state!");
  CS.solverState = nullptr;

  // Make sure that all of the retired constraints have been returned
  // to constraint system.
  assert(!hasRetiredConstraints());

  // Make sure that all of the generated constraints have been handled.
  assert(generatedConstraints.empty());

  // Re-activate constraints which were initially marked as "active"
  // to restore original state of the constraint system.
  for (auto *constraint : activeConstraints) {
    // If the constraint is already active we can just move on.
    if (constraint->isActive())
      continue;

#ifndef NDEBUG
    // Make sure that constraint is present in the "inactive" set
    // before transferring it to "active".
    auto existing = llvm::find_if(CS.InactiveConstraints,
                                  [&constraint](const Constraint &inactive) {
                                    return &inactive == constraint;
                                  });
    assert(existing != CS.InactiveConstraints.end() &&
           "All constraints should be present in 'inactive' list");
#endif

    // Transfer the constraint to "active" set.
    CS.activateConstraint(constraint);
  }

  // If global constraing debugging is off and we are finished logging the
  // current solution attempt, switch debugging back off.
  const auto &tyOpts = CS.getASTContext().TypeCheckerOpts;
  if (!tyOpts.DebugConstraintSolver &&
      tyOpts.DebugConstraintSolverAttempt &&
      tyOpts.DebugConstraintSolverAttempt == SolutionAttempt) {
    CS.Options -= ConstraintSystemFlags::DebugConstraints;
  }

  // Write our local statistics back to the overall statistics.
  #define CS_STATISTIC(Name, Description) JOIN2(Overall,Name) += Name;
  #include "ConstraintSolverStats.def"

#if LLVM_ENABLE_STATS
  // Update the "largest" statistics if this system is larger than the
  // previous one.  
  // FIXME: This is not at all thread-safe.
  if (NumStatesExplored > LargestNumStatesExplored.getValue()) {
    LargestSolutionAttemptNumber = SolutionAttempt-1;
    ++LargestSolutionAttemptNumber;
    #define CS_STATISTIC(Name, Description) \
      JOIN2(Largest,Name) = Name-1; \
      ++JOIN2(Largest,Name);
    #include "ConstraintSolverStats.def"
  }
#endif
}

ConstraintSystem::SolverScope::SolverScope(ConstraintSystem &cs)
  : cs(cs), CGScope(cs.CG)
{
  numTypeVariables = cs.TypeVariables.size();
  numSavedBindings = cs.solverState->savedBindings.size();
  numConstraintRestrictions = cs.ConstraintRestrictions.size();
  numFixes = cs.Fixes.size();
  numFixedRequirements = cs.FixedRequirements.size();
  numDisjunctionChoices = cs.DisjunctionChoices.size();
  numTrailingClosureMatchingChoices = cs.trailingClosureMatchingChoices.size();
  numOpenedTypes = cs.OpenedTypes.size();
  numOpenedExistentialTypes = cs.OpenedExistentialTypes.size();
  numDefaultedConstraints = cs.DefaultedConstraints.size();
  numAddedNodeTypes = cs.addedNodeTypes.size();
  numCheckedConformances = cs.CheckedConformances.size();
  numDisabledConstraints = cs.solverState->getNumDisabledConstraints();
  numFavoredConstraints = cs.solverState->getNumFavoredConstraints();
  numFunctionBuilderTransformed = cs.functionBuilderTransformed.size();
  numResolvedOverloads = cs.ResolvedOverloads.size();
  numInferredClosureTypes = cs.ClosureTypes.size();
  numContextualTypes = cs.contextualTypes.size();
  numSolutionApplicationTargets = cs.solutionApplicationTargets.size();
  numCaseLabelItems = cs.caseLabelItems.size();

  PreviousScore = cs.CurrentScore;

  cs.solverState->registerScope(this);
  assert(!cs.failedConstraint && "Unexpected failed constraint!");
}

ConstraintSystem::SolverScope::~SolverScope() {
  // Erase the end of various lists.
  while (cs.TypeVariables.size() > numTypeVariables)
    cs.TypeVariables.pop_back();

  truncate(cs.ResolvedOverloads, numResolvedOverloads);

  // Restore bindings.
  cs.restoreTypeVariableBindings(cs.solverState->savedBindings.size() -
                                   numSavedBindings);

  // Move any remaining active constraints into the inactive list.
  if (!cs.ActiveConstraints.empty()) {
    for (auto &constraint : cs.ActiveConstraints) {
      constraint.setActive(false);
    }
    cs.InactiveConstraints.splice(cs.InactiveConstraints.end(),
                                  cs.ActiveConstraints);
  }

  // Rollback all of the changes done to constraints by the current scope,
  // e.g. add retired constraints back to the circulation and remove generated
  // constraints introduced by the current scope.
  cs.solverState->rollback(this);

  // Remove any constraint restrictions.
  truncate(cs.ConstraintRestrictions, numConstraintRestrictions);

  // Remove any fixes.
  truncate(cs.Fixes, numFixes);

  // Remove any disjunction choices.
  truncate(cs.DisjunctionChoices, numDisjunctionChoices);

  // Remove any trailing closure matching choices;
  truncate(
      cs.trailingClosureMatchingChoices, numTrailingClosureMatchingChoices);

  // Remove any opened types.
  truncate(cs.OpenedTypes, numOpenedTypes);

  // Remove any conformances solver had to fix along
  // the current path.
  truncate(cs.FixedRequirements, numFixedRequirements);

  // Remove any opened existential types.
  truncate(cs.OpenedExistentialTypes, numOpenedExistentialTypes);

  // Remove any defaulted type variables.
  truncate(cs.DefaultedConstraints, numDefaultedConstraints);

  // Remove any node types we registered.
  for (unsigned i :
           reverse(range(numAddedNodeTypes, cs.addedNodeTypes.size()))) {
    auto node = cs.addedNodeTypes[i].first;
    if (Type oldType = cs.addedNodeTypes[i].second)
      cs.NodeTypes[node] = oldType;
    else
      cs.NodeTypes.erase(node);
  }
  truncate(cs.addedNodeTypes, numAddedNodeTypes);

  // Remove any conformances checked along the current path.
  truncate(cs.CheckedConformances, numCheckedConformances);

  /// Remove any builder transformed closures.
  truncate(cs.functionBuilderTransformed, numFunctionBuilderTransformed);

  // Remove any inferred closure types (e.g. used in function builder body).
  truncate(cs.ClosureTypes, numInferredClosureTypes);

  // Remove any contextual types.
  truncate(cs.contextualTypes, numContextualTypes);

  // Remove any solution application targets.
  truncate(cs.solutionApplicationTargets, numSolutionApplicationTargets);

  // Remove any case label item infos.
  truncate(cs.caseLabelItems, numCaseLabelItems);

  // Reset the previous score.
  cs.CurrentScore = PreviousScore;

  // Clear out other "failed" state.
  cs.failedConstraint = nullptr;
}

/// Solve the system of constraints.
///
/// \param allowFreeTypeVariables How to bind free type variables in
/// the solution.
///
/// \returns a solution if a single unambiguous one could be found, or None if
/// ambiguous or unsolvable.
Optional<Solution>
ConstraintSystem::solveSingle(FreeTypeVariableBinding allowFreeTypeVariables,
                              bool allowFixes) {

  SolverState state(*this, allowFreeTypeVariables);
  state.recordFixes = allowFixes;

  SmallVector<Solution, 4> solutions;
  solveImpl(solutions);
  filterSolutions(solutions);

  if (solutions.size() != 1)
    return Optional<Solution>();

  return std::move(solutions[0]);
}

bool ConstraintSystem::Candidate::solve(
    llvm::SmallDenseSet<OverloadSetRefExpr *> &shrunkExprs) {
  // Don't attempt to solve candidate if there is closure
  // expression involved, because it's handled specially
  // by parent constraint system (e.g. parameter lists).
  bool containsClosure = false;
  E->forEachChildExpr([&](Expr *childExpr) -> Expr * {
    if (isa<ClosureExpr>(childExpr)) {
      containsClosure = true;
      return nullptr;
    }
    return childExpr;
  });

  if (containsClosure)
    return false;

  auto cleanupImplicitExprs = [&](Expr *expr) {
    expr->forEachChildExpr([&](Expr *childExpr) -> Expr * {
      Type type = childExpr->getType();
      if (childExpr->isImplicit() && type && type->hasTypeVariable())
        childExpr->setType(Type());
      return childExpr;
    });
  };

  // Allocate new constraint system for sub-expression.
  ConstraintSystem cs(DC, None);

  // Set up expression type checker timer for the candidate.
  cs.Timer.emplace(E, cs);

  // Generate constraints for the new system.
  if (auto generatedExpr = cs.generateConstraints(E, DC)) {
    E = generatedExpr;
  } else {
    // Failure to generate constraint system for sub-expression
    // means we can't continue solving sub-expressions.
    cleanupImplicitExprs(E);
    return true;
  }

  // If this candidate is too complex given the number
  // of the domains we have reduced so far, let's bail out early.
  if (isTooComplexGiven(&cs, shrunkExprs))
    return false;

  auto &ctx = cs.getASTContext();
  if (cs.isDebugMode()) {
    auto &log = llvm::errs();
    log << "--- Solving candidate for shrinking at ";
    auto R = E->getSourceRange();
    if (R.isValid()) {
      R.print(log, ctx.SourceMgr, /*PrintText=*/ false);
    } else {
      log << "<invalid range>";
    }
    log << " ---\n";

    E->dump(log);
    log << '\n';
    cs.print(log);
  }

  // If there is contextual type present, add an explicit "conversion"
  // constraint to the system.
  if (!CT.isNull()) {
    auto constraintKind = ConstraintKind::Conversion;
    if (CTP == CTP_CallArgument)
      constraintKind = ConstraintKind::ArgumentConversion;

    cs.addConstraint(constraintKind, cs.getType(E), CT,
                     cs.getConstraintLocator(E), /*isFavored=*/true);
  }

  // Try to solve the system and record all available solutions.
  llvm::SmallVector<Solution, 2> solutions;
  {
    SolverState state(cs, FreeTypeVariableBinding::Allow);

    // Use solve which doesn't try to filter solution list.
    // Because we want the whole set of possible domain choices.
    cs.solveImpl(solutions);
  }

  if (cs.isDebugMode()) {
    auto &log = llvm::errs();
    if (solutions.empty()) {
      log << "--- No Solutions ---\n";
    } else {
      log << "--- Solutions ---\n";
      for (unsigned i = 0, n = solutions.size(); i != n; ++i) {
        auto &solution = solutions[i];
        log << "--- Solution #" << i << " ---\n";
        solution.dump(log);
      }
    }
  }

  // Record found solutions as suggestions.
  this->applySolutions(solutions, shrunkExprs);

  // Let's double-check if we have any implicit expressions
  // with type variables and nullify their types.
  cleanupImplicitExprs(E);

  // No solutions for the sub-expression means that either main expression
  // needs salvaging or it's inconsistent (read: doesn't have solutions).
  return solutions.empty();
}

void ConstraintSystem::Candidate::applySolutions(
    llvm::SmallVectorImpl<Solution> &solutions,
    llvm::SmallDenseSet<OverloadSetRefExpr *> &shrunkExprs) const {
  // A collection of OSRs with their newly reduced domains,
  // it's domains are sets because multiple solutions can have the same
  // choice for one of the type variables, and we want no duplication.
  llvm::SmallDenseMap<OverloadSetRefExpr *, llvm::SmallSet<ValueDecl *, 2>>
    domains;
  for (auto &solution : solutions) {
    for (auto choice : solution.overloadChoices) {
      // Some of the choices might not have locators.
      if (!choice.getFirst())
        continue;

      auto anchor = choice.getFirst()->getAnchor();
      auto *OSR = getAsExpr<OverloadSetRefExpr>(anchor);
      // Anchor is not available or expression is not an overload set.
      if (!OSR)
        continue;

      auto overload = choice.getSecond().choice;
      auto type = overload.getDecl()->getInterfaceType();

      // One of the solutions has polymorphic type assigned with one of it's
      // type variables. Such functions can only be properly resolved
      // via complete expression, so we'll have to forget solutions
      // we have already recorded. They might not include all viable overload
      // choices.
      if (type->is<GenericFunctionType>()) {
        return;
      }

      domains[OSR].insert(overload.getDecl());
    }
  }

  // Reduce the domains.
  for (auto &domain : domains) {
    auto OSR = domain.getFirst();
    auto &choices = domain.getSecond();

    // If the domain wasn't reduced, skip it.
    if (OSR->getDecls().size() == choices.size()) continue;

    // Update the expression with the reduced domain.
    MutableArrayRef<ValueDecl *> decls(
                                Allocator.Allocate<ValueDecl *>(choices.size()),
                                choices.size());

    std::uninitialized_copy(choices.begin(), choices.end(), decls.begin());
    OSR->setDecls(decls);

    // Record successfully shrunk expression.
    shrunkExprs.insert(OSR);
  }
}

void ConstraintSystem::shrink(Expr *expr) {
  if (getASTContext().TypeCheckerOpts.SolverDisableShrink)
    return;

  using DomainMap = llvm::SmallDenseMap<Expr *, ArrayRef<ValueDecl *>>;

  // A collection of original domains of all of the expressions,
  // so they can be restored in case of failure.
  DomainMap domains;

  struct ExprCollector : public ASTWalker {
    Expr *PrimaryExpr;

    // The primary constraint system.
    ConstraintSystem &CS;

    // All of the sub-expressions which are suitable to be solved
    // separately from the main system e.g. binary expressions, collections,
    // function calls, coercions etc.
    llvm::SmallVector<Candidate, 4> Candidates;

    // Counts the number of overload sets present in the tree so far.
    // Note that the traversal is depth-first.
    llvm::SmallVector<std::pair<Expr *, unsigned>, 4> ApplyExprs;

    // A collection of original domains of all of the expressions,
    // so they can be restored in case of failure.
    DomainMap &Domains;

    ExprCollector(Expr *expr, ConstraintSystem &cs, DomainMap &domains)
        : PrimaryExpr(expr), CS(cs), Domains(domains) {}

    std::pair<bool, Expr *> walkToExprPre(Expr *expr) override {
      // A dictionary expression is just a set of tuples; try to solve ones
      // that have overload sets.
      if (auto collectionExpr = dyn_cast<CollectionExpr>(expr)) {
        visitCollectionExpr(collectionExpr, CS.getContextualType(expr),
                            CS.getContextualTypePurpose(expr));
        // Don't try to walk into the dictionary.
        return {false, expr};
      }

      // Let's not attempt to type-check closures or expressions
      // which constrain closures, because they require special handling
      // when dealing with context and parameters declarations.
      if (isa<ClosureExpr>(expr)) {
        return {false, expr};
      }

      // Similar to 'ClosureExpr', 'TapExpr' has a 'VarDecl' the type of which
      // is determined by type checking the parent interpolated string literal.
      if (isa<TapExpr>(expr)) {
        return {false, expr};
      }

      if (auto coerceExpr = dyn_cast<CoerceExpr>(expr)) {
        if (coerceExpr->isLiteralInit())
          ApplyExprs.push_back({coerceExpr, 1});
        visitCoerceExpr(coerceExpr);
        return {false, expr};
      }

      if (auto OSR = dyn_cast<OverloadSetRefExpr>(expr)) {
        Domains[OSR] = OSR->getDecls();
      }

      if (auto applyExpr = dyn_cast<ApplyExpr>(expr)) {
        auto func = applyExpr->getFn();
        // Let's record this function application for post-processing
        // as well as if it contains overload set, see walkToExprPost.
        ApplyExprs.push_back(
            {applyExpr, isa<OverloadSetRefExpr>(func) || isa<TypeExpr>(func)});
      }

      return { true, expr };
    }

    /// Determine whether this is an arithmetic expression comprised entirely
    /// of literals.
    static bool isArithmeticExprOfLiterals(Expr *expr) {
      expr = expr->getSemanticsProvidingExpr();

      if (auto prefix = dyn_cast<PrefixUnaryExpr>(expr))
        return isArithmeticExprOfLiterals(prefix->getArg());

      if (auto postfix = dyn_cast<PostfixUnaryExpr>(expr))
        return isArithmeticExprOfLiterals(postfix->getArg());

      if (auto binary = dyn_cast<BinaryExpr>(expr))
        return isArithmeticExprOfLiterals(binary->getArg()->getElement(0)) &&
               isArithmeticExprOfLiterals(binary->getArg()->getElement(1));

      return isa<IntegerLiteralExpr>(expr) || isa<FloatLiteralExpr>(expr);
    }

    Expr *walkToExprPost(Expr *expr) override {
      auto isSrcOfPrimaryAssignment = [&](Expr *expr) -> bool {
        if (auto *AE = dyn_cast<AssignExpr>(PrimaryExpr))
          return expr == AE->getSrc();
        return false;
      };

      if (expr == PrimaryExpr || isSrcOfPrimaryAssignment(expr)) {
        // If this is primary expression and there are no candidates
        // to be solved, let's not record it, because it's going to be
        // solved regardless.
        if (Candidates.empty())
          return expr;

        auto contextualType = CS.getContextualType(expr);
        // If there is a contextual type set for this expression.
        if (!contextualType.isNull()) {
          Candidates.push_back(Candidate(CS, PrimaryExpr, contextualType,
                                         CS.getContextualTypePurpose(expr)));
          return expr;
        }

        // Or it's a function application or assignment with other candidates
        // present. Assignment should be easy to solve because we'd get a
        // contextual type from the destination expression, otherwise shrink
        // might produce incorrect results without considering aforementioned
        // destination type.
        if (isa<ApplyExpr>(expr) || isa<AssignExpr>(expr)) {
          Candidates.push_back(Candidate(CS, PrimaryExpr));
          return expr;
        }
      }

      if (!isa<ApplyExpr>(expr))
        return expr;

      unsigned numOverloadSets = 0;
      // Let's count how many overload sets do we have.
      while (!ApplyExprs.empty()) {
        auto &application = ApplyExprs.back();
        auto applyExpr = application.first;

        // Add overload sets tracked by current expression.
        numOverloadSets += application.second;
        ApplyExprs.pop_back();

        // We've found the current expression, so record the number of
        // overloads.
        if (expr == applyExpr) {
          ApplyExprs.push_back({applyExpr, numOverloadSets});
          break;
        }
      }

      // If there are fewer than two overloads in the chain
      // there is no point of solving this expression,
      // because we won't be able to reduce its domain.
      if (numOverloadSets > 1 && !isArithmeticExprOfLiterals(expr))
        Candidates.push_back(Candidate(CS, expr));

      return expr;
    }

  private:
    /// Extract type of the element from given collection type.
    ///
    /// \param collection The type of the collection container.
    ///
    /// \returns Null type, ErrorType or UnresolvedType on failure,
    /// properly constructed type otherwise.
    Type extractElementType(Type collection) {
      auto &ctx = CS.getASTContext();
      if (!collection || collection->hasError())
        return collection;

      auto base = collection.getPointer();
      auto isInvalidType = [](Type type) -> bool {
        return type.isNull() || type->hasUnresolvedType() ||
               type->hasError();
      };

      // Array type.
      if (auto array = dyn_cast<ArraySliceType>(base)) {
        auto elementType = array->getBaseType();
        // If base type is invalid let's return error type.
        return elementType;
      }

      // Map or Set or any other associated collection type.
      if (auto boundGeneric = dyn_cast<BoundGenericType>(base)) {
        if (boundGeneric->hasUnresolvedType())
          return boundGeneric;

        llvm::SmallVector<TupleTypeElt, 2> params;
        for (auto &type : boundGeneric->getGenericArgs()) {
          // One of the generic arguments in invalid or unresolved.
          if (isInvalidType(type))
            return type;

          params.push_back(type);
        }

        // If there is just one parameter, let's return it directly.
        if (params.size() == 1)
          return params[0].getType();

        return TupleType::get(params, ctx);
      }

      return Type();
    }

    bool isSuitableCollection(TypeRepr *collectionTypeRepr) {
      // Only generic identifier, array or dictionary.
      switch (collectionTypeRepr->getKind()) {
      case TypeReprKind::GenericIdent:
      case TypeReprKind::Array:
      case TypeReprKind::Dictionary:
        return true;

      default:
        return false;
      }
    }

    void visitCoerceExpr(CoerceExpr *coerceExpr) {
      auto subExpr = coerceExpr->getSubExpr();
      // Coerce expression is valid only if it has sub-expression.
      if (!subExpr) return;

      unsigned numOverloadSets = 0;
      subExpr->forEachChildExpr([&](Expr *childExpr) -> Expr * {
        if (isa<OverloadSetRefExpr>(childExpr)) {
          ++numOverloadSets;
          return childExpr;
        }

        if (auto nestedCoerceExpr = dyn_cast<CoerceExpr>(childExpr)) {
          visitCoerceExpr(nestedCoerceExpr);
          // Don't walk inside of nested coercion expression directly,
          // that is be done by recursive call to visitCoerceExpr.
          return nullptr;
        }

        // If sub-expression we are trying to coerce to type is a collection,
        // let's allow collector discover it with assigned contextual type
        // of coercion, which allows collections to be solved in parts.
        if (auto collectionExpr = dyn_cast<CollectionExpr>(childExpr)) {
          auto *const typeRepr = coerceExpr->getCastTypeRepr();

          if (typeRepr && isSuitableCollection(typeRepr)) {
            const auto coercionType =
                TypeResolution::forContextual(
                    CS.DC, None,
                    // FIXME: Should we really be unconditionally complaining
                    // about unbound generics here? For example:
                    // let foo: [Array<Float>] = [[0], [1], [2]] as [Array]
                    /*unboundTyOpener*/ nullptr)
                    .resolveType(typeRepr);

            // Looks like coercion type is invalid, let's skip this sub-tree.
            if (coercionType->hasError())
              return nullptr;

            // Visit collection expression inline.
            visitCollectionExpr(collectionExpr, coercionType,
                                CTP_CoerceOperand);
          }
        }

        return childExpr;
      });

      // It's going to be inefficient to try and solve
      // coercion in parts, so let's just make it a candidate directly,
      // if it contains at least a single overload set.

      if (numOverloadSets > 0)
        Candidates.push_back(Candidate(CS, coerceExpr));
    }

    void visitCollectionExpr(CollectionExpr *collectionExpr,
                             Type contextualType = Type(),
                             ContextualTypePurpose CTP = CTP_Unused) {
      // If there is a contextual type set for this collection,
      // let's propagate it to the candidate.
      if (!contextualType.isNull()) {
        auto elementType = extractElementType(contextualType);
        // If we couldn't deduce element type for the collection, let's
        // not attempt to solve it.
        if (!elementType ||
            elementType->hasError() ||
            elementType->hasUnresolvedType())
          return;

        contextualType = elementType;
      }

      for (auto element : collectionExpr->getElements()) {
        unsigned numOverloads = 0;
        element->walk(OverloadSetCounter(numOverloads));

        // There are no overload sets in the element; skip it.
        if (numOverloads == 0)
          continue;

        // Record each of the collection elements, which passed
        // number of overload sets rule, as a candidate for solving
        // with contextual type of the collection.
        Candidates.push_back(Candidate(CS, element, contextualType, CTP));
      }
    }
  };

  ExprCollector collector(expr, *this, domains);

  // Collect all of the binary/unary and call sub-expressions
  // so we can start solving them separately.
  expr->walk(collector);

  llvm::SmallDenseSet<OverloadSetRefExpr *> shrunkExprs;
  for (auto &candidate : collector.Candidates) {
    // If there are no results, let's forget everything we know about the
    // system so far. This actually is ok, because some of the expressions
    // might require manual salvaging.
    if (candidate.solve(shrunkExprs)) {
      // Let's restore all of the original OSR domains for this sub-expression,
      // this means that we can still make forward progress with solving of the
      // top sub-expressions.
      candidate.getExpr()->forEachChildExpr([&](Expr *childExpr) -> Expr * {
        if (auto OSR = dyn_cast<OverloadSetRefExpr>(childExpr)) {
          auto domain = domains.find(OSR);
          if (domain == domains.end())
            return childExpr;

          OSR->setDecls(domain->getSecond());
          shrunkExprs.erase(OSR);
        }

        return childExpr;
      });
    }
  }

  // Once "shrinking" is done let's re-allocate final version of
  // the candidate list to the permanent arena, so it could
  // survive even after primary constraint system is destroyed.
  for (auto &OSR : shrunkExprs) {
    auto choices = OSR->getDecls();
    auto decls =
        getASTContext().AllocateUninitialized<ValueDecl *>(choices.size());

    std::uninitialized_copy(choices.begin(), choices.end(), decls.begin());
    OSR->setDecls(decls);
  }
}

static bool debugConstraintSolverForTarget(
   ASTContext &C, SolutionApplicationTarget target) {
  if (C.TypeCheckerOpts.DebugConstraintSolver)
    return true;

  if (C.TypeCheckerOpts.DebugConstraintSolverOnLines.empty())
    // No need to compute the line number to find out it's not present.
    return false;

  // Get the lines on which the target starts and ends.
  unsigned startLine = 0, endLine = 0;
  SourceRange range = target.getSourceRange();
  if (range.isValid()) {
    auto charRange = Lexer::getCharSourceRangeFromSourceRange(C.SourceMgr, range);
    startLine =
        C.SourceMgr.getLineAndColumnInBuffer(charRange.getStart()).first;
    endLine = C.SourceMgr.getLineAndColumnInBuffer(charRange.getEnd()).first;
  }

  assert(startLine <= endLine && "expr ends before it starts?");

  auto &lines = C.TypeCheckerOpts.DebugConstraintSolverOnLines;
  assert(std::is_sorted(lines.begin(), lines.end()) &&
         "DebugConstraintSolverOnLines sorting invariant violated");

  // Check if `lines` contains at least one line `L` where
  // `startLine <= L <= endLine`. If it does, `lower_bound(startLine)` and
  // `upper_bound(endLine)` will be different.
  auto startBound = llvm::lower_bound(lines, startLine);
  auto endBound = std::upper_bound(startBound, lines.end(), endLine);

  return startBound != endBound;
}

Optional<std::vector<Solution>> ConstraintSystem::solve(
    SolutionApplicationTarget &target,
    FreeTypeVariableBinding allowFreeTypeVariables
) {
  llvm::SaveAndRestore<ConstraintSystemOptions> debugForExpr(Options);
  if (debugConstraintSolverForTarget(getASTContext(), target)) {
    Options |= ConstraintSystemFlags::DebugConstraints;
  }

  /// Dump solutions for debugging purposes.
  auto dumpSolutions = [&](const SolutionResult &result) {
    // Debug-print the set of solutions.
    if (isDebugMode()) {
      if (result.getKind() == SolutionResult::Success) {
        llvm::errs() << "---Solution---\n";
        result.getSolution().dump(llvm::errs());
      } else if (result.getKind() == SolutionResult::Ambiguous) {
        auto solutions = result.getAmbiguousSolutions();
        for (unsigned i : indices(solutions)) {
          llvm::errs() << "--- Solution #" << i << " ---\n";
          solutions[i].dump(llvm::errs());
        }
      }
    }
  };

  // Take up to two attempts at solving the system. The first attempts to
  // solve a system that is expected to be well-formed, the second kicks in
  // when there is an error and attempts to salvage an ill-formed program.
  for (unsigned stage = 0; stage != 2; ++stage) {
    auto solution = (stage == 0)
        ? solveImpl(target, allowFreeTypeVariables)
        : salvage();

    switch (solution.getKind()) {
    case SolutionResult::Success: {
      // Return the successful solution.
      dumpSolutions(solution);
      std::vector<Solution> result;
      result.push_back(std::move(solution).takeSolution());
      return std::move(result);
    }

    case SolutionResult::Error:
      maybeProduceFallbackDiagnostic(target);
      return None;

    case SolutionResult::TooComplex:
      getASTContext().Diags.diagnose(
          target.getLoc(), diag::expression_too_complex)
            .highlight(target.getSourceRange());
      solution.markAsDiagnosed();
      return None;

    case SolutionResult::Ambiguous:
      // If salvaging produced an ambiguous result, it has already been
      // diagnosed.
      if (stage == 1) {
        solution.markAsDiagnosed();
        return None;
      }

      if (Options.contains(
            ConstraintSystemFlags::AllowUnresolvedTypeVariables)) {
        dumpSolutions(solution);
        auto ambiguousSolutions = std::move(solution).takeAmbiguousSolutions();
        std::vector<Solution> result(
            std::make_move_iterator(ambiguousSolutions.begin()),
            std::make_move_iterator(ambiguousSolutions.end()));
        return std::move(result);
      }

      LLVM_FALLTHROUGH;

    case SolutionResult::UndiagnosedError:
      if (shouldSuppressDiagnostics()) {
        solution.markAsDiagnosed();
        return None;
      }

      if (stage == 1) {
        diagnoseFailureFor(target);
        solution.markAsDiagnosed();
        return None;
      }

      // Loop again to try to salvage.
      solution.markAsDiagnosed();
      continue;
    }
  }

  llvm_unreachable("Loop always returns");
}

SolutionResult
ConstraintSystem::solveImpl(SolutionApplicationTarget &target,
                            FreeTypeVariableBinding allowFreeTypeVariables) {
  if (isDebugMode()) {
    auto &log = llvm::errs();
    log << "---Constraint solving at ";
    auto R = target.getSourceRange();
    if (R.isValid()) {
      R.print(log, getASTContext().SourceMgr, /*PrintText=*/ false);
    } else {
      log << "<invalid range>";
    }
    log << "---\n";
  }

  assert(!solverState && "cannot be used directly");

  // Set up the expression type checker timer.
  if (Expr *expr = target.getAsExpr())
    Timer.emplace(expr, *this);

  if (generateConstraints(target, allowFreeTypeVariables))
    return SolutionResult::forError();;

  // Try to solve the constraint system using computed suggestions.
  SmallVector<Solution, 4> solutions;
  solve(solutions, allowFreeTypeVariables);

  if (getExpressionTooComplex(solutions))
    return SolutionResult::forTooComplex();

  switch (solutions.size()) {
  case 0:
    return SolutionResult::forUndiagnosedError();

  case 1:
    return SolutionResult::forSolved(std::move(solutions.front()));

  default:
    return SolutionResult::forAmbiguous(solutions);
  }
}

bool ConstraintSystem::solve(SmallVectorImpl<Solution> &solutions,
                             FreeTypeVariableBinding allowFreeTypeVariables) {
  // Set up solver state.
  SolverState state(*this, allowFreeTypeVariables);

  // Solve the system.
  solveImpl(solutions);

  if (isDebugMode()) {
    auto &log = llvm::errs();
    log << "---Solver statistics---\n";
    log << "Total number of scopes explored: " << solverState->NumStatesExplored << "\n";
    log << "Maximum depth reached while exploring solutions: " << solverState->maxDepth << "\n";
    if (Timer) {
      auto timeInMillis =
        1000 * Timer->getElapsedProcessTimeInFractionalSeconds();
      log << "Time: " << timeInMillis << "ms\n";
    }
  }

  // Filter deduced solutions, try to figure out if there is
  // a single best solution to use, if not explicitly disabled
  // by constraint system options.
  filterSolutions(solutions);

  // We fail if there is no solution or the expression was too complex.
  return solutions.empty() || getExpressionTooComplex(solutions);
}

void ConstraintSystem::solveImpl(SmallVectorImpl<Solution> &solutions) {
  assert(solverState);

  setPhase(ConstraintSystemPhase::Solving);

  SWIFT_DEFER { setPhase(ConstraintSystemPhase::Finalization); };

  // If constraint system failed while trying to
  // genenerate constraints, let's stop right here.
  if (failedConstraint)
    return;

  // Allocate new solver scope, so constraint system
  // could be restored to its original state afterwards.
  // Otherwise there is a risk that some of the constraints
  // are not going to be re-introduced to the system.
  SolverScope scope(*this);

  SmallVector<std::unique_ptr<SolverStep>, 16> workList;
  // First step is always wraps whole constraint system.
  workList.push_back(std::make_unique<SplitterStep>(*this, solutions));

  // Indicate whether previous step in the stack has failed
  // (returned StepResult::Kind = Error), this is useful to
  // propagate failures when unsolved steps are re-taken.
  bool prevFailed = false;

  // Advance the solver by taking a given step, which might involve
  // a prelimilary "setup", if this is the first time this step is taken.
  auto advance = [](SolverStep *step, bool prevFailed) -> StepResult {
    auto currentState = step->getState();
    if (currentState == StepState::Setup) {
      step->setup();
      step->transitionTo(StepState::Ready);
    }

    currentState = step->getState();
    step->transitionTo(StepState::Running);
    return currentState == StepState::Ready ? step->take(prevFailed)
                                            : step->resume(prevFailed);
  };

  // Execute steps in LIFO order, which means that
  // each individual step would either end up producing
  // a solution, or producing another set of mergeable
  // steps to take before arriving to solution.
  while (!workList.empty()) {
    auto &step = workList.back();

    // Now let's try to advance to the next step or re-take previous,
    // which should produce another steps to follow,
    // or error, which means that current path is inconsistent.
    {
      auto result = advance(step.get(), prevFailed);
      switch (result.getKind()) {
      // It was impossible to solve this step, let's note that
      // for followup steps, to propogate the error.
      case SolutionKind::Error:
        LLVM_FALLTHROUGH;

      // Step has been solved successfully by either
      // producing a partial solution, or more steps
      // toward that solution.
      case SolutionKind::Solved: {
        workList.pop_back();
        break;
      }

      // Keep this step in the work list to return to it
      // once all other steps are done, this could be a
      // disjunction which has to peek a new choice until
      // it completely runs out of choices, or type variable
      // binding.
      case SolutionKind::Unsolved:
        break;
      }

      prevFailed = result.getKind() == SolutionKind::Error;
      result.transfer(workList);
    }
  }
}

bool ConstraintSystem::solveForCodeCompletion(
    SolutionApplicationTarget &target, SmallVectorImpl<Solution> &solutions) {
  auto *expr = target.getAsExpr();
  // Tell the constraint system what the contextual type is.
  setContextualType(expr, target.getExprContextualTypeLoc(),
                    target.getExprContextualTypePurpose());

  // Set up the expression type checker timer.
  Timer.emplace(expr, *this);

  shrink(expr);

  if (isDebugMode()) {
    auto &log = llvm::errs();
    log << "--- Code Completion ---\n";
  }

  if (generateConstraints(target, FreeTypeVariableBinding::Disallow))
    return false;

  {
    SolverState state(*this, FreeTypeVariableBinding::Disallow);

    // Enable "diagnostic mode" by default, this means that
    // solver would produce "fixed" solutions alongside valid
    // ones, which helps code completion to rank choices.
    state.recordFixes = true;

    solveImpl(solutions);
  }

  if (isDebugMode()) {
    auto &log = llvm::errs();
    log << "--- Discovered " << solutions.size() << " solutions ---\n";
    for (const auto &solution : solutions) {
      log << "--- Solution ---\n";
      solution.dump(log);
    }
  }

  return true;
}

void ConstraintSystem::collectDisjunctions(
    SmallVectorImpl<Constraint *> &disjunctions) {
  for (auto &constraint : InactiveConstraints) {
    if (constraint.getKind() == ConstraintKind::Disjunction)
      disjunctions.push_back(&constraint);
  }
}

ConstraintSystem::SolutionKind
ConstraintSystem::filterDisjunction(
    Constraint *disjunction, bool restoreOnFail,
    llvm::function_ref<bool(Constraint *)> pred) {
  assert(disjunction->getKind() == ConstraintKind::Disjunction);

  SmallVector<Constraint *, 4> constraintsToRestoreOnFail;
  unsigned choiceIdx = 0;
  unsigned numEnabledTerms = 0;
  ASTContext &ctx = getASTContext();
  for (unsigned constraintIdx : indices(disjunction->getNestedConstraints())) {
    auto constraint = disjunction->getNestedConstraints()[constraintIdx];

    // Skip already-disabled constraints. Let's treat disabled
    // choices which have a fix as "enabled" ones here, so we can
    // potentially infer some type information from them.
    if (constraint->isDisabled() && !constraint->getFix())
      continue;

    if (pred(constraint)) {
      ++numEnabledTerms;
      choiceIdx = constraintIdx;
      continue;
    }

    if (isDebugMode()) {
      llvm::errs().indent(solverState ? solverState->depth * 2 : 0)
        << "(disabled disjunction term ";
      constraint->print(llvm::errs(), &ctx.SourceMgr);
      llvm::errs() << ")\n";
    }

    if (restoreOnFail)
      constraintsToRestoreOnFail.push_back(constraint);

    if (solverState)
      solverState->disableContraint(constraint);
    else
      constraint->setDisabled();
  }

  switch (numEnabledTerms) {
  case 0:
    for (auto constraint : constraintsToRestoreOnFail) {
      constraint->setEnabled();
    }
    return SolutionKind::Error;

  case 1: {
    // Only a single constraint remains. Retire the disjunction and make
    // the remaining constraint active.
    auto choice = disjunction->getNestedConstraints()[choiceIdx];

    // This can only happen when subscript syntax is used to lookup
    // something which doesn't exist in type marked with
    // `@dynamicMemberLookup`.
    // Since filtering currently runs as part of the `applicable function`
    // constraint processing, "keypath dynamic member lookup" choice can't
    // be attempted in-place because that would also try to operate on that
    // constraint, so instead let's keep the disjunction, but disable all
    // unviable choices.
    if (choice->getOverloadChoice().getKind() ==
        OverloadChoiceKind::KeyPathDynamicMemberLookup) {
      // Early simplification of the "keypath dynamic member lookup" choice
      // is impossible because it requires constraints associated with
      // subscript index expression to be present.
      if (Phase == ConstraintSystemPhase::ConstraintGeneration)
        return SolutionKind::Unsolved;

      for (auto *currentChoice : disjunction->getNestedConstraints()) {
        if (currentChoice != choice)
          solverState->disableContraint(currentChoice);
      }
      return SolutionKind::Solved;
    }

    // Retire the disjunction. It's been solved.
    retireConstraint(disjunction);

    // Note the choice we made and simplify it. This introduces the
    // new constraint into the system.
    if (disjunction->shouldRememberChoice()) {
      recordDisjunctionChoice(disjunction->getLocator(), choiceIdx);
    }

    if (isDebugMode()) {
      llvm::errs().indent(solverState ? solverState->depth * 2 : 0)
        << "(introducing single enabled disjunction term ";
      choice->print(llvm::errs(), &ctx.SourceMgr);
      llvm::errs() << ")\n";
    }

    simplifyDisjunctionChoice(choice);

    return failedConstraint ? SolutionKind::Unsolved : SolutionKind::Solved;
  }

  default:
    return SolutionKind::Unsolved;
  }
}

// Attempt to find a disjunction of bind constraints where all options
// in the disjunction are binding the same type variable.
//
// Prefer disjunctions where the bound type variable is also the
// right-hand side of a conversion constraint, since having a concrete
// type that we're converting to can make it possible to split the
// constraint system into multiple ones.
static Constraint *selectBestBindingDisjunction(
    ConstraintSystem &cs, SmallVectorImpl<Constraint *> &disjunctions) {

  if (disjunctions.empty())
    return nullptr;

  auto getAsTypeVar = [&cs](Type type) {
    return cs.simplifyType(type)->getRValueType()->getAs<TypeVariableType>();
  };

  Constraint *firstBindDisjunction = nullptr;
  for (auto *disjunction : disjunctions) {
    auto choices = disjunction->getNestedConstraints();
    assert(!choices.empty());

    auto *choice = choices.front();
    if (choice->getKind() != ConstraintKind::Bind)
      continue;

    // We can judge disjunction based on the single choice
    // because all of choices (of bind overload set) should
    // have the same left-hand side.
    // Only do this for simple type variable bindings, not for
    // bindings like: ($T1) -> $T2 bind String -> Int
    auto *typeVar = getAsTypeVar(choice->getFirstType());
    if (!typeVar)
      continue;

    if (!firstBindDisjunction)
      firstBindDisjunction = disjunction;

    auto constraints = cs.getConstraintGraph().gatherConstraints(
        typeVar, ConstraintGraph::GatheringKind::EquivalenceClass,
        [](Constraint *constraint) {
          return constraint->getKind() == ConstraintKind::Conversion;
        });

    for (auto *constraint : constraints) {
      if (typeVar == getAsTypeVar(constraint->getSecondType()))
        return disjunction;
    }
  }

  // If we had any binding disjunctions, return the first of
  // those. These ensure that we attempt to bind types earlier than
  // trying the elements of other disjunctions, which can often mean
  // we fail faster.
  return firstBindDisjunction;
}

// For a given type, collect any concrete types or literal
// conformances we can reach by walking the constraint graph starting
// from this point.
//
// For example, if the type is a type variable, we'll walk back
// through the constraints mentioning this type variable and find what
// types are converted to this type along with what literals are
// conformed-to by this type.
void ConstraintSystem::ArgumentInfoCollector::walk(Type argType) {
  llvm::SmallSet<TypeVariableType *, 4> visited;
  llvm::SmallVector<Type, 4> worklist;
  worklist.push_back(argType);

  while (!worklist.empty()) {
    auto itemTy = worklist.pop_back_val()->getRValueType();

    if (!itemTy->is<TypeVariableType>()) {
      addType(itemTy);
      continue;
    }

    auto tyvar = itemTy->castTo<TypeVariableType>();
    if (auto fixedTy = CS.getFixedType(tyvar)) {
      addType(fixedTy);
      continue;
    }

    auto *rep = CS.getRepresentative(tyvar);

    // FIXME: This can happen when we have two type variables that are
    // subtypes of each other. We would ideally merge those type
    // variables somewhere.
    if (visited.count(rep))
      continue;

    visited.insert(rep);

    auto constraints = CS.getConstraintGraph().gatherConstraints(
        rep, ConstraintGraph::GatheringKind::EquivalenceClass);

    for (auto *constraint : constraints) {
      switch (constraint->getKind()) {
      case ConstraintKind::LiteralConformsTo:
        addLiteralProtocol(constraint->getProtocol());
        break;

      case ConstraintKind::Bind:
      case ConstraintKind::Equal: {
        auto firstTy = constraint->getFirstType();
        auto secondTy = constraint->getSecondType();
        if (firstTy->is<TypeVariableType>()) {
          auto otherRep =
              CS.getRepresentative(firstTy->castTo<TypeVariableType>());
          if (otherRep->isEqual(rep))
            worklist.push_back(secondTy);
        }
        if (secondTy->is<TypeVariableType>()) {
          auto otherRep =
              CS.getRepresentative(secondTy->castTo<TypeVariableType>());
          if (otherRep->isEqual(rep))
            worklist.push_back(firstTy);
        }
        break;
      }

      case ConstraintKind::Subtype:
      case ConstraintKind::OperatorArgumentConversion:
      case ConstraintKind::ArgumentConversion:
      case ConstraintKind::Conversion:
      case ConstraintKind::BridgingConversion:
      case ConstraintKind::BindParam:
      case ConstraintKind::OpaqueUnderlyingType: {
        auto secondTy = constraint->getSecondType();
        if (secondTy->is<TypeVariableType>()) {
          auto otherRep =
              CS.getRepresentative(secondTy->castTo<TypeVariableType>());
          if (otherRep->isEqual(rep))
            worklist.push_back(constraint->getFirstType());
        }
        break;
      }

      case ConstraintKind::DynamicTypeOf:
      case ConstraintKind::EscapableFunctionOf: {
        auto firstTy = constraint->getFirstType();
        if (firstTy->is<TypeVariableType>()) {
          auto otherRep =
              CS.getRepresentative(firstTy->castTo<TypeVariableType>());
          if (otherRep->isEqual(rep))
            worklist.push_back(constraint->getSecondType());
        }
        break;
      }

      case ConstraintKind::OptionalObject: {
        // Get the underlying object type.
        auto secondTy = constraint->getSecondType();
        if (secondTy->is<TypeVariableType>()) {
          auto otherRep =
              CS.getRepresentative(secondTy->castTo<TypeVariableType>());
          if (otherRep->isEqual(rep)) {
            // See if we can actually determine what the underlying
            // type is.
            Type fixedTy;
            auto firstTy = constraint->getFirstType();
            if (!firstTy->is<TypeVariableType>()) {
              fixedTy = firstTy;
            } else {
              fixedTy = CS.getFixedType(firstTy->castTo<TypeVariableType>());
            }
            if (fixedTy && fixedTy->getOptionalObjectType())
              worklist.push_back(fixedTy->getOptionalObjectType());
          }
        }
        break;
      }

      case ConstraintKind::KeyPathApplication:
      case ConstraintKind::KeyPath: {
        auto firstTy = constraint->getFirstType();
        if (firstTy->is<TypeVariableType>()) {
          auto otherRep =
              CS.getRepresentative(firstTy->castTo<TypeVariableType>());
          if (otherRep->isEqual(rep))
            worklist.push_back(constraint->getThirdType());
        }
        break;
      }

      case ConstraintKind::BindToPointerType:
      case ConstraintKind::ValueMember:
      case ConstraintKind::ValueWitness:
      case ConstraintKind::UnresolvedValueMember:
      case ConstraintKind::Disjunction:
      case ConstraintKind::CheckedCast:
      case ConstraintKind::OpenedExistentialOf:
      case ConstraintKind::ApplicableFunction:
      case ConstraintKind::DynamicCallableApplicableFunction:
      case ConstraintKind::BindOverload:
      case ConstraintKind::FunctionInput:
      case ConstraintKind::FunctionResult:
      case ConstraintKind::SelfObjectOfProtocol:
      case ConstraintKind::ConformsTo:
      case ConstraintKind::Defaultable:
      case ConstraintKind::OneWayEqual:
      case ConstraintKind::OneWayBindParam:
      case ConstraintKind::DefaultClosureType:
        break;
      }
    }
  }
}

void ConstraintSystem::ArgumentInfoCollector::minimizeLiteralProtocols() {
  if (LiteralProtocols.size() <= 1)
    return;

  llvm::SmallVector<std::pair<ProtocolDecl *, Type>, 2> candidates;
  llvm::SmallVector<ProtocolDecl *, 2> skippedProtocols;

  for (auto *protocol : LiteralProtocols) {
    if (auto defaultType = TypeChecker::getDefaultType(protocol, CS.DC)) {
      candidates.push_back({protocol, defaultType});
      continue;
    }

    // Looks like argument expected to conform to something like
    // `ExpressibleByNilLiteral` which doesn't have a default
    // type and as a result can't participate in minimalization.
    skippedProtocols.push_back(protocol);
  }

  if (candidates.size() <= 1)
    return;

  unsigned result = 0;
  for (unsigned i = 1, n = candidates.size(); i != n; ++i) {
    const auto &candidate = candidates[i];

    auto first =
        TypeChecker::conformsToProtocol(candidate.second, candidates[result].first,
                                        CS.DC);
    auto second =
        TypeChecker::conformsToProtocol(candidates[result].second, candidate.first,
                                        CS.DC);
    if (first.isInvalid() == second.isInvalid())
      return;

    if (!first.isInvalid())
      result = i;
  }

  LiteralProtocols.clear();
  LiteralProtocols.insert(candidates[result].first);
  LiteralProtocols.insert(skippedProtocols.begin(), skippedProtocols.end());
}

void ConstraintSystem::ArgumentInfoCollector::dump() const {
  auto &log = llvm::errs();
  log << "types:\n";
  for (auto type : Types)
    type->print(log);
  log << "\n";

  log << "literal protocols:\n";
  for (auto *proto : LiteralProtocols)
    proto->print(log);
  log << "\n";
}

// Check to see if we know something about the types of all arguments
// in the given function type.
bool ConstraintSystem::haveTypeInformationForAllArguments(
    FunctionType *fnType) {
  llvm::SetVector<Constraint *> literalConformsTo;
  return llvm::all_of(fnType->getParams(),
                      [&](AnyFunctionType::Param param) -> bool {
                        ArgumentInfoCollector argInfo(*this, param);
                        auto countFacts = argInfo.getTypes().size() +
                                          argInfo.getLiteralProtocols().size();
                        return countFacts > 0;
                      });
}

Optional<std::pair<Constraint *, unsigned>>
ConstraintSystem::findConstraintThroughOptionals(
    TypeVariableType *typeVar, OptionalWrappingDirection optionalDirection,
    llvm::function_ref<bool(Constraint *, TypeVariableType *)> predicate) {
  unsigned numOptionals = 0;
  auto *rep = getRepresentative(typeVar);

  SmallPtrSet<TypeVariableType *, 4> visitedVars;
  while (visitedVars.insert(rep).second) {
    // Look for a disjunction that binds this type variable to an overload set.
    TypeVariableType *optionalObjectTypeVar = nullptr;
    auto constraints = getConstraintGraph().gatherConstraints(
        rep, ConstraintGraph::GatheringKind::EquivalenceClass,
        [&](Constraint *match) {
          // If we have an "optional object of" constraint, we may need to
          // look through it to find the constraint we're looking for.
          if (match->getKind() != ConstraintKind::OptionalObject)
            return predicate(match, rep);

          switch (optionalDirection) {
          case OptionalWrappingDirection::Promote: {
            // We want to go from T to T?, so check if we're on the RHS, and
            // move over to the LHS if we can.
            auto rhsTypeVar = match->getSecondType()->getAs<TypeVariableType>();
            if (rhsTypeVar && getRepresentative(rhsTypeVar) == rep) {
              optionalObjectTypeVar =
                  match->getFirstType()->getAs<TypeVariableType>();
            }
            break;
          }
          case OptionalWrappingDirection::Unwrap: {
            // We want to go from T? to T, so check if we're on the LHS, and
            // move over to the RHS if we can.
            auto lhsTypeVar = match->getFirstType()->getAs<TypeVariableType>();
            if (lhsTypeVar && getRepresentative(lhsTypeVar) == rep) {
              optionalObjectTypeVar =
                  match->getSecondType()->getAs<TypeVariableType>();
            }
            break;
          }
          }
          // Don't include the optional constraint in the results.
          return false;
        });

    // If we found a result, return it.
    if (!constraints.empty())
      return std::make_pair(constraints[0], numOptionals);

    // If we found an "optional object of" constraint, follow it.
    if (optionalObjectTypeVar && !getFixedType(optionalObjectTypeVar)) {
      numOptionals += 1;
      rep = getRepresentative(optionalObjectTypeVar);
      continue;
    }

    // Otherwise we're done.
    return None;
  }
  return None;
}

Constraint *ConstraintSystem::getUnboundBindOverloadDisjunction(
    TypeVariableType *tyvar, unsigned *numOptionalUnwraps) {
  assert(!getFixedType(tyvar));
  auto result = findConstraintThroughOptionals(
      tyvar, OptionalWrappingDirection::Promote,
      [&](Constraint *match, TypeVariableType *currentRep) {
        // Check to see if we have a bind overload disjunction that binds the
        // type var we need.
        if (match->getKind() != ConstraintKind::Disjunction ||
            match->getNestedConstraints().front()->getKind() !=
                ConstraintKind::BindOverload)
          return false;

        auto lhsTy = match->getNestedConstraints().front()->getFirstType();
        auto *lhsTyVar = lhsTy->getAs<TypeVariableType>();
        return lhsTyVar && currentRep == getRepresentative(lhsTyVar);
      });
  if (!result)
    return nullptr;

  if (numOptionalUnwraps)
    *numOptionalUnwraps = result->second;

  return result->first;
}

// Find a disjunction associated with an ApplicableFunction constraint
// where we have some information about all of the types of in the
// function application (even if we only know something about what the
// types conform to and not actually a concrete type).
Constraint *ConstraintSystem::selectApplyDisjunction() {
  for (auto &constraint : InactiveConstraints) {
    if (constraint.getKind() != ConstraintKind::ApplicableFunction)
      continue;

    auto *applicable = &constraint;
    if (haveTypeInformationForAllArguments(
            applicable->getFirstType()->castTo<FunctionType>())) {
      auto *tyvar = applicable->getSecondType()->castTo<TypeVariableType>();

      // If we have created the disjunction for this apply, find it.
      auto *disjunction = getUnboundBindOverloadDisjunction(tyvar);
      if (disjunction)
        return disjunction;
    }
  }

  return nullptr;
}

static bool isOperatorBindOverload(Constraint *bindOverload) {
  if (bindOverload->getKind() != ConstraintKind::BindOverload)
    return false;

  auto choice = bindOverload->getOverloadChoice();
  if (!choice.isDecl())
    return false;

  auto *funcDecl = dyn_cast<FuncDecl>(choice.getDecl());
  return funcDecl && funcDecl->getOperatorDecl();
}

// Given a bind overload constraint for an operator, return the
// protocol designated as the first place to look for overloads of the
// operator.
static ArrayRef<NominalTypeDecl *>
getOperatorDesignatedNominalTypes(Constraint *bindOverload) {
  auto choice = bindOverload->getOverloadChoice();
  auto *funcDecl = cast<FuncDecl>(choice.getDecl());
  auto *operatorDecl = funcDecl->getOperatorDecl();
  return operatorDecl->getDesignatedNominalTypes();
}

void ConstraintSystem::sortDesignatedTypes(
    SmallVectorImpl<NominalTypeDecl *> &nominalTypes,
    Constraint *bindOverload) {
  auto *tyvar = bindOverload->getFirstType()->castTo<TypeVariableType>();
  auto applicableFns = getConstraintGraph().gatherConstraints(
      tyvar, ConstraintGraph::GatheringKind::EquivalenceClass,
      [](Constraint *match) {
        return match->getKind() == ConstraintKind::ApplicableFunction;
      });

  // FIXME: This is not true when we run the constraint optimizer.
  // assert(applicableFns.size() <= 1);

  // We have a disjunction for an operator but no application of it,
  // so it's being passed as an argument.
  if (applicableFns.size() == 0)
    return;

  // FIXME: We have more than one applicable per disjunction as a
  //        result of merging disjunction type variables. We may want
  //        to rip that out at some point.
  Constraint *foundApplicable = nullptr;
  SmallVector<Optional<Type>, 2> argumentTypes;
  for (auto *applicableFn : applicableFns) {
    argumentTypes.clear();
    auto *fnTy = applicableFn->getFirstType()->castTo<FunctionType>();
    ArgumentInfoCollector argInfo(*this, fnTy);
    // Stop if we hit anything with concrete types or conformances to
    // literals.
    if (!argInfo.getTypes().empty() || !argInfo.getLiteralProtocols().empty()) {
      foundApplicable = applicableFn;
      break;
    }
  }

  if (!foundApplicable)
    return;

  // FIXME: It would be good to avoid this redundancy.
  auto *fnTy = foundApplicable->getFirstType()->castTo<FunctionType>();
  ArgumentInfoCollector argInfo(*this, fnTy);

  size_t nextType = 0;
  for (auto argType : argInfo.getTypes()) {
    auto *nominal = argType->getAnyNominal();
    for (size_t i = nextType; i < nominalTypes.size(); ++i) {
      if (nominal == nominalTypes[i]) {
        std::swap(nominalTypes[nextType], nominalTypes[i]);
        ++nextType;
        break;
      } else if (auto *protoDecl = dyn_cast<ProtocolDecl>(nominalTypes[i])) {
        if (TypeChecker::conformsToProtocol(argType, protoDecl, DC)) {
          std::swap(nominalTypes[nextType], nominalTypes[i]);
          ++nextType;
          break;
        }
      }
    }
  }

  if (nextType + 1 >= nominalTypes.size())
    return;

  for (auto *protocol : argInfo.getLiteralProtocols()) {
    auto defaultType = TypeChecker::getDefaultType(protocol, DC);
    // ExpressibleByNilLiteral does not have a default type.
    if (!defaultType)
      continue;
    auto *nominal = defaultType->getAnyNominal();
    for (size_t i = nextType + 1; i < nominalTypes.size(); ++i) {
      if (nominal == nominalTypes[i]) {
        std::swap(nominalTypes[nextType], nominalTypes[i]);
        ++nextType;
        break;
      }
    }
  }
}

void ConstraintSystem::partitionForDesignatedTypes(
    ArrayRef<Constraint *> Choices, ConstraintMatchLoop forEachChoice,
    PartitionAppendCallback appendPartition) {

  auto types = getOperatorDesignatedNominalTypes(Choices[0]);
  if (types.empty())
    return;

  SmallVector<NominalTypeDecl *, 4> designatedNominalTypes(types.begin(),
                                                           types.end());

  if (designatedNominalTypes.size() > 1)
    sortDesignatedTypes(designatedNominalTypes, Choices[0]);

  SmallVector<SmallVector<unsigned, 4>, 4> definedInDesignatedType;
  SmallVector<SmallVector<unsigned, 4>, 4> definedInExtensionOfDesignatedType;

  auto examineConstraint =
    [&](unsigned constraintIndex, Constraint *constraint) -> bool {
    auto *decl = constraint->getOverloadChoice().getDecl();
    auto *funcDecl = cast<FuncDecl>(decl);

    auto *parentDC = funcDecl->getParent();
    auto *parentDecl = parentDC->getSelfNominalTypeDecl();

    // Skip anything not defined in a nominal type.
    if (!parentDecl)
      return false;

    for (auto designatedTypeIndex : indices(designatedNominalTypes)) {
      auto *designatedNominal =
        designatedNominalTypes[designatedTypeIndex];

      if (parentDecl != designatedNominal)
        continue;

      auto &constraints =
          isa<ExtensionDecl>(parentDC)
              ? definedInExtensionOfDesignatedType[designatedTypeIndex]
              : definedInDesignatedType[designatedTypeIndex];

      constraints.push_back(constraintIndex);
      return true;
    }

    return false;
  };

  definedInDesignatedType.resize(designatedNominalTypes.size());
  definedInExtensionOfDesignatedType.resize(designatedNominalTypes.size());

  forEachChoice(Choices, examineConstraint);

  // Now collect the overload choices that are defined within the type
  // that was designated in the operator declaration.
  // Add partitions for each of the overloads we found in types that
  // were designated as part of the operator declaration.
  for (auto designatedTypeIndex : indices(designatedNominalTypes)) {
    if (designatedTypeIndex < definedInDesignatedType.size()) {
      auto &primary = definedInDesignatedType[designatedTypeIndex];
      appendPartition(primary);
    }
    if (designatedTypeIndex < definedInExtensionOfDesignatedType.size()) {
      auto &secondary = definedInExtensionOfDesignatedType[designatedTypeIndex];
      appendPartition(secondary);
    }
  }
}

// Performance hack: if there are two generic overloads, and one is
// more specialized than the other, prefer the more-specialized one.
static Constraint *tryOptimizeGenericDisjunction(
                                          DeclContext *dc,
                                          ArrayRef<Constraint *> constraints) {
  llvm::SmallVector<Constraint *, 4> choices;
  for (auto *choice : constraints) {
    if (choices.size() > 2)
      return nullptr;

    if (!choice->isDisabled())
      choices.push_back(choice);
  }

  if (choices.size() != 2)
    return nullptr;

  if (choices[0]->getKind() != ConstraintKind::BindOverload ||
      choices[1]->getKind() != ConstraintKind::BindOverload ||
      choices[0]->isFavored() ||
      choices[1]->isFavored())
    return nullptr;

  OverloadChoice choiceA = choices[0]->getOverloadChoice();
  OverloadChoice choiceB = choices[1]->getOverloadChoice();

  if (!choiceA.isDecl() || !choiceB.isDecl())
    return nullptr;

  auto isViable = [](ValueDecl *decl) -> bool {
    assert(decl);

    auto *AFD = dyn_cast<AbstractFunctionDecl>(decl);
    if (!AFD || !AFD->isGeneric())
      return false;

    if (AFD->getAttrs().hasAttribute<DisfavoredOverloadAttr>())
      return false;

    auto funcType = AFD->getInterfaceType();
    auto hasAnyOrOptional = funcType.findIf([](Type type) -> bool {
      if (type->getOptionalObjectType())
        return true;

      return type->isAny();
    });

    // If function declaration references `Any` or an optional type,
    // let's not attempt it, because it's unclear
    // without solving which overload is going to be better.
    return !hasAnyOrOptional;
  };

  auto *declA = choiceA.getDecl();
  auto *declB = choiceB.getDecl();

  if (!isViable(declA) || !isViable(declB))
    return nullptr;

  switch (TypeChecker::compareDeclarations(dc, declA, declB)) {
  case Comparison::Better:
    return choices[0];

  case Comparison::Worse:
    return choices[1];

  case Comparison::Unordered:
    return nullptr;
  }
  llvm_unreachable("covered switch");
}

void ConstraintSystem::partitionDisjunction(
    ArrayRef<Constraint *> Choices, SmallVectorImpl<unsigned> &Ordering,
    SmallVectorImpl<unsigned> &PartitionBeginning) {
  // Apply a special-case rule for favoring one generic function over
  // another.
  if (auto favored = tryOptimizeGenericDisjunction(DC, Choices)) {
    favorConstraint(favored);
  }

  SmallSet<Constraint *, 16> taken;

  // Local function used to iterate over the untaken choices from the
  // disjunction and use a higher-order function to determine if they
  // should be part of a partition.
  ConstraintMatchLoop forEachChoice =
      [&](ArrayRef<Constraint *>,
          std::function<bool(unsigned index, Constraint *)> fn) {
        for (auto index : indices(Choices)) {
          auto *constraint = Choices[index];
          if (taken.count(constraint))
            continue;

          if (fn(index, constraint))
            taken.insert(constraint);
        }
      };

  // First collect some things that we'll generally put near the beginning or
  // end of the partitioning.

  SmallVector<unsigned, 4> favored;
  SmallVector<unsigned, 4> simdOperators;
  SmallVector<unsigned, 4> disabled;
  SmallVector<unsigned, 4> unavailable;

  // First collect disabled and favored constraints.
  forEachChoice(Choices, [&](unsigned index, Constraint *constraint) -> bool {
    if (constraint->isDisabled()) {
      disabled.push_back(index);
      return true;
    }

    if (constraint->isFavored()) {
      favored.push_back(index);
      return true;
    }

    return false;
  });

  // Then unavailable constraints if we're skipping them.
  if (!shouldAttemptFixes()) {
    forEachChoice(Choices, [&](unsigned index, Constraint *constraint) -> bool {
      if (constraint->getKind() != ConstraintKind::BindOverload)
        return false;

      auto *decl = constraint->getOverloadChoice().getDeclOrNull();
      auto *funcDecl = dyn_cast_or_null<FuncDecl>(decl);
      if (!funcDecl)
        return false;

      if (!isDeclUnavailable(funcDecl, constraint->getLocator()))
        return false;

      unavailable.push_back(index);
      return true;
    });
  }

  // Partition SIMD operators.
  if (!getASTContext().TypeCheckerOpts.SolverEnableOperatorDesignatedTypes &&
      isOperatorBindOverload(Choices[0])) {
    forEachChoice(Choices, [&](unsigned index, Constraint *constraint) -> bool {
      if (!isOperatorBindOverload(constraint))
        return false;

      if (isSIMDOperator(constraint->getOverloadChoice().getDecl())) {
        simdOperators.push_back(index);
        return true;
      }

      return false;
    });
  }

  // Local function to create the next partition based on the options
  // passed in.
  PartitionAppendCallback appendPartition =
      [&](SmallVectorImpl<unsigned> &options) {
        if (options.size()) {
          PartitionBeginning.push_back(Ordering.size());
          Ordering.insert(Ordering.end(), options.begin(), options.end());
        }
      };

  if (getASTContext().TypeCheckerOpts.SolverEnableOperatorDesignatedTypes &&
      isOperatorBindOverload(Choices[0])) {
    partitionForDesignatedTypes(Choices, forEachChoice, appendPartition);
  }

  SmallVector<unsigned, 4> everythingElse;
  // Gather the remaining options.
  forEachChoice(Choices, [&](unsigned index, Constraint *constraint) -> bool {
    everythingElse.push_back(index);
    return true;
  });
  appendPartition(favored);
  appendPartition(everythingElse);
  appendPartition(simdOperators);

  // Now create the remaining partitions from what we previously collected.
  appendPartition(unavailable);
  appendPartition(disabled);

  assert(Ordering.size() == Choices.size());
}

Constraint *ConstraintSystem::selectDisjunction() {
  SmallVector<Constraint *, 4> disjunctions;

  collectDisjunctions(disjunctions);
  if (disjunctions.empty())
    return nullptr;

  // Attempt apply disjunctions first. When we have operators with
  // designated types, this is important, because it allows us to
  // select all the preferred operator overloads prior to other
  // disjunctions that we may not be able to short-circuit, allowing
  // us to eliminate behavior that is exponential in the number of
  // operators in the expression.
  if (getASTContext().TypeCheckerOpts.SolverEnableOperatorDesignatedTypes) {
    if (auto *disjunction = selectApplyDisjunction())
      return disjunction;
  }

  if (auto *disjunction = selectBestBindingDisjunction(*this, disjunctions))
    return disjunction;

  // Pick the disjunction with the smallest number of active choices.
  auto minDisjunction =
      std::min_element(disjunctions.begin(), disjunctions.end(),
                       [&](Constraint *first, Constraint *second) -> bool {
                         return first->countActiveNestedConstraints() <
                                second->countActiveNestedConstraints();
                       });

  if (minDisjunction != disjunctions.end())
    return *minDisjunction;

  return nullptr;
}

bool DisjunctionChoice::attempt(ConstraintSystem &cs) const {
  cs.simplifyDisjunctionChoice(Choice);

  if (ExplicitConversion)
    propagateConversionInfo(cs);

  // Attempt to simplify current choice might result in
  // immediate failure, which is recorded in constraint system.
  return !cs.failedConstraint && !cs.simplify();
}

bool DisjunctionChoice::isGenericOperator() const {
  auto *decl = getOperatorDecl(Choice);
  if (!decl)
    return false;

  auto interfaceType = decl->getInterfaceType();
  return interfaceType->is<GenericFunctionType>();
}

bool DisjunctionChoice::isSymmetricOperator() const {
  auto *decl = getOperatorDecl(Choice);
  if (!decl)
    return false;

  auto func = dyn_cast<FuncDecl>(decl);
  auto paramList = func->getParameters();
  if (paramList->size() != 2)
    return true;

  auto firstType = paramList->get(0)->getInterfaceType();
  auto secondType = paramList->get(1)->getInterfaceType();
  return firstType->isEqual(secondType);
}

void DisjunctionChoice::propagateConversionInfo(ConstraintSystem &cs) const {
  assert(ExplicitConversion);

  auto LHS = Choice->getFirstType();
  auto typeVar = LHS->getAs<TypeVariableType>();
  if (!typeVar)
    return;

  // Use the representative (if any) to lookup constraints
  // and potentially bind the coercion type to.
  typeVar = typeVar->getImpl().getRepresentative(nullptr);

  // If the representative already has a type assigned to it
  // we can't really do anything here.
  if (typeVar->getImpl().getFixedType(nullptr))
    return;

  auto bindings = cs.inferBindingsFor(typeVar);
  if (bindings.InvolvesTypeVariables || bindings.Bindings.size() != 1)
    return;

  auto conversionType = bindings.Bindings[0].BindingType;
  auto constraints = cs.CG.gatherConstraints(
      typeVar,
      ConstraintGraph::GatheringKind::EquivalenceClass,
      [](Constraint *constraint) -> bool {
        switch (constraint->getKind()) {
        case ConstraintKind::Conversion:
        case ConstraintKind::Defaultable:
        case ConstraintKind::ConformsTo:
        case ConstraintKind::LiteralConformsTo:
          return false;

        default:
          return true;
        }
      });

  if (constraints.empty())
    cs.addConstraint(ConstraintKind::Bind, typeVar, conversionType,
                     Choice->getLocator());
}
