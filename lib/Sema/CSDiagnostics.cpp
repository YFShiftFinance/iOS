//===--- CSDiagnostics.cpp - Constraint Diagnostics -----------------------===//
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
// This file implements diagnostics for constraint system.
//
//===----------------------------------------------------------------------===//

#include "CSDiagnostics.h"
#include "ConstraintSystem.h"
#include "MiscDiagnostics.h"
#include "TypeCheckProtocol.h"
#include "TypoCorrection.h"
#include "swift/Sema/IDETypeChecking.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/Decl.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/ProtocolConformanceRef.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Types.h"
#include "swift/Basic/SourceLoc.h"
#include "swift/Parse/Lexer.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SmallString.h"
#include <string>

using namespace swift;
using namespace constraints;

static bool hasFixFor(const Solution &solution, ConstraintLocator *locator) {
  return llvm::any_of(solution.Fixes, [&locator](const ConstraintFix *fix) {
    return fix->getLocator() == locator;
  });
}

FailureDiagnostic::~FailureDiagnostic() {}

bool FailureDiagnostic::diagnose(bool asNote) {
  return asNote ? diagnoseAsNote() : diagnoseAsError();
}

bool FailureDiagnostic::diagnoseAsNote() {
  return false;
}

ASTNode FailureDiagnostic::getAnchor() const {
  auto *locator = getLocator();
  // Resolve the locator to a specific expression.

  auto anchor = locator->getAnchor();

  {
    SourceRange range;
    auto path = locator->getPath();

    simplifyLocator(anchor, path, range);
    if (!anchor)
      return locator->getAnchor();
  }

  // FIXME: Work around an odd locator representation that doesn't separate the
  // base of a subscript member from the member access.
  if (locator->isLastElement<LocatorPathElt::SubscriptMember>()) {
    if (auto subscript = getAsExpr<SubscriptExpr>(anchor))
      anchor = subscript->getBase();
  }

  return anchor;
}

Type FailureDiagnostic::getType(ASTNode node, bool wantRValue) const {
  return resolveType(getRawType(node), /*reconstituteSugar=*/false, wantRValue);
}

Type FailureDiagnostic::getRawType(ASTNode node) const {
  return S.getType(node);
}

template <typename... ArgTypes>
InFlightDiagnostic
FailureDiagnostic::emitDiagnostic(ArgTypes &&... Args) const {
  return emitDiagnosticAt(getLoc(), std::forward<ArgTypes>(Args)...);
}

template <typename... ArgTypes>
InFlightDiagnostic
FailureDiagnostic::emitDiagnosticAt(ArgTypes &&... Args) const {
  auto &DE = getASTContext().Diags;
  return DE.diagnose(std::forward<ArgTypes>(Args)...);
}

Expr *FailureDiagnostic::findParentExpr(const Expr *subExpr) const {
  auto &cs = getConstraintSystem();
  return cs.getParentExpr(const_cast<Expr *>(subExpr));
}

Expr *
FailureDiagnostic::getArgumentListExprFor(ConstraintLocator *locator) const {
  auto path = locator->getPath();
  auto iter = path.begin();
  if (!locator->findFirst<LocatorPathElt::ApplyArgument>(iter))
    return nullptr;

  // Form a new locator that ends at the ApplyArgument element, then simplify
  // to get the argument list.
  auto newPath = ArrayRef<LocatorPathElt>(path.begin(), iter + 1);
  auto argListLoc = getConstraintLocator(locator->getAnchor(), newPath);
  return getAsExpr(simplifyLocatorToAnchor(argListLoc));
}

Expr *FailureDiagnostic::getBaseExprFor(const Expr *anchor) const {
  if (!anchor)
    return nullptr;

  if (auto *UDE = dyn_cast<UnresolvedDotExpr>(anchor))
    return UDE->getBase();
  else if (auto *SE = dyn_cast<SubscriptExpr>(anchor))
    return SE->getBase();
  else if (auto *MRE = dyn_cast<MemberRefExpr>(anchor))
    return MRE->getBase();
  else if (auto *call = dyn_cast<CallExpr>(anchor)) {
    auto fnType = getType(call->getFn());
    if (fnType->isCallableNominalType(getDC())) {
      return call->getFn();
    }
  }

  return nullptr;
}

Type FailureDiagnostic::restoreGenericParameters(
    Type type,
    llvm::function_ref<void(GenericTypeParamType *, Type)> substitution) {
  llvm::SmallPtrSet<GenericTypeParamType *, 4> processed;
  return type.transform([&](Type type) -> Type {
    if (auto *typeVar = type->getAs<TypeVariableType>()) {
      type = resolveType(typeVar);
      if (auto *GP = typeVar->getImpl().getGenericParameter()) {
        if (processed.insert(GP).second)
          substitution(GP, type);
        return GP;
      }
    }

    return type;
  });
}

bool FailureDiagnostic::conformsToKnownProtocol(
    Type type, KnownProtocolKind protocol) const {
  auto &cs = getConstraintSystem();
  return constraints::conformsToKnownProtocol(cs.DC, type, protocol);
}

Type RequirementFailure::getOwnerType() const {
  auto anchor = getRawAnchor();

  // If diagnostic is anchored at assignment expression
  // it means that requirement failure happend while trying
  // to convert source to destination, which means that
  // owner type is actually not an assignment expression
  // itself but its source.
  if (auto *assignment = getAsExpr<AssignExpr>(anchor))
    anchor = assignment->getSrc();

  return getType(anchor)->getInOutObjectType()->getMetatypeInstanceType();
}

const GenericContext *RequirementFailure::getGenericContext() const {
  if (auto *genericCtx = AffectedDecl->getAsGenericContext())
    return genericCtx;

  auto parentDecl = AffectedDecl->getDeclContext()->getAsDecl();
  if (!parentDecl)
    return nullptr;

  return parentDecl->getAsGenericContext();
}

const Requirement &RequirementFailure::getRequirement() const {
  // If this is a conditional requirement failure we need to
  // fetch conformance from constraint system associated with
  // type requirement this conditional conformance belongs to.
  auto requirements = isConditional()
                          ? Conformance->getConditionalRequirements()
                          : Signature->getRequirements();
  return requirements[getRequirementIndex()];
}

ProtocolConformance *RequirementFailure::getConformanceForConditionalReq(
    ConstraintLocator *locator) {
  auto &solution = getSolution();
  auto reqElt = locator->castLastElementTo<LocatorPathElt::AnyRequirement>();
  if (!reqElt.isConditionalRequirement())
    return nullptr;

  auto path = locator->getPath();
  auto *typeReqLoc = getConstraintLocator(getRawAnchor(), path.drop_back());

  auto result = llvm::find_if(
      solution.Conformances,
      [&](const std::pair<ConstraintLocator *, ProtocolConformanceRef>
              &conformance) { return conformance.first == typeReqLoc; });
  assert(result != solution.Conformances.end());

  auto conformance = result->second;
  assert(conformance.isConcrete());
  return conformance.getConcrete();
}

ValueDecl *RequirementFailure::getDeclRef() const {
  // Get a declaration associated with given type (if any).
  // This is used to retrieve affected declaration when
  // failure is in any way contextual, and declaration can't
  // be fetched directly from constraint system.
  auto getAffectedDeclFromType = [](Type type) -> ValueDecl * {
    assert(type);
    // If problem is related to a typealias, let's point this
    // diagnostic directly to its declaration without desugaring.
    if (auto *alias = dyn_cast<TypeAliasType>(type.getPointer()))
      return alias->getDecl();

    if (auto *opaque = type->getAs<OpaqueTypeArchetypeType>())
      return opaque->getDecl();

    return type->getAnyGeneric();
  };

  // If the locator is for a function builder body result type, the requirement
  // came from the function's return type.
  if (getLocator()->isForFunctionBuilderBodyResult()) {
    auto *func = getAsDecl<FuncDecl>(getAnchor());
    return getAffectedDeclFromType(func->getResultInterfaceType());
  }

  if (isFromContextualType())
    return getAffectedDeclFromType(getContextualType(getRawAnchor()));

  if (auto overload = getCalleeOverloadChoiceIfAvailable(getLocator())) {
    // If there is a declaration associated with this
    // failure e.g. an overload choice of the call
    // expression, let's see whether failure is
    // associated with it directly or rather with
    // one of its parents.
    if (auto *decl = overload->choice.getDeclOrNull()) {
      // If declaration is an operator let's always use
      // it to produce `in reference to` diagnostics.
      if (decl->isOperator())
        return decl;

      auto *DC = decl->getDeclContext();

      do {
        if (auto *parent = DC->getAsDecl()) {
          if (auto *GC = parent->getAsGenericContext()) {
            // FIXME: Is this intending an exact match?
            if (GC->getGenericSignature().getPointer() != Signature.getPointer())
              continue;

            // If this is a signature if an extension
            // then it means that code has referenced
            // something incorrectly and diagnostic
            // should point to the referenced declaration.
            if (isa<ExtensionDecl>(parent))
              break;

            return cast<ValueDecl>(parent);
          }
        }
      } while ((DC = DC->getParent()));

      return decl;
    }
  }

  return getAffectedDeclFromType(getOwnerType());
}

GenericSignature RequirementFailure::getSignature(ConstraintLocator *locator) {
  if (isConditional())
    return Conformance->getGenericSignature();

  if (auto genericElt = locator->findLast<LocatorPathElt::OpenedGeneric>())
    return genericElt->getSignature();

  llvm_unreachable("Type requirement failure should always have signature");
}

bool RequirementFailure::isFromContextualType() const {
  auto path = getLocator()->getPath();
  assert(!path.empty());
  return path.front().getKind() == ConstraintLocator::ContextualType;
}

const DeclContext *RequirementFailure::getRequirementDC() const {
  // In case of conditional requirement failure, we don't
  // have to guess where the it comes from.
  if (isConditional())
    return Conformance->getDeclContext();

  const auto &req = getRequirement();
  auto *DC = AffectedDecl->getDeclContext();

  do {
    if (auto sig = DC->getGenericSignatureOfContext()) {
      if (sig->isRequirementSatisfied(req))
        return DC;
    }
  } while ((DC = DC->getParent()));

  return AffectedDecl->getAsGenericContext();
}

bool RequirementFailure::isStaticOrInstanceMember(const ValueDecl *decl) {
  if (decl->isInstanceMember())
    return true;

  if (auto *AFD = dyn_cast<AbstractFunctionDecl>(decl))
    return AFD->isStatic() && !AFD->isOperator();

  return decl->isStatic();
}

bool RequirementFailure::diagnoseAsError() {
  const auto *reqDC = getRequirementDC();
  auto *genericCtx = getGenericContext();

  auto lhs = getLHS();
  auto rhs = getRHS();

  if (auto *OTD = dyn_cast<OpaqueTypeDecl>(AffectedDecl)) {
    auto *namingDecl = OTD->getNamingDecl();
    emitDiagnostic(diag::type_does_not_conform_in_opaque_return,
                   namingDecl->getDescriptiveKind(), namingDecl->getName(),
                   lhs, rhs, rhs->isAnyObject());

    if (auto *repr = namingDecl->getOpaqueResultTypeRepr()) {
      emitDiagnosticAt(repr->getLoc(), diag::opaque_return_type_declared_here)
          .highlight(repr->getSourceRange());
    }
    return true;
  }

  if (reqDC->isTypeContext() && genericCtx != reqDC &&
      (genericCtx->isChildContextOf(reqDC) ||
       isStaticOrInstanceMember(AffectedDecl))) {
    auto *NTD = reqDC->getSelfNominalTypeDecl();
    emitDiagnostic(
        getDiagnosticInRereference(), AffectedDecl->getDescriptiveKind(),
        AffectedDecl->getName(), NTD->getDeclaredType(), lhs, rhs);
  } else {
    emitDiagnostic(getDiagnosticOnDecl(), AffectedDecl->getDescriptiveKind(),
                   AffectedDecl->getName(), lhs, rhs);
  }

  emitRequirementNote(reqDC->getAsDecl(), lhs, rhs);
  return true;
}

bool RequirementFailure::diagnoseAsNote() {
  const auto &req = getRequirement();
  const auto *reqDC = getRequirementDC();

  emitDiagnosticAt(reqDC->getAsDecl(), getDiagnosticAsNote(), getLHS(),
                   getRHS(), req.getFirstType(), req.getSecondType(), "");
  return true;
}

void RequirementFailure::emitRequirementNote(const Decl *anchor, Type lhs,
                                             Type rhs) const {
  auto &req = getRequirement();

  if (req.getKind() != RequirementKind::SameType) {
    if (auto wrappedType = lhs->getOptionalObjectType()) {
      auto kind = (req.getKind() == RequirementKind::Superclass ?
                   ConstraintKind::Subtype : ConstraintKind::ConformsTo);
      if (TypeChecker::typesSatisfyConstraint(wrappedType, rhs,
                                              /*openArchetypes=*/false,
                                              kind, getDC()))
        emitDiagnostic(diag::wrapped_type_satisfies_requirement, wrappedType);
    }
  }

  if (isConditional()) {
    emitDiagnosticAt(anchor,
                     diag::requirement_implied_by_conditional_conformance,
                     resolveType(Conformance->getType()),
                     Conformance->getProtocol()->getDeclaredInterfaceType());
    return;
  }

  if (req.getKind() == RequirementKind::Layout ||
      rhs->isEqual(req.getSecondType())) {
    emitDiagnosticAt(anchor, diag::where_requirement_failure_one_subst,
                     req.getFirstType(), lhs);
    return;
  }

  if (lhs->isEqual(req.getFirstType())) {
    emitDiagnosticAt(anchor, diag::where_requirement_failure_one_subst,
                     req.getSecondType(), rhs);
    return;
  }

  emitDiagnosticAt(anchor, diag::where_requirement_failure_both_subst,
                   req.getFirstType(), lhs, req.getSecondType(), rhs);
}

bool MissingConformanceFailure::diagnoseAsError() {
  auto anchor = getAnchor();
  auto nonConformingType = getLHS();
  auto protocolType = getRHS();

  // If this is a requirement of a pattern-matching operator,
  // let's see whether argument already has a fix associated
  // with it and if so skip conformance error, otherwise we'd
  // produce an unrelated `<type> doesn't conform to Equatable protocol`
  // diagnostic.
  if (isPatternMatchingOperator(anchor)) {
    auto *expr = castToExpr(anchor);
    if (auto *binaryOp = dyn_cast_or_null<BinaryExpr>(findParentExpr(expr))) {
      auto *caseExpr = binaryOp->getArg()->getElement(0);

      llvm::SmallPtrSet<Expr *, 4> anchors;
      for (const auto *fix : getSolution().Fixes) {
        if (auto anchor = fix->getAnchor()) {
          if (anchor.is<Expr *>())
            anchors.insert(getAsExpr(anchor));
        }
      }

      bool hasFix = false;
      forEachExprInConstraintSystem(caseExpr, [&](Expr *expr) -> Expr * {
        hasFix |= anchors.count(expr);
        return hasFix ? nullptr : expr;
      });

      if (hasFix)
        return false;
    }
  }

  // If the problem has been (unambiguously) determined to be related
  // to one of of the standard comparison operators and argument is
  // enum with associated values, let's produce a tailored note which
  // says that conformances for enums with associated values can't be
  // synthesized.
  if (isStandardComparisonOperator(anchor)) {
    auto *expr = castToExpr(anchor);
    auto isEnumWithAssociatedValues = [](Type type) -> bool {
      if (auto *enumType = type->getAs<EnumType>())
        return !enumType->getDecl()->hasOnlyCasesWithoutAssociatedValues();
      return false;
    };

    // Limit this to `Equatable` and `Comparable` protocols for now.
    auto *protocol = getRHS()->castTo<ProtocolType>()->getDecl();
    if (isEnumWithAssociatedValues(getLHS()) &&
        (protocol->isSpecificProtocol(KnownProtocolKind::Equatable) ||
         protocol->isSpecificProtocol(KnownProtocolKind::Comparable))) {
      if (RequirementFailure::diagnoseAsError()) {
        auto opName = getOperatorName(expr);
        emitDiagnostic(diag::no_binary_op_overload_for_enum_with_payload,
                       opName->str());
        return true;
      }
    }
  }

  if (diagnoseAsAmbiguousOperatorRef())
    return true;

  if (nonConformingType->isObjCExistentialType()) {
    emitDiagnostic(diag::protocol_does_not_conform_static, nonConformingType,
                   protocolType);
    return true;
  }

  if (diagnoseTypeCannotConform(nonConformingType, protocolType))
    return true;

  // If none of the special cases could be diagnosed,
  // let's fallback to the most general diagnostic.
  return RequirementFailure::diagnoseAsError();
}

bool MissingConformanceFailure::diagnoseTypeCannotConform(
    Type nonConformingType, Type protocolType) const {
  if (getRequirement().getKind() == RequirementKind::Layout ||
      !(nonConformingType->is<AnyFunctionType>() ||
      nonConformingType->is<TupleType>() ||
      nonConformingType->isExistentialType() ||
      nonConformingType->is<AnyMetatypeType>())) {
    return false;
  }

  emitDiagnostic(diag::type_cannot_conform,
                 nonConformingType->isExistentialType(), 
                 nonConformingType, 
                 nonConformingType->isEqual(protocolType),
                 protocolType);

  if (auto *OTD = dyn_cast<OpaqueTypeDecl>(AffectedDecl)) {
    auto *namingDecl = OTD->getNamingDecl();
    if (auto *repr = namingDecl->getOpaqueResultTypeRepr()) {
      emitDiagnosticAt(repr->getLoc(), diag::required_by_opaque_return,
                       namingDecl->getDescriptiveKind(),
                       namingDecl->getName())
          .highlight(repr->getSourceRange());
    }
    return true;
  }

  auto &req = getRequirement();
  auto *reqDC = getRequirementDC();
  auto *genericCtx = getGenericContext();
  auto noteLocation = reqDC->getAsDecl()->getLoc();

  if (!noteLocation.isValid())
    noteLocation = getLoc();

  if (isConditional()) {
    emitDiagnosticAt(noteLocation,
                     diag::requirement_implied_by_conditional_conformance,
                     resolveType(Conformance->getType()),
                     Conformance->getProtocol()->getDeclaredInterfaceType());
  } else if (genericCtx != reqDC && (genericCtx->isChildContextOf(reqDC) ||
                                     isStaticOrInstanceMember(AffectedDecl))) {
    emitDiagnosticAt(noteLocation, diag::required_by_decl_ref,
                     AffectedDecl->getDescriptiveKind(),
                     AffectedDecl->getName(),
                     reqDC->getSelfNominalTypeDecl()->getDeclaredType(),
                     req.getFirstType(), nonConformingType);
  } else {
    emitDiagnosticAt(noteLocation, diag::required_by_decl,
                     AffectedDecl->getDescriptiveKind(),
                     AffectedDecl->getName(), req.getFirstType(),
                     nonConformingType);
  }

  return true;
}

bool MissingConformanceFailure::diagnoseAsAmbiguousOperatorRef() {
  auto anchor = getRawAnchor();
  auto *ODRE = getAsExpr<OverloadedDeclRefExpr>(anchor);
  if (!ODRE)
    return false;

  auto name = ODRE->getDecls().front()->getBaseName();
  if (!(name.isOperator() && getLHS()->isStdlibType() && getRHS()->isStdlibType()))
    return false;

  // If this is an operator reference and both types are from stdlib,
  // let's produce a generic diagnostic about invocation and a note
  // about missing conformance just in case.
  auto operatorID = name.getIdentifier();

  auto *fnType = getType(anchor)->getAs<AnyFunctionType>();
  auto params = fnType->getParams();
  if (params.size() == 2) {
    auto lhsType = params[0].getPlainType();
    auto rhsType = params[1].getPlainType();

    if (lhsType->isEqual(rhsType)) {
      emitDiagnostic(diag::cannot_apply_binop_to_same_args, operatorID.str(),
                     lhsType);
    } else {
      emitDiagnostic(diag::cannot_apply_binop_to_args, operatorID.str(),
                     lhsType, rhsType);
    }
  } else {
    emitDiagnostic(diag::cannot_apply_unop_to_arg, operatorID.str(),
                   params[0].getPlainType());
  }

  diagnoseAsNote();
  return true;
}

Optional<Diag<Type, Type>> GenericArgumentsMismatchFailure::getDiagnosticFor(
    ContextualTypePurpose context) {
  switch (context) {
  case CTP_Initialization:
  case CTP_AssignSource:
    return diag::cannot_convert_assign;
  case CTP_ReturnStmt:
  case CTP_ReturnSingleExpr:
    return diag::cannot_convert_to_return_type;
  case CTP_DefaultParameter:
  case CTP_AutoclosureDefaultParameter:
    return diag::cannot_convert_default_arg_value;
  case CTP_YieldByValue:
    return diag::cannot_convert_yield_value;
  case CTP_CallArgument:
    return diag::cannot_convert_argument_value;
  case CTP_ClosureResult:
    return diag::cannot_convert_closure_result;
  case CTP_ArrayElement:
    return diag::cannot_convert_array_element;
  case CTP_DictionaryKey:
    return diag::cannot_convert_dict_key;
  case CTP_DictionaryValue:
    return diag::cannot_convert_dict_value;
  case CTP_CoerceOperand:
    return diag::cannot_convert_coerce;
  case CTP_SubscriptAssignSource:
    return diag::cannot_convert_subscript_assign;
  case CTP_Condition:
    return diag::cannot_convert_condition_value;
  case CTP_WrappedProperty:
    return diag::wrapped_value_mismatch;
  case CTP_ComposedPropertyWrapper:
    return diag::composed_property_wrapper_mismatch;

  case CTP_ThrowStmt:
  case CTP_ForEachStmt:
  case CTP_Unused:
  case CTP_CannotFail:
  case CTP_YieldByReference:
  case CTP_CalleeResult:
  case CTP_EnumCaseRawValue:
    break;
  }
  return None;
}

void GenericArgumentsMismatchFailure::emitNoteForMismatch(int position) {
  auto *locator = getLocator();
  // Since there could be implicit conversions associated with argument
  // to parameter conversions, let's use parameter type as a source of
  // generic parameter information.
  auto paramSourceTy =
      locator->isLastElement<LocatorPathElt::ApplyArgToParam>() ? getRequired()
                                                                : getActual();

  auto genericTypeDecl = paramSourceTy->getAnyGeneric();
  auto param = genericTypeDecl->getGenericParams()->getParams()[position];

  auto lhs = getActual()->getGenericArgs()[position];
  auto rhs = getRequired()->getGenericArgs()[position];

  auto noteLocation = param->getLoc();

  if (!noteLocation.isValid())
    noteLocation = getLoc();

  emitDiagnosticAt(noteLocation, diag::generic_argument_mismatch,
                   param->getName(), lhs, rhs);
}

bool GenericArgumentsMismatchFailure::diagnoseAsError() {
  auto anchor = getAnchor();

  auto fromType = getFromType();
  auto toType = getToType();

  // This is a situation where right-hand size type is wrapped
  // into a number of optionals and argument isn't e.g.
  //
  // func test(_: UnsafePointer<Int>??) {}
  //
  // var value: Float = 0
  // test(&value)
  //
  // `value` has to get implicitly wrapped into 2 optionals
  // before pointer types could be compared.
  auto path = getLocator()->getPath();
  unsigned toDrop = 0;
  for (const auto &elt : llvm::reverse(path)) {
    if (!elt.is<LocatorPathElt::OptionalPayload>())
      break;

    // Disregard optional payload element to look at its source.
    ++toDrop;
  }

  path = path.drop_back(toDrop);

  Optional<Diag<Type, Type>> diagnostic;
  if (path.empty()) {
    if (isExpr<AssignExpr>(anchor)) {
      diagnostic = getDiagnosticFor(CTP_AssignSource);
    } else if (isExpr<CoerceExpr>(anchor)) {
      diagnostic = getDiagnosticFor(CTP_CoerceOperand);
    } else {
      return false;
    }
  } else {
    const auto &last = path.back();
    switch (last.getKind()) {
    case ConstraintLocator::ContextualType: {
      auto purpose = getContextualTypePurpose();
      assert(!(purpose == CTP_Unused || purpose == CTP_CannotFail));

      // If this is call to a closure e.g. `let _: A = { B() }()`
      // let's point diagnostic to its result.
      if (auto *call = getAsExpr<CallExpr>(anchor)) {
        auto *fnExpr = call->getFn();
        if (auto *closure = dyn_cast<ClosureExpr>(fnExpr)) {
          purpose = CTP_ClosureResult;
          if (closure->hasSingleExpressionBody())
            anchor = closure->getSingleExpressionBody();
        }
      }

      diagnostic = getDiagnosticFor(purpose);
      break;
    }

    case ConstraintLocator::AutoclosureResult:
    case ConstraintLocator::ApplyArgToParam:
    case ConstraintLocator::ApplyArgument: {
      diagnostic = diag::cannot_convert_argument_value;
      break;
    }

    case ConstraintLocator::ParentType: {
      diagnostic = diag::cannot_convert_parent_type;
      break;
    }

    case ConstraintLocator::ClosureBody:
    case ConstraintLocator::ClosureResult: {
      diagnostic = diag::cannot_convert_closure_result;
      break;
    }

    case ConstraintLocator::TupleElement: {
      auto rawAnchor = getRawAnchor();

      if (isExpr<ArrayExpr>(rawAnchor)) {
        diagnostic = getDiagnosticFor(CTP_ArrayElement);
      } else if (isExpr<DictionaryExpr>(rawAnchor)) {
        auto eltLoc = last.castTo<LocatorPathElt::TupleElement>();
        diagnostic = getDiagnosticFor(
            eltLoc.getIndex() == 0 ? CTP_DictionaryKey : CTP_DictionaryValue);
      }
      break;
    }

    case ConstraintLocator::UnresolvedMemberChainResult: {
      diagnostic = diag::cannot_convert_chain_result_type;
      break;
    }

    default:
      break;
    }
  }

  if (!diagnostic) {
    // Handle all mismatches involving an `AssignExpr`
    if (auto *assignExpr = getAsExpr<AssignExpr>(anchor)) {
      diagnostic = getDiagnosticFor(CTP_AssignSource);
      fromType = getType(assignExpr->getSrc());
      toType = getType(assignExpr->getDest());
    } else {
      // If we couldn't find a specific diagnostic let's fallback to
      // attempt to handle cases where we have an apply arg to param.
      auto applyInfo = getFunctionArgApplyInfo(getLocator());
      if (applyInfo) {
        diagnostic = diag::cannot_convert_argument_value;
        fromType = applyInfo->getArgType();
        toType = applyInfo->getParamType();
      }
    }
  }
  
  if (!diagnostic)
    return false;

  emitDiagnosticAt(::getLoc(anchor), *diagnostic, fromType, toType);
  emitNotesForMismatches();
  return true;
}

bool LabelingFailure::diagnoseAsError() {
  auto *argExpr = getArgumentListExprFor(getLocator());
  if (!argExpr)
    return false;

  return diagnoseArgumentLabelError(getASTContext(), argExpr, CorrectLabels,
                                    isExpr<SubscriptExpr>(getRawAnchor()));
}

bool LabelingFailure::diagnoseAsNote() {
  auto *argExpr = getArgumentListExprFor(getLocator());
  if (!argExpr)
    return false;

  SmallVector<Identifier, 4> argLabels;
  if (isa<ParenExpr>(argExpr)) {
    argLabels.push_back(Identifier());
  } else if (auto *tuple = dyn_cast<TupleExpr>(argExpr)) {
    argLabels.append(tuple->getElementNames().begin(),
                     tuple->getElementNames().end());
  } else {
    return false;
  }

  auto stringifyLabels = [](ArrayRef<Identifier> labels) -> std::string {
    std::string str;
    for (auto label : labels) {
      str += label.empty() ? "_" : label.str();
      str += ':';
    }
    return "(" + str + ")";
  };

  auto selectedOverload = getCalleeOverloadChoiceIfAvailable(getLocator());
  if (!selectedOverload)
    return false;

  const auto &choice = selectedOverload->choice;
  if (auto *decl = choice.getDeclOrNull()) {
    emitDiagnosticAt(decl, diag::candidate_expected_different_labels,
                     stringifyLabels(argLabels),
                     stringifyLabels(CorrectLabels));
    return true;
  }

  return false;
}

bool NoEscapeFuncToTypeConversionFailure::diagnoseAsError() {
  if (diagnoseParameterUse())
    return true;

  if (auto *typeVar = getRawFromType()->getAs<TypeVariableType>()) {
    if (auto *GP = typeVar->getImpl().getGenericParameter()) {
      emitDiagnostic(diag::converting_noescape_to_type, GP);
      return true;
    }
  }

  emitDiagnostic(diag::converting_noescape_to_type, getToType());
  return true;
}

bool NoEscapeFuncToTypeConversionFailure::diagnoseParameterUse() const {
  auto convertTo = getToType();
  // If the other side is not a function, we have common case diagnostics
  // which handle function-to-type conversion diagnostics.
  if (!convertTo->is<FunctionType>())
    return false;

  auto anchor = getAnchor();
  auto diagnostic = diag::general_noescape_to_escaping;

  ParamDecl *PD = nullptr;
  if (auto *DRE = getAsExpr<DeclRefExpr>(anchor)) {
    PD = dyn_cast<ParamDecl>(DRE->getDecl());

    // If anchor is not a parameter declaration there
    // is no need to dig up more information.
    if (!PD)
      return false;

    // Let's check whether this is a function parameter passed
    // as an argument to another function which accepts @escaping
    // function at that position.
    if (auto argApplyInfo = getFunctionArgApplyInfo(getLocator())) {
      auto paramInterfaceTy = argApplyInfo->getParamInterfaceType();
      if (paramInterfaceTy->isTypeParameter()) {
        auto diagnoseGenericParamFailure = [&](GenericTypeParamDecl *decl) {
          emitDiagnostic(diag::converting_noespace_param_to_generic_type,
                         PD->getName(), paramInterfaceTy);

          auto declLoc = decl->getLoc();
          if (declLoc.isValid())
            emitDiagnosticAt(decl, diag::generic_parameters_always_escaping);
        };

        // If this is a situation when non-escaping parameter is passed
        // to the argument which represents generic parameter, there is
        // a tailored diagnostic for that.

        if (auto *DMT = paramInterfaceTy->getAs<DependentMemberType>()) {
          diagnoseGenericParamFailure(DMT->getRootGenericParam()->getDecl());
          return true;
        }

        if (auto *GP = paramInterfaceTy->getAs<GenericTypeParamType>()) {
          diagnoseGenericParamFailure(GP->getDecl());
          return true;
        }
      }

      // If there are no generic parameters involved, this could
      // only mean that parameter is expecting @escaping function type.
      diagnostic = diag::passing_noescape_to_escaping;
    }
  } else if (auto *AE = getAsExpr<AssignExpr>(getRawAnchor())) {
    if (auto *DRE = dyn_cast<DeclRefExpr>(AE->getSrc())) {
      PD = dyn_cast<ParamDecl>(DRE->getDecl());
      diagnostic = diag::assigning_noescape_to_escaping;
    }
  }

  if (!PD)
    return false;

  emitDiagnostic(diagnostic, PD->getName());

  // Give a note and fix-it
  auto note = emitDiagnosticAt(PD, diag::noescape_parameter, PD->getName());

  SourceLoc reprLoc;
  SourceLoc autoclosureEndLoc;
  if (auto *repr = PD->getTypeRepr()) {
    reprLoc = repr->getStartLoc();
    if (auto *attrRepr = dyn_cast<AttributedTypeRepr>(repr)) {
      autoclosureEndLoc = Lexer::getLocForEndOfToken(
          getASTContext().SourceMgr,
          attrRepr->getAttrs().getLoc(TAK_autoclosure));
    }
  }
  if (!PD->isAutoClosure()) {
    note.fixItInsert(reprLoc, "@escaping ");
  } else {
    note.fixItInsertAfter(autoclosureEndLoc, " @escaping");
  }

  return true;
}

