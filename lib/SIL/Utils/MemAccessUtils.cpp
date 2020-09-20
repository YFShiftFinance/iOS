//===--- MemAccessUtils.cpp - Utilities for SIL memory access. ------------===//
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

#define DEBUG_TYPE "sil-access-utils"

#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILGlobalVariable.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILUndef.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                            MARK: General Helpers
//===----------------------------------------------------------------------===//

// TODO: When the optimizer stops stripping begin_access markers, then we should
// be able to assert that the result is a BeginAccessInst and the default case
// is unreachable.
SILValue swift::getAddressAccess(SILValue v) {
  while (true) {
    assert(v->getType().isAddress());
    auto projection = AccessProjection(v);
    if (!projection)
      return v;

    v = projection.baseAddress();
  }
}

SILValue swift::getAccessedAddress(SILValue v) {
  while (true) {
    SILValue v2 = stripAccessMarkers(getAddressAccess(v));
    if (v2 == v)
      return v;
    v = v2;
  }
}

bool swift::isLetAddress(SILValue accessedAddress) {
  assert(accessedAddress == getAccessedAddress(accessedAddress)
         && "caller must find the address root");
  // Is this an address of a "let" class member?
  if (auto *rea = dyn_cast<RefElementAddrInst>(accessedAddress))
    return rea->getField()->isLet();

  // Is this an address of a global "let"?
  if (auto *gai = dyn_cast<GlobalAddrInst>(accessedAddress)) {
    auto *globalDecl = gai->getReferencedGlobal()->getDecl();
    return globalDecl && globalDecl->isLet();
  }
  return false;
}

//===----------------------------------------------------------------------===//
//                            MARK: AccessedStorage
//===----------------------------------------------------------------------===//

AccessedStorage::AccessedStorage(SILValue base, Kind kind) {
  assert(base && "invalid storage base");
  initKind(kind);

  switch (kind) {
  case Box:
    assert(isa<AllocBoxInst>(base));
    value = base;
    break;
  case Stack:
    assert(isa<AllocStackInst>(base));
    value = base;
    break;
  case Nested:
    assert(isa<BeginAccessInst>(base));
    value = base;
    break;
  case Yield:
    assert(isa<BeginApplyResult>(base));
    value = base;
    break;
  case Unidentified:
    value = base;
    break;
  case Argument:
    value = base;
    setElementIndex(cast<SILFunctionArgument>(base)->getIndex());
    break;
  case Global:
    if (auto *GAI = dyn_cast<GlobalAddrInst>(base))
      global = GAI->getReferencedGlobal();
    else {
      FullApplySite apply(cast<ApplyInst>(base));
      auto *funcRef = apply.getReferencedFunctionOrNull();
      assert(funcRef);
      global = getVariableOfGlobalInit(funcRef);
      assert(global);
      // Require a decl for all formally accessed globals defined in this
      // module. (Access of globals defined elsewhere has Unidentified storage).
      // AccessEnforcementWMO requires this.
      assert(global->getDecl());
    }
    break;
  case Class: {
    // Do a best-effort to find the identity of the object being projected
    // from. It is OK to be unsound here (i.e. miss when two ref_element_addrs
    // actually refer the same address) because these addresses will be
    // dynamically checked, and static analysis will be sufficiently
    // conservative given that classes are not "uniquely identified".
    auto *REA = cast<RefElementAddrInst>(base);
    value = stripBorrow(REA->getOperand());
    setElementIndex(REA->getFieldNo());
    break;
  }
  case Tail: {
    auto *RTA = cast<RefTailAddrInst>(base);
    value = stripBorrow(RTA->getOperand());
    break;
  }
  }
}

// Return true if the given access is on a 'let' lvalue.
bool AccessedStorage::isLetAccess(SILFunction *F) const {
  if (auto *decl = dyn_cast_or_null<VarDecl>(getDecl()))
    return decl->isLet();

  // It's unclear whether a global will ever be missing it's varDecl, but
  // technically we only preserve it for debug info. So if we don't have a decl,
  // check the flag on SILGlobalVariable, which is guaranteed valid,
  if (getKind() == AccessedStorage::Global)
    return getGlobal()->isLet();

  return false;
}

