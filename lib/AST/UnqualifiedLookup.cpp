//===--- UnqualifiedLookup.cpp - Swift Name Lookup Routines ---------------===//
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
///
/// This file implements unqualified lookup, which searches for an identifier
/// from a given context.
///
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/DebuggerClient.h"
#include "swift/AST/ImportCache.h"
#include "swift/AST/ModuleNameLookup.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/NameLookupRequests.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Statistic.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "namelookup"

using namespace swift;
using namespace swift::namelookup;

namespace {
  class UnqualifiedLookupFactory {

    friend class ASTScopeDeclConsumerForUnqualifiedLookup;

  public:
    using Flags = UnqualifiedLookupFlags;
    using Options = UnqualifiedLookupOptions;
    using ResultsVector = SmallVector<LookupResultEntry, 4>;
    
  private:
    /// Finds lookup results based on the types that self conforms to.
    /// For instance, self always conforms to a struct, enum or class.
    /// But in addition, self could conform to any number of protocols.
    /// For example, when there's a protocol extension, e.g. extension P where
    /// self: P2, self also conforms to P2 so P2 must be searched.
    class ResultFinderForTypeContext {
      UnqualifiedLookupFactory *const factory;
      /// Nontypes are formally members of the base type, i.e. the dynamic type
      /// of the activation record.
      DeclContext *const dynamicContext;
      /// Types are formally members of the metatype, i.e. the static type of the
      /// activation record.
      DeclContext *const staticContext;
      using SelfBounds = SmallVector<NominalTypeDecl *, 2>;
      SelfBounds selfBounds;
      
    public:
      /// \p staticContext is also the context from which to derive the self types
      ResultFinderForTypeContext(UnqualifiedLookupFactory *factory,
                                 DeclContext *dynamicContext,
                                 DeclContext *staticContext);
      
      SWIFT_DEBUG_DUMP;
      
    private:
      SelfBounds findSelfBounds(DeclContext *dc);
      
      // Classify this declaration.
      // Types are formally members of the metatype.
      DeclContext *whereValueIsMember(const ValueDecl *const member) const {
        return isa<TypeDecl>(member) ? staticContext : dynamicContext;
      }
      
    public:
      /// Do the lookups and add matches to results.
      void findResults(const DeclNameRef &Name, bool isCascadingUse,
                       NLOptions baseNLOptions, DeclContext *contextForLookup,
                       SmallVectorImpl<LookupResultEntry> &results) const;
    };
    
    enum class AddGenericParameters { Yes, No };

#ifndef NDEBUG
    /// A consumer for debugging that lets the UnqualifiedLookupFactory know when
    /// finding something.
    class InstrumentedNamedDeclConsumer : public NamedDeclConsumer {
      virtual void anchor() override;
      UnqualifiedLookupFactory *factory;
      
    public:
      InstrumentedNamedDeclConsumer(UnqualifiedLookupFactory *factory,
                                    DeclNameRef name,
                                    SmallVectorImpl<LookupResultEntry> &results,
                                    bool isTypeLookup)
      : NamedDeclConsumer(name, results, isTypeLookup), factory(factory) {}
      
      virtual void foundDecl(ValueDecl *VD, DeclVisibilityKind Reason,
                             DynamicLookupInfo dynamicLookupInfo = {}) override {
        unsigned before = results.size();
        NamedDeclConsumer::foundDecl(VD, Reason, dynamicLookupInfo);
        unsigned after = results.size();
        if (after > before)
          factory->addedResult(results.back());
      }
    };
#endif
    // Inputs
    const DeclNameRef Name;
    DeclContext *const DC;
    ModuleDecl &M;
    const ASTContext &Ctx;
    const SourceLoc Loc;
    const SourceManager &SM;
    
    /// Used to find the file-local names.
    DebuggerClient *const DebugClient;
    
    const Options options;
    const bool isOriginallyTypeLookup;
    const NLOptions baseNLOptions;
    // Transputs
#ifndef NDEBUG
    InstrumentedNamedDeclConsumer Consumer;
#else
    NamedDeclConsumer Consumer;
#endif
    // Outputs
    SmallVectorImpl<LookupResultEntry> &Results;
    size_t &IndexOfFirstOuterResult;
    ResultsVector UnavailableInnerResults;

#ifndef NDEBUG
    static unsigned lookupCounter;
    static const unsigned targetLookup;
#endif