ASTNode MissingForcedDowncastFailure::getAnchor() const {
  auto anchor = FailureDiagnostic::getAnchor();
  if (auto *assignExpr = getAsExpr<AssignExpr>(anchor))
    return assignExpr->getSrc();
  return anchor;
}

bool MissingForcedDowncastFailure::diagnoseAsError() {
  auto fromType = getFromType();
  auto toType = getToType();

  emitDiagnostic(diag::missing_forced_downcast, fromType, toType)
      .highlight(getSourceRange())
      .fixItReplace(getLoc(), "as!");
  return true;
}

bool MissingAddressOfFailure::diagnoseAsError() {
  auto argTy = getFromType();
  auto paramTy = getToType();

  if (paramTy->getAnyPointerElementType()) {
    emitDiagnostic(diag::cannot_convert_argument_value, argTy, paramTy)
        .fixItInsert(getSourceRange().Start, "&");
  } else {
    emitDiagnostic(diag::missing_address_of, argTy)
        .fixItInsert(getSourceRange().Start, "&");
  }
  return true;
}

ASTNode MissingExplicitConversionFailure::getAnchor() const {
  auto anchor = FailureDiagnostic::getAnchor();

  if (auto *assign = getAsExpr<AssignExpr>(anchor))
    return assign->getSrc();

  if (auto *paren = getAsExpr<ParenExpr>(anchor))
    return paren->getSubExpr();

  return anchor;
}

bool MissingExplicitConversionFailure::diagnoseAsError() {
  auto *DC = getDC();
  auto *anchor = castToExpr(getAnchor());

  auto fromType = getFromType();
  auto toType = getToType();

  if (!toType->hasTypeRepr())
    return false;

  bool useAs = TypeChecker::isExplicitlyConvertibleTo(fromType, toType, DC);

  auto *expr = findParentExpr(anchor);
  if (!expr)
    expr = const_cast<Expr *>(anchor);

  // If we're performing pattern matching,
  // "as" means something completely different...
  if (auto binOpExpr = dyn_cast<BinaryExpr>(expr)) {
    auto overloadedFn = dyn_cast<OverloadedDeclRefExpr>(binOpExpr->getFn());
    if (overloadedFn && !overloadedFn->getDecls().empty()) {
      ValueDecl *decl0 = overloadedFn->getDecls()[0];
      if (decl0->getBaseName() == decl0->getASTContext().Id_MatchOperator)
        return false;
    }
  }

  bool needsParensInside = exprNeedsParensBeforeAddingAs(anchor);
  bool needsParensOutside = exprNeedsParensAfterAddingAs(anchor, expr);

  llvm::SmallString<2> insertBefore;
  llvm::SmallString<32> insertAfter;
  if (needsParensOutside) {
    insertBefore += "(";
  }
  if (needsParensInside) {
    insertBefore += "(";
    insertAfter += ")";
  }
  insertAfter += useAs ? " as " : " as! ";
  insertAfter += toType->getWithoutParens()->getString();
  if (needsParensOutside)
    insertAfter += ")";

  auto diagID =
      useAs ? diag::missing_explicit_conversion : diag::missing_forced_downcast;
  auto diag = emitDiagnostic(diagID, fromType, toType);

  if (!insertBefore.empty()) {
    diag.fixItInsert(getSourceRange().Start, insertBefore);
  }
  diag.fixItInsertAfter(getSourceRange().End, insertAfter);
  return true;
}

SourceRange MemberAccessOnOptionalBaseFailure::getSourceRange() const {
  if (auto componentPathElt =
          getLocator()->getLastElementAs<LocatorPathElt::KeyPathComponent>()) {
    auto anchor = getAnchor();
    auto keyPathExpr = castToExpr<KeyPathExpr>(anchor);
    if (componentPathElt->getIndex() == 0) {
      if (auto rootType = keyPathExpr->getRootType()) {
        return rootType->getSourceRange();
      } else {
        return keyPathExpr->getComponents().front().getLoc();
      }
    } else {
      auto componentIdx = componentPathElt->getIndex() - 1;
      auto component = keyPathExpr->getComponents()[componentIdx];
      return component.getSourceRange();
    }
  }
  return FailureDiagnostic::getSourceRange();
}

bool MemberAccessOnOptionalBaseFailure::diagnoseAsError() {
  auto baseType = getMemberBaseType();
  auto locator = getLocator();
  
  bool resultIsOptional = ResultTypeIsOptional;

  // If we've resolved the member overload to one that returns an optional
  // type, then the result of the expression is optional (and we want to offer
  // only a '?' fixit) even though the constraint system didn't need to add any
  // additional optionality.
  auto overload = getOverloadChoiceIfAvailable(locator);
  if (overload && overload->openedType->getOptionalObjectType())
    resultIsOptional = true;

  auto unwrappedBaseType = baseType->getOptionalObjectType();
  if (!unwrappedBaseType)
    return false;
  
  auto sourceRange = getSourceRange();

  auto componentPathElt =
      locator->getLastElementAs<LocatorPathElt::KeyPathComponent>();
  if (componentPathElt && componentPathElt->getIndex() == 0) {
    // For members where the base type is an optional key path root
    // let's emit a tailored note suggesting to use its unwrapped type.
    auto *keyPathExpr = castToExpr<KeyPathExpr>(getAnchor());
    if (auto rootType = keyPathExpr->getRootType()) {
      emitDiagnostic(diag::optional_base_not_unwrapped, baseType, Member,
                     unwrappedBaseType);

      emitDiagnostic(diag::optional_base_remove_optional_for_keypath_root,
                     unwrappedBaseType)
          .fixItReplace(rootType->getSourceRange(),
                        unwrappedBaseType.getString());
    } else {
      emitDiagnostic(diag::invalid_optional_infered_keypath_root, baseType,
                     Member, unwrappedBaseType);
      emitDiagnostic(diag::optional_key_path_root_base_chain, Member)
          .fixItInsert(sourceRange.End, "?.");
      emitDiagnostic(diag::optional_key_path_root_base_unwrap, Member)
          .fixItInsert(sourceRange.End, "!.");
    }
  } else {
    emitDiagnostic(diag::optional_base_not_unwrapped, baseType, Member,
                   unwrappedBaseType);

    // FIXME: It would be nice to immediately offer "base?.member ?? defaultValue"
    // for non-optional results where that would be appropriate. For the moment
    // always offering "?" means that if the user chooses chaining, we'll end up
    // in MissingOptionalUnwrapFailure:diagnose() to offer a default value during
    // the next compile.
    emitDiagnostic(diag::optional_base_chain, Member)
        .fixItInsertAfter(sourceRange.End, "?");

    if (!resultIsOptional) {
      emitDiagnostic(diag::unwrap_with_force_value)
          .fixItInsertAfter(sourceRange.End, "!");
    }
  }

  return true;
}

void MissingOptionalUnwrapFailure::offerDefaultValueUnwrapFixIt(
    DeclContext *DC, const Expr *expr) const {
  assert(expr);

  auto *anchor = getAsExpr(getAnchor());
  // If anchor is n explicit address-of, or expression which produces
  // an l-value (e.g. first argument of `+=` operator), let's not
  // suggest default value here because that would produce r-value type.
  if (!anchor || isa<InOutExpr>(anchor))
    return;

  if (auto argApplyInfo = getFunctionArgApplyInfo(getLocator()))
    if (argApplyInfo->getParameterFlags().isInOut())
      return;

  auto diag = emitDiagnosticAt(expr->getLoc(), diag::unwrap_with_default_value);

  // Figure out what we need to parenthesize.
  bool needsParensInside =
      exprNeedsParensBeforeAddingNilCoalescing(DC, const_cast<Expr *>(expr));
  auto parentExpr = findParentExpr(anchor);
  if (parentExpr && isa<UnresolvedMemberChainResultExpr>(parentExpr))
    parentExpr = findParentExpr(parentExpr);
  bool needsParensOutside = exprNeedsParensAfterAddingNilCoalescing(
      DC, const_cast<Expr *>(expr), parentExpr);

  llvm::SmallString<2> insertBefore;
  llvm::SmallString<32> insertAfter;
  if (needsParensOutside) {
    insertBefore += "(";
  }
  if (needsParensInside) {
    insertBefore += "(";
    insertAfter += ")";
  }
  insertAfter += " ?? <" "#default value#" ">";
  if (needsParensOutside)
    insertAfter += ")";

  if (!insertBefore.empty()) {
    diag.fixItInsert(expr->getStartLoc(), insertBefore);
  }
  diag.fixItInsertAfter(expr->getEndLoc(), insertAfter);
}

// Suggest a force-unwrap.
void MissingOptionalUnwrapFailure::offerForceUnwrapFixIt(
    const Expr *expr) const {
  auto diag = emitDiagnosticAt(expr->getLoc(), diag::unwrap_with_force_value);

  // If expr is optional as the result of an optional chain and this last
  // dot isn't a member returning optional, then offer to force the last
  // link in the chain, rather than an ugly parenthesized postfix force.
  if (auto optionalChain = dyn_cast<OptionalEvaluationExpr>(expr)) {
    if (auto dotExpr =
        dyn_cast<UnresolvedDotExpr>(optionalChain->getSubExpr())) {
      auto bind = dyn_cast<BindOptionalExpr>(dotExpr->getBase());
      if (bind && !getType(dotExpr)->getOptionalObjectType()) {
        diag.fixItReplace(SourceRange(bind->getLoc()), "!");
        return;
      }
    }
  }

  if (expr->canAppendPostfixExpression(true)) {
    diag.fixItInsertAfter(expr->getEndLoc(), "!");
  } else {
    diag.fixItInsert(expr->getStartLoc(), "(")
        .fixItInsertAfter(expr->getEndLoc(), ")!");
  }
}

class VarDeclMultipleReferencesChecker : public ASTWalker {
  VarDecl *varDecl;
  int count;

  std::pair<bool, Expr *> walkToExprPre(Expr *E) override {
    if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
      if (DRE->getDecl() == varDecl)
        ++count;
    }
    return { true, E };
  }

public:
  VarDeclMultipleReferencesChecker(VarDecl *varDecl) : varDecl(varDecl),count(0) {}
  int referencesCount() { return count; }
};

bool MissingOptionalUnwrapFailure::diagnoseAsError() {
  if (!getUnwrappedType()->isBool()) {
    if (diagnoseConversionToBool())
      return true;
  }

  auto *anchor = castToExpr(getAnchor());

  if (auto assignExpr = dyn_cast<AssignExpr>(anchor))
    anchor = assignExpr->getSrc();

  auto *unwrappedExpr = anchor->getValueProvidingExpr();

  if (auto *tryExpr = dyn_cast<OptionalTryExpr>(unwrappedExpr)) {
    bool isSwift5OrGreater = getASTContext().isSwiftVersionAtLeast(5);
    auto subExprType = getType(tryExpr->getSubExpr());
    bool subExpressionIsOptional = (bool)subExprType->getOptionalObjectType();

    if (isSwift5OrGreater && subExpressionIsOptional) {
      // Using 'try!' won't change the type for a 'try?' with an optional
      // sub-expr under Swift 5+, so just report that a missing unwrap can't be
      // handled here.
      return false;
    }

    emitDiagnosticAt(tryExpr->getTryLoc(), diag::missing_unwrap_optional_try,
                     getType(anchor))
        .fixItReplace({tryExpr->getTryLoc(), tryExpr->getQuestionLoc()},
                      "try!");
    return true;
  }

  auto baseType = getBaseType();
  auto unwrappedType = getUnwrappedType();

  assert(!baseType->hasTypeVariable() &&
         "Base type must not be a type variable");
  assert(!baseType->isHole() && "Base type must not be a type hole");
  assert(!unwrappedType->hasTypeVariable() &&
         "Unwrapped type must not be a type variable");
  assert(!unwrappedType->isHole() && "Unwrapped type must not be a type hole");

  if (!baseType->getOptionalObjectType())
    return false;

  emitDiagnosticAt(unwrappedExpr->getLoc(), diag::optional_not_unwrapped,
                   baseType, unwrappedType);

  // If the expression we're unwrapping is the only reference to a
  // local variable whose type isn't explicit in the source, then
  // offer unwrapping fixits on the initializer as well.
  if (auto declRef = dyn_cast<DeclRefExpr>(unwrappedExpr)) {
    if (auto varDecl = dyn_cast<VarDecl>(declRef->getDecl())) {
      bool singleUse = false;
      AbstractFunctionDecl *AFD = nullptr;
      if (auto contextDecl = varDecl->getDeclContext()->getAsDecl()) {
        if ((AFD = dyn_cast<AbstractFunctionDecl>(contextDecl))) {
          auto checker = VarDeclMultipleReferencesChecker(varDecl);
          AFD->getBody()->walk(checker);
          singleUse = checker.referencesCount() == 1;
        }
      }

      PatternBindingDecl *binding = varDecl->getParentPatternBinding();
      if (singleUse && binding && binding->getNumPatternEntries() == 1 &&
          varDecl->getTypeSourceRangeForDiagnostics().isInvalid()) {

        auto *initializer = varDecl->getParentInitializer();
        if (!initializer)
          return true;

        if (auto declRefExpr = dyn_cast<DeclRefExpr>(initializer)) {
          if (declRefExpr->getDecl()->isImplicitlyUnwrappedOptional()) {
            emitDiagnosticAt(declRefExpr->getLoc(),
                             diag::unwrap_iuo_initializer, baseType);
          }
        }

        auto fnTy = AFD->getInterfaceType()->castTo<AnyFunctionType>();
        bool voidReturn =
            fnTy->getResult()->isEqual(TupleType::getEmpty(getASTContext()));

        auto diag =
            emitDiagnosticAt(varDecl->getLoc(), diag::unwrap_with_guard);
        diag.fixItInsert(binding->getStartLoc(), "guard ");
        if (voidReturn) {
          diag.fixItInsertAfter(binding->getEndLoc(), " else { return }");
        } else {
          diag.fixItInsertAfter(binding->getEndLoc(), " else { return <"
                                                      "#default value#"
                                                      "> }");
        }
        diag.flush();

        offerDefaultValueUnwrapFixIt(varDecl->getDeclContext(), initializer);
        offerForceUnwrapFixIt(initializer);
      }
    }
  }

  offerDefaultValueUnwrapFixIt(getDC(), unwrappedExpr);
  offerForceUnwrapFixIt(unwrappedExpr);
  return true;
}

bool RValueTreatedAsLValueFailure::diagnoseAsError() {
  Diag<StringRef> subElementDiagID;
  Diag<Type> rvalueDiagID = diag::assignment_lhs_not_lvalue;
  auto diagExpr = castToExpr(getRawAnchor());
  SourceLoc loc = diagExpr->getLoc();

  // Assignment is not allowed inside of a condition,
  // so let's not diagnose immutability, because
  // most likely the problem is related to use of `=` itself.
  if (getContextualTypePurpose(diagExpr) == CTP_Condition)
    return false;

  // If the failure happened at the end of an unresolved member chain, it should
  // be diagnosed instead as though it happened at the last element.
  if (auto chainExpr = dyn_cast<UnresolvedMemberChainResultExpr>(diagExpr))
      diagExpr = chainExpr->getSubExpr();

  if (auto assignExpr = dyn_cast<AssignExpr>(diagExpr)) {
    // Let's check whether this is an attempt to assign
    // variable or property to itself.
    if (TypeChecker::diagnoseSelfAssignment(assignExpr))
      return true;

    diagExpr = assignExpr->getDest();
  }

  if (auto callExpr = dyn_cast<ApplyExpr>(diagExpr)) {
    Expr *argExpr = callExpr->getArg();
    loc = callExpr->getFn()->getLoc();
    auto *locator = getLocator();

    // `argument attribute` is used for identification purposes
    // only, so it could be looked through in this situation.
    if (locator->isLastElement<LocatorPathElt::ArgumentAttribute>()) {
      auto path = locator->getPath();
      locator = getConstraintLocator(getRawAnchor(), path.drop_back());
    }

    if (isa<PrefixUnaryExpr>(callExpr) || isa<PostfixUnaryExpr>(callExpr)) {
      subElementDiagID = diag::cannot_apply_lvalue_unop_to_subelement;
      rvalueDiagID = diag::cannot_apply_lvalue_unop_to_rvalue;
      diagExpr = argExpr;
    } else if (isa<BinaryExpr>(callExpr)) {
      subElementDiagID = diag::cannot_apply_lvalue_binop_to_subelement;
      rvalueDiagID = diag::cannot_apply_lvalue_binop_to_rvalue;
      diagExpr = castToExpr(simplifyLocatorToAnchor(locator));
    } else if (auto argElt =
                   locator
                       ->getLastElementAs<LocatorPathElt::ApplyArgToParam>()) {
      subElementDiagID = diag::cannot_pass_rvalue_inout_subelement;
      rvalueDiagID = diag::cannot_pass_rvalue_inout;
      if (auto argTuple = dyn_cast<TupleExpr>(argExpr))
        diagExpr = argTuple->getElement(argElt->getArgIdx());
      else if (auto parens = dyn_cast<ParenExpr>(argExpr))
        diagExpr = parens->getSubExpr();
    } else {
      subElementDiagID = diag::assignment_lhs_is_apply_expression;
    }
  } else if (auto inoutExpr = dyn_cast<InOutExpr>(diagExpr)) {
    if (auto *parentExpr = findParentExpr(inoutExpr)) {
      if (auto *call =
              dyn_cast_or_null<ApplyExpr>(findParentExpr(parentExpr))) {
        // Since this `inout` expression is an argument to a call/operator
        // let's figure out whether this is an impliict conversion from
        // array to an unsafe pointer type and diagnose it.
        unsigned argIdx = 0;
        if (auto *TE = dyn_cast<TupleExpr>(parentExpr)) {
          for (unsigned n = TE->getNumElements(); argIdx != n; ++argIdx) {
            if (TE->getElement(argIdx) == inoutExpr)
              break;
          }
        }

        auto *argLoc = getConstraintLocator(
            call, {ConstraintLocator::ApplyArgument,
                   LocatorPathElt::ApplyArgToParam(argIdx, argIdx,
                                                   ParameterTypeFlags())});

        if (auto info = getFunctionArgApplyInfo(argLoc)) {
          auto paramType = info->getParamType();
          auto argType = getType(inoutExpr)->getWithoutSpecifierType();

          PointerTypeKind ptr;
          if (isArrayType(argType) &&
              paramType->getAnyPointerElementType(ptr) &&
              (ptr == PTK_UnsafePointer || ptr == PTK_UnsafeRawPointer)) {
            emitDiagnosticAt(inoutExpr->getLoc(),
                             diag::extra_address_of_unsafepointer, paramType)
                .highlight(inoutExpr->getSourceRange())
                .fixItRemove(inoutExpr->getStartLoc());
            return true;
          }
        }
      }
    }

    subElementDiagID = diag::cannot_pass_rvalue_inout_subelement;
    rvalueDiagID = diag::cannot_pass_rvalue_inout;
    diagExpr = inoutExpr->getSubExpr();
  } else if (isa<DeclRefExpr>(diagExpr)) {
    subElementDiagID = diag::assignment_lhs_is_immutable_variable;
  } else if (isa<ForceValueExpr>(diagExpr)) {
    subElementDiagID = diag::assignment_bang_has_immutable_subcomponent;
  } else if (isa<MemberRefExpr>(diagExpr)) {
    subElementDiagID = diag::assignment_lhs_is_immutable_property;
  } else if (auto member = dyn_cast<UnresolvedDotExpr>(diagExpr)) {
    subElementDiagID = diag::assignment_lhs_is_immutable_property;

    if (auto *ctor = dyn_cast<ConstructorDecl>(getDC())) {
      if (auto *baseRef = dyn_cast<DeclRefExpr>(member->getBase())) {
        if (baseRef->getDecl() == ctor->getImplicitSelfDecl() &&
            ctor->getDelegatingOrChainedInitKind(nullptr) ==
            ConstructorDecl::BodyInitKind::Delegating) {
          emitDiagnosticAt(loc, diag::assignment_let_property_delegating_init,
                           member->getName());
          if (auto overload = getOverloadChoiceIfAvailable(
                  getConstraintLocator(member, ConstraintLocator::Member))) {
            if (auto *ref = overload->choice.getDeclOrNull())
              emitDiagnosticAt(ref, diag::decl_declared_here,
                               ref->getName());
          }
          return true;
        }
      }
    }

    if (auto resolvedOverload = getOverloadChoiceIfAvailable(getLocator())) {
      if (resolvedOverload->choice.getKind() ==
          OverloadChoiceKind::DynamicMemberLookup)
        subElementDiagID = diag::assignment_dynamic_property_has_immutable_base;

      if (resolvedOverload->choice.getKind() ==
          OverloadChoiceKind::KeyPathDynamicMemberLookup) {
        if (!getType(member->getBase(), /*wantRValue=*/false)->hasLValueType())
          subElementDiagID =
              diag::assignment_dynamic_property_has_immutable_base;
      }
    }
  } else if (isa<SubscriptExpr>(diagExpr)) {
      subElementDiagID = diag::assignment_subscript_has_immutable_base;
  } else if (auto *UME = dyn_cast<UnresolvedMemberExpr>(diagExpr)) {
    subElementDiagID = diag::assignment_lhs_is_immutable_property;
  } else {
    subElementDiagID = diag::assignment_lhs_is_immutable_variable;
  }

  AssignmentFailure failure(diagExpr, getSolution(), loc, subElementDiagID,
                            rvalueDiagID);
  return failure.diagnose();
}

bool RValueTreatedAsLValueFailure::diagnoseAsNote() {
  auto overload = getCalleeOverloadChoiceIfAvailable(getLocator());
  if (!(overload && overload->choice.isDecl()))
    return false;

  auto *decl = overload->choice.getDecl();
  emitDiagnosticAt(decl, diag::candidate_is_not_assignable,
                   decl->getDescriptiveKind(), decl->getName());
  return true;
}

static Decl *findSimpleReferencedDecl(const Expr *E) {
  if (auto *LE = dyn_cast<LoadExpr>(E))
    E = LE->getSubExpr();

  if (auto *DRE = dyn_cast<DeclRefExpr>(E))
    return DRE->getDecl();

  return nullptr;
}

static std::pair<Decl *, Decl *> findReferencedDecl(const Expr *E) {
  E = E->getValueProvidingExpr();

  if (auto *LE = dyn_cast<LoadExpr>(E))
    return findReferencedDecl(LE->getSubExpr());

  if (auto *AE = dyn_cast<AssignExpr>(E))
    return findReferencedDecl(AE->getDest());

  if (auto *D = findSimpleReferencedDecl(E))
    return std::make_pair(nullptr, D);

  if (auto *MRE = dyn_cast<MemberRefExpr>(E)) {
    if (auto *BaseDecl = findSimpleReferencedDecl(MRE->getBase()))
      return std::make_pair(BaseDecl, MRE->getMember().getDecl());
  }

  return std::make_pair(nullptr, nullptr);
}

bool TypeChecker::diagnoseSelfAssignment(const Expr *expr) {
  auto *assignExpr = dyn_cast<AssignExpr>(expr);
  if (!assignExpr)
    return false;

  auto *dstExpr = assignExpr->getDest();
  auto *srcExpr = assignExpr->getSrc();

  auto dstDecl = findReferencedDecl(dstExpr);
  auto srcDecl = findReferencedDecl(srcExpr);

  if (dstDecl.second && dstDecl == srcDecl) {
    auto &DE = dstDecl.second->getASTContext().Diags;
    DE.diagnose(expr->getLoc(), dstDecl.first ? diag::self_assignment_prop
                                              : diag::self_assignment_var)
        .highlight(dstExpr->getSourceRange())
        .highlight(srcExpr->getSourceRange());
    return true;
  }

  return false;
}

bool TrailingClosureAmbiguityFailure::diagnoseAsNote() {
  auto *anchor = castToExpr(getAnchor());
  const auto *expr = findParentExpr(anchor);
  auto *callExpr = dyn_cast_or_null<CallExpr>(expr);
  if (!callExpr)
    return false;
  if (!callExpr->hasTrailingClosure())
    return false;
  if (callExpr->getFn() != anchor)
    return false;

  llvm::SmallMapVector<Identifier, const ValueDecl *, 8> choicesByLabel;
  for (const auto &choice : Choices) {
    auto *callee = dyn_cast<AbstractFunctionDecl>(choice.getDecl());
    if (!callee)
      return false;

    const ParameterList *paramList = callee->getParameters();
    const ParamDecl *param = paramList->getArray().back();

    // Sanity-check that the trailing closure corresponds to this parameter.
    if (!param->hasInterfaceType() ||
        !param->getInterfaceType()->is<AnyFunctionType>())
      return false;

    Identifier trailingClosureLabel = param->getArgumentName();
    auto &choiceForLabel = choicesByLabel[trailingClosureLabel];

    // FIXME: Cargo-culted from diagnoseAmbiguity: apparently the same decl can
    // appear more than once?
    if (choiceForLabel == callee)
      continue;

    // If just providing the trailing closure label won't solve the ambiguity,
    // don't bother offering the fix-it.
    if (choiceForLabel != nullptr)
      return false;

    choiceForLabel = callee;
  }

  // If we got here, then all of the choices have unique labels. Offer them in
  // order.
  for (const auto &choicePair : choicesByLabel) {
    auto diag = emitDiagnosticAt(
        expr->getLoc(), diag::ambiguous_because_of_trailing_closure,
        choicePair.first.empty(), choicePair.second->getName());
    swift::fixItEncloseTrailingClosure(getASTContext(), diag, callExpr,
                                       choicePair.first);
  }

  return true;
}

AssignmentFailure::AssignmentFailure(Expr *destExpr, const Solution &solution,
                                     SourceLoc diagnosticLoc)
    : FailureDiagnostic(solution, destExpr), DestExpr(destExpr),
      Loc(diagnosticLoc),
      DeclDiagnostic(findDeclDiagonstic(getASTContext(), destExpr)),
      TypeDiagnostic(diag::assignment_lhs_not_lvalue) {}

bool AssignmentFailure::diagnoseAsError() {
  auto *DC = getDC();

  // Walk through the destination expression, resolving what the problem is.  If
  // we find a node in the lvalue path that is problematic, this returns it.
  Expr *immutableExpr;
  Optional<OverloadChoice> choice;
  std::tie(immutableExpr, choice) = resolveImmutableBase(DestExpr);

  // Attempt diagnostics based on the overload choice.
  if (choice.hasValue()) {

    auto getKeyPathArgument = [](SubscriptExpr *expr) {
      auto *TE = dyn_cast<TupleExpr>(expr->getIndex());
      assert(TE->getNumElements() == 1);
      assert(TE->getElementName(0).str() == "keyPath");
      return TE->getElement(0);
    };

    if (!choice->isDecl()) {
      if (choice->getKind() == OverloadChoiceKind::KeyPathApplication &&
          !isa<ApplyExpr>(immutableExpr)) {
        std::string message = "key path is read-only";
        if (auto *SE = dyn_cast<SubscriptExpr>(immutableExpr)) {
          if (auto *DRE = dyn_cast<DeclRefExpr>(getKeyPathArgument(SE))) {
            auto identifier = DRE->getDecl()->getBaseIdentifier();
            message =
                "'" + identifier.str().str() + "' is a read-only key path";
          }
        }
        emitDiagnosticAt(Loc, DeclDiagnostic, message)
            .highlight(immutableExpr->getSourceRange());
        return true;
      }
      return false;
    }

    // Otherwise, we cannot resolve this because the available setter candidates
    // are all mutating and the base must be mutating.  If we dug out a
    // problematic decl, we can produce a nice tailored diagnostic.
    if (auto *VD = dyn_cast<VarDecl>(choice->getDecl())) {
      std::string message = "'";
      message += VD->getName().str().str();
      message += "'";

      auto type = getType(immutableExpr);

      if (isKnownKeyPathType(type))
        message += " is read-only";
      else if (VD->isCaptureList())
        message += " is an immutable capture";
      else if (VD->isImplicit())
        message += " is immutable";
      else if (VD->isLet())
        message += " is a 'let' constant";
      else if (!VD->isSettable(DC))
        message += " is a get-only property";
      else if (!VD->isSetterAccessibleFrom(DC))
        message += " setter is inaccessible";
      else {
        message += " is immutable";
      }

      emitDiagnosticAt(Loc, DeclDiagnostic, message)
          .highlight(immutableExpr->getSourceRange());

      // If there is a masked property of the same type, emit a
      // note to fixit prepend a 'self.' or 'Type.'.
      if (auto typeContext = DC->getInnermostTypeContext()) {
        SmallVector<ValueDecl *, 2> results;
        DC->lookupQualified(typeContext->getSelfNominalTypeDecl(),
                            VD->createNameRef(), NL_QualifiedDefault, results);

        auto foundProperty = llvm::find_if(results, [&](ValueDecl *decl) {
          // We're looking for a settable property that is the same type as the
          // var we found.
          auto *var = dyn_cast<VarDecl>(decl);
          if (!var || var == VD)
            return false;

          if (!var->isSettable(DC) || !var->isSetterAccessibleFrom(DC))
            return false;

          if (!var->getType()->isEqual(VD->getType()))
            return false;

          // Don't suggest a property if we're in one of its accessors.
          auto *methodDC = DC->getInnermostMethodContext();
          if (auto *AD = dyn_cast_or_null<AccessorDecl>(methodDC))
            if (AD->getStorage() == var)
              return false;

          return true;
        });

        if (foundProperty != results.end()) {
          auto startLoc = immutableExpr->getStartLoc();
          auto *property = *foundProperty;
          auto selfTy = typeContext->getSelfTypeInContext();

          // If we found an instance property, suggest inserting "self.",
          // otherwise suggest "Type." for a static property.
          std::string fixItText;
          if (property->isInstanceMember()) {
            fixItText = "self.";
          } else {
            fixItText = selfTy->getString() + ".";
          }
          emitDiagnosticAt(startLoc, diag::masked_mutable_property, fixItText,
                           property->getDescriptiveKind(), selfTy)
              .fixItInsert(startLoc, fixItText);
        }
      }

      // If this is a simple variable marked with a 'let', emit a note to fixit
      // hint it to 'var'.
      VD->emitLetToVarNoteIfSimple(DC);
      return true;
    }

    // If the underlying expression was a read-only subscript, diagnose that.
    if (auto *SD = dyn_cast_or_null<SubscriptDecl>(choice->getDecl())) {
      StringRef message;
      if (!SD->supportsMutation())
        message = "subscript is get-only";
      else if (!SD->isSetterAccessibleFrom(DC))
        message = "subscript setter is inaccessible";
      else
        message = "subscript is immutable";

      emitDiagnosticAt(Loc, DeclDiagnostic, message)
          .highlight(immutableExpr->getSourceRange());
      return true;
    }

    // If we're trying to set an unapplied method, say that.
    if (auto *VD = choice->getDecl()) {
      std::string message = "'";
      message += VD->getBaseIdentifier().str();
      message += "'";

      auto diagID = DeclDiagnostic;
      if (auto *AFD = dyn_cast<AbstractFunctionDecl>(VD)) {
        if (AFD->hasImplicitSelfDecl()) {
          message += " is a method";
          diagID = diag::assignment_lhs_is_immutable_variable;
        } else {
          message += " is a function";
        }
      } else
        message += " is not settable";

      emitDiagnosticAt(Loc, diagID, message)
          .highlight(immutableExpr->getSourceRange());
      return true;
    }
  }

  // Fall back to producing diagnostics based on the expression since we
  // couldn't determine anything from the OverloadChoice.

  // If a keypath was the problem but wasn't resolved into a vardecl
  // it is ambiguous or unable to be used for setting.
  if (auto *KPE = dyn_cast_or_null<KeyPathExpr>(immutableExpr)) {
    emitDiagnosticAt(Loc, DeclDiagnostic, "immutable key path")
        .highlight(KPE->getSourceRange());
    return true;
  }

  if (auto LE = dyn_cast<LiteralExpr>(immutableExpr)) {
    emitDiagnosticAt(Loc, DeclDiagnostic, "literals are not mutable")
        .highlight(LE->getSourceRange());
    return true;
  }

  // If the expression is the result of a call, it is an rvalue, not a mutable
  // lvalue.
  if (auto *AE = dyn_cast<ApplyExpr>(immutableExpr)) {
    // Handle literals, which are a call to the conversion function.
    auto argsTuple =
        dyn_cast<TupleExpr>(AE->getArg()->getSemanticsProvidingExpr());
    if (isa<CallExpr>(AE) && AE->isImplicit() && argsTuple &&
        argsTuple->getNumElements() == 1) {
      if (auto LE = dyn_cast<LiteralExpr>(
              argsTuple->getElement(0)->getSemanticsProvidingExpr())) {
        emitDiagnosticAt(Loc, DeclDiagnostic, "literals are not mutable")
            .highlight(LE->getSourceRange());
        return true;
      }
    }

    std::string name = "call";
    if (isa<PrefixUnaryExpr>(AE) || isa<PostfixUnaryExpr>(AE))
      name = "unary operator";
    else if (isa<BinaryExpr>(AE))
      name = "binary operator";
    else if (isa<CallExpr>(AE))
      name = "function call";
    else if (isa<DotSyntaxCallExpr>(AE) || isa<DotSyntaxBaseIgnoredExpr>(AE))
      name = "method call";

    if (auto *DRE = dyn_cast<DeclRefExpr>(AE->getFn()->getValueProvidingExpr()))
      name = std::string("'") +
             DRE->getDecl()->getBaseIdentifier().str().str() + "'";

    emitDiagnosticAt(Loc, DeclDiagnostic, name + " returns immutable value")
        .highlight(AE->getSourceRange());
    return true;
  }

  if (auto contextualType = getContextualType(immutableExpr)) {
    Type neededType = contextualType->getInOutObjectType();
    Type actualType = getType(immutableExpr)->getInOutObjectType();
    if (!neededType->isEqual(actualType)) {
      if (DeclDiagnostic.ID != diag::cannot_pass_rvalue_inout_subelement.ID) {
        emitDiagnosticAt(Loc, DeclDiagnostic,
                         "implicit conversion from '" +
                             actualType->getString() + "' to '" +
                             neededType->getString() + "' requires a temporary")
            .highlight(immutableExpr->getSourceRange());
      }
      return true;
    }
  }

  if (auto IE = dyn_cast<IfExpr>(immutableExpr)) {
    emitDiagnosticAt(Loc, DeclDiagnostic,
                     "result of conditional operator '? :' is never mutable")
        .highlight(IE->getQuestionLoc())
        .highlight(IE->getColonLoc());
    return true;
  }

  emitDiagnosticAt(Loc, TypeDiagnostic, getType(DestExpr))
      .highlight(immutableExpr->getSourceRange());
  return true;
}