const ValueDecl *AccessedStorage::getDecl() const {
  switch (getKind()) {
  case Box:
    return cast<AllocBoxInst>(value)->getLoc().getAsASTNode<VarDecl>();

  case Stack:
    return cast<AllocStackInst>(value)->getDecl();

  case Global:
    return global->getDecl();

  case Class: {
    auto *decl = getObject()->getType().getNominalOrBoundGenericNominal();
    return decl->getStoredProperties()[getPropertyIndex()];
  }
  case Tail:
    return nullptr;

  case Argument:
    return getArgument()->getDecl();

  case Yield:
    return nullptr;

  case Nested:
    return nullptr;

  case Unidentified:
    return nullptr;
  }
  llvm_unreachable("unhandled kind");
}

const char *AccessedStorage::getKindName(AccessedStorage::Kind k) {
  switch (k) {
  case Box:
    return "Box";
  case Stack:
    return "Stack";
  case Nested:
    return "Nested";
  case Unidentified:
    return "Unidentified";
  case Argument:
    return "Argument";
  case Yield:
    return "Yield";
  case Global:
    return "Global";
  case Class:
    return "Class";
  case Tail:
    return "Tail";
  }
  llvm_unreachable("unhandled kind");
}

void AccessedStorage::print(raw_ostream &os) const {
  if (!*this) {
    os << "INVALID\n";
    return;
  }
  os << getKindName(getKind()) << " ";
  switch (getKind()) {
  case Box:
  case Stack:
  case Nested:
  case Yield:
  case Unidentified:
    os << value;
    break;
  case Argument:
    os << value;
    break;
  case Global:
    os << *global;
    break;
  case Class:
    os << getObject();
    os << "  Field: ";
    getDecl()->print(os);
    os << " Index: " << getPropertyIndex() << "\n";
    break;
  case Tail:
    os << getObject();
    os << "  Tail\n";
  }
}

void AccessedStorage::dump() const { print(llvm::dbgs()); }

namespace {
// Find common AccessedStorage that leads to all arguments of a given
// pointer phi use. Return an invalid SILValue on failure.
//
// Also guarantees that all phi inputs follow the same access path. If any phi
// inputs have different access path components, then the phi is considered an
// invalid access. This is ok because path components always have an address
// type, and we are phasing out all address-type phis. Pointer-phis will
// continue to be allowed but they cannot affect the access path.
template <typename StorageVisitor>
class FindPhiStorageVisitor
    : public AccessUseDefChainVisitor<FindPhiStorageVisitor<StorageVisitor>> {
  StorageVisitor &storageVisitor;
  Optional<SILValue> commonDefinition;
  SmallVector<SILValue, 8> pointerWorklist;
  SmallPtrSet<SILPhiArgument *, 4> nestedPhis;

public:
  FindPhiStorageVisitor(StorageVisitor &storageVisitor)
      : storageVisitor(storageVisitor) {}

  // Main entry point.
  void findPhiStorage(SILPhiArgument *phiArg) {
    // Visiting a phi will call storageVisitor to set the storage result
    // whenever it finds a base.
    visitPhi(phiArg);
    while (!pointerWorklist.empty()) {
      this->visit(pointerWorklist.pop_back_val());
    }
    // If a common path component was found, recursively look for the storage.
    if (commonDefinition) {
      if (commonDefinition.getValue()) {
        auto storage = storageVisitor.findStorage(commonDefinition.getValue());
        (void)storage; // The same storageVisitor called us. It has already
                       // recorded the storage that it found.
      } else {
        // If divergent paths were found, invalidate any previously discovered
        // storage.
        storageVisitor.setStorage(AccessedStorage());
      }
    }
  }

  // Visitor helper.
  void setDefinition(SILValue def) {
    if (!commonDefinition) {
      commonDefinition = def;
      return;
    }
    if (commonDefinition.getValue() != def)
      commonDefinition = SILValue();
  }

  // MARK: Visitor implementation.

  void checkResult(SILValue result) {
    assert(!result && "must override any visitor that returns a result");
  }

  // Recursively call the original storageVisitor for each base. We can't simply
  // look for a common definition on all phi inputs, because the base may be
  // cloned on each path. For example, two global_addr instructions may refer to
  // the same global storage. Those global_addr instructions may each be
  // converted to a RawPointer before being passed into the non-address phi.
  void visitBase(SILValue base, AccessedStorage::Kind kind) {
    checkResult(storageVisitor.visitBase(base, kind));
  }

  void visitNonAccess(SILValue value) {
    checkResult(storageVisitor.visitNonAccess(value));
  }

  void visitNestedAccess(BeginAccessInst *access) {
    checkResult(storageVisitor.visitNestedAccess(access));
  }

  void visitPhi(SILPhiArgument *phiArg) {
    if (nestedPhis.insert(phiArg).second)
      phiArg->getIncomingPhiValues(pointerWorklist);
  }

  void visitCast(SingleValueInstruction *projectedAddr, Operand *parentAddr) {
    // Allow conversions to/from pointers and addresses on disjoint phi paths.
    this->pointerWorklist.push_back(parentAddr->get());
  }

  void visitPathComponent(SingleValueInstruction *projectedAddr,
                          Operand *parentAddr) {
    // Path components are not expected to occur on disjoint phi paths. Stop
    // searching at this projection.
    setDefinition(projectedAddr);
  }
};
} // namespace