  public: // for exp debugging
    unsigned resultsSizeBeforeLocalsPass = ~0;

  public:
    // clang-format off
    UnqualifiedLookupFactory(DeclNameRef Name,
                             DeclContext *const DC,
                             SourceLoc Loc,
                             Options options,
                             SmallVectorImpl<LookupResultEntry> &Results,
                             size_t &IndexOfFirstOuterResult);
    // clang-format on
    
    void performUnqualifiedLookup();
    
  private:
    void lookUpTopLevelNamesInModuleScopeContext(DeclContext *);

    void lookInASTScopes();

    /// Can lookup stop searching for results, assuming hasn't looked for outer
    /// results yet?
    bool isFirstResultEnough() const;
    
    /// Every time lookup finishes searching a scope, call me
    /// to record the dividing line between results from first fruitful scope and
    /// the result.
    void recordCompletionOfAScope();

#pragma mark context-based lookup declarations

    bool isOutsideBodyOfFunction(const AbstractFunctionDecl *const AFD) const;
    
    /// For diagnostic purposes, move aside the unavailables, and put
    /// them back as a last-ditch effort.
    /// Could be cleaner someday with a richer interface to UnqualifiedLookup.
    void setAsideUnavailableResults(size_t firstPossiblyUnavailableResult);
    
    void addImportedResults(DeclContext *const dc);
    
    void addNamesKnownToDebugClient(DeclContext *dc);
    
    void addUnavailableInnerResults();
    
    void lookForAModuleWithTheGivenName(DeclContext *const dc);
    
#pragma mark common helper declarations
    static NLOptions
    computeBaseNLOptions(const UnqualifiedLookupOptions options,
                         const bool isOriginallyTypeLookup);

    Optional<bool> getInitialIsCascadingUse() const {
      return options.contains(Flags::KnownPrivate) ? Optional<bool>(false)
                                                   : None;
    }

    void findResultsAndSaveUnavailables(
                                        DeclContext *lookupContextForThisContext,
                                        ResultFinderForTypeContext &&resultFinderForTypeContext,
                                        bool isCascadingUse, NLOptions baseNLOptions);
    
  public:
    SWIFT_DEBUG_DUMP;
    SWIFT_DEBUG_DUMPER(dumpResults());
    SWIFT_DEBUG_DUMPER(dumpScopes());

    void printScopes(raw_ostream &OS) const;
    void print(raw_ostream &OS) const;
    void printResults(raw_ostream &OS) const;

#ifndef NDEBUG
    bool isTargetLookup() const;
    void stopForDebuggingIfStartingTargetLookup(bool isASTScopeLookup) const;
    void stopForDebuggingIfDuringTargetLookup(bool isASTScopeLookup) const;
    void
    stopForDebuggingIfAddingTargetLookupResult(const LookupResultEntry &) const;
    void addedResult(const LookupResultEntry &) const;
#endif
  };

} // namespace

namespace {
  /// Used to gather lookup results
class ASTScopeDeclConsumerForUnqualifiedLookup
    : public AbstractASTScopeDeclConsumer {
  UnqualifiedLookupFactory &factory;

  /// The 'self' parameter from the innermost scope containing the lookup
  /// location to be used when an instance member of a type is accessed,
  /// or nullptr if instance members should not be 'self' qualified.
  DeclContext *candidateSelfDC;

public:
  ASTScopeDeclConsumerForUnqualifiedLookup(UnqualifiedLookupFactory &factory)
      : factory(factory), candidateSelfDC(nullptr) {}

  virtual ~ASTScopeDeclConsumerForUnqualifiedLookup() = default;

  void maybeUpdateSelfDC(VarDecl *var);

  bool consume(ArrayRef<ValueDecl *> values, DeclVisibilityKind vis,
               NullablePtr<DeclContext> baseDC = nullptr) override;

  /// returns true if finished and new value for isCascadingUse
  bool lookInMembers(DeclContext *const scopeDC, NominalTypeDecl *const nominal,
                     function_ref<bool(Optional<bool>)>) override;

#ifndef NDEBUG
  void startingNextLookupStep() override {
    factory.stopForDebuggingIfDuringTargetLookup(true);
  }
  bool isTargetLookup() const override { return factory.isTargetLookup(); }

  void finishingLookup(std::string msg) const override {
    if (isTargetLookup())
      llvm::errs() << "Finishing lookup: " << msg << "\n";
  }
#endif
  };
} // namespace