std::pair<Expr *, Optional<OverloadChoice>>
AssignmentFailure::resolveImmutableBase(Expr *expr) const {
  auto *DC = getDC();
  expr = expr->getValueProvidingExpr();

  auto isImmutable = [&DC](ValueDecl *decl) {
    if (auto *storage = dyn_cast<AbstractStorageDecl>(decl))
      return !storage->isSettable(nullptr) ||
             !storage->isSetterAccessibleFrom(DC);

    // If this is not something which could possibly be mutable,
    // then it's immutable.
    return true;
  };

  // Provide specific diagnostics for assignment to subscripts whose base expr
  // is known to be an rvalue.
  if (auto *SE = dyn_cast<SubscriptExpr>(expr)) {
    // If we found a decl for the subscript, check to see if it is a set-only
    // subscript decl.
    if (SE->hasDecl()) {
      const auto &declRef = SE->getDecl();
      if (auto *subscript =
              dyn_cast_or_null<SubscriptDecl>(declRef.getDecl())) {
        if (isImmutable(subscript))
          return {expr, OverloadChoice(getType(SE->getBase()), subscript,
                                       FunctionRefKind::DoubleApply)};
      }
    }

    Optional<OverloadChoice> member = getMemberRef(
        getConstraintLocator(SE, ConstraintLocator::SubscriptMember));

    // If it isn't settable, return it.
    if (member) {
      if (member->isDecl() && isImmutable(member->getDecl()))
        return {expr, member};

      // We still have a choice, the choice is not a decl
      if (!member->isDecl()) {
        // This must be a keypath application
        assert(member->getKind() == OverloadChoiceKind::KeyPathApplication);

        auto *argType = getType(SE->getIndex())->castTo<TupleType>();
        assert(argType->getNumElements() == 1);

        auto indexType = resolveType(argType->getElementType(0));

        if (auto bgt = indexType->getAs<BoundGenericType>()) {
          // In Swift versions lower than 5, this check will fail as read only
          // key paths can masquerade as writable for compatibilty reasons.
          // This is fine as in this case we just fall back on old diagnostics.
          auto &ctx = getASTContext();
          if (bgt->getDecl() == ctx.getKeyPathDecl() ||
              bgt->getDecl() == ctx.getPartialKeyPathDecl()) {
            return {expr, member};
          }
        }
      }
    }

    // If it is settable, then the base must be the problem, recurse.
    return resolveImmutableBase(SE->getBase());
  }

  // Look through property references.
  if (auto *UDE = dyn_cast<UnresolvedDotExpr>(expr)) {
    // If we found a decl for the UDE, check it.
    auto loc = getConstraintLocator(UDE, ConstraintLocator::Member);

    auto member = getMemberRef(loc);

    // If we can resolve a member, we can determine whether it is settable in
    // this context.
    if (member && member->isDecl() && isImmutable(member->getDecl()))
      return {expr, member};

    // If we weren't able to resolve a member or if it is mutable, then the
    // problem must be with the base, recurse.
    return resolveImmutableBase(UDE->getBase());
  }

  if (auto *MRE = dyn_cast<MemberRefExpr>(expr)) {
    // If the member isn't settable, then it is the problem: return it.
    if (auto member = dyn_cast<AbstractStorageDecl>(MRE->getMember().getDecl()))
      if (isImmutable(member))
        return {expr, OverloadChoice(getType(MRE->getBase()), member,
                                     FunctionRefKind::SingleApply)};

    // If we weren't able to resolve a member or if it is mutable, then the
    // problem must be with the base, recurse.
    return resolveImmutableBase(MRE->getBase());
  }

  if (auto *UME = dyn_cast<UnresolvedMemberExpr>(expr)) {
    auto loc = getConstraintLocator(UME, ConstraintLocator::UnresolvedMember);
    auto member = getMemberRef(loc);

    // If we can resolve a member, we can determine whether it is settable in
    // this context.
    if (member && member->isDecl() && isImmutable(member->getDecl()))
      return {expr, member};
    else
      return {expr, None};
  }

  if (auto *DRE = dyn_cast<DeclRefExpr>(expr))
    return {expr,
            OverloadChoice(Type(), DRE->getDecl(), FunctionRefKind::Unapplied)};

  // Look through x!
  if (auto *FVE = dyn_cast<ForceValueExpr>(expr))
    return resolveImmutableBase(FVE->getSubExpr());

  // Look through x?
  if (auto *BOE = dyn_cast<BindOptionalExpr>(expr))
    return resolveImmutableBase(BOE->getSubExpr());

  // Look through implicit conversions
  if (auto *ICE = dyn_cast<ImplicitConversionExpr>(expr))
    if (!isa<LoadExpr>(ICE->getSubExpr()))
      return resolveImmutableBase(ICE->getSubExpr());

  if (auto *SAE = dyn_cast<SelfApplyExpr>(expr))
    return resolveImmutableBase(SAE->getFn());

  return {expr, None};
}

Optional<OverloadChoice>
AssignmentFailure::getMemberRef(ConstraintLocator *locator) const {
  auto member = getOverloadChoiceIfAvailable(locator);
  if (!member)
    return None;

  if (!member->choice.isDecl())
    return member->choice;

  auto *DC = getDC();
  auto *decl = member->choice.getDecl();
  if (isa<SubscriptDecl>(decl) &&
      isValidDynamicMemberLookupSubscript(cast<SubscriptDecl>(decl), DC)) {
    auto *subscript = cast<SubscriptDecl>(decl);
    // If this is a keypath dynamic member lookup, we have to
    // adjust the locator to find member referred by it.
    if (isValidKeyPathDynamicMemberLookup(subscript)) {
      // Type has a following format:
      // `(Self) -> (dynamicMember: {Writable}KeyPath<T, U>) -> U`
      auto *fullType = member->openedFullType->castTo<FunctionType>();
      auto *fnType = fullType->getResult()->castTo<FunctionType>();

      auto paramTy = fnType->getParams()[0].getPlainType();
      auto keyPath = paramTy->getAnyNominal();
      auto memberLoc = getConstraintLocator(
          locator, LocatorPathElt::KeyPathDynamicMember(keyPath));

      auto memberRef = getOverloadChoiceIfAvailable(memberLoc);
      return memberRef ? Optional<OverloadChoice>(memberRef->choice) : None;
    }

    // If this is a string based dynamic lookup, there is no member declaration.
    return None;
  }

  return member->choice;
}

Diag<StringRef> AssignmentFailure::findDeclDiagonstic(ASTContext &ctx,
                                                      const Expr *destExpr) {
  if (isa<ApplyExpr>(destExpr) || isa<SelfApplyExpr>(destExpr))
    return diag::assignment_lhs_is_apply_expression;

  if (isa<UnresolvedDotExpr>(destExpr) || isa<MemberRefExpr>(destExpr))
    return diag::assignment_lhs_is_immutable_property;

  if (auto *subscript = dyn_cast<SubscriptExpr>(destExpr)) {
    auto diagID = diag::assignment_subscript_has_immutable_base;
    // If the destination is a subscript with a 'dynamicLookup:' label and if
    // the tuple is implicit, then this was actually a @dynamicMemberLookup
    // access. Emit a more specific diagnostic.
    if (subscript->getIndex()->isImplicit() &&
        subscript->getArgumentLabels().size() == 1 &&
        subscript->getArgumentLabels().front() == ctx.Id_dynamicMember)
      diagID = diag::assignment_dynamic_property_has_immutable_base;

    return diagID;
  }

  return diag::assignment_lhs_is_immutable_variable;
}

bool ContextualFailure::diagnoseAsError() {
  auto anchor = getAnchor();
  auto path = getLocator()->getPath();

  if (CTP == CTP_ReturnSingleExpr || CTP == CTP_ReturnStmt) {
    // Special case the "conversion to void".
    if (getToType()->isVoid()) {
      emitDiagnostic(diag::cannot_return_value_from_void_func)
          .highlight(getSourceRange());
      return true;
    }
  }

  if (diagnoseConversionToNil())
    return true;

  if (path.empty()) {
    if (auto *KPE = getAsExpr<KeyPathExpr>(anchor)) {
      emitDiagnosticAt(KPE->getLoc(),
                       diag::expr_smart_keypath_value_covert_to_contextual_type,
                       getFromType(), getToType());
      return true;
    }

    if (diagnoseCoercionToUnrelatedType())
      return true;

    if (isExpr<OptionalTryExpr>(anchor)) {
      emitDiagnostic(diag::cannot_convert_initializer_value, getFromType(),
                     getToType());
      return true;
    }

    if (isExpr<AssignExpr>(anchor)) {
      emitDiagnostic(diag::cannot_convert_assign, getFromType(), getToType());
      return true;
    }

    return false;
  }

  if (diagnoseMissingFunctionCall())
    return true;

  // Special case of some common conversions involving Swift.String
  // indexes, catching cases where people attempt to index them with an integer.
  if (isIntegerToStringIndexConversion()) {
    emitDiagnostic(diag::string_index_not_integer, getFromType())
        .highlight(getSourceRange());
    emitDiagnostic(diag::string_index_not_integer_note);
    return true;
  }

  auto fromType = getFromType();
  auto toType = getToType();

  Diag<Type, Type> diagnostic;
  switch (path.back().getKind()) {
  case ConstraintLocator::ClosureBody:
  case ConstraintLocator::ClosureResult: {
    auto *closure = castToExpr<ClosureExpr>(getRawAnchor());
    if (closure->hasExplicitResultType() &&
        closure->getExplicitResultTypeRepr()) {
      auto resultRepr = closure->getExplicitResultTypeRepr();
      emitDiagnosticAt(resultRepr->getStartLoc(),
                       diag::incorrect_explicit_closure_result, fromType,
                       toType)
          .fixItReplace(resultRepr->getSourceRange(), toType.getString());
      return true;
    }

    diagnostic = diag::cannot_convert_closure_result;
    break;
  }

  case ConstraintLocator::Condition: {
    // Tailored diagnostics for optional or assignment use
    // in condition expression.
    if (diagnoseConversionToBool())
      return true;

    diagnostic = diag::cannot_convert_condition_value;
    break;
  }
      
  case ConstraintLocator::InstanceType: {
    if (diagnoseCoercionToUnrelatedType())
      return true;
    break;
  }

  case ConstraintLocator::TernaryBranch: {
    auto *ifExpr = castToExpr<IfExpr>(getRawAnchor());
    fromType = getType(ifExpr->getThenExpr());
    toType = getType(ifExpr->getElseExpr());
    diagnostic = diag::if_expr_cases_mismatch;
    break;
  }

  case ConstraintLocator::ContextualType: {
    if (diagnoseConversionToBool())
      return true;

    if (diagnoseThrowsTypeMismatch())
      return true;

    if (diagnoseYieldByReferenceMismatch())
      return true;

    if (isExpr<OptionalTryExpr>(anchor) ||
        isExpr<OptionalEvaluationExpr>(anchor)) {
      auto objectType = fromType->getOptionalObjectType();
      if (objectType->isEqual(toType)) {
        MissingOptionalUnwrapFailure failure(getSolution(), getType(anchor),
                                             toType,
                                             getConstraintLocator(anchor));
        if (failure.diagnoseAsError())
          return true;
      }
    }

    if (CTP == CTP_ForEachStmt) {
      if (fromType->isAnyExistentialType()) {
        emitDiagnostic(diag::type_cannot_conform,
                       /*isExistentialType=*/true, fromType, 
                       fromType->isEqual(toType), toType);
        return true;
      }

      emitDiagnostic(
          diag::foreach_sequence_does_not_conform_to_expected_protocol,
          fromType, toType, bool(fromType->getOptionalObjectType()))
          .highlight(getSourceRange());
      return true;
    }

    if (auto *call = getAsExpr<CallExpr>(anchor)) {
      if (isa<ClosureExpr>(call->getFn()))
        CTP = CTP_ClosureResult;
    }

    if (auto msg = getDiagnosticFor(CTP, toType)) {
      diagnostic = *msg;
      break;
    }
    return false;
  }

  case ConstraintLocator::UnresolvedMemberChainResult: {
    auto &solution = getSolution();

    auto overload =
        getCalleeOverloadChoiceIfAvailable(getConstraintLocator(anchor));
    if (!(overload && overload->choice.isDecl()))
      return false;

    auto *choice = overload->choice.getDecl();
    auto fnType = fromType->getAs<FunctionType>();
    if (!fnType) {
      emitDiagnostic(diag::expected_result_in_contextual_member,
                     choice->getName(), fromType, toType);
      return true;
    }

    // If member type is a function and contextual type matches
    // its result type, most likely problem is related to a
    // missing call e.g.:
    //
    // struct S {
    //   static func foo() -> S {}
    // }
    //
    // let _: S = .foo

    auto params = fnType->getParams();

    ParameterListInfo info(
        params, choice,
        hasAppliedSelf(overload->choice, [&solution](Type type) {
          return solution.simplifyType(type);
        }));
    auto numMissingArgs = llvm::count_if(
        indices(params), [&info](const unsigned paramIdx) -> bool {
          return !info.hasDefaultArgument(paramIdx);
        });

    if (numMissingArgs == 0 || numMissingArgs > 1) {
      auto applyFixIt = [&](InFlightDiagnostic &diagnostic) {
        // If there are no parameters we can suggest a fix-it
        // to form an explicit call.
        if (numMissingArgs == 0)
          diagnostic.fixItInsertAfter(getSourceRange().End, "()");
      };
      if (fnType->getResult()->isEqual(toType)) {
        auto diag = emitDiagnostic(
                      diag::expected_parens_in_contextual_member_type,
                      choice->getName(), fnType->getResult());
        applyFixIt(diag);
      } else {
        auto diag = emitDiagnostic(diag::expected_parens_in_contextual_member,
                                   choice->getName());
        applyFixIt(diag);
      }
    } else {
      emitDiagnostic(diag::expected_argument_in_contextual_member,
                     choice->getName(), params.front().getPlainType());
    }

    return true;
  }

  case ConstraintLocator::FunctionBuilderBodyResult: {
    diagnostic = *getDiagnosticFor(CTP_Initialization, toType);
    break;
  }

  default:
    return false;
  }

  auto diag = emitDiagnostic(diagnostic, fromType, toType);
  diag.highlight(getSourceRange());

  (void)tryFixIts(diag);
  return true;
}

bool ContextualFailure::diagnoseAsNote() {
  auto overload = getCalleeOverloadChoiceIfAvailable(getLocator());
  if (!(overload && overload->choice.isDecl()))
    return false;

  auto *decl = overload->choice.getDecl();
  emitDiagnosticAt(decl, diag::found_candidate_type, getFromType());
  return true;
}

static Optional<Diag<Type>>
getContextualNilDiagnostic(ContextualTypePurpose CTP) {
  switch (CTP) {
  case CTP_Unused:
  case CTP_CannotFail:
    llvm_unreachable("These contextual type purposes cannot fail with a "
                     "conversion type specified!");
  case CTP_CalleeResult:
    llvm_unreachable("CTP_CalleeResult does not actually install a "
                     "contextual type");
  case CTP_Initialization:
    return diag::cannot_convert_initializer_value_nil;

  case CTP_ReturnSingleExpr:
  case CTP_ReturnStmt:
    return diag::cannot_convert_to_return_type_nil;

  case CTP_ThrowStmt:
  case CTP_ForEachStmt:
  case CTP_YieldByReference:
  case CTP_WrappedProperty:
  case CTP_ComposedPropertyWrapper:
    return None;

  case CTP_EnumCaseRawValue:
    return diag::cannot_convert_raw_initializer_value_nil;
  case CTP_DefaultParameter:
  case CTP_AutoclosureDefaultParameter:
    return diag::cannot_convert_default_arg_value_nil;
  case CTP_YieldByValue:
    return diag::cannot_convert_yield_value_nil;
  case CTP_CallArgument:
    return diag::cannot_convert_argument_value_nil;
  case CTP_ClosureResult:
    return diag::cannot_convert_closure_result_nil;
  case CTP_ArrayElement:
    return diag::cannot_convert_array_element_nil;
  case CTP_DictionaryKey:
    return diag::cannot_convert_dict_key_nil;
  case CTP_DictionaryValue:
    return diag::cannot_convert_dict_value_nil;
  case CTP_CoerceOperand:
    return diag::cannot_convert_coerce_nil;
  case CTP_AssignSource:
    return diag::cannot_convert_assign_nil;
  case CTP_SubscriptAssignSource:
    return diag::cannot_convert_subscript_assign_nil;
  case CTP_Condition:
    return diag::cannot_convert_condition_value_nil;
  }
  llvm_unreachable("Unhandled ContextualTypePurpose in switch");
}

bool ContextualFailure::diagnoseConversionToNil() const {
  auto anchor = getAnchor();

  if (!isExpr<NilLiteralExpr>(anchor))
    return false;

  auto *locator = getLocator();

  Optional<ContextualTypePurpose> CTP;
  // Easy case were failure has been identified as contextual already.
  if (locator->isLastElement<LocatorPathElt::ContextualType>()) {
    CTP = getContextualTypePurpose();
  } else {
    // Here we need to figure out where where `nil` is located.
    // It could be e.g. an argument to a subscript/call, assignment
    // source like `s[0] = nil` or an array element like `[nil]` or
    // `[nil: 42]` as a sub-expression to a larger one.
    auto *parentExpr = findParentExpr(getAsExpr(anchor));

    // Looks like it's something similar to `let _ = nil`.
    if (!parentExpr) {
      emitDiagnostic(diag::unresolved_nil_literal);
      return true;
    }

    // Two choices here - whether it's a regular assignment
    // e.g. `let _: S = nil` or a subscript one e.g. `s[0] = nil`.
    if (auto *AE = dyn_cast<AssignExpr>(parentExpr)) {
      CTP = isa<SubscriptExpr>(AE->getDest()) ? CTP_SubscriptAssignSource
                                              : CTP_AssignSource;
    } else if (isa<ArrayExpr>(parentExpr)) {
      CTP = CTP_ArrayElement;
    } else if (isa<ClosureExpr>(parentExpr)) {
      CTP = CTP_ClosureResult;
    } else if (isa<ParenExpr>(parentExpr) || isa<TupleExpr>(parentExpr)) {
      auto *enclosingExpr = findParentExpr(parentExpr);

      if (!enclosingExpr) {
        // If there is no enclosing expression it's something like
        // `(nil)` or `(a: nil)` which can't be inferred without a
        // contextual type.
        emitDiagnostic(diag::unresolved_nil_literal);
        return true;
      }

      if (auto *TE = dyn_cast<TupleExpr>(parentExpr)) {
        // In case of dictionary e.g. `[42: nil]` we need to figure
        // out whether nil is a "key" or a "value".
        if (isa<DictionaryExpr>(enclosingExpr)) {
          assert(TE->getNumElements() == 2);
          CTP = TE->getElement(0) == castToExpr(anchor) ? CTP_DictionaryKey
                                                        : CTP_DictionaryValue;
        } else {
          // Can't initialize one of the tuple elements with `nil`.
          CTP = CTP_Initialization;
        }
      }

      // `nil` is passed as an argument to a parameter which doesn't
      // expect it e.g. `foo(a: nil)`, `s[x: nil]` or `\S.[x: nil]`.
      // FIXME: Find a more robust way of checking this.
      if (isa<ApplyExpr>(enclosingExpr) || isa<SubscriptExpr>(enclosingExpr) ||
          isa<KeyPathExpr>(enclosingExpr))
        CTP = CTP_CallArgument;
    } else if (isa<CoerceExpr>(parentExpr)) {
      // `nil` is passed as a left-hand side of the coercion
      // operator e.g. `nil as Foo`
      CTP = CTP_CoerceOperand;
    } else {
      // Otherwise let's produce a generic `nil` conversion diagnostic.
      emitDiagnostic(diag::cannot_use_nil_with_this_type, getToType());
      return true;
    }
  }

  if (!CTP)
    return false;

  if (CTP == CTP_ThrowStmt) {
    emitDiagnostic(diag::cannot_throw_nil);
    return true;
  }

  auto diagnostic = getContextualNilDiagnostic(*CTP);
  if (!diagnostic)
    return false;

  emitDiagnostic(*diagnostic, getToType());

  if (CTP == CTP_Initialization) {
    auto *patternTR = getContextualTypeLoc(getRawAnchor()).getTypeRepr();
    if (!patternTR)
      return true;

    auto diag = emitDiagnosticAt(patternTR->getLoc(), diag::note_make_optional,
                                 OptionalType::get(getToType()));
    if (patternTR->isSimple()) {
      diag.fixItInsertAfter(patternTR->getEndLoc(), "?");
    } else {
      diag.fixItInsert(patternTR->getStartLoc(), "(");
      diag.fixItInsertAfter(patternTR->getEndLoc(), ")?");
    }
  }

  return true;
}

void ContextualFailure::tryFixIts(InFlightDiagnostic &diagnostic) const {
  auto *locator = getLocator();
  // Can't apply any of the fix-its below if this failure
  // is related to `inout` argument.
  if (locator->isLastElement<LocatorPathElt::LValueConversion>())
    return;

  if (trySequenceSubsequenceFixIts(diagnostic))
    return;

  if (tryIntegerCastFixIts(diagnostic))
    return;

  if (tryProtocolConformanceFixIt(diagnostic))
    return;

  if (tryTypeCoercionFixIt(diagnostic))
    return;
}

bool ContextualFailure::diagnoseMissingFunctionCall() const {
  if (getLocator()->isLastElement<LocatorPathElt::RValueAdjustment>())
    return false;

  if (getLocator()
      ->isLastElement<LocatorPathElt::UnresolvedMemberChainResult>())
    return false;

  auto *srcFT = getFromType()->getAs<FunctionType>();
  if (!srcFT ||
      !(srcFT->getParams().empty() ||
        getLocator()->isLastElement<LocatorPathElt::PatternMatch>()))
    return false;

  auto toType = getToType();
  if (toType->is<AnyFunctionType>() ||
      !TypeChecker::isConvertibleTo(srcFT->getResult(), toType, getDC()))
    return false;

  // Diagnose cases where the pattern tried to match associated values but
  // the case we found had none.
  if (auto match =
          getLocator()->getLastElementAs<LocatorPathElt::PatternMatch>()) {
    if (auto enumElementPattern =
            dyn_cast<EnumElementPattern>(match->getPattern())) {
      emitDiagnosticAt(enumElementPattern->getNameLoc(),
                       diag::enum_element_pattern_assoc_values_mismatch,
                       enumElementPattern->getName());
      emitDiagnosticAt(enumElementPattern->getNameLoc(),
                       diag::enum_element_pattern_assoc_values_remove)
          .fixItRemove(enumElementPattern->getSubPattern()->getSourceRange());
      return true;
    }
  }

  emitDiagnostic(diag::missing_nullary_call, srcFT->getResult())
      .highlight(getSourceRange())
      .fixItInsertAfter(getSourceRange().End, "()");

  tryComputedPropertyFixIts();
  return true;
}

bool ContextualFailure::diagnoseCoercionToUnrelatedType() const {
  auto anchor = getAnchor();

  if (auto *coerceExpr = getAsExpr<CoerceExpr>(anchor)) {
    const auto fromType = getType(coerceExpr->getSubExpr());
    const auto toType = getType(coerceExpr->getCastTypeRepr());

    auto diagnostic = getDiagnosticFor(CTP_CoerceOperand, toType);

    auto diag = emitDiagnostic(*diagnostic, fromType, toType);
    diag.highlight(getSourceRange());

    (void)tryFixIts(diag);
    
    return true;
  }

  return false;
}

bool ContextualFailure::diagnoseConversionToBool() const {
  auto toType = getToType();
  if (!toType->isBool())
    return false;

  auto *anchor = castToExpr(getAnchor());
  // Check for "=" converting to Bool.  The user probably meant ==.
  if (auto *AE = dyn_cast<AssignExpr>(anchor->getValueProvidingExpr())) {
    emitDiagnosticAt(AE->getEqualLoc(), diag::use_of_equal_instead_of_equality)
        .fixItReplace(AE->getEqualLoc(), "==")
        .highlight(AE->getDest()->getLoc())
        .highlight(AE->getSrc()->getLoc());
    return true;
  }

  // Determine if the boolean negation operator was applied to the anchor. This
  // upwards traversal of the AST is somewhat fragile, but enables much better
  // diagnostics if someone attempts to use an optional or integer as a boolean
  // condition.
  SourceLoc notOperatorLoc;
  if (Expr *parent = findParentExpr(anchor)) {
    if (isa<ParenExpr>(parent) && parent->isImplicit()) {
      if ((parent = findParentExpr(parent))) {
        auto parentOperatorApplication = dyn_cast<PrefixUnaryExpr>(parent);
        if (parentOperatorApplication) {
          auto operatorRefExpr =
              dyn_cast<DeclRefExpr>(parentOperatorApplication->getFn());
          if (operatorRefExpr && operatorRefExpr->getDecl()->getBaseName() ==
                                     getASTContext().Id_NegationOperator) {
            notOperatorLoc = operatorRefExpr->getLoc();
          }
        }
      }
    }
  }

  // If we're trying to convert something from optional type to Bool, then a
  // comparison against nil was probably expected.
  auto fromType = getFromType();
  if (fromType->getOptionalObjectType()) {
    StringRef prefix = "((";
    StringRef suffix;
    if (notOperatorLoc.isValid())
      suffix = ") == nil)";
    else
      suffix = ") != nil)";

    // Check if we need the inner parentheses.
    // Technically we only need them if there's something in 'expr' with
    // lower precedence than '!=', but the code actually comes out nicer
    // in most cases with parens on anything non-trivial.
    if (anchor->canAppendPostfixExpression()) {
      prefix = prefix.drop_back();
      suffix = suffix.drop_front();
    }
    // FIXME: The outer parentheses may be superfluous too.

    emitDiagnostic(diag::optional_used_as_boolean, fromType,
                   notOperatorLoc.isValid())
        .fixItInsert(getSourceRange().Start, prefix)
        .fixItInsertAfter(getSourceRange().End, suffix)
        .fixItRemove(notOperatorLoc);
    return true;
  }

  // If we're trying to convert something from optional type to an integer, then
  // a comparison against nil was probably expected.
  if (conformsToKnownProtocol(fromType, KnownProtocolKind::BinaryInteger) &&
      conformsToKnownProtocol(fromType,
                              KnownProtocolKind::ExpressibleByIntegerLiteral)) {
    StringRef prefix = "((";
    StringRef suffix;
    if (notOperatorLoc.isValid())
      suffix = ") == 0)";
    else
      suffix = ") != 0)";

    // Check if we need the inner parentheses.
    // Technically we only need them if there's something in 'expr' with
    // lower precedence than '!=', but the code actually comes out nicer
    // in most cases with parens on anything non-trivial.
    if (anchor->canAppendPostfixExpression()) {
      prefix = prefix.drop_back();
      suffix = suffix.drop_front();
    }
    // FIXME: The outer parentheses may be superfluous too.

    emitDiagnostic(diag::integer_used_as_boolean, fromType,
                   notOperatorLoc.isValid())
        .fixItInsert(getSourceRange().Start, prefix)
        .fixItInsertAfter(getSourceRange().End, suffix)
        .fixItRemove(notOperatorLoc);
    return true;
  }

  return false;
}