namespace {
// Implementation of AccessUseDefChainVisitor that looks for a single common
// AccessedStorage object for all projection paths.
template <typename Impl>
class FindAccessedStorageVisitorBase
    : public AccessUseDefChainVisitor<Impl, SILValue> {
protected:
  Optional<AccessedStorage> storage;
  SmallPtrSet<SILPhiArgument *, 4> visitedPhis;

public:
  // Main entry point. May be called reentrantly by the phi visitor.
  AccessedStorage findStorage(SILValue sourceAddr) {
    SILValue nextAddr = this->visit(sourceAddr);
    while (nextAddr) {
      assert(nextAddr->getType().isAddress()
             || isa<SILBoxType>(nextAddr->getType().getASTType())
             || isa<BuiltinRawPointerType>(nextAddr->getType().getASTType()));
      nextAddr = this->visit(nextAddr);
    }
    return storage.getValueOr(AccessedStorage());
  }

  void setStorage(AccessedStorage foundStorage) {
    if (!storage) {
      storage = foundStorage;
    } else {
      // `storage` may still be invalid. If both `storage` and `foundStorage`
      // are invalid, this check passes, but we return an invalid storage
      // below.
      if (!storage->hasIdenticalBase(foundStorage))
        storage = AccessedStorage();
    }
  }

  // MARK: visitor implementation.

  SILValue visitBase(SILValue base, AccessedStorage::Kind kind) {
    setStorage(AccessedStorage(base, kind));
    return SILValue();
  }

  SILValue visitNonAccess(SILValue value) {
    setStorage(AccessedStorage());
    return SILValue();
  }

  SILValue visitPhi(SILPhiArgument *phiArg) {
    // Cycles involving phis are only handled within FindPhiStorageVisitor.
    // Path components are not allowed in phi cycles.
    if (visitedPhis.insert(phiArg).second) {
      FindPhiStorageVisitor<Impl>(this->asImpl()).findPhiStorage(phiArg);
      return SILValue();
    }
    // Cannot treat unresolved phis as "unidentified" because they may alias
    // with global or class access.
    return visitNonAccess(phiArg);
  }

  SILValue visitCast(SingleValueInstruction *projectedAddr,
                     Operand *parentAddr) {
    return parentAddr->get();
  }

  SILValue visitPathComponent(SingleValueInstruction *projectedAddr,
                              Operand *parentAddr) {
    return parentAddr->get();
  }
};

struct FindAccessedStorageVisitor
    : public FindAccessedStorageVisitorBase<FindAccessedStorageVisitor> {

  SILValue visitNestedAccess(BeginAccessInst *access) {
    return access->getSource();
  }
};

struct IdentifyAccessedStorageVisitor
    : public FindAccessedStorageVisitorBase<IdentifyAccessedStorageVisitor> {};

} // namespace

AccessedStorage swift::findAccessedStorage(SILValue sourceAddr) {
  return FindAccessedStorageVisitor().findStorage(sourceAddr);
}

AccessedStorage swift::identifyAccessedStorageImpl(SILValue sourceAddr) {
  return IdentifyAccessedStorageVisitor().findStorage(sourceAddr);
}

//===----------------------------------------------------------------------===//
//                               MARK: Helper API
//===----------------------------------------------------------------------===//