#pragma mark UnqualifiedLookupFactory functions

// clang-format off
UnqualifiedLookupFactory::UnqualifiedLookupFactory(
                            DeclNameRef Name,
                            DeclContext *const DC,
                            SourceLoc Loc,
                            Options options,
                            SmallVectorImpl<LookupResultEntry> &Results,
                            size_t &IndexOfFirstOuterResult)
:
  Name(Name),
  DC(DC),
  M(*DC->getParentModule()),
  Ctx(M.getASTContext()),
  Loc(Loc),
  SM(Ctx.SourceMgr),
  DebugClient(M.getDebugClient()),
  options(options),
  isOriginallyTypeLookup(options.contains(Flags::TypeLookup)),
  baseNLOptions(computeBaseNLOptions(options, isOriginallyTypeLookup)),
  #ifdef NDEBUG
  Consumer(Name, Results, isOriginallyTypeLookup),
  #else
  Consumer(this, Name, Results, isOriginallyTypeLookup),
  #endif
  Results(Results),
  IndexOfFirstOuterResult(IndexOfFirstOuterResult)
{}
// clang-format on

void UnqualifiedLookupFactory::performUnqualifiedLookup() {
#ifndef NDEBUG
  ++lookupCounter;
  auto localCounter = lookupCounter;
  (void)localCounter; // for debugging
#endif
  FrontendStatsTracer StatsTracer(Ctx.Stats,
                                  "performUnqualifedLookup",
                                  DC->getParentSourceFile());

  if (Loc.isValid()) {
    lookInASTScopes();
  } else {
    assert(DC->isModuleScopeContext() &&
           "Unqualified lookup without a source location must start from "
           "a module-scope context");

#ifndef NDEBUG
    stopForDebuggingIfStartingTargetLookup(false);
#endif
  }

  recordCompletionOfAScope();
  if (!isFirstResultEnough()) {
    // If no result has been found yet, the dependency must be on a top-level
    // name, since up to now, the search has been for non-top-level names.
    auto *moduleScopeContext = DC->getModuleScopeContext();
    lookUpTopLevelNamesInModuleScopeContext(moduleScopeContext);
  }
}

void UnqualifiedLookupFactory::lookUpTopLevelNamesInModuleScopeContext(
    DeclContext *DC) {
  // TODO: Does the debugger client care about compound names?
  if (Name.isSimpleName() && !Name.isSpecial() && DebugClient &&
      DebugClient->lookupOverrides(Name.getBaseName(), DC, Loc,
                                   isOriginallyTypeLookup, Results))
    return;

  addImportedResults(DC);
  addNamesKnownToDebugClient(DC);
  if (Results.empty()) {
    // If we still haven't found anything, but we do have some
    // declarations that are "unavailable in the current Swift", drop
    // those in.
    addUnavailableInnerResults();
    if (Results.empty())
      lookForAModuleWithTheGivenName(DC);
  }
  recordCompletionOfAScope();
}

#pragma mark context-based lookup definitions

bool UnqualifiedLookupFactory::isOutsideBodyOfFunction(
    const AbstractFunctionDecl *const AFD) const {
  return !AFD->isImplicit() && Loc.isValid() &&
         AFD->getBodySourceRange().isValid() &&
         !SM.rangeContainsTokenLoc(AFD->getBodySourceRange(), Loc);
}