bool ContextualFailure::diagnoseThrowsTypeMismatch() const {
  // If this is conversion failure due to a return statement with an argument
  // that cannot be coerced to the result type of the function, emit a
  // specific error.
  if (CTP != CTP_ThrowStmt)
    return false;

  auto anchor = getAnchor();

  // If we tried to throw the error code of an error type, suggest object
  // construction.
  auto &Ctx = getASTContext();
  if (auto errorCodeProtocol =
          Ctx.getProtocol(KnownProtocolKind::ErrorCodeProtocol)) {
    Type errorCodeType = getFromType();
    auto conformance = TypeChecker::conformsToProtocol(
        errorCodeType, errorCodeProtocol, getDC());
    if (conformance) {
      Type errorType =
          conformance
              .getTypeWitnessByName(errorCodeType, getASTContext().Id_ErrorType)
              ->getCanonicalType();
      if (errorType) {
        auto diagnostic = emitDiagnostic(diag::cannot_throw_error_code,
                                         errorCodeType, errorType);
        if (auto *UDE = getAsExpr<UnresolvedDotExpr>(anchor)) {
          diagnostic.fixItInsert(UDE->getDotLoc(), "(");
          diagnostic.fixItInsertAfter(UDE->getEndLoc(), ")");
        }
        return true;
      }
    }
  }

  // The conversion destination of throw is always ErrorType (at the moment)
  // if this ever expands, this should be a specific form like () is for
  // return.
  emitDiagnostic(diag::cannot_convert_thrown_type, getFromType())
      .highlight(getSourceRange());
  return true;
}

bool ContextualFailure::diagnoseYieldByReferenceMismatch() const {
  if (CTP != CTP_YieldByReference)
    return false;

  auto anchor = getAnchor();
  auto exprType = getType(anchor, /*wantRValue=*/false);
  auto contextualType = getToType();

  if (auto exprLV = exprType->getAs<LValueType>()) {
    emitDiagnostic(diag::cannot_yield_wrong_type_by_reference,
                   exprLV->getObjectType(), contextualType);
  } else if (exprType->isEqual(contextualType)) {
    emitDiagnostic(diag::cannot_yield_rvalue_by_reference_same_type, exprType);
  } else {
    emitDiagnostic(diag::cannot_yield_rvalue_by_reference, exprType,
                   contextualType);
  }
  return true;
}

bool ContextualFailure::tryIntegerCastFixIts(
    InFlightDiagnostic &diagnostic) const {
  auto fromType = getFromType();
  auto toType = getToType();

  if (!isIntegerType(fromType) || !isIntegerType(toType))
    return false;

  auto getInnerCastedExpr = [&](const Expr *expr) -> Expr * {
    if (auto *CE = dyn_cast<CoerceExpr>(expr))
      return CE->getSubExpr();

    auto *CE = dyn_cast<CallExpr>(expr);
    if (!CE)
      return nullptr;
    if (!isa<ConstructorRefCallExpr>(CE->getFn()))
      return nullptr;
    auto *parenE = dyn_cast<ParenExpr>(CE->getArg());
    if (!parenE)
      return nullptr;
    return parenE->getSubExpr();
  };

  if (auto *anchor = getAsExpr(getAnchor())) {
    if (Expr *innerE = getInnerCastedExpr(anchor)) {
      Type innerTy = getType(innerE);
      if (TypeChecker::isConvertibleTo(innerTy, toType, getDC())) {
        // Remove the unnecessary cast.
        diagnostic.fixItRemoveChars(getLoc(), innerE->getStartLoc())
            .fixItRemove(getSourceRange().End);
        return true;
      }
    }
  }

  // Add a wrapping integer cast.
  std::string convWrapBefore = toType.getString();
  convWrapBefore += "(";
  std::string convWrapAfter = ")";
  SourceRange exprRange = getSourceRange();
  diagnostic.fixItInsert(exprRange.Start, convWrapBefore);
  diagnostic.fixItInsertAfter(exprRange.End, convWrapAfter);
  return true;
}

bool ContextualFailure::trySequenceSubsequenceFixIts(
    InFlightDiagnostic &diagnostic) const {
  if (!getASTContext().getStdlibModule())
    return false;

  auto String = getASTContext().getStringDecl()->getDeclaredInterfaceType();
  auto Substring = getASTContext().getSubstringDecl()->getDeclaredInterfaceType();

  // Substring -> String conversion
  // Wrap in String.init
  if (getFromType()->isEqual(Substring)) {
    if (getToType()->isEqual(String)) {
      auto *anchor = castToExpr(getAnchor())->getSemanticsProvidingExpr();
      if (auto *CE = dyn_cast<CoerceExpr>(anchor)) {
        anchor = CE->getSubExpr();
      }

      if (auto *call = dyn_cast<CallExpr>(anchor)) {
        auto *fnExpr = call->getFn();
        if (auto *closure = dyn_cast<ClosureExpr>(fnExpr)) {
          if (closure->hasSingleExpressionBody())
            anchor = closure->getSingleExpressionBody();
        }
      }

      auto range = anchor->getSourceRange();
      diagnostic.fixItInsert(range.Start, "String(");
      diagnostic.fixItInsertAfter(range.End, ")");
      return true;
    }
  }

  return false;
}

bool ContextualFailure::tryTypeCoercionFixIt(
    InFlightDiagnostic &diagnostic) const {
  auto fromType = getFromType();
  auto toType = getToType();

  // Look through optional types; casts can add them, but can't remove extra
  // ones.
  bool bothOptional =
      fromType->getOptionalObjectType() && toType->getOptionalObjectType();
  if (bothOptional)
    fromType = fromType->getOptionalObjectType();
  toType = toType->lookThroughAllOptionalTypes();

  if (!toType->hasTypeRepr())
    return false;

  CheckedCastKind Kind =
      TypeChecker::typeCheckCheckedCast(fromType, toType,
                                        CheckedCastContextKind::None, getDC(),
                                        SourceLoc(), nullptr, SourceRange());

  if (Kind != CheckedCastKind::Unresolved) {
    bool canUseAs = Kind == CheckedCastKind::Coercion ||
                    Kind == CheckedCastKind::BridgingCoercion;
    if (bothOptional && canUseAs)
      toType = OptionalType::get(toType);
    diagnostic.fixItInsert(Lexer::getLocForEndOfToken(getASTContext().SourceMgr,
                                                      getSourceRange().End),
                           diag::insert_type_coercion, canUseAs, toType);
    return true;
  }

  return false;
}

bool ContextualFailure::tryProtocolConformanceFixIt(
    InFlightDiagnostic &diagnostic) const {
  auto innermostTyCtx = getDC()->getInnermostTypeContext();
  if (!innermostTyCtx)
    return false;

  auto nominal = innermostTyCtx->getSelfNominalTypeDecl();
  if (!nominal)
    return false;

  auto fromType = getFromType();
  // We need to get rid of optionals and parens as it's not relevant when
  // printing the diagnostic and the fix-it.
  auto unwrappedToType =
      getToType()->lookThroughAllOptionalTypes()->getWithoutParens();

  // If the protocol requires a class & we don't have one (maybe the context
  // is a struct), then bail out instead of offering a broken fix-it later on.
  auto requiresClass = false;
  ExistentialLayout layout;
  if (unwrappedToType->isExistentialType()) {
    layout = unwrappedToType->getExistentialLayout();
    requiresClass = layout.requiresClass();
  }

  if (requiresClass && !fromType->is<ClassType>()) {
    return false;
  }

  // We can only offer a fix-it if we're assigning to a protocol type and
  // the type we're assigning is the same as the innermost type context.
  bool shouldOfferFixIt = nominal->getSelfTypeInContext()->isEqual(fromType) &&
                          unwrappedToType->isExistentialType();
  if (!shouldOfferFixIt)
    return false;

  diagnostic.flush();

  // Let's build a list of protocols that the context does not conform to.
  SmallVector<std::string, 8> missingProtoTypeStrings;
  SmallVector<ProtocolDecl *, 8> missingProtocols;
  for (auto protocol : layout.getProtocols()) {
    if (!TypeChecker::conformsToProtocol(fromType, protocol->getDecl(), getDC())) {
      missingProtoTypeStrings.push_back(protocol->getString());
      missingProtocols.push_back(protocol->getDecl());
    }
  }

  // If we have a protocol composition type and we don't conform to all
  // the protocols of the composition, then store the composition directly.
  // This is because we need to append 'Foo & Bar' instead of 'Foo, Bar' in
  // order to match the written type.
  if (auto compositionTy = unwrappedToType->getAs<ProtocolCompositionType>()) {
    if (compositionTy->getMembers().size() == missingProtoTypeStrings.size()) {
      missingProtoTypeStrings = {compositionTy->getString()};
    }
  }

  assert(!missingProtoTypeStrings.empty() &&
         "type already conforms to all the protocols?");

  // Combine all protocol names together, separated by commas.
  std::string protoString = llvm::join(missingProtoTypeStrings, ", ");

  // Emit a diagnostic to inform the user that they need to conform to the
  // missing protocols.
  auto conformanceDiag =
      emitDiagnostic(diag::assign_protocol_conformance_fix_it, unwrappedToType,
                     nominal->getDescriptiveKind(), fromType);
  if (nominal->getInherited().size() > 0) {
    auto lastInherited = nominal->getInherited().back().getLoc();
    auto lastInheritedEndLoc =
        Lexer::getLocForEndOfToken(getASTContext().SourceMgr, lastInherited);
    conformanceDiag.fixItInsert(lastInheritedEndLoc, ", " + protoString);
  } else {
    auto nameEndLoc = Lexer::getLocForEndOfToken(getASTContext().SourceMgr,
                                                 nominal->getNameLoc());
    conformanceDiag.fixItInsert(nameEndLoc, ": " + protoString);
  }

  // Emit fix-its to insert requirement stubs if we're in editor mode.
  if (!getASTContext().LangOpts.DiagnosticsEditorMode) {
    return true;
  }

  {
    llvm::SmallString<128> Text;
    llvm::raw_svector_ostream SS(Text);
    llvm::SetVector<MissingWitness> missingWitnesses;
    for (auto protocol : missingProtocols) {
      auto conformance = NormalProtocolConformance(
          nominal->getDeclaredType(), protocol, SourceLoc(), nominal,
          ProtocolConformanceState::Incomplete);
      ConformanceChecker checker(getASTContext(), &conformance,
                                 missingWitnesses);
      checker.resolveValueWitnesses();
      checker.resolveTypeWitnesses();
    }

    for (auto decl : missingWitnesses) {
      swift::printRequirementStub(decl.requirement, nominal, nominal->getDeclaredType(),
                                  nominal->getStartLoc(), SS);
    }

    if (!Text.empty()) {
      conformanceDiag.fixItInsertAfter(nominal->getBraces().Start, Text.str());
    }
  }

  return true;
}

void ContextualFailure::tryComputedPropertyFixIts() const {
  if (!isExpr<ClosureExpr>(getAnchor()))
    return;

  // It is possible that we're looking at a stored property being
  // initialized with a closure. Something like:
  //
  // var foo: Int = { return 0 }
  //
  // Let's offer another fix-it to remove the '=' to turn the stored
  // property into a computed property. If the variable is immutable, then
  // replace the 'let' with a 'var'.

  PatternBindingDecl *PBD = nullptr;

  if (auto TLCD = dyn_cast<TopLevelCodeDecl>(getDC())) {
    if (TLCD->getBody()->isImplicit()) {
      if (auto decl = TLCD->getBody()->getFirstElement().dyn_cast<Decl *>()) {
        if (auto binding = dyn_cast<PatternBindingDecl>(decl)) {
          PBD = binding;
        }
      }
    }
  } else if (auto PBI = dyn_cast<PatternBindingInitializer>(getDC())) {
    PBD = PBI->getBinding();
  }

  if (PBD) {
    if (auto VD = PBD->getSingleVar()) {
      const auto i = PBD->getPatternEntryIndexForVarDecl(VD);
      auto *initExpr = PBD->getInit(i);
      if (!VD->isStatic() &&
          !VD->getAttrs().getAttribute<DynamicReplacementAttr>() &&
          initExpr && isa<ClosureExpr>(initExpr)) {
        auto diag = emitDiagnostic(diag::extension_stored_property_fixit,
                                   VD->getName());
        diag.fixItRemove(PBD->getEqualLoc(i));

        if (VD->isLet()) {
          diag.fixItReplace(PBD->getStartLoc(), getTokenText(tok::kw_var));
        }

        if (auto lazyAttr = VD->getAttrs().getAttribute<LazyAttr>()) {
          diag.fixItRemove(lazyAttr->getRange());
        }
      }
    }
  }
}

bool ContextualFailure::isIntegerToStringIndexConversion() const {
  auto kind = KnownProtocolKind::ExpressibleByIntegerLiteral;

  auto fromType = getFromType();
  auto toType = getToType()->getCanonicalType();
  return (conformsToKnownProtocol(fromType, kind) &&
          toType.getString() == "String.CharacterView.Index");
}

Optional<Diag<Type, Type>>
ContextualFailure::getDiagnosticFor(ContextualTypePurpose context,
                                    Type contextualType) {
  auto forProtocol = contextualType->isExistentialType();
  switch (context) {
  case CTP_Initialization: {
    if (contextualType->isAnyObject())
      return diag::cannot_convert_initializer_value_anyobject;

    return forProtocol ? diag::cannot_convert_initializer_value_protocol
                       : diag::cannot_convert_initializer_value;
  }
  case CTP_ReturnStmt:
  case CTP_ReturnSingleExpr: {
    if (contextualType->isAnyObject())
      return diag::cannot_convert_return_type_to_anyobject;

    return forProtocol ? diag::cannot_convert_to_return_type_protocol
                       : diag::cannot_convert_to_return_type;
  }
  case CTP_EnumCaseRawValue:
    return diag::cannot_convert_raw_initializer_value;
  case CTP_DefaultParameter:
  case CTP_AutoclosureDefaultParameter:
    return forProtocol ? diag::cannot_convert_default_arg_value_protocol
                       : diag::cannot_convert_default_arg_value;
  case CTP_YieldByValue:
    return forProtocol ? diag::cannot_convert_yield_value_protocol
                       : diag::cannot_convert_yield_value;
  case CTP_CallArgument: {
    if (contextualType->isAnyObject())
      return diag::cannot_convert_argument_value_anyobject;

    return forProtocol ? diag::cannot_convert_argument_value_protocol
                       : diag::cannot_convert_argument_value;
  }
  case CTP_ClosureResult:
    return forProtocol ? diag::cannot_convert_closure_result_protocol
                       : diag::cannot_convert_closure_result;
  case CTP_ArrayElement:
    return forProtocol ? diag::cannot_convert_array_element_protocol
                       : diag::cannot_convert_array_element;
  case CTP_DictionaryKey:
    return forProtocol ? diag::cannot_convert_dict_key_protocol
                       : diag::cannot_convert_dict_key;
  case CTP_DictionaryValue:
    return forProtocol ? diag::cannot_convert_dict_value_protocol
                       : diag::cannot_convert_dict_value;
  case CTP_CoerceOperand:
    return forProtocol ? diag::cannot_convert_coerce_protocol
                       : diag::cannot_convert_coerce;
  case CTP_AssignSource: {
    if (contextualType->isAnyObject())
      return diag::cannot_convert_assign_anyobject;

    return forProtocol ? diag::cannot_convert_assign_protocol
                       : diag::cannot_convert_assign;
  }
  case CTP_SubscriptAssignSource:
    return forProtocol ? diag::cannot_convert_subscript_assign_protocol
                       : diag::cannot_convert_subscript_assign;
  case CTP_Condition:
    return diag::cannot_convert_condition_value;

  case CTP_WrappedProperty:
    return diag::wrapped_value_mismatch;
  case CTP_ComposedPropertyWrapper:
    return diag::composed_property_wrapper_mismatch;

  case CTP_ThrowStmt:
  case CTP_ForEachStmt:
  case CTP_Unused:
  case CTP_CannotFail:
  case CTP_YieldByReference:
  case CTP_CalleeResult:
    break;
  }
  return None;
}

bool TupleContextualFailure::diagnoseAsError() {
  Diag<Type, Type> diagnostic;
  auto purpose = getContextualTypePurpose();
  if (isNumElementsMismatch())
    diagnostic = diag::tuple_types_not_convertible_nelts;
  else if ((purpose == CTP_Initialization) && !getContextualType(getAnchor()))
    diagnostic = diag::tuple_types_not_convertible;
  else if (auto diag = getDiagnosticFor(purpose, getToType()))
    diagnostic = *diag;
  else
    return false;

  emitDiagnostic(diagnostic, getFromType(), getToType());
  return true;
}

bool FunctionTypeMismatch::diagnoseAsError() {
  auto purpose = getContextualTypePurpose();
  auto diagnostic = getDiagnosticFor(purpose, getToType());
  if (!diagnostic)
    return false;

  emitDiagnostic(*diagnostic, getFromType(), getToType());
  return true;
}

bool AutoClosureForwardingFailure::diagnoseAsError() {
  auto argRange = getSourceRange();
  emitDiagnostic(diag::invalid_autoclosure_forwarding)
      .highlight(argRange)
      .fixItInsertAfter(argRange.End, "()");
  return true;
}

bool AutoClosurePointerConversionFailure::diagnoseAsError() {
  auto diagnostic = diag::invalid_autoclosure_pointer_conversion;
  emitDiagnostic(diagnostic, getFromType(), getToType())
      .highlight(getSourceRange());
  return true;
}

bool NonOptionalUnwrapFailure::diagnoseAsError() {
  auto anchor = getAnchor();

  auto diagnostic = diag::invalid_optional_chain;
  if (isExpr<ForceValueExpr>(anchor))
    diagnostic = diag::invalid_force_unwrap;

  auto range = getSourceRange();
  emitDiagnostic(diagnostic, BaseType).highlight(range).fixItRemove(range.End);
  return true;
}

ASTNode MissingCallFailure::getAnchor() const {
  auto anchor = FailureDiagnostic::getAnchor();

  if (auto *FVE = getAsExpr<ForceValueExpr>(anchor))
    return FVE->getSubExpr();

  return anchor;
}

bool MissingCallFailure::diagnoseAsError() {
  auto anchor = getAnchor();
  SourceLoc insertLoc = getSourceRange().End;

  // Calls are not yet supported by key path, but it
  // is useful to record this fix to diagnose chaining
  // where one of the key path components is a method
  // reference.
  if (isExpr<KeyPathExpr>(anchor))
    return false;

  auto path = getLocator()->getPath();
  if (!path.empty()) {
    const auto &last = path.back();

    switch (last.getKind()) {
    case ConstraintLocator::ContextualType:
    case ConstraintLocator::ApplyArgToParam: {
      auto fnType = getType(anchor)->castTo<FunctionType>();
      emitDiagnostic(diag::missing_nullary_call, fnType->getResult())
          .fixItInsertAfter(insertLoc, "()");
      return true;
    }

    case ConstraintLocator::FunctionResult: {
      path = path.drop_back();
      if (path.back().getKind() != ConstraintLocator::AutoclosureResult)
        break;

      LLVM_FALLTHROUGH;
    }

    case ConstraintLocator::AutoclosureResult: {
      auto loc = getConstraintLocator(getRawAnchor(), path.drop_back());
      AutoClosureForwardingFailure failure(getSolution(), loc);
      return failure.diagnoseAsError();
    }
    default:
      break;
    }
  }

  if (auto *DRE = getAsExpr<DeclRefExpr>(anchor)) {
    emitDiagnostic(diag::did_not_call_function,
                   DRE->getDecl()->getBaseIdentifier())
        .fixItInsertAfter(insertLoc, "()");
    return true;
  }

  if (auto *UDE = getAsExpr<UnresolvedDotExpr>(anchor)) {
    emitDiagnostic(diag::did_not_call_method,
                   UDE->getName().getBaseIdentifier())
        .fixItInsertAfter(insertLoc, "()");
    return true;
  }

  if (auto *DSCE = getAsExpr<DotSyntaxCallExpr>(anchor)) {
    if (auto *DRE = dyn_cast<DeclRefExpr>(DSCE->getFn())) {
      emitDiagnostic(diag::did_not_call_method,
                     DRE->getDecl()->getBaseIdentifier())
          .fixItInsertAfter(insertLoc, "()");
      return true;
    }
  }

  if (auto *AE = getAsExpr<AssignExpr>(anchor)) {
    auto *srcExpr = AE->getSrc();
    if (auto *fnType = getType(srcExpr)->getAs<FunctionType>()) {
      emitDiagnosticAt(srcExpr->getLoc(), diag::missing_nullary_call,
                       fnType->getResult())
          .highlight(srcExpr->getSourceRange())
          .fixItInsertAfter(srcExpr->getEndLoc(), "()");
      return true;
    }
  }

  emitDiagnostic(diag::did_not_call_function_value)
      .fixItInsertAfter(insertLoc, "()");
  return true;
}

bool ExtraneousPropertyWrapperUnwrapFailure::diagnoseAsError() {
  auto newPrefix = usingProjection() ? "$" : "_";

  if (auto *member = getReferencedMember()) {
    emitDiagnostic(diag::incorrect_property_wrapper_reference_member,
                   member->getDescriptiveKind(), member->getName(), false,
                   getToType())
        .fixItInsert(getLoc(), newPrefix);
    return true;
  }

  emitDiagnostic(diag::incorrect_property_wrapper_reference, getPropertyName(),
                 getFromType(), getToType(), false)
      .fixItInsert(getLoc(), newPrefix);
  return true;
}

bool MissingPropertyWrapperUnwrapFailure::diagnoseAsError() {
  auto endLoc = getLoc().getAdvancedLoc(1);

  if (auto *member = getReferencedMember()) {
    emitDiagnostic(diag::incorrect_property_wrapper_reference_member,
                   member->getDescriptiveKind(), member->getName(), true,
                   getToType())
        .fixItRemoveChars(getLoc(), endLoc);
    return true;
  }

  emitDiagnostic(diag::incorrect_property_wrapper_reference, getPropertyName(),
                 getFromType(), getToType(), true)
      .fixItRemoveChars(getLoc(), endLoc);
  return true;
}

bool SubscriptMisuseFailure::diagnoseAsError() {
  auto *locator = getLocator();
  auto &sourceMgr = getASTContext().SourceMgr;

  auto *memberExpr = castToExpr<UnresolvedDotExpr>(getRawAnchor());

  auto memberRange = getSourceRange();

  {
    auto rawAnchor = getRawAnchor();
    auto path = locator->getPath();
    simplifyLocator(rawAnchor, path, memberRange);
  }

  auto nameLoc = DeclNameLoc(memberRange.Start);

  auto diag = emitDiagnostic(diag::could_not_find_subscript_member_did_you_mean,
                             getType(getAnchor()));

  diag.highlight(memberRange).highlight(nameLoc.getSourceRange());

  if (auto *parentExpr = dyn_cast_or_null<ApplyExpr>(findParentExpr(memberExpr))) {
    auto *argExpr = parentExpr->getArg();

    auto toCharSourceRange = Lexer::getCharSourceRangeFromSourceRange;
    auto lastArgSymbol = toCharSourceRange(sourceMgr, argExpr->getEndLoc());

    diag.fixItReplace(SourceRange(argExpr->getStartLoc()),
                      getTokenText(tok::l_square));
    diag.fixItRemove(nameLoc.getSourceRange());
    diag.fixItRemove(SourceRange(memberExpr->getDotLoc()));

    if (sourceMgr.extractText(lastArgSymbol) == getTokenText(tok::r_paren))
      diag.fixItReplace(SourceRange(argExpr->getEndLoc()),
                        getTokenText(tok::r_square));
    else
      diag.fixItInsertAfter(argExpr->getEndLoc(), getTokenText(tok::r_square));
  } else {
    diag.fixItReplace(SourceRange(memberExpr->getDotLoc(), memberExpr->getLoc()), "[<#index#>]");
  }
  diag.flush();

  if (auto overload = getOverloadChoiceIfAvailable(locator)) {
    emitDiagnosticAt(overload->choice.getDecl(), diag::kind_declared_here,
                     DescriptiveDeclKind::Subscript);
  }

  return true;
}

bool SubscriptMisuseFailure::diagnoseAsNote() {
  if (auto overload = getOverloadChoiceIfAvailable(getLocator())) {
    emitDiagnosticAt(overload->choice.getDecl(), diag::found_candidate);
    return true;
  }
  return false;
}

/// When a user refers a enum case with a wrong member name, we try to find a
/// enum element whose name differs from the wrong name only in convention;
/// meaning their lower case counterparts are identical.
///   - DeclName is valid when such a correct case is found; invalid otherwise.
DeclName MissingMemberFailure::findCorrectEnumCaseName(
    Type Ty, TypoCorrectionResults &corrections, DeclNameRef memberName) {
  if (memberName.isSpecial() || !memberName.isSimpleName())
    return DeclName();
  if (!Ty->getEnumOrBoundGenericEnum())
    return DeclName();
  auto candidate =
      corrections.getUniqueCandidateMatching([&](ValueDecl *candidate) {
        return (isa<EnumElementDecl>(candidate) &&
                candidate->getBaseIdentifier().str().equals_lower(
                    memberName.getBaseIdentifier().str()));
      });
  return (candidate ? candidate->getName() : DeclName());
}

bool MissingMemberFailure::diagnoseAsError() {
  auto anchor = getRawAnchor();
  auto memberBase = getAnchor();

  if (diagnoseForDynamicCallable())
    return true;
  
  if (diagnoseInLiteralCollectionContext())
    return true;

  if (diagnoseForSubscriptMemberWithTupleBase())
    return true;

  auto baseType = resolveType(getBaseType())->getWithoutSpecifierType();

  DeclNameLoc nameLoc(::getLoc(anchor));
  if (auto *UDE = getAsExpr<UnresolvedDotExpr>(anchor)) {
    nameLoc = UDE->getNameLoc();
  } else if (auto *UME = getAsExpr<UnresolvedMemberExpr>(anchor)) {
    nameLoc = UME->getNameLoc();
  }

  auto emitBasicError = [&](Type baseType) {
    auto diagnostic = diag::could_not_find_value_member;

    if (auto *metatype = baseType->getAs<MetatypeType>()) {
      baseType = metatype->getInstanceType();
      diagnostic = diag::could_not_find_type_member;
    }

    if (baseType->is<TupleType>())
      diagnostic = diag::could_not_find_tuple_member;

    bool hasUnresolvedPattern = false;
    if (auto *E = getAsExpr(anchor)) {
      forEachExprInConstraintSystem(const_cast<Expr *>(E), [&](Expr *expr) {
        hasUnresolvedPattern |= isa<UnresolvedPatternExpr>(expr);
        return hasUnresolvedPattern ? nullptr : expr;
      });
    }

    if (hasUnresolvedPattern && !baseType->getAs<EnumType>()) {
      emitDiagnostic(diag::cannot_match_unresolved_expr_pattern_with_value,
                     baseType);
      return;
    }

    emitDiagnostic(diagnostic, baseType, getName())
        .highlight(getSourceRange())
        .highlight(nameLoc.getSourceRange());
  };

  TypoCorrectionResults corrections(getName(), nameLoc);
  auto tryTypoCorrection = [&] (Type type) {
    TypeChecker::performTypoCorrection(getDC(), DeclRefKind::Ordinary, type,
                                       defaultMemberLookupOptions, corrections);
  };

  if (getName().getBaseName().getKind() == DeclBaseName::Kind::Subscript) {
    if (auto *metatype = baseType->getAs<MetatypeType>()) {
      emitDiagnostic(diag::could_not_find_type_member,
                     metatype->getInstanceType(), getName())
          .highlight(getSourceRange());
    } else {
      emitDiagnostic(diag::could_not_find_value_subscript, baseType)
          .highlight(getSourceRange());
    }
  } else if (getName().getBaseName() == "deinit") {
    // Specialised diagnostic if trying to access deinitialisers
    emitDiagnostic(diag::destructor_not_accessible).highlight(getSourceRange());
  } else if (auto metatypeTy = baseType->getAs<MetatypeType>()) {
    auto instanceTy = metatypeTy->getInstanceType();
    tryTypoCorrection(baseType);

    if (DeclName rightName =
            findCorrectEnumCaseName(instanceTy, corrections, getName())) {
      emitDiagnostic(diag::could_not_find_enum_case, instanceTy, getName(),
                     rightName)
          .fixItReplace(nameLoc.getBaseNameLoc(),
                        rightName.getBaseIdentifier().str());
      return true;
    }

    if (auto correction = corrections.claimUniqueCorrection()) {
      auto diagnostic =
          emitDiagnostic(diag::could_not_find_type_member_corrected, instanceTy,
                         getName(), correction->CorrectedName);
      diagnostic.highlight(getSourceRange())
          .highlight(nameLoc.getSourceRange());
      correction->addFixits(diagnostic);
    } else if (instanceTy->getAnyNominal() &&
               getName().getBaseName() == DeclBaseName::createConstructor()) {
      auto &cs = getConstraintSystem();

      auto result = cs.performMemberLookup(
          ConstraintKind::ValueMember, getName().withoutArgumentLabels(),
          metatypeTy, FunctionRefKind::DoubleApply, getLocator(),
          /*includeInaccessibleMembers=*/true);

      // If there are no `init` members at all produce a tailored
      // diagnostic for that, otherwise fallback to generic
      // "no such member" one.
      if (result.ViableCandidates.empty() &&
          result.UnviableCandidates.empty()) {
        emitDiagnostic(diag::no_accessible_initializers, instanceTy)
            .highlight(getSourceRange());
      } else {
        emitBasicError(baseType);
      }
    } else {
      emitBasicError(baseType);
    }
  } else if (auto moduleTy = baseType->getAs<ModuleType>()) {
    emitDiagnosticAt(::getLoc(memberBase), diag::no_member_of_module,
                     moduleTy->getModule()->getName(), getName())
        .highlight(getSourceRange())
        .highlight(nameLoc.getSourceRange());
    return true;
  } else {
    // Check for a few common cases that can cause missing members.
    auto *ED = baseType->getEnumOrBoundGenericEnum();
    if (ED && getName().isSimpleName("rawValue")) {
      auto loc = ED->getNameLoc();
      if (loc.isValid()) {
        emitBasicError(baseType);
        emitDiagnosticAt(loc, diag::did_you_mean_raw_type);
        return true;
      }
    } else if (baseType->isAny()) {
      emitBasicError(baseType);

      auto range = getSourceRange();
      emitDiagnostic(diag::any_as_anyobject_fixit)
          .fixItInsert(range.Start, "(")
          .fixItInsertAfter(range.End, " as AnyObject)");
      return true;
    }
    
    tryTypoCorrection(baseType);
    
    // If locator points to the member found via key path dynamic member lookup,
    // we provide a custom diagnostic and emit typo corrections for the wrapper type too.
    if (getLocator()->isForKeyPathDynamicMemberLookup()) {
      auto memberBaseType = getType(memberBase)->getWithoutSpecifierType();

      tryTypoCorrection(memberBaseType);

      if (auto correction = corrections.claimUniqueCorrection()) {
        auto diagnostic = emitDiagnostic(
            diag::could_not_find_value_dynamic_member_corrected, memberBaseType,
            baseType, getName(), correction->CorrectedName);
        diagnostic.highlight(getSourceRange())
            .highlight(nameLoc.getSourceRange());
        correction->addFixits(diagnostic);
      } else {
        auto diagnostic =
            emitDiagnostic(diag::could_not_find_value_dynamic_member,
                           memberBaseType, baseType, getName());
        diagnostic.highlight(getSourceRange())
            .highlight(nameLoc.getSourceRange());
      }
    } else {
      if (auto correction = corrections.claimUniqueCorrection()) {
        auto diagnostic =
            emitDiagnostic(diag::could_not_find_value_member_corrected,
                           baseType, getName(), correction->CorrectedName);
        diagnostic.highlight(getSourceRange())
            .highlight(nameLoc.getSourceRange());

        correction->addFixits(diagnostic);
      } else {
        emitBasicError(baseType);
      }
    }
  }

  // Note all the correction candidates.
  corrections.noteAllCandidates();
  return true;
}