static bool isScratchBuffer(SILValue value) {
  // Special case unsafe value buffer access.
  return value->getType().is<BuiltinUnsafeValueBufferType>();
}

bool swift::memInstMustInitialize(Operand *memOper) {
  SILValue address = memOper->get();
  SILInstruction *memInst = memOper->getUser();

  switch (memInst->getKind()) {
  default:
    return false;

  case SILInstructionKind::CopyAddrInst: {
    auto *CAI = cast<CopyAddrInst>(memInst);
    return CAI->getDest() == address && CAI->isInitializationOfDest();
  }
  case SILInstructionKind::InitExistentialAddrInst:
  case SILInstructionKind::InitEnumDataAddrInst:
  case SILInstructionKind::InjectEnumAddrInst:
    return true;

  case SILInstructionKind::StoreInst:
    return cast<StoreInst>(memInst)->getOwnershipQualifier()
           == StoreOwnershipQualifier::Init;

#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case SILInstructionKind::Store##Name##Inst: \
    return cast<Store##Name##Inst>(memInst)->isInitializationOfDest();
#include "swift/AST/ReferenceStorage.def"
  }
}

bool swift::isSingleInitAllocStack(AllocStackInst *asi,
                                   SmallVectorImpl<Operand *> &destroyingUses) {
  // For now, we just look through projections and rely on memInstMustInitialize
  // to classify all other uses as init or not.
  SmallVector<Operand *, 32> worklist(asi->getUses());
  bool foundInit = false;

  while (!worklist.empty()) {
    auto *use = worklist.pop_back_val();
    auto *user = use->getUser();

    if (Projection::isAddressProjection(user)
        || isa<OpenExistentialAddrInst>(user)) {
      // Look through address projections.
      for (SILValue r : user->getResults()) {
        llvm::copy(r->getUses(), std::back_inserter(worklist));
      }
      continue;
    }

    if (auto *li = dyn_cast<LoadInst>(user)) {
      // If we are not taking,
      if (li->getOwnershipQualifier() != LoadOwnershipQualifier::Take) {
        continue;
      }
      // Treat load [take] as a write.
      return false;
    }

    switch (user->getKind()) {
    default:
      break;
    case SILInstructionKind::DestroyAddrInst:
      destroyingUses.push_back(use);
      continue;
    case SILInstructionKind::DeallocStackInst:
    case SILInstructionKind::LoadBorrowInst:
    case SILInstructionKind::DebugValueAddrInst:
      continue;
    }

    // See if we have an initializer and that such initializer is in the same
    // block.
    if (memInstMustInitialize(use)) {
      if (user->getParent() != asi->getParent() || foundInit) {
        return false;
      }

      foundInit = true;
      continue;
    }

    // Otherwise, if we have found something not in our allowlist, return false.
    return false;
  }

  // We did not find any users that we did not understand. So we can
  // conservatively return true here.
  return true;
}

/// Return true if the given address value is produced by a special address
/// producer that is only used for local initialization, not formal access.
bool swift::isAddressForLocalInitOnly(SILValue sourceAddr) {
  switch (sourceAddr->getKind()) {
  default:
    return false;

  // Value to address conversions: the operand is the non-address source
  // value. These allow local mutation of the value but should never be used
  // for formal access of an lvalue.
  case ValueKind::OpenExistentialBoxInst:
  case ValueKind::ProjectExistentialBoxInst:
    return true;

  // Self-evident local initialization.
  case ValueKind::InitEnumDataAddrInst:
  case ValueKind::InitExistentialAddrInst:
  case ValueKind::AllocExistentialBoxInst:
  case ValueKind::AllocValueBufferInst:
  case ValueKind::ProjectValueBufferInst:
    return true;
  }
}

// Return true if the given apply invokes a global addressor defined in another
// module.
bool swift::isExternalGlobalAddressor(ApplyInst *AI) {
  FullApplySite apply(AI);
  auto *funcRef = apply.getReferencedFunctionOrNull();
  if (!funcRef)
    return false;

  return funcRef->isGlobalInit() && funcRef->isExternalDeclaration();
}

// Return true if the given StructExtractInst extracts the RawPointer from
// Unsafe[Mutable]Pointer.
bool swift::isUnsafePointerExtraction(StructExtractInst *SEI) {
  assert(isa<BuiltinRawPointerType>(SEI->getType().getASTType()));
  auto &C = SEI->getModule().getASTContext();
  auto *decl = SEI->getStructDecl();
  return decl == C.getUnsafeMutablePointerDecl()
         || decl == C.getUnsafePointerDecl();
}