void UnqualifiedLookupFactory::ResultFinderForTypeContext::findResults(
    const DeclNameRef &Name, bool isCascadingUse, NLOptions baseNLOptions,
    DeclContext *contextForLookup,
    SmallVectorImpl<LookupResultEntry> &results) const {
  // An optimization:
  if (selfBounds.empty())
    return;
  const NLOptions options =
      baseNLOptions | (isCascadingUse ? NL_KnownCascadingDependency
                                      : NL_KnownNonCascadingDependency);

  SmallVector<ValueDecl *, 4> Lookup;
  contextForLookup->lookupQualified(selfBounds, Name, options, Lookup);
  for (auto Result : Lookup) {
    results.push_back(LookupResultEntry(whereValueIsMember(Result), Result));
#ifndef NDEBUG
    factory->addedResult(results.back());
#endif
  }
}

// TODO (someday): Instead of adding unavailable entries to Results,
// then later shunting them aside, just put them in the right place
// to begin with.

void UnqualifiedLookupFactory::setAsideUnavailableResults(
    const size_t firstPossiblyUnavailableResult) {
  // An optimization:
  assert(Results.size() >= firstPossiblyUnavailableResult);
  if (Results.size() == firstPossiblyUnavailableResult)
    return;
  // Predicate that determines whether a lookup result should
  // be unavailable except as a last-ditch effort.
  auto unavailableLookupResult = [&](const LookupResultEntry &result) {
    auto &effectiveVersion = Ctx.LangOpts.EffectiveLanguageVersion;
    return result.getValueDecl()->getAttrs().isUnavailableInSwiftVersion(
        effectiveVersion);
  };

  // If all of the results we found are unavailable, keep looking.
  auto begin = Results.begin() + firstPossiblyUnavailableResult;
  if (std::all_of(begin, Results.end(), unavailableLookupResult)) {
    // better to have more structure in results
    UnavailableInnerResults.append(begin, Results.end());
    Results.erase(begin, Results.end());
    return;
  }
  // The debugger may have a different private discriminator
  // in order to support lookup relative to the place where
  // execution is suspended.
  filterForDiscriminator(Results, DebugClient);
}

void UnqualifiedLookupFactory::addImportedResults(DeclContext *const dc) {
  using namespace namelookup;
  SmallVector<ValueDecl *, 8> CurModuleResults;
  auto resolutionKind = isOriginallyTypeLookup ? ResolutionKind::TypesOnly
                                               : ResolutionKind::Overloadable;
  lookupInModule(dc, Name.getFullName(), CurModuleResults, NLKind::UnqualifiedLookup,
                 resolutionKind, dc);

  // Always perform name shadowing for type lookup.
  if (options.contains(Flags::TypeLookup)) {
    removeShadowedDecls(CurModuleResults, dc);
  }

  for (auto VD : CurModuleResults) {
    Results.push_back(LookupResultEntry(VD));
#ifndef NDEBUG
    addedResult(Results.back());
#endif
  }

  filterForDiscriminator(Results, DebugClient);
}

void UnqualifiedLookupFactory::addNamesKnownToDebugClient(DeclContext *dc) {
  if (Name.isSimpleName() && DebugClient)
    DebugClient->lookupAdditions(Name.getBaseName(), dc, Loc,
                                 isOriginallyTypeLookup, Results);
}

void UnqualifiedLookupFactory::addUnavailableInnerResults() {
  Results = std::move(UnavailableInnerResults);
}

void UnqualifiedLookupFactory::lookForAModuleWithTheGivenName(
    DeclContext *const dc) {
  using namespace namelookup;
  if (!Name.isSimpleName() || Name.isSpecial())
    return;

  // Look for a module with the given name.
  if (Name.isSimpleName(M.getName())) {
    Results.push_back(LookupResultEntry(&M));
#ifndef NDEBUG
    addedResult(Results.back());
#endif
    return;
  }
  ModuleDecl *desiredModule = Ctx.getLoadedModule(Name.getBaseIdentifier());
  if (!desiredModule && Name.getFullName() == Ctx.TheBuiltinModule->getName())
    desiredModule = Ctx.TheBuiltinModule;
  if (desiredModule) {
    // Make sure the desired module is actually visible from the current
    // context.
    if (Ctx.getImportCache().isImportedBy(desiredModule, dc)) {
      Results.push_back(LookupResultEntry(desiredModule));
#ifndef NDEBUG
      addedResult(Results.back());
#endif
    }
  }
}