bool MissingMemberFailure::diagnoseForDynamicCallable() const {
  auto *locator = getLocator();
  if (!locator->isLastElement<LocatorPathElt::DynamicCallable>())
    return false;

  auto memberName = getName();
  auto arguments = memberName.getArgumentNames();
  assert(arguments.size() == 1);

  auto &ctx = getASTContext();
  if (arguments.front() == ctx.Id_withKeywordArguments) {
    emitDiagnostic(diag::missing_dynamic_callable_kwargs_method, getBaseType());
    return true;
  }

  return false;
}

bool MissingMemberFailure::diagnoseInLiteralCollectionContext() const {
  auto *expr = castToExpr(getAnchor());
  auto *parentExpr = findParentExpr(expr);
  auto &solution = getSolution();

  if (!(parentExpr && isa<UnresolvedMemberExpr>(expr)))
    return false;

  if (!isa<UnresolvedMemberChainResultExpr>(parentExpr))
    return false;

  parentExpr = findParentExpr(parentExpr);
  if (!parentExpr)
    return false;

  auto parentType = getType(parentExpr);

  if (!parentType->isKnownStdlibCollectionType() && !parentType->is<TupleType>())
    return false;

  if (isa<TupleExpr>(parentExpr)) {
    parentExpr = findParentExpr(parentExpr);
    if (!parentExpr)
      return false;
  }

  if (auto *defaultableVar =
          getRawType(parentExpr)->getAs<TypeVariableType>()) {
    if (solution.DefaultedConstraints.count(
            defaultableVar->getImpl().getLocator()) != 0) {
      emitDiagnostic(diag::unresolved_member_no_inference, getName());
      return true;
    }
  }
  return false;
}

bool MissingMemberFailure::diagnoseForSubscriptMemberWithTupleBase() const {
  auto locator = getLocator();
  auto baseType = resolveType(getBaseType())->getWithoutSpecifierType();

  auto *SE = getAsExpr<SubscriptExpr>(locator->getAnchor());
  if (!SE)
    return false;

  auto tupleType = baseType->getAs<TupleType>();
  // For non-tuple type or empty tuples, let's fallback to the general
  // diagnostic logic.
  if (!tupleType || tupleType->getNumElements() == 0)
    return false;

  auto *index = SE->getIndex();
  if (SE->getNumArguments() == 1) {
    auto *literal =
        dyn_cast<IntegerLiteralExpr>(index->getSemanticsProvidingExpr());

    llvm::Regex NumericRegex("^[0-9]+$");
    // Literal expressions may have other types of representations e.g. 0x01,
    // 0b01. So let's make sure to only suggest this tailored literal fix-it for
    // number only literals.
    if (literal && NumericRegex.match(literal->getDigitsText())) {
      unsigned int literalValue = 0;
      literal->getDigitsText().getAsInteger(/*Radix=*/0, literalValue);

      // Verify if the literal value is within the bounds of tuple elements.
      if (!literal->isNegative() &&
          literalValue < tupleType->getNumElements()) {
        llvm::SmallString<4> dotAccess;
        llvm::raw_svector_ostream OS(dotAccess);
        OS << "." << literalValue;

        emitDiagnostic(
            diag::could_not_find_subscript_member_tuple_did_you_mean_use_dot,
            baseType, literal->getDigitsText())
            .fixItReplace(index->getSourceRange(), OS.str());
        return true;
      }
    }

    // For subscript access on tuple base types where the subscript index is a
    // string literal expression which value matches a tuple element label,
    // let's suggest tuple label access.
    auto stringLiteral =
        dyn_cast<StringLiteralExpr>(index->getSemanticsProvidingExpr());
    if (stringLiteral && !stringLiteral->getValue().empty() &&
        llvm::any_of(tupleType->getElements(), [&](TupleTypeElt element) {
          return element.getName().is(stringLiteral->getValue());
        })) {
      llvm::SmallString<16> dotAccess;
      llvm::raw_svector_ostream OS(dotAccess);
      OS << "." << stringLiteral->getValue();

      emitDiagnostic(
          diag::could_not_find_subscript_member_tuple_did_you_mean_use_dot,
          baseType, stringLiteral->getValue())
          .fixItReplace(index->getSourceRange(), OS.str());
      return true;
    }
  }

  emitDiagnostic(diag::could_not_find_subscript_member_tuple, baseType);
  return true;
}

bool UnintendedExtraGenericParamMemberFailure::diagnoseAsError() {
  MissingMemberFailure::diagnoseAsError();

  auto baseType = resolveType(getBaseType())->getWithoutSpecifierType();
  auto archetype = baseType->getMetatypeInstanceType()->castTo<ArchetypeType>();
  auto genericTy =
      archetype->mapTypeOutOfContext()->castTo<GenericTypeParamType>();
  SourceLoc loc = genericTy->getDecl()->getSourceRange().End;
  StringRef replacement;

  if (archetype->getConformsTo().size()) {
    loc = loc.getAdvancedLoc(
        archetype->getConformsTo().back()->getName().getLength());
    replacement = " &";
  } else {
    loc = loc.getAdvancedLoc(archetype->getName().getLength());
    replacement = ":";
  }
  emitDiagnosticAt(loc, diag::did_you_mean_generic_param_as_conformance,
                   ParamName, archetype)
      .fixItReplaceChars(loc, loc.getAdvancedLoc(1), replacement);
  return true;
}

bool InvalidMemberRefOnExistential::diagnoseAsError() {
  auto anchor = getRawAnchor();

  DeclNameLoc nameLoc;
  if (auto *UDE = getAsExpr<UnresolvedDotExpr>(anchor)) {
    nameLoc = UDE->getNameLoc();
  } else if (auto *UME = getAsExpr<UnresolvedMemberExpr>(anchor)) {
    nameLoc = UME->getNameLoc();
  }

  emitDiagnostic(diag::could_not_use_member_on_existential, getBaseType(),
                 getName())
      .highlight(nameLoc.getSourceRange())
      .highlight(getSourceRange());
  return true;
}

bool AllowTypeOrInstanceMemberFailure::diagnoseAsError() {
  auto loc = getLoc();
  auto *DC = getDC();
  auto locator = getLocator();

  if (loc.isInvalid()) {
    return true;
  }

  auto getRootExpr = [this](const Expr *childExpr) {
    auto *currExpr = const_cast<Expr *>(childExpr);
    while (auto parent = findParentExpr(currExpr))
      currExpr = parent;
    return currExpr;
  };

  auto anchor = getAnchor();

  if (!anchor.is<Expr *>())
    return false;

  Expr *expr = findParentExpr(castToExpr(anchor));
  SourceRange baseRange = expr ? expr->getSourceRange() : SourceRange();

  // If the base is an implicit self type reference, and we're in a
  // an initializer, then the user wrote something like:
  //
  //   class Foo { let x = 1, y = x }
  //
  // which runs in type context, not instance context, or
  //
  //   class Bar {
  //     let otherwise = 1              // instance member
  //     var x: Int
  //     func init(x: Int =otherwise) { // default parameter
  //       self.x = x
  //     }
  //   }
  //
  // in which an instance member is used as a default value for a
  // parameter.
  //
  // Produce a tailored diagnostic for these cases since this
  // comes up and is otherwise non-obvious what is going on.

  if (Name.isSimpleName(DeclBaseName::createConstructor()) &&
      !BaseType->is<AnyMetatypeType>()) {
    if (auto *ctorRef = getAsExpr<UnresolvedDotExpr>(getRawAnchor())) {
      if (isa<SuperRefExpr>(ctorRef->getBase())) {
        emitDiagnostic(diag::super_initializer_not_in_initializer);
        return true;
      }

      auto isCallArgument = [this](Expr *expr) {
        auto argExpr = findParentExpr(expr);
        if (!argExpr)
          return false;
        auto possibleApplyExpr = findParentExpr(expr);
        return possibleApplyExpr && isa<ApplyExpr>(possibleApplyExpr);
      };

      auto *initCall = findParentExpr(findParentExpr(ctorRef));

      auto isMutable = [&DC](ValueDecl *decl) {
        if (auto *storage = dyn_cast<AbstractStorageDecl>(decl))
          return storage->isSettable(DC) && storage->isSetterAccessibleFrom(DC);

        return true;
      };

      auto *baseLoc = getConstraintLocator(ctorRef->getBase());
      if (auto selection = getCalleeOverloadChoiceIfAvailable(baseLoc)) {
        OverloadChoice choice = selection->choice;
        if (choice.isDecl() && isMutable(choice.getDecl()) &&
            !isCallArgument(initCall) &&
            getContextualTypePurpose(getRootExpr(ctorRef)) == CTP_Unused) {
          auto fixItLoc = ctorRef->getBase()->getSourceRange().End;
          emitDiagnostic(diag::init_not_instance_member_use_assignment)
              .fixItInsertAfter(fixItLoc, " = ");
          return true;
        }

        SourceRange fixItRng = ctorRef->getBase()->getSourceRange();
        emitDiagnostic(diag::init_not_instance_member)
            .fixItInsert(fixItRng.Start, "type(of: ")
            .fixItInsertAfter(fixItRng.End, ")");
        return true;
      }
    }
  }

  if (BaseType->is<AnyMetatypeType>() && !Member->isStatic()) {
    auto instanceTy = BaseType;

    if (auto *AMT = instanceTy->getAs<AnyMetatypeType>()) {
      instanceTy = AMT->getInstanceType();
    }

    auto *DC = getDC();
    if (DC->getContextKind() == DeclContextKind::Initializer) {
      auto *TypeDC = DC->getParent();
      bool propertyInitializer = true;
      // If the parent context is not a type context, we expect it
      // to be a defaulted parameter in a function declaration.
      if (!TypeDC->isTypeContext()) {
        assert(TypeDC->getContextKind() ==
               DeclContextKind::AbstractFunctionDecl &&
               "Expected function decl context for initializer!");
        TypeDC = TypeDC->getParent();
        propertyInitializer = false;
      }
      
      assert(TypeDC->isTypeContext() && "Expected type decl context!");
      
      if (TypeDC->getSelfNominalTypeDecl() == instanceTy->getAnyNominal()) {
        if (propertyInitializer) {
          emitDiagnostic(diag::instance_member_in_initializer, Name);
          return true;
        } else {
          emitDiagnostic(diag::instance_member_in_default_parameter, Name);
          return true;
        }
      }
    }

    if (auto *maybeCallExpr = getAsExpr(getRawAnchor())) {
      if (auto *UDE = dyn_cast<UnresolvedDotExpr>(maybeCallExpr)) {
        maybeCallExpr = UDE->getBase();
      }

      if (auto callExpr = dyn_cast<ApplyExpr>(maybeCallExpr)) {
        auto fnExpr = callExpr->getFn();
        auto fnType = getType(fnExpr)->getRValueType();
        auto arg = callExpr->getArg();

        if (fnType->is<ExistentialMetatypeType>()) {
          emitDiagnosticAt(arg->getStartLoc(),
                           diag::missing_init_on_metatype_initialization)
              .highlight(fnExpr->getSourceRange());
          return true;
        }
      }
    }

    // Check whether the instance member is declared on parent context and if so
    // provide more specialized message.
    auto memberTypeContext =
        Member->getDeclContext()->getInnermostTypeContext();
    auto currentTypeContext = getDC()->getInnermostTypeContext();

    if (memberTypeContext && currentTypeContext &&
        memberTypeContext->getSemanticDepth() <
        currentTypeContext->getSemanticDepth()) {
      emitDiagnostic(diag::could_not_use_instance_member_on_type,
                     currentTypeContext->getDeclaredInterfaceType(), Name,
                     memberTypeContext->getDeclaredInterfaceType(), true)
          .highlight(baseRange)
          .highlight(Member->getSourceRange());
      return true;
    }

    if (auto *UDE = getAsExpr<UnresolvedDotExpr>(getRawAnchor())) {
      auto *baseExpr = UDE->getBase();
      if (isa<TypeExpr>(baseExpr)) {
        emitDiagnostic(diag::instance_member_use_on_type, instanceTy, Name)
            .highlight(getSourceRange());
        return true;
      }
    }

    // Just emit a generic "instance member cannot be used" error
    emitDiagnostic(diag::could_not_use_instance_member_on_type, instanceTy,
                   Name, instanceTy, false)
        .highlight(getSourceRange());
    return true;
  } else {
    // If the base of the lookup is a protocol metatype, suggest
    // to replace the metatype with 'Self'
    // error saying the lookup cannot be on a protocol metatype
    Optional<InFlightDiagnostic> Diag;
    auto baseTy = BaseType;

    if (auto metatypeTy = baseTy->getAs<AnyMetatypeType>()) {
      auto instanceTy = metatypeTy->getInstanceType();

      // This will only happen if we have an unresolved dot expression
      // (.foo) where foo is a protocol member and the contextual type is
      // an optional protocol metatype.
      if (auto objectTy = instanceTy->getOptionalObjectType()) {
        instanceTy = objectTy;
        baseTy = MetatypeType::get(objectTy);
      }

      if (instanceTy->isExistentialType()) {
        // Give a customized message if we're accessing a member type
        // of a protocol -- otherwise a diagnostic talking about
        // static members doesn't make a whole lot of sense
        if (isa<TypeAliasDecl>(Member)) {
          Diag.emplace(
              emitDiagnostic(diag::typealias_outside_of_protocol, Name));
        } else if (isa<AssociatedTypeDecl>(Member)) {
          Diag.emplace(
              emitDiagnostic(diag::assoc_type_outside_of_protocol, Name));
        } else if (isa<ConstructorDecl>(Member)) {
          Diag.emplace(
              emitDiagnostic(diag::construct_protocol_by_name, instanceTy));
        } else {
          Diag.emplace(emitDiagnostic(
              diag::could_not_use_type_member_on_protocol_metatype, baseTy,
              Name));
        }

        Diag->highlight(baseRange).highlight(getSourceRange());

        // See through function decl context
        if (auto parent = getDC()->getInnermostTypeContext()) {
          // If we are in a protocol extension of 'Proto' and we see
          // 'Proto.static', suggest 'Self.static'
          if (auto extensionContext = parent->getExtendedProtocolDecl()) {
            if (extensionContext->getDeclaredType()->isEqual(instanceTy)) {
              Diag->fixItReplace(getSourceRange(), "Self");
            }
          }
        }

        return true;
      }
    }

    // If this is a reference to a static member by one of the key path
    // components, let's provide a tailored diagnostic and return because
    // that is unsupported so there is no fix-it.
    if (locator->isForKeyPathComponent()) {
      InvalidStaticMemberRefInKeyPath failure(getSolution(), Member, locator);
      return failure.diagnoseAsError();
    }

    if (isa<EnumElementDecl>(Member)) {
      Diag.emplace(
          emitDiagnostic(diag::could_not_use_enum_element_on_instance, Name));
    } else {
      Diag.emplace(emitDiagnostic(diag::could_not_use_type_member_on_instance,
                                  baseTy, Name));
    }

    Diag->highlight(getSourceRange());

    if (Name.isSimpleName(DeclBaseName::createConstructor()) &&
        !baseTy->is<AnyMetatypeType>()) {
      if (auto ctorRef = getAsExpr<UnresolvedDotExpr>(getRawAnchor())) {
        SourceRange fixItRng = ctorRef->getNameLoc().getSourceRange();
        Diag->fixItInsert(fixItRng.Start, "type(of: ");
        Diag->fixItInsertAfter(fixItRng.End, ")");
        return true;
      }
    }

    // Determine the contextual type of the expression
    Type contextualType = getContextualType(getRawAnchor());
    // Try to provide a fix-it that only contains a '.'
    if (contextualType && baseTy->isEqual(contextualType)) {
      Diag->fixItInsert(loc, ".");
      return true;
    }

    // Check if the expression is the matching operator ~=, most often used in
    // case statements. If so, try to provide a single dot fix-it
    const Expr *contextualTypeNode = getRootExpr(getAsExpr(getAnchor()));

    // The '~=' operator is an overloaded decl ref inside a binaryExpr
    if (auto binaryExpr = dyn_cast<BinaryExpr>(contextualTypeNode)) {
      if (auto overloadedFn
          = dyn_cast<OverloadedDeclRefExpr>(binaryExpr->getFn())) {
        if (!overloadedFn->getDecls().empty()) {
          // Fetch any declaration to check if the name is '~='
          ValueDecl *decl0 = overloadedFn->getDecls()[0];
          
          if (decl0->getBaseName() == decl0->getASTContext().Id_MatchOperator) {
            assert(binaryExpr->getArg()->getElements().size() == 2);
            
            // If the rhs of '~=' is the enum type, a single dot suffixes
            // since the type can be inferred
            Type secondArgType = getType(binaryExpr->getArg()->getElement(1));
            if (secondArgType->isEqual(baseTy)) {
              Diag->fixItInsert(loc, ".");
              return true;
            }
          }
        }
      }
    }

    // Fall back to a fix-it with a full type qualifier
    Expr *baseExpr = nullptr;
    if (const auto *SE = getAsExpr<SubscriptExpr>(getRawAnchor()))
      baseExpr = SE->getBase();
    else if (const auto UDE = getAsExpr<UnresolvedDotExpr>(getRawAnchor()))
      baseExpr = UDE->getBase();

    // An implicit 'self' reference base expression means we should
    // prepend with qualification.
    if (baseExpr && !baseExpr->isImplicit()) {
      Diag->fixItReplace(baseExpr->getSourceRange(),
                         diag::replace_with_type, baseTy);
    } else {
      Diag->fixItInsert(loc, diag::insert_type_qualification, baseTy);
    }

    return true;
  }

  return false;
}

bool PartialApplicationFailure::diagnoseAsError() {
  auto *anchor = castToExpr<UnresolvedDotExpr>(getRawAnchor());

  RefKind kind = RefKind::MutatingMethod;

  // If this is initializer delegation chain, we have a tailored message.
  if (getOverloadChoiceIfAvailable(
          getConstraintLocator(anchor, ConstraintLocator::ConstructorMember))) {
    kind = anchor->getBase()->isSuperExpr() ? RefKind::SuperInit
                                            : RefKind::SelfInit;
  } else if (anchor->getBase()->isSuperExpr()) {
    kind = RefKind::SuperMethod;
  }

  auto diagnostic = CompatibilityWarning
                        ? diag::partial_application_of_function_invalid_swift4
                        : diag::partial_application_of_function_invalid;

  emitDiagnosticAt(anchor->getNameLoc(), diagnostic, kind);
  return true;
}

bool InvalidDynamicInitOnMetatypeFailure::diagnoseAsError() {
  emitDiagnostic(diag::dynamic_construct_class,
                 BaseType->getMetatypeInstanceType())
      .highlight(BaseRange);
  emitDiagnosticAt(Init, diag::note_nonrequired_initializer, Init->isImplicit(),
                   Init->getName());
  return true;
}

bool InitOnProtocolMetatypeFailure::diagnoseAsError() {
  if (IsStaticallyDerived) {
    emitDiagnostic(diag::construct_protocol_by_name,
                   BaseType->getMetatypeInstanceType())
        .highlight(BaseRange);
  } else {
    emitDiagnostic(diag::construct_protocol_value, BaseType)
        .highlight(BaseRange);
  }

  return true;
}

SourceLoc ImplicitInitOnNonConstMetatypeFailure::getLoc() const {
  if (auto *apply = getAsExpr<ApplyExpr>(getRawAnchor()))
    return apply->getArg()->getStartLoc();

  return FailureDiagnostic::getLoc();
}

bool ImplicitInitOnNonConstMetatypeFailure::diagnoseAsError() {
  emitDiagnostic(diag::missing_init_on_metatype_initialization)
      .fixItInsert(getLoc(), ".init");
  return true;
}

ASTNode MissingArgumentsFailure::getAnchor() const {
  auto anchor = FailureDiagnostic::getAnchor();

  if (auto *captureList = getAsExpr<CaptureListExpr>(anchor))
    return captureList->getClosureBody();

  return anchor;
}

bool MissingArgumentsFailure::diagnoseAsError() {
  auto *locator = getLocator();

  if (!(locator->isLastElement<LocatorPathElt::ApplyArgToParam>() ||
        locator->isLastElement<LocatorPathElt::ContextualType>() ||
        locator->isLastElement<LocatorPathElt::ApplyArgument>() ||
        locator->isLastElement<LocatorPathElt::ClosureResult>() ||
        locator->isLastElement<LocatorPathElt::ClosureBody>()))
    return false;

  // If this is a misplaced `missng argument` situation, it would be
  // diagnosed by invalid conversion fix.
  if (isMisplacedMissingArgument(getSolution(), locator))
    return false;

  auto anchor = getAnchor();

  if (auto *closure = getAsExpr<ClosureExpr>(anchor))
    return diagnoseClosure(closure);

  // This is a situation where function type is passed as an argument
  // to a function type parameter and their argument arity is different.
  //
  // ```
  // func foo(_: (Int) -> Void) {}
  // func bar() {}
  //
  // foo(bar) // `() -> Void` vs. `(Int) -> Void`
  // ```
  if (locator->isLastElement<LocatorPathElt::ApplyArgToParam>()) {
    auto info = *(getFunctionArgApplyInfo(locator));

    auto *argExpr = info.getArgExpr();
    emitDiagnosticAt(argExpr->getLoc(), diag::cannot_convert_argument_value,
                     info.getArgType(), info.getParamType());
    // TODO: It would be great so somehow point out which arguments are missing.
    return true;
  }

  // Function type has fewer arguments than expected by context:
  //
  // ```
  // func foo() {}
  // let _: (Int) -> Void = foo
  // ```
  if (locator->isLastElement<LocatorPathElt::ContextualType>()) {
    emitDiagnostic(diag::cannot_convert_initializer_value, getType(anchor),
                   resolveType(getContextualType(getAnchor())));
    // TODO: It would be great so somehow point out which arguments are missing.
    return true;
  }

  if (diagnoseInvalidTupleDestructuring())
    return true;

  if (SynthesizedArgs.size() == 1)
    return diagnoseSingleMissingArgument();

  // At this point we know that this is a situation when
  // there are multiple arguments missing, so let's produce
  // a diagnostic which lists all of them and a fix-it
  // to add arguments at appropriate positions.

  SmallString<32> diagnostic;
  llvm::raw_svector_ostream arguments(diagnostic);

  interleave(
      SynthesizedArgs,
      [&](const std::pair<unsigned, AnyFunctionType::Param> &e) {
        const auto paramIdx = e.first;
        const auto &arg = e.second;

        if (arg.hasLabel()) {
          arguments << "'" << arg.getLabel().str() << "'";
        } else {
          arguments << "#" << (paramIdx + 1);
        }
      },
      [&] { arguments << ", "; });

  auto diag = emitDiagnostic(diag::missing_arguments_in_call, arguments.str());

  Expr *fnExpr = nullptr;
  Expr *argExpr = nullptr;
  unsigned numArguments = 0;
  Optional<unsigned> firstTrailingClosure = None;

  std::tie(fnExpr, argExpr, numArguments, firstTrailingClosure) =
      getCallInfo(getRawAnchor());

  // TODO(diagnostics): We should be able to suggest this fix-it
  // unconditionally.
  if (argExpr && numArguments == 0) {
    SmallString<32> scratch;
    llvm::raw_svector_ostream fixIt(scratch);
    interleave(
        SynthesizedArgs,
        [&](const std::pair<unsigned, AnyFunctionType::Param> &arg) {
          forFixIt(fixIt, arg.second);
        },
        [&] { fixIt << ", "; });

    auto *tuple = cast<TupleExpr>(argExpr);
    diag.fixItInsertAfter(tuple->getLParenLoc(), fixIt.str());
  }

  diag.flush();

  if (auto selectedOverload = getCalleeOverloadChoiceIfAvailable(locator)) {
    if (auto *decl = selectedOverload->choice.getDeclOrNull()) {
      emitDiagnosticAt(decl, diag::decl_declared_here, decl->getName());
    }
  }

  return true;
}

bool MissingArgumentsFailure::diagnoseAsNote() {
  auto *locator = getLocator();
  if (auto overload = getCalleeOverloadChoiceIfAvailable(locator)) {
    auto *fn = resolveType(overload->openedType)->getAs<AnyFunctionType>();
    auto loc = overload->choice.getDecl()->getLoc();

    if (loc.isInvalid())
      loc = getLoc();

    emitDiagnosticAt(loc, diag::candidate_partial_match,
                     fn->getParamListAsString(fn->getParams()));
    return true;
  }

  return false;
}

bool MissingArgumentsFailure::diagnoseSingleMissingArgument() const {
  auto &ctx = getASTContext();

  auto anchor = getRawAnchor();
  if (!(isExpr<CallExpr>(anchor) || isExpr<SubscriptExpr>(anchor) ||
        isExpr<UnresolvedMemberExpr>(anchor) ||
        isExpr<ObjectLiteralExpr>(anchor)))
    return false;

  if (SynthesizedArgs.size() != 1)
    return false;

  const auto &argument = SynthesizedArgs.front();
  auto position = argument.first;
  auto label = argument.second.getLabel();

  Expr *fnExpr = nullptr;
  Expr *argExpr = nullptr;
  unsigned numArgs = 0;
  Optional<unsigned> firstTrailingClosure = None;

  std::tie(fnExpr, argExpr, numArgs, firstTrailingClosure) =
      getCallInfo(anchor);

  if (!argExpr) {
    return false;
  }

  // Will the parameter accept a trailing closure?
  Type paramType = resolveType(argument.second.getPlainType());
  bool paramAcceptsTrailingClosure = paramType
      ->lookThroughAllOptionalTypes()->is<AnyFunctionType>();

  // Determine whether we're inserting as a trailing closure.
  bool insertingTrailingClosure =
    firstTrailingClosure && position > *firstTrailingClosure;

  SmallString<32> insertBuf;
  llvm::raw_svector_ostream insertText(insertBuf);

  if (insertingTrailingClosure)
    insertText << " ";
  else if (position != 0)
    insertText << ", ";

  forFixIt(insertText, argument.second);

  if (position == 0 && numArgs > 0 &&
      (!firstTrailingClosure || position < *firstTrailingClosure))
    insertText << ", ";

  SourceLoc insertLoc;

  if (position >= numArgs && insertingTrailingClosure) {
    // Add a trailing closure to the end.

    // fn { closure }:
    //   fn {closure} label: [argMissing]
    // fn() { closure }:
    //   fn() {closure} label: [argMissing]
    // fn(argX) { closure }:
    //   fn(argX) { closure } label: [argMissing]
    insertLoc = Lexer::getLocForEndOfToken(
        ctx.SourceMgr, argExpr->getEndLoc());
  } else if (auto *TE = dyn_cast<TupleExpr>(argExpr)) {
    // fn(argX, argY):
    //   fn([argMissing, ]argX, argY)
    //   fn(argX[, argMissing], argY)
    // fn(argX) { closure }:
    //   fn([argMissing, ]argX) { closure }
    //   fn(argX[, argMissing]) { closure }
    // fn(argX, argY):
    //   fn(argX, argY[, argMissing])
    if (numArgs == 0) {
      insertLoc = TE->getRParenLoc();
    } else if (position != 0) {
      auto argPos = std::min(TE->getNumElements(), position) - 1;
      insertLoc = Lexer::getLocForEndOfToken(
          ctx.SourceMgr, TE->getElement(argPos)->getEndLoc());
    } else {
      insertLoc = TE->getElementNameLoc(0);
      if (insertLoc.isInvalid())
        insertLoc = TE->getElement(0)->getStartLoc();
    }
  } else {
    auto *PE = cast<ParenExpr>(argExpr);
    if (PE->getRParenLoc().isValid()) {
      // fn():
      //   fn([argMissing])
      // fn(argX):
      //   fn(argX[, argMissing])
      //   fn([argMissing, ]argX)
      // fn() { closure }:
      //   fn([argMissing]) {closure}
      if (position == 0) {
        insertLoc = Lexer::getLocForEndOfToken(ctx.SourceMgr,
                                               PE->getLParenLoc());
      } else {
        insertLoc = Lexer::getLocForEndOfToken(
            ctx.SourceMgr, PE->getSubExpr()->getEndLoc());
      }
    } else {
      // fn { closure }:
      //   fn[(argMissing)] { closure }
      assert(!isExpr<SubscriptExpr>(anchor) && "bracket less subscript");
      assert(firstTrailingClosure &&
             "paren less ParenExpr without trailing closure");
      insertBuf.insert(insertBuf.begin(), '(');
      insertBuf.insert(insertBuf.end(), ')');
      insertLoc =
          Lexer::getLocForEndOfToken(ctx.SourceMgr, fnExpr->getEndLoc());
    }
  }

  if (insertLoc.isInvalid())
    return false;

  // If we are trying to insert a trailing closure but the parameter
  // corresponding to the missing argument doesn't support a trailing closure,
  // don't provide a Fix-It.
  // FIXME: It's possible to parenthesize and relabel the argument list to
  // accomodate this, but it's tricky.
  bool shouldEmitFixIt =
    !(insertingTrailingClosure && !paramAcceptsTrailingClosure);

  if (label.empty()) {
    auto diag = emitDiagnosticAt(
        insertLoc, diag::missing_argument_positional, position + 1);
    if (shouldEmitFixIt)
      diag.fixItInsert(insertLoc, insertText.str());
  } else if (isPropertyWrapperInitialization()) {
    auto *TE = cast<TypeExpr>(fnExpr);
    emitDiagnosticAt(TE->getLoc(), diag::property_wrapper_missing_arg_init,
                     label, resolveType(TE->getInstanceType())->getString());
  } else {
    auto diag = emitDiagnosticAt(
        insertLoc, diag::missing_argument_named, label);
    if (shouldEmitFixIt)
      diag.fixItInsert(insertLoc, insertText.str());
  }

  if (auto selectedOverload =
          getCalleeOverloadChoiceIfAvailable(getLocator())) {
    if (auto *decl = selectedOverload->choice.getDeclOrNull()) {
      emitDiagnosticAt(decl, diag::decl_declared_here, decl->getName());
    }
  }

  return true;
}