// Given a block argument address base, check if it is actually a box projected
// from a switch_enum. This is a valid pattern at any SIL stage resulting in a
// block-type phi. In later SIL stages, the optimizer may form address-type
// phis, causing this assert if called on those cases.
void swift::checkSwitchEnumBlockArg(SILPhiArgument *arg) {
  assert(!arg->getType().isAddress());
  SILBasicBlock *Pred = arg->getParent()->getSinglePredecessorBlock();
  if (!Pred || !isa<SwitchEnumInst>(Pred->getTerminator())) {
    arg->dump();
    llvm_unreachable("unexpected box source.");
  }
}

bool swift::isPossibleFormalAccessBase(const AccessedStorage &storage,
                                       SILFunction *F) {
  switch (storage.getKind()) {
  case AccessedStorage::Nested:
    assert(false && "don't pass nested storage to this helper");
    return false;

  case AccessedStorage::Box:
  case AccessedStorage::Stack:
    if (isScratchBuffer(storage.getValue()))
      return false;
    break;
  case AccessedStorage::Global:
    break;
  case AccessedStorage::Class:
    break;
  case AccessedStorage::Tail:
    return false;

  case AccessedStorage::Yield:
    // Yields are accessed by the caller.
    return false;
  case AccessedStorage::Argument:
    // Function arguments are accessed by the caller.
    return false;

  case AccessedStorage::Unidentified:
    if (isAddressForLocalInitOnly(storage.getValue()))
      return false;

    if (isa<SILPhiArgument>(storage.getValue())) {
      checkSwitchEnumBlockArg(cast<SILPhiArgument>(storage.getValue()));
      return false;
    }
    // Pointer-to-address exclusivity cannot be enforced. `baseAddress` may be
    // pointing anywhere within an object.
    if (isa<PointerToAddressInst>(storage.getValue()))
      return false;

    if (isa<SILUndef>(storage.getValue()))
      return false;

    if (isScratchBuffer(storage.getValue()))
      return false;

    break;
  }
  // Additional checks that apply to anything that may fall through.

  // Immutable values are only accessed for initialization.
  if (storage.isLetAccess(F))
    return false;

  return true;
}

SILBasicBlock::iterator swift::removeBeginAccess(BeginAccessInst *beginAccess) {
  while (!beginAccess->use_empty()) {
    Operand *op = *beginAccess->use_begin();

    // Delete any associated end_access instructions.
    if (auto endAccess = dyn_cast<EndAccessInst>(op->getUser())) {
      endAccess->eraseFromParent();

      // Forward all other uses to the original address.
    } else {
      op->set(beginAccess->getSource());
    }
  }
  return beginAccess->getParent()->erase(beginAccess);
}

//===----------------------------------------------------------------------===//
//                            Verification
//===----------------------------------------------------------------------===//

/// Helper for visitApplyAccesses that visits address-type call arguments,
/// including arguments to @noescape functions that are passed as closures to
/// the current call.
static void visitApplyAccesses(ApplySite apply,
                               llvm::function_ref<void(Operand *)> visitor) {
  for (Operand &oper : apply.getArgumentOperands()) {
    // Consider any address-type operand an access. Whether it is read or modify
    // depends on the argument convention.
    if (oper.get()->getType().isAddress()) {
      visitor(&oper);
      continue;
    }
    auto fnType = oper.get()->getType().getAs<SILFunctionType>();
    if (!fnType || !fnType->isNoEscape())
      continue;

    // When @noescape function closures are passed as arguments, their
    // arguments are considered accessed at the call site.
    TinyPtrVector<PartialApplyInst *> partialApplies;
    findClosuresForFunctionValue(oper.get(), partialApplies);
    // Recursively visit @noescape function closure arguments.
    for (auto *PAI : partialApplies)
      visitApplyAccesses(PAI, visitor);
  }
}