#pragma mark common helper definitions


void UnqualifiedLookupFactory::findResultsAndSaveUnavailables(
       DeclContext *lookupContextForThisContext,
       ResultFinderForTypeContext &&resultFinderForTypeContext,
       bool isCascadingUse, NLOptions baseNLOptions) {
  auto firstPossiblyUnavailableResult = Results.size();
  resultFinderForTypeContext.findResults(Name, isCascadingUse, baseNLOptions,
                                         lookupContextForThisContext, Results);
  setAsideUnavailableResults(firstPossiblyUnavailableResult);
}


NLOptions UnqualifiedLookupFactory::computeBaseNLOptions(
    const UnqualifiedLookupOptions options,
    const bool isOriginallyTypeLookup) {
  NLOptions baseNLOptions = NL_UnqualifiedDefault;
  if (options.contains(Flags::AllowProtocolMembers))
    baseNLOptions |= NL_ProtocolMembers;
  if (isOriginallyTypeLookup)
    baseNLOptions |= NL_OnlyTypes;
  if (options.contains(Flags::IgnoreAccessControl))
    baseNLOptions |= NL_IgnoreAccessControl;
  return baseNLOptions;
}

bool UnqualifiedLookupFactory::isFirstResultEnough()
    const {
  return !Results.empty() && !options.contains(Flags::IncludeOuterResults);
}

void UnqualifiedLookupFactory::recordCompletionOfAScope() {
  // OK to call (NOOP) if there are more inner results and Results is empty
  if (IndexOfFirstOuterResult == 0)
    IndexOfFirstOuterResult = Results.size();
}

UnqualifiedLookupFactory::ResultFinderForTypeContext::
    ResultFinderForTypeContext(UnqualifiedLookupFactory *factory,
                               DeclContext *dynamicContext,
                               DeclContext *staticContext)
    : factory(factory),
      dynamicContext(dynamicContext), staticContext(staticContext),
      selfBounds(findSelfBounds(staticContext)) {}

UnqualifiedLookupFactory::ResultFinderForTypeContext::SelfBounds
UnqualifiedLookupFactory::ResultFinderForTypeContext::findSelfBounds(
    DeclContext *dc) {
  auto nominal = dc->getSelfNominalTypeDecl();
  if (!nominal)
    return {};

  SelfBounds selfBounds;
  selfBounds.push_back(nominal);

  // For a protocol extension, check whether there are additional "Self"
  // constraints that can affect name lookup.
  if (dc->getExtendedProtocolDecl()) {
    auto ext = cast<ExtensionDecl>(dc);
    auto bounds = getSelfBoundsFromWhereClause(ext);
    for (auto bound : bounds.decls)
      selfBounds.push_back(bound);
  }
  return selfBounds;
}

#pragma mark ASTScopeImpl support

void UnqualifiedLookupFactory::lookInASTScopes() {

  ASTScopeDeclConsumerForUnqualifiedLookup consumer(*this);

#ifndef NDEBUG
  stopForDebuggingIfStartingTargetLookup(true);
#endif

  ASTScope::unqualifiedLookup(DC->getParentSourceFile(),
                              Name, Loc, consumer);
}

void ASTScopeDeclConsumerForUnqualifiedLookup::maybeUpdateSelfDC(
    VarDecl *var) {
  // We have a binding named 'self'.
  //
  // There are three possibilities:
  //
  // 1) This binding is the 'self' parameter of a method,
  // 2) This binding is a bona-fide 'self' capture, meaning a capture
  //    list entry named 'self' with initial value expression 'self',
  // 3) None of the above.
  //
  // How we handle these cases depends on whether we've already seen
  // another 'self' binding.
  if (candidateSelfDC == nullptr) {
    // We haven't seen one yet, so record it.
    if (var->isSelfParameter())
      candidateSelfDC = var->getDeclContext();
    else if (var->isSelfParamCapture())
      candidateSelfDC = var->getParentCaptureList()->getClosureBody();
  } else {
    // If we see a binding named 'self' that is not a bona-fide
    // 'self', we have to forget about the previous 'self' capture
    // because it's not going to be the right one for accessing
    // instance members of the innermost nominal type. Eg,
    //
    // class C {
    //   func bar() {}
    //   func foo() {
    //     _ { [self=12] { [self] bar() } }
    //   }
    // }
    //
    // Instead, we're going to move on and look for the next-innermost
    // 'self' binding.
    if (!var->isSelfParameter() &&
        !var->isSelfParamCapture())
      candidateSelfDC = nullptr;
  }
}