bool MissingArgumentsFailure::diagnoseClosure(const ClosureExpr *closure) {
  FunctionType *funcType = nullptr;

  auto *locator = getLocator();
  if (locator->isForContextualType()) {
    funcType = getContextualType(locator->getAnchor())->getAs<FunctionType>();
  } else if (auto info = getFunctionArgApplyInfo(locator)) {
    auto paramType = info->getParamType();
    // Drop a single layer of optionality because argument could get injected
    // into optional and that doesn't contribute to the problem.
    if (auto objectType = paramType->getOptionalObjectType())
      paramType = objectType;
    funcType = paramType->getAs<FunctionType>();
  } else if (locator->isLastElement<LocatorPathElt::ClosureResult>() ||
             locator->isLastElement<LocatorPathElt::ClosureBody>()) {
    // Based on the locator we know this this is something like this:
    // `let _: () -> ((Int) -> Void) = { return {} }`.
    funcType = getType(getRawAnchor())
                   ->castTo<FunctionType>()
                   ->getResult()
                   ->castTo<FunctionType>();
  }

  if (!funcType)
    return false;

  unsigned numSynthesized = SynthesizedArgs.size();
  auto diff = funcType->getNumParams() - numSynthesized;

  // If the closure didn't specify any arguments and it is in a context that
  // needs some, produce a fixit to turn "{...}" into "{ _,_ in ...}".
  if (diff == 0) {
    auto diag =
        emitDiagnosticAt(closure->getStartLoc(),
                         diag::closure_argument_list_missing, numSynthesized);

    std::string fixText; // Let's provide fixits for up to 10 args.
    if (funcType->getNumParams() <= 10) {
      fixText += " ";
      interleave(
          funcType->getParams(),
          [&fixText](const AnyFunctionType::Param &param) { fixText += '_'; },
          [&fixText] { fixText += ','; });
      fixText += " in ";
    }

    if (!fixText.empty()) {
      // Determine if there is already a space after the { in the closure to
      // make sure we introduce the right whitespace.
      auto afterBrace = closure->getStartLoc().getAdvancedLoc(1);
      auto text = getASTContext().SourceMgr.extractText({afterBrace, 1});
      if (text.size() == 1 && text == " ")
        fixText = fixText.erase(fixText.size() - 1);
      else
        fixText = fixText.erase(0, 1);
      diag.fixItInsertAfter(closure->getStartLoc(), fixText);
    }

    return true;
  }

  auto params = closure->getParameters();
  bool onlyAnonymousParams =
      std::all_of(params->begin(), params->end(),
                  [](ParamDecl *param) { return !param->hasName(); });

  auto diag = emitDiagnosticAt(
      params->getStartLoc(), diag::closure_argument_list_tuple,
      resolveType(funcType), funcType->getNumParams(), diff, diff == 1);

  // If the number of parameters is less than number of inferred
  // let's try to suggest a fix-it with the rest of the missing parameters.
  if (!closure->hasExplicitResultType() &&
      closure->getInLoc().isValid()) {
    SmallString<32> fixIt;
    llvm::raw_svector_ostream OS(fixIt);

    OS << ",";
    for (unsigned i = 0; i != numSynthesized; ++i) {
      OS << ((onlyAnonymousParams) ? "_" : "<#arg#>");
      OS << ((i == numSynthesized - 1) ? " " : ",");
    }

    diag.fixItInsertAfter(params->getEndLoc(), OS.str());
  }

  return true;
}

bool MissingArgumentsFailure::diagnoseInvalidTupleDestructuring() const {
  auto *locator = getLocator();
  if (!locator->isLastElement<LocatorPathElt::ApplyArgument>())
    return false;

  if (SynthesizedArgs.size() < 2)
    return false;

  auto anchor = getAnchor();

  Expr *argExpr = nullptr;
  // Something like `foo(x: (1, 2))`
  if (auto *TE = getAsExpr<TupleExpr>(anchor)) {
    if (TE->getNumElements() == 1)
      argExpr = TE->getElement(0);
  } else { // or `foo((1, 2))`
    argExpr = castToExpr<ParenExpr>(anchor)->getSubExpr();
  }

  if (!(argExpr && getType(argExpr)->getRValueType()->is<TupleType>()))
    return false;

  auto selectedOverload = getCalleeOverloadChoiceIfAvailable(locator);
  if (!selectedOverload)
    return false;

  auto *decl = selectedOverload->choice.getDeclOrNull();
  if (!decl)
    return false;

  auto name = decl->getBaseName();
  auto diagnostic =
      emitDiagnostic(diag::cannot_convert_single_tuple_into_multiple_arguments,
                     decl->getDescriptiveKind(), name, name.isSpecial(),
                     SynthesizedArgs.size(), isa<TupleExpr>(argExpr));

  // If argument is a literal tuple, let's suggest removal of parentheses.
  if (auto *TE = dyn_cast<TupleExpr>(argExpr)) {
    diagnostic.fixItRemove(TE->getLParenLoc()).fixItRemove(TE->getRParenLoc());
  }

  diagnostic.flush();

  // Add a note which points to the overload choice location.
  emitDiagnosticAt(decl, diag::decl_declared_here, decl->getName());
  return true;
}

bool MissingArgumentsFailure::isPropertyWrapperInitialization() const {
  auto *call = getAsExpr<CallExpr>(getRawAnchor());
  if (!(call && call->isImplicit()))
    return false;

  auto TE = dyn_cast<TypeExpr>(call->getFn());
  if (!TE)
    return false;

  auto instanceTy = TE->getInstanceType();
  if (!instanceTy)
    return false;

  auto *NTD = resolveType(instanceTy)->getAnyNominal();
  return NTD && NTD->getAttrs().hasAttribute<PropertyWrapperAttr>();
}

bool MissingArgumentsFailure::isMisplacedMissingArgument(
    const Solution &solution, ConstraintLocator *locator) {
  auto *calleeLocator = solution.getCalleeLocator(locator);
  auto overloadChoice = solution.getOverloadChoiceIfAvailable(calleeLocator);
  if (!overloadChoice)
    return false;

  auto *fnType =
      solution.simplifyType(overloadChoice->openedType)->getAs<FunctionType>();
  if (!(fnType && fnType->getNumParams() == 2))
    return false;

  auto anchor = locator->getAnchor();

  auto hasFixFor = [&](FixKind kind, ConstraintLocator *locator) -> bool {
    auto fix = llvm::find_if(solution.Fixes, [&](const ConstraintFix *fix) {
      return fix->getLocator() == locator;
    });

    if (fix == solution.Fixes.end())
      return false;

    return (*fix)->getKind() == kind;
  };

  auto *callLocator =
      solution.getConstraintLocator(anchor, {ConstraintLocator::ApplyArgument});

  auto argFlags = fnType->getParams()[0].getParameterFlags();
  auto *argLoc = solution.getConstraintLocator(
      callLocator, LocatorPathElt::ApplyArgToParam(0, 0, argFlags));

  if (!(hasFixFor(FixKind::AllowArgumentTypeMismatch, argLoc) &&
        hasFixFor(FixKind::AddMissingArguments, callLocator)))
    return false;

  Expr *argExpr = nullptr;
  if (auto *call = getAsExpr<CallExpr>(anchor)) {
    argExpr = call->getArg();
  } else if (auto *subscript = getAsExpr<SubscriptExpr>(anchor)) {
    argExpr = subscript->getIndex();
  } else {
    return false;
  }

  Expr *argument = nullptr;
  if (auto *PE = dyn_cast<ParenExpr>(argExpr)) {
    argument = PE->getSubExpr();
  } else {
    auto *tuple = cast<TupleExpr>(argExpr);
    if (tuple->getNumElements() != 1)
      return false;
    argument = tuple->getElement(0);
  }

  auto argType = solution.simplifyType(solution.getType(argument));
  auto paramType = fnType->getParams()[1].getPlainType();

  return TypeChecker::isConvertibleTo(argType, paramType, solution.getDC());
}

std::tuple<Expr *, Expr *, unsigned, Optional<unsigned>>
MissingArgumentsFailure::getCallInfo(ASTNode anchor) const {
  if (auto *call = getAsExpr<CallExpr>(anchor)) {
    return std::make_tuple(call->getFn(), call->getArg(),
                           call->getNumArguments(),
                           call->getUnlabeledTrailingClosureIndex());
  } else if (auto *SE = getAsExpr<SubscriptExpr>(anchor)) {
    return std::make_tuple(SE, SE->getIndex(), SE->getNumArguments(),
                           SE->getUnlabeledTrailingClosureIndex());
  } else if (auto *OLE = getAsExpr<ObjectLiteralExpr>(anchor)) {
    return std::make_tuple(OLE, OLE->getArg(), OLE->getNumArguments(),
                           OLE->getUnlabeledTrailingClosureIndex());
  }

  return std::make_tuple(nullptr, nullptr, 0, None);
}

void MissingArgumentsFailure::forFixIt(
    llvm::raw_svector_ostream &out,
    const AnyFunctionType::Param &argument) const {
  if (argument.hasLabel())
    out << argument.getLabel().str() << ": ";

  // Explode inout type.
  if (argument.isInOut())
    out << "&";

  auto resolvedType = resolveType(argument.getPlainType());
  // @autoclosure; the type should be the result type.
  if (argument.isAutoClosure())
    resolvedType = resolvedType->castTo<FunctionType>()->getResult();

  out << "<#" << resolvedType << "#>";
}

SourceLoc ClosureParamDestructuringFailure::getLoc() const {
  auto *closure = castToExpr<ClosureExpr>(getAnchor());
  auto paramList = closure->getParameters();
  return paramList->getStartLoc();
}

SourceRange ClosureParamDestructuringFailure::getSourceRange() const {
  auto *closure = castToExpr<ClosureExpr>(getAnchor());
  auto paramList = closure->getParameters();
  return paramList->getSourceRange();
}

bool ClosureParamDestructuringFailure::diagnoseAsError() {
  auto *closure = castToExpr<ClosureExpr>(getAnchor());
  auto params = closure->getParameters();

  // In case of implicit parameters e.g. $0, $1 we
  // can't really provide good fix-it because
  // structure of parameter type itself is unclear.
  for (auto *param : params->getArray()) {
    if (param->isImplicit()) {
      emitDiagnostic(diag::closure_tuple_parameter_destructuring_implicit,
                     getParameterType());
      return true;
    }
  }

  auto diag = emitDiagnostic(diag::closure_tuple_parameter_destructuring,
                             getParameterType());

  auto *closureBody = closure->getBody();
  if (!closureBody)
    return true;

  auto &sourceMgr = getASTContext().SourceMgr;
  auto bodyStmts = closureBody->getElements();

  SourceLoc bodyLoc;
  SourceLoc inLoc = closure->getInLoc();
  // If location for `in` is unknown we can't proceed
  // since we'll not be able to figure out source line
  // to place the fix-it on.
  if (inLoc.isInvalid())
    return true;

  // If the body is empty let's put the cursor
  // right after "in", otherwise make it start
  // location of the first statement in the body.
  if (bodyStmts.empty())
    bodyLoc = Lexer::getLocForEndOfToken(sourceMgr, inLoc);
  else
    bodyLoc = bodyStmts.front().getStartLoc();

  if (bodyLoc.isInvalid())
    return true;

  SmallString<64> fixIt;
  llvm::raw_svector_ostream OS(fixIt);

  // If this is multi-line closure we'd have to insert new lines
  // in the suggested 'let' to keep the structure of the code intact,
  // otherwise just use ';' to keep everything on the same line.
  auto inLine = sourceMgr.getLineAndColumnInBuffer(inLoc).first;
  auto bodyLine = sourceMgr.getLineAndColumnInBuffer(bodyLoc).first;
  auto isMultiLineClosure = bodyLine > inLine;
  auto indent =
      bodyStmts.empty() ? "" : Lexer::getIndentationForLine(sourceMgr, bodyLoc);

  SmallString<16> parameter;
  llvm::raw_svector_ostream parameterOS(parameter);

  parameterOS << "(";
  interleave(
      params->getArray(),
      [&](const ParamDecl *param) { parameterOS << param->getNameStr(); },
      [&] { parameterOS << ", "; });
  parameterOS << ")";

  // Check if there are any explicit types associated
  // with parameters, if there are, we'll have to add
  // type information to the replacement argument.
  bool explicitTypes =
      llvm::any_of(params->getArray(),
                   [](const ParamDecl *param) { return param->getTypeRepr(); });

  if (isMultiLineClosure)
    OS << '\n' << indent;

  // Let's form 'let <name> : [<type>]? = arg' expression.
  OS << "let " << parameterOS.str() << " = arg"
     << (isMultiLineClosure ? "\n" + indent : "; ");

  SmallString<64> argName;
  llvm::raw_svector_ostream nameOS(argName);
  if (explicitTypes) {
    nameOS << "(arg: " << getParameterType()->getString() << ")";
  } else {
    nameOS << "(arg)";
  }

  if (closure->hasSingleExpressionBody()) {
    // Let's see if we need to add result type to the argument/fix-it:
    //  - if the there is a result type associated with the closure;
    //  - and it's not a void type;
    //  - and it hasn't been explicitly written.
    auto resultType = resolveType(ContextualType->getResult());
    auto hasResult = [](Type resultType) -> bool {
      return resultType && !resultType->isVoid();
    };

    auto isValidType = [](Type resultType) -> bool {
      return resultType && !resultType->hasUnresolvedType() &&
             !resultType->hasTypeVariable();
    };

    // If there an expected result type but it hasn't been explicitly
    // provided, let's add it to the argument.
    if (hasResult(resultType) && !closure->hasExplicitResultType()) {
      nameOS << " -> ";
      if (isValidType(resultType))
        nameOS << resultType->getString();
      else
        nameOS << "<#Result#>";
    }

    if (auto stmt = bodyStmts.front().get<Stmt *>()) {
      // If the body is a single expression with implicit return.
      if (isa<ReturnStmt>(stmt) && stmt->isImplicit()) {
        // And there is non-void expected result type,
        // because we add 'let' expression to the body
        // we need to make such 'return' explicit.
        if (hasResult(resultType))
          OS << "return ";
      }
    }
  }

  diag.fixItReplace(getSourceRange(), nameOS.str())
      .fixItInsert(bodyLoc, OS.str());
  return true;
}

bool OutOfOrderArgumentFailure::diagnoseAsError() {
  auto anchor = getRawAnchor();
  auto *argExpr = isExpr<TupleExpr>(anchor)
                      ? castToExpr<TupleExpr>(anchor)
                      : getArgumentListExprFor(getLocator());
  if (!argExpr)
    return false;

  auto *tuple = cast<TupleExpr>(argExpr);

  Identifier first = tuple->getElementName(ArgIdx);
  Identifier second = tuple->getElementName(PrevArgIdx);

  // Build a mapping from arguments to parameters.
  SmallVector<unsigned, 4> argBindings(tuple->getNumElements());
  for (unsigned paramIdx = 0; paramIdx != Bindings.size(); ++paramIdx) {
    for (auto argIdx : Bindings[paramIdx])
      argBindings[argIdx] = paramIdx;
  }

  auto argRange = [&](unsigned argIdx, Identifier label) -> SourceRange {
    auto range = tuple->getElement(argIdx)->getSourceRange();
    if (!label.empty())
      range.Start = tuple->getElementNameLoc(argIdx);

    unsigned paramIdx = argBindings[argIdx];
    if (Bindings[paramIdx].size() > 1)
      range.End = tuple->getElement(Bindings[paramIdx].back())->getEndLoc();

    return range;
  };

  auto firstRange = argRange(ArgIdx, first);
  auto secondRange = argRange(PrevArgIdx, second);

  SourceLoc diagLoc = firstRange.Start;

  auto addFixIts = [&](InFlightDiagnostic diag) {
    // Don't add Fix-Its if one of the ranges is outside of the argument
    // list, which can happen when we're splicing together an argument list
    // from multiple sources.
    auto &SM = getASTContext().SourceMgr;
    auto argsRange = tuple->getSourceRange();
    if (!SM.rangeContains(argsRange, firstRange) ||
        !SM.rangeContains(argsRange, secondRange))
      return;

    diag.highlight(firstRange).highlight(secondRange);

    // Move the misplaced argument by removing it from one location and
    // inserting it in another location. To maintain argument comma
    // separation, since the argument is always moving to an earlier index
    // the preceding comma and whitespace is removed and a new trailing
    // comma and space is inserted with the moved argument.
    auto text = SM.extractText(
        Lexer::getCharSourceRangeFromSourceRange(SM, firstRange));

    auto removalRange =
        SourceRange(Lexer::getLocForEndOfToken(
                        SM, tuple->getElement(ArgIdx - 1)->getEndLoc()),
                    firstRange.End);
    diag.fixItRemove(removalRange);
    diag.fixItInsert(secondRange.Start,
                     text.str() + (isExpr<BinaryExpr>(anchor) ? "" : ", "));
  };

  // There are 4 diagnostic messages variations depending on
  // labeled/unlabeled arguments.
  if (first.empty() && second.empty()) {
    addFixIts(
        emitDiagnosticAt(diagLoc,
                         isExpr<BinaryExpr>(anchor)
                             ? diag::argument_out_of_order_binary_op
                             : diag::argument_out_of_order_unnamed_unnamed,
                         ArgIdx + 1, PrevArgIdx + 1));
  } else if (first.empty() && !second.empty()) {
    addFixIts(emitDiagnosticAt(diagLoc,
                               diag::argument_out_of_order_unnamed_named,
                               ArgIdx + 1, second));
  } else if (!first.empty() && second.empty()) {
    addFixIts(emitDiagnosticAt(diagLoc,
                               diag::argument_out_of_order_named_unnamed, first,
                               PrevArgIdx + 1));
  } else {
    addFixIts(emitDiagnosticAt(diagLoc, diag::argument_out_of_order_named_named,
                               first, second));
  }
  return true;
}

bool ExtraneousArgumentsFailure::diagnoseAsError() {
  // Simplified anchor would point directly to the
  // argument in case of contextual mismatch.
  auto anchor = getAnchor();

  if (auto *closure = getAsExpr<ClosureExpr>(anchor)) {
    auto fnType = ContextualType;
    auto params = closure->getParameters();

    auto diag = emitDiagnosticAt(
        params->getStartLoc(), diag::closure_argument_list_tuple, fnType,
        fnType->getNumParams(), params->size(), (params->size() == 1));

    bool onlyAnonymousParams =
        std::all_of(params->begin(), params->end(),
                    [](ParamDecl *param) { return !param->hasName(); });

    // If closure expects no parameters but N was given,
    // and all of them are anonymous let's suggest removing them.
    if (fnType->getNumParams() == 0 && onlyAnonymousParams) {
      auto inLoc = closure->getInLoc();
      auto &sourceMgr = getASTContext().SourceMgr;

      if (inLoc.isValid())
        diag.fixItRemoveChars(params->getStartLoc(),
                              Lexer::getLocForEndOfToken(sourceMgr, inLoc));
    }
    return true;
  }

  if (isContextualMismatch()) {
    auto *locator = getLocator();
    emitDiagnostic(locator->isLastElement<LocatorPathElt::ContextualType>()
                       ? diag::cannot_convert_initializer_value
                       : diag::cannot_convert_argument_value,
                   getType(anchor), ContextualType);
    return true;
  }

  if (ExtraArgs.size() == 1) {
    return diagnoseSingleExtraArgument();
  }

  if (ContextualType->getNumParams() == 0) {
    if (auto argExpr = getArgumentListExprFor(getLocator())) {
      emitDiagnostic(diag::extra_argument_to_nullary_call)
          .highlight(argExpr->getSourceRange())
          .fixItRemove(argExpr->getSourceRange());
      return true;
    }
  }

  if (ExtraArgs.size() < 2)
    return false;

  llvm::SmallString<64> positions;
  llvm::raw_svector_ostream OS(positions);

  interleave(
      ExtraArgs,
      [&](const std::pair<unsigned, AnyFunctionType::Param> &arg) {
        OS << "#" << (arg.first + 1);
      },
      [&] { OS << ", "; });

  emitDiagnostic(diag::extra_arguments_in_call, OS.str());

  if (auto overload = getCalleeOverloadChoiceIfAvailable(getLocator())) {
    if (auto *decl = overload->choice.getDeclOrNull()) {
      emitDiagnosticAt(decl, diag::decl_declared_here, decl->getName());
    }
  }

  return true;
}

bool ExtraneousArgumentsFailure::diagnoseAsNote() {
  auto overload = getCalleeOverloadChoiceIfAvailable(getLocator());
  if (!(overload && overload->choice.isDecl()))
    return false;

  auto *decl = overload->choice.getDecl();
  auto numArgs = getTotalNumArguments();
  emitDiagnosticAt(decl, diag::candidate_with_extraneous_args, ContextualType,
                   ContextualType->getNumParams(), numArgs, (numArgs == 1),
                   isExpr<ClosureExpr>(getAnchor()));
  return true;
}

bool ExtraneousArgumentsFailure::diagnoseSingleExtraArgument() const {
  auto *locator = getLocator();

  // This specifically handles a case of `Void(...)` which generates
  // constraints differently from other constructor invocations and
  // wouldn't have `ApplyArgument` as a last element in the locator.
  if (auto *call = getAsExpr<CallExpr>(getRawAnchor())) {
    auto *TE = dyn_cast<TypeExpr>(call->getFn());
    if (TE && getType(TE)->getMetatypeInstanceType()->isVoid()) {
      emitDiagnosticAt(call->getLoc(), diag::extra_argument_to_nullary_call)
          .highlight(call->getArg()->getSourceRange());
      return true;
    }
  }

  auto *arguments = getArgumentListExprFor(locator);
  if (!arguments)
    return false;

  const auto &e = ExtraArgs.front();
  auto index = e.first;
  auto argument = e.second;

  auto tuple = dyn_cast<TupleExpr>(arguments);
  auto argExpr = tuple ? tuple->getElement(index)
                       : cast<ParenExpr>(arguments)->getSubExpr();

  auto loc = argExpr->getLoc();
  if (tuple && index == tuple->getNumElements() - 1 &&
      tuple->hasTrailingClosure()) {
    emitDiagnosticAt(loc, diag::extra_trailing_closure_in_call)
        .highlight(argExpr->getSourceRange());
  } else if (ContextualType->getNumParams() == 0) {
    auto *PE = dyn_cast<ParenExpr>(arguments);
    Expr *subExpr = nullptr;
    if (PE)
      subExpr = PE->getSubExpr();

    if (subExpr && argument.getPlainType()->isVoid()) {
      emitDiagnosticAt(loc, diag::extra_argument_to_nullary_call)
          .fixItRemove(subExpr->getSourceRange());
    } else {
      emitDiagnosticAt(loc, diag::extra_argument_to_nullary_call)
          .highlight(argExpr->getSourceRange());
    }
  } else if (argument.hasLabel()) {
    emitDiagnosticAt(loc, diag::extra_argument_named, argument.getLabel())
        .highlight(argExpr->getSourceRange());
  } else {
    emitDiagnosticAt(loc, diag::extra_argument_positional)
        .highlight(argExpr->getSourceRange());
  }
  return true;
}

bool InaccessibleMemberFailure::diagnoseAsError() {
  auto anchor = getRawAnchor();
  // Let's try to avoid over-diagnosing chains of inaccessible
  // members e.g.:
  //
  // struct A {
  //   struct B {
  //     struct C {}
  //   }
  // }
  //
  // _ = A.B.C()
  //
  // We'll have a fix for each `B', `C` and `C.init` but it makes
  // sense to diagnose only `B` and consider the rest hidden.
  Expr *baseExpr = nullptr;
  DeclNameLoc nameLoc;
  if (auto *UDE = getAsExpr<UnresolvedDotExpr>(anchor)) {
    baseExpr = UDE->getBase();
    nameLoc = UDE->getNameLoc();
  } else if (auto *UME = getAsExpr<UnresolvedMemberExpr>(anchor)) {
    nameLoc = UME->getNameLoc();
  } else if (auto *SE = getAsExpr<SubscriptExpr>(anchor)) {
    baseExpr = SE->getBase();
  } else if (auto *call = getAsExpr<CallExpr>(anchor)) {
    baseExpr = call->getFn();
  }

  if (baseExpr) {
    auto *locator = getConstraintLocator(baseExpr, ConstraintLocator::Member);
    const auto &solution = getSolution();
    if (llvm::any_of(solution.Fixes, [&locator](const ConstraintFix *fix) {
          return fix->getLocator() == locator;
        }))
      return false;
  }

  auto loc = nameLoc.isValid() ? nameLoc.getStartLoc() : ::getLoc(anchor);
  auto accessLevel = Member->getFormalAccessScope().accessLevelForDiagnostics();
  if (auto *CD = dyn_cast<ConstructorDecl>(Member)) {
    emitDiagnosticAt(loc, diag::init_candidate_inaccessible,
                     CD->getResultInterfaceType(), accessLevel)
        .highlight(nameLoc.getSourceRange());
  } else {
    emitDiagnosticAt(loc, diag::candidate_inaccessible, Member->getBaseName(),
                     accessLevel)
        .highlight(nameLoc.getSourceRange());
  }

  emitDiagnosticAt(Member, diag::decl_declared_here, Member->getName());
  return true;
}

SourceLoc AnyObjectKeyPathRootFailure::getLoc() const {
  auto anchor = getAnchor();

  if (auto *KPE = getAsExpr<KeyPathExpr>(anchor)) {
    if (auto rootTyRepr = KPE->getRootType())
      return rootTyRepr->getLoc();
  }

  return ::getLoc(anchor);
}

SourceRange AnyObjectKeyPathRootFailure::getSourceRange() const {
  auto anchor = getAnchor();

  if (auto *KPE = getAsExpr<KeyPathExpr>(anchor)) {
    if (auto rootTyRepr = KPE->getRootType())
      return rootTyRepr->getSourceRange();
  }

  return ::getSourceRange(anchor);
}

bool AnyObjectKeyPathRootFailure::diagnoseAsError() {
  // Diagnose use of AnyObject as root for a keypath
  emitDiagnostic(diag::expr_swift_keypath_anyobject_root)
      .highlight(getSourceRange());
  return true;
}

SourceLoc KeyPathSubscriptIndexHashableFailure::getLoc() const {
  auto *locator = getLocator();

  if (locator->isKeyPathSubscriptComponent()) {
    auto *KPE = castToExpr<KeyPathExpr>(getAnchor());
    if (auto kpElt = locator->findFirst<LocatorPathElt::KeyPathComponent>())
      return KPE->getComponents()[kpElt->getIndex()].getLoc();
  }

  return FailureDiagnostic::getLoc();
}

bool KeyPathSubscriptIndexHashableFailure::diagnoseAsError() {
  emitDiagnostic(diag::expr_keypath_subscript_index_not_hashable,
                 resolveType(NonConformingType));
  return true;
}

SourceLoc InvalidMemberRefInKeyPath::getLoc() const {
  auto anchor = getRawAnchor();

  if (auto *KPE = getAsExpr<KeyPathExpr>(anchor)) {
    auto *locator = getLocator();
    auto component = locator->findFirst<LocatorPathElt::KeyPathComponent>();
    assert(component);
    return KPE->getComponents()[component->getIndex()].getLoc();
  }

  return ::getLoc(anchor);
}

bool InvalidStaticMemberRefInKeyPath::diagnoseAsError() {
  emitDiagnostic(diag::expr_keypath_static_member, getName(),
                 isForKeyPathDynamicMemberLookup());
  return true;
}

bool InvalidEnumCaseRefInKeyPath::diagnoseAsError() {
  emitDiagnostic(diag::expr_keypath_enum_case, getName(),
                 isForKeyPathDynamicMemberLookup());
  return true;
}

bool InvalidMemberWithMutatingGetterInKeyPath::diagnoseAsError() {
  emitDiagnostic(diag::expr_keypath_mutating_getter, getName(),
                 isForKeyPathDynamicMemberLookup());
  return true;
}

bool InvalidMethodRefInKeyPath::diagnoseAsError() {
  emitDiagnostic(diag::expr_keypath_not_property, getKind(), getName(),
                 isForKeyPathDynamicMemberLookup());
  return true;
}

SourceLoc InvalidUseOfAddressOf::getLoc() const {
  auto anchor = getAnchor();

  if (auto *assign = getAsExpr<AssignExpr>(anchor))
    return assign->getSrc()->getLoc();

  return ::getLoc(anchor);
}

bool InvalidUseOfAddressOf::diagnoseAsError() {
  if (auto argApplyInfo = getFunctionArgApplyInfo(getLocator())) {
    if (!argApplyInfo->getParameterFlags().isInOut()) {
      emitDiagnostic(diag::extra_address_of, getToType())
          .highlight(getSourceRange())
          .fixItRemove(getSourceRange().Start);
      return true;
    }
  }

  emitDiagnostic(diag::extraneous_address_of);
  return true;
}

bool ExtraneousReturnFailure::diagnoseAsError() {
  emitDiagnostic(diag::cannot_return_value_from_void_func);
  if (auto FD = dyn_cast<FuncDecl>(getDC())) {
    // We only want to emit the note + fix-it if the function does not
    // have an explicit return type. The reason we also need to check
    // whether the parameter list has a valid loc is to guard against
    // cases like like 'var foo: () { return 1 }' as here that loc will
    // be invalid. We also need to check that the name is not empty,
    // because certain decls will have empty name (like setters).
    if (FD->getResultTypeRepr() == nullptr &&
        FD->getParameters()->getStartLoc().isValid() &&
        !FD->getBaseIdentifier().empty()) {
      auto fixItLoc = Lexer::getLocForEndOfToken(
          getASTContext().SourceMgr, FD->getParameters()->getEndLoc());
      emitDiagnostic(diag::add_return_type_note)
          .fixItInsert(fixItLoc, " -> <#Return Type#>");
    }
  }

  return true;
}

bool CollectionElementContextualFailure::diagnoseAsError() {
  auto anchor = getRawAnchor();
  auto *locator = getLocator();

  auto eltType = getFromType();
  auto contextualType = getToType();

  Optional<InFlightDiagnostic> diagnostic;
  if (isExpr<ArrayExpr>(anchor)) {
    if (diagnoseMergedLiteralElements())
      return true;

    diagnostic.emplace(emitDiagnostic(diag::cannot_convert_array_element,
                                      eltType, contextualType));
  }

  if (isExpr<DictionaryExpr>(anchor)) {
    auto eltLoc = locator->castLastElementTo<LocatorPathElt::TupleElement>();
    switch (eltLoc.getIndex()) {
    case 0: // key
      diagnostic.emplace(emitDiagnostic(diag::cannot_convert_dict_key, eltType,
                                        contextualType));
      break;

    case 1: // value
      diagnostic.emplace(emitDiagnostic(diag::cannot_convert_dict_value,
                                        eltType, contextualType));
      break;

    default:
      break;
    }
  }

  if (locator->isForSequenceElementType()) {
    // If this is a conversion failure related to binding of `for-each`
    // statement it has to be diagnosed as pattern match if there are
    // holes present in the contextual type.
    if (FailureDiagnostic::getContextualTypePurpose(getAnchor()) ==
            ContextualTypePurpose::CTP_ForEachStmt &&
        contextualType->hasUnresolvedType()) {
      diagnostic.emplace(emitDiagnostic(
          (contextualType->is<TupleType>() && !eltType->is<TupleType>())
              ? diag::cannot_match_expr_tuple_pattern_with_nontuple_value
              : diag::cannot_match_unresolved_expr_pattern_with_value,
          eltType));
    } else {
      diagnostic.emplace(
          emitDiagnostic(contextualType->isExistentialType()
                             ? diag::cannot_convert_sequence_element_protocol
                             : diag::cannot_convert_sequence_element_value,
                         eltType, contextualType));
    }
  }

  if (!diagnostic)
    return false;

  (void)trySequenceSubsequenceFixIts(*diagnostic);
  return true;
}