static void visitBuiltinAddress(BuiltinInst *builtin,
                                llvm::function_ref<void(Operand *)> visitor) {
  if (auto kind = builtin->getBuiltinKind()) {
    switch (kind.getValue()) {
    default:
      builtin->dump();
      llvm_unreachable("unexpected builtin memory access.");

      // WillThrow exists for the debugger, does nothing.
    case BuiltinValueKind::WillThrow:
      return;

      // Buitins that affect memory but can't be formal accesses.
    case BuiltinValueKind::UnexpectedError:
    case BuiltinValueKind::ErrorInMain:
    case BuiltinValueKind::IsOptionalType:
    case BuiltinValueKind::AllocRaw:
    case BuiltinValueKind::DeallocRaw:
    case BuiltinValueKind::Fence:
    case BuiltinValueKind::StaticReport:
    case BuiltinValueKind::Once:
    case BuiltinValueKind::OnceWithContext:
    case BuiltinValueKind::Unreachable:
    case BuiltinValueKind::CondUnreachable:
    case BuiltinValueKind::DestroyArray:
    case BuiltinValueKind::COWBufferForReading:
    case BuiltinValueKind::UnsafeGuaranteed:
    case BuiltinValueKind::UnsafeGuaranteedEnd:
    case BuiltinValueKind::Swift3ImplicitObjCEntrypoint:
    case BuiltinValueKind::TSanInoutAccess:
      return;

      // General memory access to a pointer in first operand position.
    case BuiltinValueKind::CmpXChg:
    case BuiltinValueKind::AtomicLoad:
    case BuiltinValueKind::AtomicStore:
    case BuiltinValueKind::AtomicRMW:
      // Currently ignored because the access is on a RawPointer, not a
      // SIL address.
      // visitor(&builtin->getAllOperands()[0]);
      return;

      // Arrays: (T.Type, Builtin.RawPointer, Builtin.RawPointer,
      // Builtin.Word)
    case BuiltinValueKind::CopyArray:
    case BuiltinValueKind::TakeArrayNoAlias:
    case BuiltinValueKind::TakeArrayFrontToBack:
    case BuiltinValueKind::TakeArrayBackToFront:
    case BuiltinValueKind::AssignCopyArrayNoAlias:
    case BuiltinValueKind::AssignCopyArrayFrontToBack:
    case BuiltinValueKind::AssignCopyArrayBackToFront:
    case BuiltinValueKind::AssignTakeArray:
      // Currently ignored because the access is on a RawPointer.
      // visitor(&builtin->getAllOperands()[1]);
      // visitor(&builtin->getAllOperands()[2]);
      return;
    }
  }
  if (auto ID = builtin->getIntrinsicID()) {
    switch (ID.getValue()) {
      // Exhaustively verifying all LLVM instrinsics that access memory is
      // impractical. Instead, we call out the few common cases and return in
      // the default case.
    default:
      return;
    case llvm::Intrinsic::memcpy:
    case llvm::Intrinsic::memmove:
      // Currently ignored because the access is on a RawPointer.
      // visitor(&builtin->getAllOperands()[0]);
      // visitor(&builtin->getAllOperands()[1]);
      return;
    case llvm::Intrinsic::memset:
      // Currently ignored because the access is on a RawPointer.
      // visitor(&builtin->getAllOperands()[0]);
      return;
    }
  }
  llvm_unreachable("Must be either a builtin or intrinsic.");
}