bool ASTScopeDeclConsumerForUnqualifiedLookup::consume(
    ArrayRef<ValueDecl *> values, DeclVisibilityKind vis,
    NullablePtr<DeclContext> baseDC) {
  for (auto *value: values) {
    if (factory.isOriginallyTypeLookup && !isa<TypeDecl>(value))
      continue;

    // Try to resolve the base for unqualified instance member
    // references. This is used by lookInMembers().
    if (auto *var = dyn_cast<VarDecl>(value)) {
      if (var->getName() == factory.Ctx.Id_self) {
        maybeUpdateSelfDC(var);
      }
    }

    if (!value->getName().matchesRef(factory.Name.getFullName()))
      continue;

    // In order to preserve the behavior of the existing context-based lookup,
    // which finds all results for non-local variables at the top level instead
    // of stopping at the first one, ignore results at the top level that are
    // not local variables. The caller \c lookInASTScopes will
    // then do the appropriate work when the scope lookup fails. In
    // FindLocalVal::visitBraceStmt, it sees PatternBindingDecls, not VarDecls,
    // so a VarDecl at top level would not be found by the context-based lookup.
    if (isa<SourceFile>(value->getDeclContext()) &&
        (vis != DeclVisibilityKind::LocalVariable || isa<VarDecl>(value)))
      return false;

    factory.Results.push_back(LookupResultEntry(value));
#ifndef NDEBUG
    factory.stopForDebuggingIfAddingTargetLookupResult(factory.Results.back());
#endif
  }
  factory.recordCompletionOfAScope();
  return factory.isFirstResultEnough();
}

bool ASTScopeDeclGatherer::consume(ArrayRef<ValueDecl *> valuesArg,
                                   DeclVisibilityKind,
                                   NullablePtr<DeclContext>) {
  for (auto *v: valuesArg)
    values.push_back(v);
  return false;
}

// TODO: in future, migrate this functionality into ASTScopes
bool ASTScopeDeclConsumerForUnqualifiedLookup::lookInMembers(
    DeclContext *const scopeDC,
    NominalTypeDecl *const nominal,
    function_ref<bool(Optional<bool>)> calculateIsCascadingUse) {
  if (candidateSelfDC) {
    if (auto *afd = dyn_cast<AbstractFunctionDecl>(candidateSelfDC)) {
      assert(!factory.isOutsideBodyOfFunction(afd) && "Should be inside");
    }
  }

  // We're looking for members of a type.
  //
  // If we started the looking from inside a scope where a 'self' parameter
  // is visible, instance members are returned with the 'self' parameter's
  // DeclContext as the base, which is how the expression checker knows to
  // convert the unqualified reference into a self member access.
  auto resultFinder = UnqualifiedLookupFactory::ResultFinderForTypeContext(
      &factory, candidateSelfDC ? candidateSelfDC : scopeDC, scopeDC);
  const bool isCascadingUse =
      calculateIsCascadingUse(factory.getInitialIsCascadingUse());
  factory.findResultsAndSaveUnavailables(scopeDC, std::move(resultFinder),
                                         isCascadingUse, factory.baseNLOptions);
  factory.recordCompletionOfAScope();

  // We're done looking inside a nominal type declaration. It is possible
  // that this nominal type is nested inside of another type, in which case
  // we will visit the outer type next. Make sure to clear out the known
  // 'self' parameeter context, since any members of the outer type are
  // not accessed via the innermost 'self' parameter.
  candidateSelfDC = nullptr;

  return factory.isFirstResultEnough();
}