bool CollectionElementContextualFailure::diagnoseMergedLiteralElements() {
  auto elementAnchor = simplifyLocatorToAnchor(getLocator());
  if (!elementAnchor)
    return false;

  auto *typeVar = getRawType(elementAnchor)->getAs<TypeVariableType>();
  if (!typeVar || !typeVar->getImpl().getAtomicLiteralKind())
    return false;

  // This element is a literal whose type variable could have been merged with others,
  // but the conversion constraint to the array element type was only placed on one
  // of them. So, we want to emit the error for each element whose type variable is in
  // this equivalence class.
  auto &cs = getConstraintSystem();
  auto node = cs.getRepresentative(typeVar)->getImpl().getGraphNode();
  for (const auto *typeVar : node->getEquivalenceClass()) {
    auto anchor = typeVar->getImpl().getLocator()->getAnchor();
    emitDiagnosticAt(constraints::getLoc(anchor), diag::cannot_convert_array_element,
                     getFromType(), getToType());
  }

  return true;
}

bool MissingContextualConformanceFailure::diagnoseAsError() {
  auto anchor = getAnchor();
  auto path = getLocator()->getPath();

  Optional<Diag<Type, Type>> diagnostic;
  if (path.empty()) {
    assert(isExpr<AssignExpr>(anchor));
    if (isa<SubscriptExpr>(castToExpr<AssignExpr>(anchor)->getDest())) {
      diagnostic = getDiagnosticFor(CTP_SubscriptAssignSource, getToType());
    } else {
      diagnostic = getDiagnosticFor(CTP_AssignSource, getToType());
    }
  } else {
    const auto &last = path.back();
    switch (last.getKind()) {
    case ConstraintLocator::ContextualType:
      assert(Context != CTP_Unused);
      diagnostic = getDiagnosticFor(Context, getToType());
      break;

    case ConstraintLocator::SequenceElementType: {
      diagnostic = diag::cannot_convert_sequence_element_protocol;
      break;
    }

    default:
      break;
    }
  }

  if (!diagnostic)
    return false;

  auto srcType = getFromType();
  auto dstType = getToType();

  emitDiagnostic(*diagnostic, srcType, dstType);

  if (isExpr<InOutExpr>(anchor))
    return true;

  if (srcType->isAny() && dstType->isAnyObject()) {
    emitDiagnostic(diag::any_as_anyobject_fixit)
        .fixItInsertAfter(getSourceRange().End, " as AnyObject");
  }

  return true;
}

bool MissingGenericArgumentsFailure::hasLoc(GenericTypeParamType *GP) const {
  return GP->getDecl()->getStartLoc().isValid();
}

bool MissingGenericArgumentsFailure::diagnoseAsError() {
  llvm::SmallDenseMap<TypeRepr *, SmallVector<GenericTypeParamType *, 4>>
      scopedParameters;

  auto isScoped =
      findArgumentLocations([&](TypeRepr *base, GenericTypeParamType *GP) {
        scopedParameters[base].push_back(GP);
      });

  // FIXME: this code should be generalized now that we can anchor the
  // fixes on the TypeRepr with the missing generic arg.
  if (!isScoped) {
    assert(getAnchor().is<Expr *>() || getAnchor().is<TypeRepr *>());
    if (auto *expr = getAsExpr(getAnchor()))
      return diagnoseForAnchor(expr, Parameters);

    return diagnoseForAnchor(getAnchor().get<TypeRepr *>(), Parameters);
  }

  bool diagnosed = false;
  for (const auto &scope : scopedParameters)
    diagnosed |= diagnoseForAnchor(scope.first, scope.second);
  return diagnosed;
}

bool MissingGenericArgumentsFailure::diagnoseForAnchor(
    Anchor anchor, ArrayRef<GenericTypeParamType *> params) const {
  bool diagnosed = false;
  for (auto *GP : params)
    diagnosed |= diagnoseParameter(anchor, GP);

  if (!diagnosed)
    return false;

  auto *DC = getDeclContext();
  if (!DC)
    return true;

  if (auto *SD = dyn_cast<SubscriptDecl>(DC)) {
    emitDiagnosticAt(SD, diag::note_call_to_subscript, SD->getName());
    return true;
  }

  if (auto *AFD = dyn_cast<AbstractFunctionDecl>(DC)) {
    if (isa<ConstructorDecl>(AFD)) {
      emitDiagnosticAt(AFD, diag::note_call_to_initializer);
    } else {
      emitDiagnosticAt(AFD,
                       AFD->isOperator() ? diag::note_call_to_operator
                                         : diag::note_call_to_func,
                       AFD->getName());
    }
    return true;
  }

  emitGenericSignatureNote(anchor);
  return true;
}

bool MissingGenericArgumentsFailure::diagnoseParameter(
    Anchor anchor, GenericTypeParamType *GP) const {
  auto &solution = getSolution();

  auto loc = anchor.is<Expr *>() ? anchor.get<Expr *>()->getLoc()
                                 : anchor.get<TypeRepr *>()->getLoc();

  auto *locator = getLocator();
  // Type variables associated with missing generic parameters are
  // going to be completely cut off from the rest of constraint system,
  // that's why we'd get two fixes in this case which is not ideal.
  if (locator->isForContextualType() &&
      llvm::count_if(solution.DefaultedConstraints,
                     [&GP](const ConstraintLocator *locator) {
                       return locator->getGenericParameter() == GP;
                     }) > 1) {
    return false;
  }

  if (auto *CE = getAsExpr<ExplicitCastExpr>(getRawAnchor())) {
    const auto castTo = getType(CE->getCastTypeRepr());
    auto *NTD = castTo->getAnyNominal();
    emitDiagnosticAt(loc, diag::unbound_generic_parameter_cast, GP,
                     NTD ? NTD->getDeclaredType() : castTo);
  } else {
    emitDiagnosticAt(loc, diag::unbound_generic_parameter, GP);
  }

  Type baseTyForNote;
  auto *DC = getDeclContext();
  if (!DC)
    return true;

  if (!hasLoc(GP))
    return true;

  if (auto *NTD =
          dyn_cast_or_null<NominalTypeDecl>(DC->getSelfNominalTypeDecl())) {
    baseTyForNote = NTD->getDeclaredType();
  } else if (auto *TAD = dyn_cast<TypeAliasDecl>(DC)) {
    baseTyForNote = TAD->getUnboundGenericType();
  } else {
    return true;
  }

  emitDiagnosticAt(GP->getDecl(), diag::archetype_declared_in_type, GP,
                   baseTyForNote);
  return true;
}

void MissingGenericArgumentsFailure::emitGenericSignatureNote(
    Anchor anchor) const {
  auto &solution = getSolution();
  auto *paramDC = getDeclContext();

  if (!paramDC)
    return;

  auto *GTD = dyn_cast<GenericTypeDecl>(paramDC);
  if (!GTD || anchor.is<Expr *>())
    return;

  auto getParamDecl =
      [](const ConstraintLocator *locator) -> GenericTypeParamDecl * {
    return locator->isForGenericParameter()
               ? locator->getGenericParameter()->getDecl()
               : nullptr;
  };

  llvm::SmallDenseMap<GenericTypeParamDecl *, Type> params;
  for (auto &entry : solution.typeBindings) {
    auto *typeVar = entry.first;

    auto *GP = typeVar->getImpl().getGenericParameter();
    if (!GP)
      continue;

    auto type = resolveType(typeVar);
    assert(!type->is<TypeVariableType>());

    // If this is one of the defaulted parameter types, attempt
    // to emit placeholder for it instead of `Any`.
    if (llvm::any_of(solution.DefaultedConstraints,
                     [&](const ConstraintLocator *locator) {
                       return GP->getDecl() == getParamDecl(locator);
                     }))
      continue;

    params[GP->getDecl()] = type;
  }

  auto getPreferredType = [&](const GenericTypeParamDecl *GP) -> Type {
    auto type = params.find(GP);
    return (type == params.end()) ? Type() : type->second;
  };

  SmallString<64> paramsAsString;
  auto baseType = anchor.get<TypeRepr *>();
  if (TypeChecker::getDefaultGenericArgumentsString(paramsAsString, GTD,
                                                    getPreferredType)) {
    auto diagnostic = emitDiagnosticAt(
        baseType->getLoc(), diag::unbound_generic_parameter_explicit_fix);

    if (auto *genericTy = dyn_cast<GenericIdentTypeRepr>(baseType)) {
      // If some of the eneric arguments have been specified, we need to
      // replace existing signature with a new one.
      diagnostic.fixItReplace(genericTy->getAngleBrackets(), paramsAsString);
    } else {
      // Otherwise we can simply insert new generic signature.
      diagnostic.fixItInsertAfter(baseType->getEndLoc(), paramsAsString);
    }
  }
}

bool MissingGenericArgumentsFailure::findArgumentLocations(
    llvm::function_ref<void(TypeRepr *, GenericTypeParamType *)> callback) {
  using Callback = llvm::function_ref<void(TypeRepr *, GenericTypeParamType *)>;

  auto *const typeRepr = [this]() -> TypeRepr * {
    const auto anchor = getRawAnchor();
    if (const auto *TE = getAsExpr<TypeExpr>(anchor))
      return TE->getTypeRepr();
    else if (const auto *ECE = getAsExpr<ExplicitCastExpr>(anchor))
      return ECE->getCastTypeRepr();
    else
      return nullptr;
  }();

  if (!typeRepr)
    return false;

  struct AssociateMissingParams : public ASTWalker {
    llvm::SmallVector<GenericTypeParamType *, 4> Params;
    Callback Fn;

    AssociateMissingParams(ArrayRef<GenericTypeParamType *> params,
                           Callback callback)
        : Params(params.begin(), params.end()), Fn(callback) {}

    bool walkToTypeReprPre(TypeRepr *T) override {
      if (Params.empty())
        return false;

      auto *ident = dyn_cast<ComponentIdentTypeRepr>(T);
      if (!ident)
        return true;

      auto *decl = dyn_cast_or_null<GenericTypeDecl>(ident->getBoundDecl());
      if (!decl)
        return true;

      auto *paramList = decl->getGenericParams();
      if (!paramList)
        return true;

      // There could a situation like `S<S>()`, so we need to be
      // careful not to point at first `S` because it has all of
      // its generic parameters specified.
      if (auto *generic = dyn_cast<GenericIdentTypeRepr>(ident)) {
        if (paramList->size() == generic->getNumGenericArgs())
          return true;
      }

      for (auto *candidate : paramList->getParams()) {
        auto result =
            llvm::find_if(Params, [&](const GenericTypeParamType *param) {
              return candidate == param->getDecl();
            });

        if (result != Params.end()) {
          Fn(ident, *result);
          Params.erase(result);
        }
      }

      // Keep walking.
      return true;
    }

    bool allParamsAssigned() const { return Params.empty(); }

  } associator(Parameters, callback);

  typeRepr->walk(associator);
  return associator.allParamsAssigned();
}

SourceLoc SkipUnhandledConstructInFunctionBuilderFailure::getLoc() const {
  if (auto stmt = unhandled.dyn_cast<Stmt *>())
    return stmt->getStartLoc();

  return unhandled.get<Decl *>()->getLoc();
}

/// Determine whether the given "if" chain has a missing "else".
static bool hasMissingElseInChain(IfStmt *ifStmt) {
  if (!ifStmt->getElseStmt())
    return true;

  if (auto ifElse = dyn_cast<IfStmt>(ifStmt->getElseStmt()))
    return hasMissingElseInChain(ifElse);

  return false;
}

void SkipUnhandledConstructInFunctionBuilderFailure::diagnosePrimary(
    bool asNote) {
  if (auto stmt = unhandled.dyn_cast<Stmt *>()) {
    emitDiagnostic(asNote ? diag::note_function_builder_control_flow
                          : diag::function_builder_control_flow,
                   builder->getName());

    // Emit custom notes to help the user introduce the appropriate 'build'
    // functions.
    SourceLoc buildInsertionLoc;
    std::string stubIndent;
    Type componentType;
    std::tie(buildInsertionLoc, stubIndent, componentType) =
        determineFunctionBuilderBuildFixItInfo(builder);

    if (buildInsertionLoc.isInvalid()) {
      // Do nothing.
    } else if (isa<IfStmt>(stmt) && hasMissingElseInChain(cast<IfStmt>(stmt))) {
      auto diag = emitDiagnosticAt(
          builder->getLoc(), diag::function_builder_missing_build_optional,
          builder->getDeclaredInterfaceType());

      std::string fixItString;
      {
        llvm::raw_string_ostream out(fixItString);
        printFunctionBuilderBuildFunction(
            builder, componentType, FunctionBuilderBuildFunction::BuildOptional,
            stubIndent, out);
      }

      diag.fixItInsert(buildInsertionLoc, fixItString);
    } else if (isa<SwitchStmt>(stmt) || isa<IfStmt>(stmt)) {
      auto diag = emitDiagnosticAt(
          builder->getLoc(), diag::function_builder_missing_build_either,
          builder->getDeclaredInterfaceType());

      std::string fixItString;
      {
        llvm::raw_string_ostream out(fixItString);
        printFunctionBuilderBuildFunction(
            builder, componentType,
            FunctionBuilderBuildFunction::BuildEitherFirst,
            stubIndent, out);
        out << '\n';
        printFunctionBuilderBuildFunction(
            builder, componentType,
            FunctionBuilderBuildFunction::BuildEitherSecond,
            stubIndent, out);
      }

      diag.fixItInsert(buildInsertionLoc, fixItString);
    } else if (isa<ForEachStmt>(stmt)) {
      auto diag = emitDiagnosticAt(
          builder->getLoc(), diag::function_builder_missing_build_array,
          builder->getDeclaredInterfaceType());

      std::string fixItString;
      {
        llvm::raw_string_ostream out(fixItString);
        printFunctionBuilderBuildFunction(
            builder, componentType, FunctionBuilderBuildFunction::BuildArray,
            stubIndent, out);
      }

      diag.fixItInsert(buildInsertionLoc, fixItString);
    }
  } else {
    emitDiagnostic(asNote ? diag::note_function_builder_decl
                          : diag::function_builder_decl,
                   builder->getName());
  }
}

bool SkipUnhandledConstructInFunctionBuilderFailure::diagnoseAsError() {
  diagnosePrimary(/*asNote=*/false);
  emitDiagnosticAt(builder, diag::kind_declname_declared_here,
                   builder->getDescriptiveKind(), builder->getName());
  return true;
}

bool SkipUnhandledConstructInFunctionBuilderFailure::diagnoseAsNote() {
  diagnosePrimary(/*asNote=*/true);
  return true;
}

bool MutatingMemberRefOnImmutableBase::diagnoseAsError() {
  auto *anchor = castToExpr(getRawAnchor());
  auto baseExpr = getBaseExprFor(anchor);
  if (!baseExpr)
    return false;

  auto diagIDsubelt = diag::cannot_pass_rvalue_mutating_subelement;
  auto diagIDmember = diag::cannot_pass_rvalue_mutating;

  if (auto *storage = dyn_cast<AbstractStorageDecl>(Member)) {
    if (storage->isGetterMutating()) {
      diagIDsubelt = diag::cannot_pass_rvalue_mutating_getter_subelement;
      diagIDmember = diag::cannot_pass_rvalue_mutating_getter;
    }
  }

  const auto &solution = getSolution();
  AssignmentFailure failure(baseExpr, solution, anchor->getLoc(), diagIDsubelt,
                            diagIDmember);
  return failure.diagnoseAsError();
}

bool InvalidTupleSplatWithSingleParameterFailure::diagnoseAsError() {
  auto selectedOverload = getCalleeOverloadChoiceIfAvailable(getLocator());
  if (!selectedOverload || !selectedOverload->choice.isDecl())
    return false;

  auto *choice = selectedOverload->choice.getDecl();

  auto *argExpr = getArgumentListExprFor(getLocator());
  if (!argExpr)
    return false;

  using Substitution = std::pair<GenericTypeParamType *, Type>;
  llvm::SmallVector<Substitution, 8> substitutions;

  auto paramTy = restoreGenericParameters(
      ParamType, [&](GenericTypeParamType *GP, Type resolvedType) {
        substitutions.push_back(std::make_pair(GP, resolvedType));
      });

  DeclBaseName name = choice->getBaseName();

  std::string subsStr;
  if (!substitutions.empty()) {
    llvm::array_pod_sort(
        substitutions.begin(), substitutions.end(),
        [](const std::pair<GenericTypeParamType *, Type> *lhs,
           const std::pair<GenericTypeParamType *, Type> *rhs) -> int {
          GenericParamKey key1(lhs->first);
          GenericParamKey key2(rhs->first);
          return key1 < key2 ? -1 : (key1 == key2) ? 0 : 1;
        });

    subsStr += " [with ";
    interleave(
        substitutions,
        [&subsStr](const Substitution &substitution) {
          subsStr += substitution.first->getString();
          subsStr += " = ";
          subsStr += substitution.second->getString();
        },
        [&subsStr] { subsStr += ", "; });
    subsStr += ']';
  }

  auto diagnostic =
      name.isSpecial()
          ? emitDiagnosticAt(argExpr->getLoc(),
                             diag::single_tuple_parameter_mismatch_special,
                             choice->getDescriptiveKind(), paramTy, subsStr)
          : emitDiagnosticAt(
                argExpr->getLoc(), diag::single_tuple_parameter_mismatch_normal,
                choice->getDescriptiveKind(), name, paramTy, subsStr);

  auto newLeftParenLoc = argExpr->getStartLoc();
  if (auto *TE = dyn_cast<TupleExpr>(argExpr)) {
    auto firstArgLabel = TE->getElementName(0);
    // Cover situations like:
    //
    // func foo(x: (Int, Int)) {}
    // foo(x: 0, 1)
    //
    // Where left paren should be suggested after the label,
    // since the label belongs to the parameter itself.
    if (!firstArgLabel.empty()) {
      auto paramTuple = resolveType(ParamType)->castTo<TupleType>();
      // If the label of the first argument matches the one required
      // by the parameter it would be omitted from the fixed parameter type.
      if (!paramTuple->getElement(0).hasName())
        newLeftParenLoc = Lexer::getLocForEndOfToken(getASTContext().SourceMgr,
                                                     TE->getElementNameLoc(0));
    }
  }

  diagnostic.highlight(argExpr->getSourceRange())
      .fixItInsertAfter(newLeftParenLoc, "(")
      .fixItInsert(argExpr->getEndLoc(), ")");

  return true;
}

bool ThrowingFunctionConversionFailure::diagnoseAsError() {
  emitDiagnostic(diag::throws_functiontype_mismatch, getFromType(),
                 getToType());
  return true;
}

bool InOutConversionFailure::diagnoseAsError() {
  auto *locator = getLocator();
  auto path = locator->getPath();

  if (!path.empty() &&
      path.back().getKind() == ConstraintLocator::FunctionArgument) {
    if (auto argApplyInfo = getFunctionArgApplyInfo(locator)) {
      emitDiagnostic(diag::cannot_convert_argument_value,
                     argApplyInfo->getArgType(), argApplyInfo->getParamType());
    } else {
      assert(locator->findLast<LocatorPathElt::ContextualType>());
      auto anchor = getAnchor();
      auto contextualType = getContextualType(anchor);
      auto purpose = getContextualTypePurpose();
      auto diagnostic = getDiagnosticFor(purpose, contextualType);

      if (!diagnostic)
        return false;

      emitDiagnostic(*diagnostic, getType(anchor), contextualType);
    }

    return true;
  }

  emitDiagnostic(diag::cannot_pass_rvalue_inout_converted, getFromType(),
                 getToType());
  fixItChangeArgumentType();
  return true;
}

void InOutConversionFailure::fixItChangeArgumentType() const {
  auto *argExpr = castToExpr(getAnchor());
  auto *DC = getDC();

  if (auto *IOE = dyn_cast<InOutExpr>(argExpr))
    argExpr = IOE->getSubExpr();

  auto *DRE = dyn_cast<DeclRefExpr>(argExpr);
  if (!DRE)
    return;

  auto *VD = dyn_cast_or_null<VarDecl>(DRE->getDecl());
  if (!VD)
    return;

  // Don't emit for non-local variables.
  // (But in script-mode files, we consider module-scoped
  // variables in the same file to be local variables.)
  auto VDC = VD->getDeclContext();
  bool isLocalVar = VDC->isLocalContext();
  if (!isLocalVar && VDC->isModuleScopeContext()) {
    auto argFile = DC->getParentSourceFile();
    auto varFile = VDC->getParentSourceFile();
    isLocalVar = (argFile == varFile && argFile->isScriptMode());
  }
  if (!isLocalVar)
    return;

  auto actualType = getFromType();
  auto neededType = getToType();

  SmallString<32> scratch;
  SourceLoc endLoc;   // Filled in if we decide to diagnose this
  SourceLoc startLoc; // Left invalid if we're inserting

  auto isSimpleTypelessPattern = [](Pattern *P) -> bool {
    if (auto VP = dyn_cast_or_null<BindingPattern>(P))
      P = VP->getSubPattern();
    return P && isa<NamedPattern>(P);
  };

  auto typeRange = VD->getTypeSourceRangeForDiagnostics();
  if (typeRange.isValid()) {
    startLoc = typeRange.Start;
    endLoc = typeRange.End;
  } else if (isSimpleTypelessPattern(VD->getParentPattern())) {
    endLoc = VD->getNameLoc();
    scratch += ": ";
  }

  if (endLoc.isInvalid())
    return;

  scratch += neededType.getString();

  // Adjust into the location where we actually want to insert
  endLoc = Lexer::getLocForEndOfToken(getASTContext().SourceMgr, endLoc);

  // Since we already adjusted endLoc, this will turn an insertion
  // into a zero-character replacement.
  if (!startLoc.isValid())
    startLoc = endLoc;

  emitDiagnosticAt(VD, diag::inout_change_var_type_if_possible, actualType,
                   neededType)
      .fixItReplaceChars(startLoc, endLoc, scratch);
}

bool ArgumentMismatchFailure::diagnoseAsError() {
  if (diagnoseMisplacedMissingArgument())
    return true;

  if (diagnoseConversionToBool())
    return true;

  if (diagnoseArchetypeMismatch())
    return true;

  if (diagnosePatternMatchingMismatch())
    return true;

  if (diagnoseUseOfReferenceEqualityOperator())
    return true;

  if (diagnosePropertyWrapperMismatch())
    return true;

  if (diagnoseTrailingClosureMismatch())
    return true;

  auto argType = getFromType();
  auto paramType = getToType();

  if (paramType->isAnyObject()) {
    emitDiagnostic(diag::cannot_convert_argument_value_anyobject, argType,
                   paramType);
    return true;
  }

  Diag<Type, Type> diagnostic = diag::cannot_convert_argument_value;

  // If parameter type is a protocol value, let's says that
  // argument doesn't conform to a give protocol.
  if (paramType->isExistentialType())
    diagnostic = diag::cannot_convert_argument_value_protocol;

  auto diag = emitDiagnostic(diagnostic, argType, paramType);

  // If argument is an l-value type and parameter is a pointer type,
  // let's match up its element type to the argument to see whether
  // it would be appropriate to suggest adding `&`.
  auto argument = getAnchor();
  if (getType(argument, /*wantRValue=*/false)->is<LValueType>()) {
    auto elementTy = paramType->getAnyPointerElementType();
    if (elementTy && argType->isEqual(elementTy)) {
      diag.fixItInsert(::getSourceRange(argument).Start, "&");
      return true;
    }
  }

  tryFixIts(diag);
  return true;
}

bool ArgumentMismatchFailure::diagnoseAsNote() {
  auto *locator = getLocator();
  if (auto *callee = getCallee()) {
    emitDiagnosticAt(
        callee, diag::candidate_has_invalid_argument_at_position, getToType(),
        getParamPosition(),
        locator->isLastElement<LocatorPathElt::LValueConversion>());
    return true;
  }

  return false;
}

bool ArgumentMismatchFailure::diagnoseUseOfReferenceEqualityOperator() const {
  auto *locator = getLocator();

  if (!isArgumentOfReferenceEqualityOperator(locator))
    return false;

  auto *binaryOp = castToExpr<BinaryExpr>(getRawAnchor());
  auto *lhs = binaryOp->getArg()->getElement(0);
  auto *rhs = binaryOp->getArg()->getElement(1);

  auto name = *getOperatorName(binaryOp->getFn());

  auto lhsType = getType(lhs);
  auto rhsType = getType(rhs);

  // If both arguments where incorrect e.g. both are function types,
  // let's avoid producing a diagnostic second time, because first
  // one would cover both arguments.
  if (getAsExpr(getAnchor()) == rhs && rhsType->is<FunctionType>()) {
    auto *argLoc = getConstraintLocator(
        binaryOp,
        {ConstraintLocator::ApplyArgument,
         LocatorPathElt::ApplyArgToParam(0, 0, getParameterFlagsAtIndex(0))});

    if (llvm::any_of(getSolution().Fixes, [&argLoc](const ConstraintFix *fix) {
          return fix->getLocator() == argLoc;
        }))
      return true;
  }

  // Regardless of whether the type has reference or value semantics,
  // comparison with nil is illegal, albeit for different reasons spelled
  // out by the diagnosis.
  if (isa<NilLiteralExpr>(lhs) || isa<NilLiteralExpr>(rhs)) {
    std::string revisedName = std::string(name);
    revisedName.pop_back();

    auto loc = binaryOp->getLoc();
    auto nonNilType = isa<NilLiteralExpr>(lhs) ? rhsType : lhsType;
    auto nonNilExpr = isa<NilLiteralExpr>(lhs) ? rhs : lhs;

    // If we made it here, then we're trying to perform a comparison with
    // reference semantics rather than value semantics. The fixit will
    // lop off the extra '=' in the operator.
    if (nonNilType->getOptionalObjectType()) {
      emitDiagnosticAt(
          loc, diag::value_type_comparison_with_nil_illegal_did_you_mean,
          nonNilType)
          .fixItReplace(loc, revisedName);
    } else {
      emitDiagnosticAt(loc, diag::value_type_comparison_with_nil_illegal,
                       nonNilType)
          .highlight(nonNilExpr->getSourceRange());
    }

    return true;
  }

  if (lhsType->is<FunctionType>() || rhsType->is<FunctionType>()) {
    emitDiagnosticAt(binaryOp->getLoc(), diag::cannot_reference_compare_types,
                     name.str(), lhsType, rhsType)
        .highlight(lhs->getSourceRange())
        .highlight(rhs->getSourceRange());
    return true;
  }

  return false;
}

bool ArgumentMismatchFailure::diagnosePatternMatchingMismatch() const {
  if (!isArgumentOfPatternMatchingOperator(getLocator()))
    return false;

  auto *op = castToExpr<BinaryExpr>(getRawAnchor());
  auto *lhsExpr = op->getArg()->getElement(0);
  auto *rhsExpr = op->getArg()->getElement(1);

  auto lhsType = getType(lhsExpr);
  auto rhsType = getType(rhsExpr);

  auto diagnostic =
      lhsType->is<UnresolvedType>()
          ? emitDiagnostic(
                diag::cannot_match_unresolved_expr_pattern_with_value, rhsType)
          : emitDiagnostic(diag::cannot_match_expr_pattern_with_value, lhsType,
                           rhsType);

  diagnostic.highlight(lhsExpr->getSourceRange());
  diagnostic.highlight(rhsExpr->getSourceRange());

  if (auto optUnwrappedType = rhsType->getOptionalObjectType()) {
    if (lhsType->isEqual(optUnwrappedType)) {
      diagnostic.fixItInsertAfter(lhsExpr->getEndLoc(), "?");
    }
  }

  return true;
}

bool ArgumentMismatchFailure::diagnoseArchetypeMismatch() const {
  auto *argTy = getFromType()->getAs<ArchetypeType>();
  auto *paramTy = getToType()->getAs<ArchetypeType>();

  if (!(argTy && paramTy))
    return false;

  // Produce this diagnostic only if the names
  // of the generic parameters are the same.
  if (argTy->getName() != paramTy->getName())
    return false;

  auto getGenericTypeDecl = [&](ArchetypeType *archetype) -> ValueDecl * {
    auto paramType = archetype->getInterfaceType();

    if (auto *GTPT = paramType->getAs<GenericTypeParamType>())
      return GTPT->getDecl();

    if (auto *DMT = paramType->getAs<DependentMemberType>())
      return DMT->getAssocType();

    return nullptr;
  };

  auto *argDecl = getGenericTypeDecl(argTy);
  auto *paramDecl = getGenericTypeDecl(paramTy);

  if (!(paramDecl && argDecl))
    return false;

  emitDiagnostic(diag::cannot_convert_argument_value_generic, argTy,
                 describeGenericType(argDecl), paramTy,
                 describeGenericType(paramDecl));

  emitDiagnosticAt(argDecl, diag::descriptive_generic_type_declared_here,
                   describeGenericType(argDecl, true));

  emitDiagnosticAt(paramDecl, diag::descriptive_generic_type_declared_here,
                   describeGenericType(paramDecl, true));

  return true;
}

bool ArgumentMismatchFailure::diagnoseMisplacedMissingArgument() const {
  const auto &solution = getSolution();
  auto *locator = getLocator();

  if (!MissingArgumentsFailure::isMisplacedMissingArgument(solution, locator))
    return false;

  // Assign new type variable to a type of a parameter.
  auto *fnType = getFnType();
  const auto &param = fnType->getParams()[0];

  auto anchor = getRawAnchor();

  MissingArgumentsFailure failure(
      solution, {std::make_pair(0, param)},
      getConstraintLocator(anchor, ConstraintLocator::ApplyArgument));

  return failure.diagnoseSingleMissingArgument();
}

bool ArgumentMismatchFailure::diagnosePropertyWrapperMismatch() const {
  auto argType = getFromType();
  auto paramType = getToType();

  // Verify that this is an implicit call to a property wrapper initializer
  // in a form of `init(wrappedValue:)` or deprecated `init(initialValue:)`.
  auto *call = getAsExpr<CallExpr>(getRawAnchor());
  if (!(call && call->isImplicit() && isa<TypeExpr>(call->getFn()) &&
        call->getNumArguments() == 1 &&
        (call->getArgumentLabels().front() == getASTContext().Id_wrappedValue ||
         call->getArgumentLabels().front() == getASTContext().Id_initialValue)))
    return false;

  auto argExpr = cast<TupleExpr>(call->getArg())->getElement(0);
  // If this is an attempt to initialize property wrapper with opaque value
  // of error type, let's just ignore that problem since original mismatch
  // has been diagnosed already.
  if (argExpr->isImplicit() && isa<OpaqueValueExpr>(argExpr) &&
      argType->is<ErrorType>())
    return true;

  emitDiagnostic(diag::cannot_convert_initializer_value, argType, paramType);
  return true;
}

bool ArgumentMismatchFailure::diagnoseTrailingClosureMismatch() const {
  if (!Info.isTrailingClosure())
    return false;

  auto paramType = getToType();
  if (paramType->lookThroughAllOptionalTypes()->is<AnyFunctionType>())
    return false;

  emitDiagnostic(diag::trailing_closure_bad_param, paramType)
      .highlight(getSourceRange());

  if (auto overload = getCalleeOverloadChoiceIfAvailable(getLocator())) {
    if (auto *decl = overload->choice.getDeclOrNull()) {
      emitDiagnosticAt(decl, diag::decl_declared_here, decl->getName());
    }
  }

  return true;
}