void swift::visitAccessedAddress(SILInstruction *I,
                                 llvm::function_ref<void(Operand *)> visitor) {
  assert(I->mayReadOrWriteMemory());

  // Reference counting instructions do not access user visible memory.
  if (isa<RefCountingInst>(I))
    return;

  if (isa<DeallocationInst>(I))
    return;

  if (auto apply = FullApplySite::isa(I)) {
    visitApplyAccesses(apply, visitor);
    return;
  }

  if (auto builtin = dyn_cast<BuiltinInst>(I)) {
    visitBuiltinAddress(builtin, visitor);
    return;
  }

  switch (I->getKind()) {
  default:
    I->dump();
    llvm_unreachable("unexpected memory access.");

  case SILInstructionKind::AssignInst:
  case SILInstructionKind::AssignByWrapperInst:
    visitor(&I->getAllOperands()[AssignInst::Dest]);
    return;

  case SILInstructionKind::CheckedCastAddrBranchInst:
    visitor(&I->getAllOperands()[CheckedCastAddrBranchInst::Src]);
    visitor(&I->getAllOperands()[CheckedCastAddrBranchInst::Dest]);
    return;

  case SILInstructionKind::CopyAddrInst:
    visitor(&I->getAllOperands()[CopyAddrInst::Src]);
    visitor(&I->getAllOperands()[CopyAddrInst::Dest]);
    return;

#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case SILInstructionKind::Store##Name##Inst:
#include "swift/AST/ReferenceStorage.def"
  case SILInstructionKind::StoreInst:
  case SILInstructionKind::StoreBorrowInst:
    visitor(&I->getAllOperands()[StoreInst::Dest]);
    return;

  case SILInstructionKind::SelectEnumAddrInst:
    visitor(&I->getAllOperands()[0]);
    return;

#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case SILInstructionKind::Load##Name##Inst:
#include "swift/AST/ReferenceStorage.def"
  case SILInstructionKind::InitExistentialAddrInst:
  case SILInstructionKind::InjectEnumAddrInst:
  case SILInstructionKind::LoadInst:
  case SILInstructionKind::LoadBorrowInst:
  case SILInstructionKind::OpenExistentialAddrInst:
  case SILInstructionKind::SwitchEnumAddrInst:
  case SILInstructionKind::UncheckedTakeEnumDataAddrInst:
  case SILInstructionKind::UnconditionalCheckedCastInst: {
    // Assuming all the above have only a single address operand.
    assert(I->getNumOperands() - I->getNumTypeDependentOperands() == 1);
    Operand *singleOperand = &I->getAllOperands()[0];
    // Check the operand type because UnconditionalCheckedCastInst may operate
    // on a non-address.
    if (singleOperand->get()->getType().isAddress())
      visitor(singleOperand);
    return;
  }
  // Non-access cases: these are marked with memory side effects, but, by
  // themselves, do not access formal memory.
#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  case SILInstructionKind::StrongCopy##Name##ValueInst:
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)            \
  case SILInstructionKind::StrongCopy##Name##ValueInst:
#include "swift/AST/ReferenceStorage.def"
  case SILInstructionKind::AbortApplyInst:
  case SILInstructionKind::AllocBoxInst:
  case SILInstructionKind::AllocExistentialBoxInst:
  case SILInstructionKind::AllocGlobalInst:
  case SILInstructionKind::BeginAccessInst:
  case SILInstructionKind::BeginApplyInst:
  case SILInstructionKind::BeginBorrowInst:
  case SILInstructionKind::BeginUnpairedAccessInst:
  case SILInstructionKind::BindMemoryInst:
  case SILInstructionKind::CheckedCastValueBranchInst:
  case SILInstructionKind::CondFailInst:
  case SILInstructionKind::CopyBlockInst:
  case SILInstructionKind::CopyBlockWithoutEscapingInst:
  case SILInstructionKind::CopyValueInst:
  case SILInstructionKind::DeinitExistentialAddrInst:
  case SILInstructionKind::DeinitExistentialValueInst:
  case SILInstructionKind::DestroyAddrInst:
  case SILInstructionKind::DestroyValueInst:
  case SILInstructionKind::EndAccessInst:
  case SILInstructionKind::EndApplyInst:
  case SILInstructionKind::EndBorrowInst:
  case SILInstructionKind::EndUnpairedAccessInst:
  case SILInstructionKind::EndLifetimeInst:
  case SILInstructionKind::ExistentialMetatypeInst:
  case SILInstructionKind::FixLifetimeInst:
  case SILInstructionKind::InitExistentialValueInst:
  case SILInstructionKind::IsUniqueInst:
  case SILInstructionKind::IsEscapingClosureInst:
  case SILInstructionKind::KeyPathInst:
  case SILInstructionKind::OpenExistentialBoxInst:
  case SILInstructionKind::OpenExistentialBoxValueInst:
  case SILInstructionKind::OpenExistentialValueInst:
  case SILInstructionKind::PartialApplyInst:
  case SILInstructionKind::ProjectValueBufferInst:
  case SILInstructionKind::YieldInst:
  case SILInstructionKind::UnwindInst:
  case SILInstructionKind::UncheckedOwnershipConversionInst:
  case SILInstructionKind::UncheckedRefCastAddrInst:
  case SILInstructionKind::UnconditionalCheckedCastAddrInst:
  case SILInstructionKind::UnconditionalCheckedCastValueInst:
  case SILInstructionKind::ValueMetatypeInst:
    return;
  }
}