LookupResult
UnqualifiedLookupRequest::evaluate(Evaluator &evaluator,
                                   UnqualifiedLookupDescriptor desc) const {
  SmallVector<LookupResultEntry, 4> results;
  size_t indexOfFirstOuterResult = 0;
  UnqualifiedLookupFactory factory(desc.Name, desc.DC, desc.Loc, desc.Options,
                                   results, indexOfFirstOuterResult);
  factory.performUnqualifiedLookup();
  return LookupResult(results, indexOfFirstOuterResult);
}

#pragma mark debugging
#ifndef NDEBUG
void UnqualifiedLookupFactory::InstrumentedNamedDeclConsumer::anchor() {}
#endif

void UnqualifiedLookupFactory::ResultFinderForTypeContext::dump() const {
  (void)factory;
  llvm::errs() << "dynamicContext: ";
  dynamicContext->dumpContext();
  llvm::errs() << "staticContext: ";
  staticContext->dumpContext();
  llvm::errs() << "selfBounds: ";
  for (const auto *D : selfBounds)
    D->dump(llvm::errs(), 1);
  llvm::errs() << "\n";
}

void UnqualifiedLookupFactory::dump() const { print(llvm::errs()); }
void UnqualifiedLookupFactory::dumpScopes() const { printScopes(llvm::errs()); }
void UnqualifiedLookupFactory::dumpResults() const { printResults(llvm::errs()); }

void UnqualifiedLookupFactory::printScopes(raw_ostream &out) const {
  out << "\n\nScopes:\n";
  DC->getParentSourceFile()->getScope().print(out);
  out << "\n";
}

void UnqualifiedLookupFactory::printResults(raw_ostream &out) const {
  for (auto i : indices(Results)) {
    if (i == resultsSizeBeforeLocalsPass)
      out << "============== next pass ============\n";
    out << i << ": ";
    Results[i].print(out);
    out << "\n";
  }
  if (resultsSizeBeforeLocalsPass == Results.size())
    out << "============== next pass ============\n";
  if (resultsSizeBeforeLocalsPass == ~0u)
    out << "never tried locals\n\n";
}

void UnqualifiedLookupFactory::print(raw_ostream &OS) const {
  OS << "Look up";
#ifndef NDEBUG
  OS << " (" << lookupCounter << ")";
#endif
  OS << " '" << Name << "' at: ";
  Loc.print(OS, DC->getASTContext().SourceMgr);
  OS << "\nStarting in: ";
  DC->printContext(OS);
  OS << "\n";
}

#pragma mark breakpointing
#ifndef NDEBUG

bool UnqualifiedLookupFactory::isTargetLookup() const {
  return lookupCounter == targetLookup;
}

void UnqualifiedLookupFactory::stopForDebuggingIfStartingTargetLookup(
    const bool isASTScopeLookup) const {
  if (!isTargetLookup())
    return;
  if (isASTScopeLookup)
    llvm::errs() << "starting target ASTScopeImpl lookup\n";
  else
    llvm::errs() << "starting target context-based lookup\n";
}

void UnqualifiedLookupFactory::stopForDebuggingIfDuringTargetLookup(
    const bool isASTScopeLookup) const {
  if (!isTargetLookup())
    return;
  if (isASTScopeLookup)
    llvm::errs() << "during target ASTScopeImpl lookup\n";
  else
    llvm::errs() << "during target context-based lookup\n";
}

void UnqualifiedLookupFactory::stopForDebuggingIfAddingTargetLookupResult(
    const LookupResultEntry &e) const {
  if (!isTargetLookup())
    return;
  auto &out = llvm::errs();
  out << "\nresult for Target lookup:\n";
  e.print(out);
  out << "\n";
}

void UnqualifiedLookupFactory::addedResult(const LookupResultEntry &e) const {
  stopForDebuggingIfAddingTargetLookupResult(e);
}

unsigned UnqualifiedLookupFactory::lookupCounter = 0;

// set to ~0 when not debugging
const unsigned UnqualifiedLookupFactory::targetLookup = ~0;

#endif // NDEBUG