void ExpandArrayIntoVarargsFailure::tryDropArrayBracketsFixIt(
    const Expr *anchor) const {
  // If this is an array literal, offer to remove the brackets and pass the
  // elements directly as variadic arguments.
  if (auto *arrayExpr = dyn_cast<ArrayExpr>(anchor)) {
    auto diag = emitDiagnosticAt(arrayExpr->getLoc(),
                                 diag::suggest_pass_elements_directly);
    diag.fixItRemove(arrayExpr->getLBracketLoc())
        .fixItRemove(arrayExpr->getRBracketLoc());
    // Handle the case where the array literal has a trailing comma.
    if (arrayExpr->getNumCommas() == arrayExpr->getNumElements())
      diag.fixItRemove(arrayExpr->getCommaLocs().back());
  }
}

bool ExpandArrayIntoVarargsFailure::diagnoseAsError() {
  if (auto *anchor = getAsExpr(getAnchor())) {
    emitDiagnostic(diag::cannot_convert_array_to_variadic, getFromType(),
                   getToType());
    tryDropArrayBracketsFixIt(anchor);
    // TODO: Array splat fix-it once that's supported.
    return true;
  }
  return false;
}

bool ExpandArrayIntoVarargsFailure::diagnoseAsNote() {
  auto overload = getCalleeOverloadChoiceIfAvailable(getLocator());
  auto *anchor = getAsExpr(getAnchor());
  if (!overload || !anchor)
    return false;

  if (auto chosenDecl = overload->choice.getDeclOrNull()) {
    emitDiagnosticAt(chosenDecl, diag::candidate_would_match_array_to_variadic,
                     getToType());
    tryDropArrayBracketsFixIt(anchor);
    return true;
  }
  return false;
}

bool ExtraneousCallFailure::diagnoseAsError() {
  auto anchor = getAnchor();
  auto *locator = getLocator();

  // If this is something like `foo()` where `foo` is a variable
  // or a property, let's suggest dropping `()`.
  auto removeParensFixIt = [&](InFlightDiagnostic &diagnostic) {
    auto *argLoc =
        getConstraintLocator(getRawAnchor(), ConstraintLocator::ApplyArgument);

    if (auto *TE = getAsExpr<TupleExpr>(simplifyLocatorToAnchor(argLoc))) {
      if (TE->getNumElements() == 0) {
        diagnostic.fixItRemove(TE->getSourceRange());
      }
    }
  };

  if (auto overload = getCalleeOverloadChoiceIfAvailable(locator)) {
    if (auto *decl = overload->choice.getDeclOrNull()) {
      if (auto *enumCase = dyn_cast<EnumElementDecl>(decl)) {
        auto diagnostic =
            emitDiagnostic(diag::unexpected_arguments_in_enum_case,
                           enumCase->getBaseIdentifier());
        removeParensFixIt(diagnostic);
        return true;
      }
    }
  }

  if (auto *UDE = getAsExpr<UnresolvedDotExpr>(anchor)) {
    auto *baseExpr = UDE->getBase();
    auto *call = castToExpr<CallExpr>(getRawAnchor());

    if (getType(baseExpr)->isAnyObject()) {
      emitDiagnostic(diag::cannot_call_with_params,
                     UDE->getName().getBaseName().userFacingName(),
                     getType(call->getArg())->getString(),
                     isa<TypeExpr>(baseExpr));
      return true;
    }
  }

  auto diagnostic =
      emitDiagnostic(diag::cannot_call_non_function_value, getType(anchor));
  removeParensFixIt(diagnostic);
  return true;
}

void NonEphemeralConversionFailure::emitSuggestionNotes() const {
  auto getPointerKind = [](Type ty) -> PointerTypeKind {
    PointerTypeKind pointerKind;
    auto pointeeType = ty->lookThroughSingleOptionalType()
                         ->getAnyPointerElementType(pointerKind);
    assert(pointeeType && "Expected a pointer!");
    (void)pointeeType;

    return pointerKind;
  };

  // This must stay in sync with diag::ephemeral_use_array_with_unsafe_buffer
  // and diag::ephemeral_use_with_unsafe_pointer.
  enum AlternativeKind {
    AK_Raw = 0,
    AK_MutableRaw,
    AK_Typed,
    AK_MutableTyped,
  };

  auto getAlternativeKind = [&]() -> Optional<AlternativeKind> {
    switch (getPointerKind(getParamType())) {
    case PTK_UnsafeRawPointer:
      return AK_Raw;
    case PTK_UnsafeMutableRawPointer:
      return AK_MutableRaw;
    case PTK_UnsafePointer:
      return AK_Typed;
    case PTK_UnsafeMutablePointer:
      return AK_MutableTyped;
    case PTK_AutoreleasingUnsafeMutablePointer:
      return None;
    }
    llvm_unreachable("invalid pointer kind");
  };

  // First emit a note about the implicit conversion only lasting for the
  // duration of the call.
  auto *argExpr = getArgExpr();
  emitDiagnosticAt(
      argExpr->getLoc(), diag::ephemeral_pointer_argument_conversion_note,
      getArgType(), getParamType(), getCallee(), getCalleeFullName())
      .highlight(argExpr->getSourceRange());

  // Then try to find a suitable alternative.
  switch (ConversionKind) {
  case ConversionRestrictionKind::ArrayToPointer: {
    // Don't suggest anything for optional arrays, as there's currently no
    // direct alternative.
    if (getArgType()->getOptionalObjectType())
      break;

    // We can suggest using withUnsafe[Mutable][Bytes/BufferPointer].
    if (auto alternative = getAlternativeKind())
      emitDiagnosticAt(argExpr->getLoc(),
                       diag::ephemeral_use_array_with_unsafe_buffer,
                       *alternative);
    break;
  }
  case ConversionRestrictionKind::StringToPointer: {
    // Don't suggest anything for optional strings, as there's currently no
    // direct alternative.
    if (getArgType()->getOptionalObjectType())
      break;

    // We can suggest withCString as long as the resulting pointer is
    // immutable.
    switch (getPointerKind(getParamType())) {
    case PTK_UnsafePointer:
    case PTK_UnsafeRawPointer:
      emitDiagnosticAt(argExpr->getLoc(),
                       diag::ephemeral_use_string_with_c_string);
      break;
    case PTK_UnsafeMutableRawPointer:
    case PTK_UnsafeMutablePointer:
    case PTK_AutoreleasingUnsafeMutablePointer:
      // There's nothing really sensible we can suggest for a mutable pointer.
      break;
    }
    break;
  }
  case ConversionRestrictionKind::InoutToPointer:
    // For an arbitrary inout-to-pointer, we can suggest
    // withUnsafe[Mutable][Bytes/Pointer].
    if (auto alternative = getAlternativeKind())
      emitDiagnosticAt(argExpr->getLoc(),
                       diag::ephemeral_use_with_unsafe_pointer, *alternative);
    break;
  case ConversionRestrictionKind::DeepEquality:
  case ConversionRestrictionKind::Superclass:
  case ConversionRestrictionKind::Existential:
  case ConversionRestrictionKind::MetatypeToExistentialMetatype:
  case ConversionRestrictionKind::ExistentialMetatypeToMetatype:
  case ConversionRestrictionKind::ValueToOptional:
  case ConversionRestrictionKind::OptionalToOptional:
  case ConversionRestrictionKind::ClassMetatypeToAnyObject:
  case ConversionRestrictionKind::ExistentialMetatypeToAnyObject:
  case ConversionRestrictionKind::ProtocolMetatypeToProtocolClass:
  case ConversionRestrictionKind::PointerToPointer:
  case ConversionRestrictionKind::ArrayUpcast:
  case ConversionRestrictionKind::DictionaryUpcast:
  case ConversionRestrictionKind::SetUpcast:
  case ConversionRestrictionKind::HashableToAnyHashable:
  case ConversionRestrictionKind::CFTollFreeBridgeToObjC:
  case ConversionRestrictionKind::ObjCTollFreeBridgeToCF:
    llvm_unreachable("Expected an ephemeral conversion!");
  }
}

bool NonEphemeralConversionFailure::diagnosePointerInit() const {
  auto *constructor = dyn_cast_or_null<ConstructorDecl>(getCallee());
  if (!constructor)
    return false;

  auto constructedTy = getFnType()->getResult();

  // Strip off a level of optionality if we have a failable initializer.
  if (constructor->isFailable())
    constructedTy = constructedTy->getOptionalObjectType();

  // This must stay in sync with diag::cannot_construct_dangling_pointer.
  enum ConstructorKind {
    CK_Pointer = 0,
    CK_BufferPointer,
  };

  // Consider OpaquePointer as well as the other kinds of pointers.
  auto isConstructingPointer =
      constructedTy->getAnyPointerElementType() ||
      constructedTy->getAnyNominal() == getASTContext().getOpaquePointerDecl();

  ConstructorKind constructorKind;
  auto parameterCount = constructor->getParameters()->size();
  if (isConstructingPointer && parameterCount == 1) {
    constructorKind = CK_Pointer;
  } else if (constructedTy->getAnyBufferPointerElementType() &&
             parameterCount == 2) {
    constructorKind = CK_BufferPointer;
  } else {
    return false;
  }

  auto diagID = DowngradeToWarning
                    ? diag::cannot_construct_dangling_pointer_warning
                    : diag::cannot_construct_dangling_pointer;

  auto anchor = getRawAnchor();
  emitDiagnosticAt(::getLoc(anchor), diagID, constructedTy, constructorKind)
      .highlight(::getSourceRange(anchor));

  emitSuggestionNotes();
  return true;
}

bool NonEphemeralConversionFailure::diagnoseAsNote() {
  // We can only emit a useful note if we have a callee.
  if (auto *callee = getCallee()) {
    emitDiagnosticAt(callee, diag::candidate_performs_illegal_ephemeral_conv,
                     getParamPosition());
    return true;
  }
  return false;
}

bool NonEphemeralConversionFailure::diagnoseAsError() {
  // Emit a specialized diagnostic for
  // Unsafe[Mutable][Raw]Pointer.init([mutating]:) &
  // Unsafe[Mutable][Raw]BufferPointer.init(start:count:).
  if (diagnosePointerInit())
    return true;

  // Otherwise, emit a more general diagnostic.
  SmallString<8> scratch;
  auto argDesc = getArgDescription(scratch);

  auto *argExpr = getArgExpr();
  if (isa<InOutExpr>(argExpr)) {
    auto diagID = DowngradeToWarning
                      ? diag::cannot_use_inout_non_ephemeral_warning
                      : diag::cannot_use_inout_non_ephemeral;

    emitDiagnosticAt(argExpr->getLoc(), diagID, argDesc, getCallee(),
                     getCalleeFullName())
        .highlight(argExpr->getSourceRange());
  } else {
    auto diagID = DowngradeToWarning
                      ? diag::cannot_pass_type_to_non_ephemeral_warning
                      : diag::cannot_pass_type_to_non_ephemeral;

    emitDiagnosticAt(argExpr->getLoc(), diagID, getArgType(), argDesc,
                     getCallee(), getCalleeFullName())
        .highlight(argExpr->getSourceRange());
  }
  emitSuggestionNotes();
  return true;
}

bool AssignmentTypeMismatchFailure::diagnoseMissingConformance() const {
  auto srcType = getFromType();
  auto dstType = getToType()->lookThroughAllOptionalTypes();

  llvm::SmallPtrSet<ProtocolDecl *, 4> srcMembers;
  llvm::SmallPtrSet<ProtocolDecl *, 4> dstMembers;

  auto retrieveProtocols = [](Type type,
                              llvm::SmallPtrSetImpl<ProtocolDecl *> &members) {
    if (auto *protocol = type->getAs<ProtocolType>())
      members.insert(protocol->getDecl());

    if (auto *composition = type->getAs<ProtocolCompositionType>()) {
      for (auto member : composition->getMembers()) {
        if (auto *protocol = member->getAs<ProtocolType>())
          members.insert(protocol->getDecl());
      }
    }
  };

  retrieveProtocols(srcType, srcMembers);
  retrieveProtocols(dstType, dstMembers);

  if (srcMembers.empty() || dstMembers.empty())
    return false;

  // Let's check whether there is an overlap between source and destination.
  for (auto *member : srcMembers)
    dstMembers.erase(member);

  if (dstMembers.size() == 1)
    dstType = (*dstMembers.begin())->getDeclaredType();

  emitDiagnostic(diag::cannot_convert_assign_protocol, srcType, dstType);
  return true;
}

bool AssignmentTypeMismatchFailure::diagnoseAsError() {
  if (diagnoseMissingConformance())
    return true;

  return ContextualFailure::diagnoseAsError();
}

bool AssignmentTypeMismatchFailure::diagnoseAsNote() {
  auto anchor = getAnchor();

  if (auto overload =
          getCalleeOverloadChoiceIfAvailable(getConstraintLocator(anchor))) {
    if (auto *decl = overload->choice.getDeclOrNull()) {
      emitDiagnosticAt(decl,
                       diag::cannot_convert_candidate_result_to_contextual_type,
                       decl->getName(), getFromType(), getToType());
      return true;
    }
  }

  return false;
}

bool MissingContextualBaseInMemberRefFailure::diagnoseAsError() {
  auto *anchor = castToExpr(getAnchor());
  // Member reference could be wrapped into a number of parens
  // e.g. `((.foo))`.
  auto *parentExpr = findParentExpr(anchor);

  // Look through immediate call of unresolved member (e.g., `.foo(0)`).
  if (parentExpr && isa<CallExpr>(parentExpr))
    parentExpr = findParentExpr(parentExpr);

  // FIXME: We should probably look through the entire member chain so that
  // something like `let _ = .foo().bar` gets the "no contextual type" error
  // rather than the "Cannot infer contextual base" error.
  UnresolvedMemberChainResultExpr *resultExpr = nullptr;
  if (parentExpr && isa<UnresolvedMemberChainResultExpr>(parentExpr)) {
    resultExpr = cast<UnresolvedMemberChainResultExpr>(parentExpr);
    parentExpr = findParentExpr(parentExpr);
  }

  do {
    // If we have found something which isn't a paren let's stop,
    // otherwise let's keep unwrapping until there are either no
    // more parens or no more parents...
    if (!parentExpr || !isa<ParenExpr>(parentExpr))
      break;
  } while ((parentExpr = findParentExpr(parentExpr)));

  auto diagnostic = parentExpr || (resultExpr && getContextualType(resultExpr))
                        ? diag::cannot_infer_base_of_unresolved_member
                        : diag::unresolved_member_no_inference;

  emitDiagnostic(diagnostic, MemberName).highlight(getSourceRange());
  return true;
}

bool UnableToInferClosureParameterType::diagnoseAsError() {
  auto *closure = castToExpr<ClosureExpr>(getRawAnchor());

  // Let's check whether this closure is an argument to
  // a call which couldn't be properly resolved e.g.
  // missing  member or invalid contextual reference and
  // if so let's not diagnose this problem because main
  // issue here is inability to establish context for
  // closure inference.
  //
  // TODO(diagnostics): Once we gain an ability to determine
  // originating source of type holes this check could be
  // significantly simplified.
  {
    auto &solution = getSolution();

    // If there is a contextual mismatch associated with this
    // closure, let's not diagnose any parameter type issues.
    if (hasFixFor(solution, getConstraintLocator(
                                closure, LocatorPathElt::ContextualType())))
      return false;

    if (auto *parentExpr = findParentExpr(closure)) {
      while (parentExpr &&
             (isa<TupleExpr>(parentExpr) || isa<ParenExpr>(parentExpr))) {
        parentExpr = findParentExpr(parentExpr);
      }

      if (parentExpr) {
        // Missing or invalid member reference in call.
        if (auto *AE = dyn_cast<ApplyExpr>(parentExpr)) {
          if (getType(AE->getFn())->is<UnresolvedType>())
            return false;
        }

        // Any fix anchored on parent expression makes it unnecessary
        // to diagnose unability to infer parameter type because it's
        // an indication that proper context couldn't be established to
        // resolve the closure.
        ASTNode parentNode(parentExpr);
        if (llvm::any_of(solution.Fixes,
                         [&parentNode](const ConstraintFix *fix) -> bool {
                           return fix->getAnchor() == parentNode;
                         }))
          return false;
      }
    }
  }

  auto paramIdx = getLocator()
                      ->castLastElementTo<LocatorPathElt::TupleElement>()
                      .getIndex();

  auto *PD = closure->getParameters()->get(paramIdx);

  llvm::SmallString<16> id;
  llvm::raw_svector_ostream OS(id);

  if (PD->isAnonClosureParam()) {
    OS << "$" << paramIdx;
  } else {
    OS << "'" << PD->getParameterName() << "'";
  }

  auto loc = PD->isAnonClosureParam() ? getLoc() : PD->getLoc();
  emitDiagnosticAt(loc, diag::cannot_infer_closure_parameter_type, OS.str());
  return true;
}

bool UnableToInferClosureReturnType::diagnoseAsError() {
  auto *closure = castToExpr<ClosureExpr>(getRawAnchor());

  auto diagnostic = emitDiagnostic(diag::cannot_infer_closure_result_type,
                                   closure->hasSingleExpressionBody());

  // If there is a location for an 'in' token, then the argument list was
  // specified somehow but no return type was.  Insert a "-> ReturnType "
  // before the in token.
  if (closure->getInLoc().isValid()) {
    diagnostic.fixItInsert(closure->getInLoc(),
                           diag::insert_closure_return_type_placeholder,
                           /*argListSpecified=*/false);
  } else if (closure->getParameters()->size() == 0) {
    // Otherwise, the closure must take zero arguments.
    //
    // As such, we insert " () -> ReturnType in " right after the '{' that
    // starts the closure body.
    diagnostic.fixItInsertAfter(closure->getBody()->getLBraceLoc(),
                                diag::insert_closure_return_type_placeholder,
                                /*argListSpecified=*/true);
  }

  return true;
}

static std::pair<StringRef, StringRef>
getImportModuleAndDefaultType(const ASTContext &ctx,
                              const ObjectLiteralExpr *expr) {
  const auto &target = ctx.LangOpts.Target;

  switch (expr->getLiteralKind()) {
    case ObjectLiteralExpr::colorLiteral: {
      if (target.isMacOSX()) {
        return std::make_pair("AppKit", "NSColor");
      } else if (target.isiOS() || target.isTvOS()) {
        return std::make_pair("UIKit", "UIColor");
      }
      break;
    }

    case ObjectLiteralExpr::imageLiteral: {
      if (target.isMacOSX()) {
        return std::make_pair("AppKit", "NSImage");
      } else if (target.isiOS() || target.isTvOS()) {
        return std::make_pair("UIKit", "UIImage");
      }
      break;
    }

    case ObjectLiteralExpr::fileLiteral: {
      return std::make_pair("Foundation", "URL");
    }
  }

  return std::make_pair("", "");
}

SourceLoc UnableToInferProtocolLiteralType::getLoc() const {
  return ::getLoc(getRawAnchor());
}

bool UnableToInferProtocolLiteralType::diagnoseAsError() {
  auto &ctx = getASTContext();
  auto *expr = castToExpr<ObjectLiteralExpr>(getRawAnchor());

  StringRef importModule;
  StringRef importDefaultTypeName;
  std::tie(importModule, importDefaultTypeName) =
      getImportModuleAndDefaultType(ctx, expr);

  auto plainName = expr->getLiteralKindPlainName();
  emitDiagnostic(diag::object_literal_default_type_missing, plainName);
  if (!importModule.empty()) {
    emitDiagnostic(diag::object_literal_resolve_import, importModule,
                   importDefaultTypeName, plainName);
  }

  return true;
}

bool MissingQuialifierInMemberRefFailure::diagnoseAsError() {
  auto selectedOverload = getOverloadChoiceIfAvailable(getLocator());
  if (!selectedOverload)
    return false;

  auto *UDE = castToExpr<UnresolvedDotExpr>(getRawAnchor());

  auto baseType = getType(UDE->getBase());

  auto methodKind = baseType->isAnyExistentialType()
                        ? DescriptiveDeclKind::StaticMethod
                        : DescriptiveDeclKind::Method;

  auto choice = selectedOverload->choice.getDeclOrNull();
  if (!choice)
    return false;

  auto *DC = choice->getDeclContext();
  if (!(DC->isModuleContext() || DC->isModuleScopeContext())) {
    emitDiagnostic(diag::member_shadows_function, UDE->getName(), methodKind,
                   choice->getDescriptiveKind(), choice->getName());
    return true;
  }

  auto qualifier = DC->getParentModule()->getName();

  emitDiagnostic(diag::member_shadows_global_function, UDE->getName(),
                 methodKind, choice->getDescriptiveKind(),
                 choice->getName(), qualifier);

  SmallString<32> namePlusDot = qualifier.str();
  namePlusDot.push_back('.');

  emitDiagnostic(diag::fix_unqualified_access_top_level_multi, namePlusDot,
                 choice->getDescriptiveKind(), qualifier)
      .fixItInsert(UDE->getStartLoc(), namePlusDot);

  emitDiagnosticAt(choice, diag::decl_declared_here, choice->getName());
  return true;
}

bool CoercionAsForceCastFailure::diagnoseAsError() {
  emitDiagnostic(diag::coercion_may_fail_warning, getFromType(), getToType())
      .highlight(getSourceRange());
  return true;
}

bool KeyPathRootTypeMismatchFailure::diagnoseAsError() {
  auto locator = getLocator();
  assert(locator->isKeyPathRoot() && "Expected a key path root");
  
  auto baseType = getFromType();
  auto rootType = getToType();

  emitDiagnostic(diag::expr_keypath_root_type_mismatch,
                 rootType, baseType);
  return true;
}

bool MultiArgFuncKeyPathFailure::diagnoseAsError() {
  // Diagnose use a keypath where a function with multiple arguments is expected
  emitDiagnostic(diag::expr_keypath_multiparam_func_conversion,
                 resolveType(functionType));
  return true;
}

bool UnableToInferKeyPathRootFailure::diagnoseAsError() {
  assert(isExpr<KeyPathExpr>(getAnchor()) && "Expected key path expression");
  auto &ctx = getASTContext();
  auto contextualType = getContextualType(getAnchor());
  auto *keyPathExpr = castToExpr<KeyPathExpr>(getAnchor());

  auto emitKeyPathDiagnostic = [&]() {
    if (contextualType &&
        contextualType->getAnyNominal() == ctx.getAnyKeyPathDecl()) {
      return emitDiagnostic(
          diag::cannot_infer_keypath_root_anykeypath_context);
    }
    return emitDiagnostic(
        diag::cannot_infer_contextual_keypath_type_specify_root);
  };

  emitKeyPathDiagnostic()
      .highlight(keyPathExpr->getLoc())
      .fixItInsertAfter(keyPathExpr->getStartLoc(), "<#Root#>");
  return true;
}

Optional<Diag<Type, Type>>
AbstractRawRepresentableFailure::getDiagnostic() const {
  auto *locator = getLocator();

  if (locator->isForContextualType()) {
    return diag::cannot_convert_initializer_value;
  } else if (locator->isForAssignment()) {
    return diag::cannot_convert_assign;
  } else if (locator->isLastElement<LocatorPathElt::ApplyArgToParam>()) {
    return diag::cannot_convert_argument_value;
  }

  return None;
}

bool AbstractRawRepresentableFailure::diagnoseAsError() {
  auto message = getDiagnostic();
  if (!message)
    return false;

  auto diagnostic = emitDiagnostic(*message, getFromType(), getToType());
  fixIt(diagnostic);
  return true;
}

bool AbstractRawRepresentableFailure::diagnoseAsNote() {
  auto *locator = getLocator();

  Optional<InFlightDiagnostic> diagnostic;
  if (locator->isForContextualType()) {
    auto overload = getCalleeOverloadChoiceIfAvailable(locator);
    if (!overload)
      return false;

    if (auto *decl = overload->choice.getDeclOrNull()) {
      diagnostic.emplace(emitDiagnosticAt(
          decl, diag::cannot_convert_candidate_result_to_contextual_type,
          decl->getName(), ExpectedType, RawReprType));
    }
  } else if (auto argConv =
                 locator->getLastElementAs<LocatorPathElt::ApplyArgToParam>()) {
    diagnostic.emplace(
        emitDiagnostic(diag::candidate_has_invalid_argument_at_position,
                       RawReprType, argConv->getParamIdx(), /*inOut=*/false));
  }

  if (diagnostic) {
    fixIt(*diagnostic);
    return true;
  }

  return false;
}

void MissingRawRepresentableInitFailure::fixIt(
    InFlightDiagnostic &diagnostic) const {
  if (auto *E = getAsExpr(getAnchor())) {
    auto range = E->getSourceRange();
    auto rawReprObjType = RawReprType->getOptionalObjectType();
    auto valueObjType = ExpectedType->getOptionalObjectType();

    if (rawReprObjType && valueObjType) {
      std::string mapCodeFix;

      // Check whether expression has been be wrapped in parens first.
      if (!E->canAppendPostfixExpression()) {
        diagnostic.fixItInsert(range.Start, "(");
        mapCodeFix += ")";
      }

      mapCodeFix += ".map { ";
      mapCodeFix += rawReprObjType->getString();
      mapCodeFix += "(rawValue: $0) }";

      diagnostic.fixItInsertAfter(range.End, mapCodeFix);
    } else if (rawReprObjType) {
      diagnostic
          .fixItInsert(range.Start, rawReprObjType->getString() + "(rawValue: ")
          .fixItInsertAfter(range.End, ")");
    } else if (valueObjType) {
      diagnostic.flush();

      std::string fixItBefore = RawReprType->getString() + "(rawValue: ";
      std::string fixItAfter;

      if (!E->canAppendPostfixExpression(true)) {
        fixItBefore += "(";
        fixItAfter += ")";
      }

      fixItAfter += "!) ?? <#default value#>";

      emitDiagnostic(diag::construct_raw_representable_from_unwrapped_value,
                     RawReprType, valueObjType)
          .highlight(range)
          .fixItInsert(range.Start, fixItBefore)
          .fixItInsertAfter(range.End, fixItAfter);
    } else {
      diagnostic
          .fixItInsert(range.Start, RawReprType->getString() + "(rawValue: ")
          .fixItInsertAfter(range.End, ") ?? <#default value#>");
    }
  }
}

void MissingRawValueFailure::fixIt(InFlightDiagnostic &diagnostic) const {
  auto *E = getAsExpr(getAnchor());
  if (!E)
    return;

  std::string fix;

  auto range = E->getSourceRange();
  if (!E->canAppendPostfixExpression()) {
    diagnostic.fixItInsert(range.Start, "(");
    fix += ")";
  }

  // If raw representable is an optional we need to map its raw value out
  // out first and then, if destination is not optional, allow to specify
  // default value.
  if (RawReprType->getOptionalObjectType()) {
    fix += "?.rawValue";

    if (!ExpectedType->getOptionalObjectType())
      fix += " ?? <#default value#>";
  } else {
    fix += ".rawValue";
  }

  diagnostic.fixItInsertAfter(range.End, fix);
}

bool MissingOptionalUnwrapKeyPathFailure::diagnoseAsError() {
  emitDiagnostic(diag::optional_not_unwrapped, getFromType(),
                 getFromType()->lookThroughSingleOptionalType());
  
  emitDiagnostic(diag::optional_keypath_application_base)
      .fixItInsertAfter(getLoc(), "?");
  emitDiagnostic(diag::unwrap_with_force_value)
      .fixItInsertAfter(getLoc(), "!");
  return true;
}

SourceLoc MissingOptionalUnwrapKeyPathFailure::getLoc() const {
  auto *SE = castToExpr<SubscriptExpr>(getAnchor());
  return SE->getBase()->getEndLoc();
}

bool TrailingClosureRequiresExplicitLabel::diagnoseAsError() {
  auto argInfo = *getFunctionArgApplyInfo(getLocator());

  {
    auto diagnostic = emitDiagnostic(
        diag::unlabeled_trailing_closure_deprecated, argInfo.getParamLabel());
    fixIt(diagnostic, argInfo);
  }

  if (auto *callee = argInfo.getCallee()) {
    emitDiagnosticAt(callee, diag::decl_declared_here, callee->getName());
  }

  return true;
}

void TrailingClosureRequiresExplicitLabel::fixIt(
    InFlightDiagnostic &diagnostic, const FunctionArgApplyInfo &info) const {
  auto &ctx = getASTContext();

  // Dig out source locations.
  SourceLoc existingRParenLoc;
  SourceLoc leadingCommaLoc;

  auto anchor = getRawAnchor();
  auto *arg = info.getArgListExpr();
  Expr *fn = nullptr;

  if (auto *applyExpr = getAsExpr<ApplyExpr>(anchor)) {
    fn = applyExpr->getFn();
  } else {
    // Covers subscripts, unresolved members etc.
    fn = getAsExpr(anchor);
  }

  if (!fn)
    return;

  auto *trailingClosure = info.getArgExpr();

  if (auto tupleExpr = dyn_cast<TupleExpr>(arg)) {
    existingRParenLoc = tupleExpr->getRParenLoc();
    assert(tupleExpr->getNumElements() >= 2 && "Should be a ParenExpr?");
    leadingCommaLoc = Lexer::getLocForEndOfToken(
        ctx.SourceMgr,
        tupleExpr->getElements()[tupleExpr->getNumElements() - 2]->getEndLoc());
  } else {
    auto parenExpr = cast<ParenExpr>(arg);
    existingRParenLoc = parenExpr->getRParenLoc();
  }

  // Figure out the text to be inserted before the trailing closure.
  SmallString<16> insertionText;
  SourceLoc insertionLoc;
  if (leadingCommaLoc.isValid()) {
    insertionText += ", ";
    assert(existingRParenLoc.isValid());
    insertionLoc = leadingCommaLoc;
  } else if (existingRParenLoc.isInvalid()) {
    insertionText += "(";
    insertionLoc = Lexer::getLocForEndOfToken(ctx.SourceMgr, fn->getEndLoc());
  } else {
    insertionLoc = existingRParenLoc;
  }

  // Add the label, if there is one.
  auto paramName = info.getParamLabel();
  if (!paramName.empty()) {
    insertionText += paramName.str();
    insertionText += ": ";
  }

  // If there is an existing right parentheses/brace, remove it while we
  // insert the new text.
  if (existingRParenLoc.isValid()) {
    SourceLoc afterExistingRParenLoc =
        Lexer::getLocForEndOfToken(ctx.SourceMgr, existingRParenLoc);
    diagnostic.fixItReplaceChars(insertionLoc, afterExistingRParenLoc,
                                 insertionText);
  } else {
    // Insert the appropriate prefix.
    diagnostic.fixItInsert(insertionLoc, insertionText);
  }

  // Insert a right parenthesis/brace after the closing '}' of the trailing
  // closure;
  SourceLoc newRParenLoc =
      Lexer::getLocForEndOfToken(ctx.SourceMgr, trailingClosure->getEndLoc());
  diagnostic.fixItInsert(newRParenLoc,
                         isExpr<SubscriptExpr>(anchor) ? "]" : ")");
}

bool InvalidEmptyKeyPathFailure::diagnoseAsError() {
  auto *KPE = getAsExpr<KeyPathExpr>(getAnchor());
  assert(KPE && KPE->hasSingleInvalidComponent() &&
         "Expected a malformed key path expression");

  // If we have a string interpolation represented as key path expressions
  // e.g. \(x), \(x, a: 1). Let's skip it because this would be already
  // diagnosed and it is not the case for an extra empty key path diagnostic.
  auto *root = KPE->getParsedRoot();
  if (root && (isa<ParenExpr>(root) || isa<TupleExpr>(root)))
    return true;

  emitDiagnostic(diag::expr_swift_keypath_empty);
  return true;
}
