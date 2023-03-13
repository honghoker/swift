//===--- SILGenPack.cpp - Helper routines for lowering variadic packs -----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "Initialization.h"
#include "Scope.h"
#include "SILGenFunction.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/GenericEnvironment.h"

using namespace swift;
using namespace Lowering;

namespace {
/// Cleanup to deallocate a now-uninitialized pack.
class DeallocPackCleanup : public Cleanup {
  SILValue Addr;
public:
  DeallocPackCleanup(SILValue addr) : Addr(addr) {}

  void emit(SILGenFunction &SGF, CleanupLocation l,
            ForUnwind_t forUnwind) override {
    SGF.B.createDeallocPack(l, Addr);
  }

  void dump(SILGenFunction &) const override {
#ifndef NDEBUG
    llvm::errs() << "DeallocPackCleanup\n"
                 << "State:" << getState() << "\n"
                 << "Addr:" << Addr << "\n";
#endif
  }
};

/// Cleanup to destroy all the values in a pack.
class DestroyPackCleanup : public Cleanup {
  SILValue Addr;
  CanPackType FormalPackType;
public:
  DestroyPackCleanup(SILValue addr, CanPackType formalPackType)
    : Addr(addr), FormalPackType(formalPackType) {}

  void emit(SILGenFunction &SGF, CleanupLocation l,
            ForUnwind_t forUnwind) override {
    SGF.emitDestroyPack(l, Addr, FormalPackType);
  }

  void dump(SILGenFunction &) const override {
#ifndef NDEBUG
    llvm::errs() << "DestroyPackCleanup\n"
                 << "State:" << getState() << "\n"
                 << "Addr:" << Addr << "\n"
                 << "FormalPackType:" << FormalPackType << "\n";
#endif
  }
};

/// Cleanup to destroy the preceding values in a pack-expansion
/// component of a pack.
class PartialDestroyPackCleanup : public Cleanup {
  SILValue Addr;
  unsigned PackComponentIndex;
  SILValue LimitWithinComponent;
  CanPackType FormalPackType;
public:
  PartialDestroyPackCleanup(SILValue addr,
                            CanPackType formalPackType,
                            unsigned packComponentIndex,
                            SILValue limitWithinComponent)
    : Addr(addr), PackComponentIndex(packComponentIndex),
      LimitWithinComponent(limitWithinComponent),
      FormalPackType(formalPackType) {}

  void emit(SILGenFunction &SGF, CleanupLocation l,
            ForUnwind_t forUnwind) override {
    SGF.emitPartialDestroyPack(l, Addr, FormalPackType, PackComponentIndex,
                               LimitWithinComponent);
  }

  void dump(SILGenFunction &) const override {
#ifndef NDEBUG
    llvm::errs() << "PartialDestroyPackCleanup\n"
                 << "State:" << getState() << "\n"
                 << "Addr:" << Addr << "\n"
                 << "FormalPackType:" << FormalPackType << "\n"
                 << "ComponentIndex:" << PackComponentIndex << "\n"
                 << "LimitWithinComponent:" << LimitWithinComponent << "\n";
#endif
  }
};

/// Cleanup to destroy the preceding values in a pack-expansion
/// component of a tuple.
class PartialDestroyTupleCleanup : public Cleanup {
  SILValue Addr;
  unsigned ComponentIndex;
  SILValue LimitWithinComponent;
  CanPackType InducedPackType;
public:
  PartialDestroyTupleCleanup(SILValue tupleAddr,
                             CanPackType inducedPackType,
                             unsigned componentIndex,
                             SILValue limitWithinComponent)
    : Addr(tupleAddr), ComponentIndex(componentIndex),
      LimitWithinComponent(limitWithinComponent),
      InducedPackType(inducedPackType) {}

  void emit(SILGenFunction &SGF, CleanupLocation l,
            ForUnwind_t forUnwind) override {
    SGF.emitPartialDestroyTuple(l, Addr, InducedPackType, ComponentIndex,
                                LimitWithinComponent);
  }

  void dump(SILGenFunction &) const override {
#ifndef NDEBUG
    llvm::errs() << "PartialDestroyTupleCleanup\n"
                 << "State:" << getState() << "\n"
                 << "Addr:" << Addr << "\n"
                 << "InducedPackType:" << InducedPackType << "\n"
                 << "ComponentIndex:" << ComponentIndex << "\n"
                 << "LimitWithinComponent:" << LimitWithinComponent << "\n";
#endif
  }
};

/// Cleanup to destroy the remaining values in a pack-expansion
/// component of a tuple.
class PartialDestroyRemainingTupleCleanup : public Cleanup {
  SILValue Addr;
  unsigned ComponentIndex;
  SILValue CurrentIndexWithinComponent;
  CanPackType InducedPackType;
public:
  PartialDestroyRemainingTupleCleanup(SILValue tupleAddr,
                                      CanPackType inducedPackType,
                                      unsigned componentIndex,
                                      SILValue currentIndexWithinComponent)
    : Addr(tupleAddr), ComponentIndex(componentIndex),
      CurrentIndexWithinComponent(currentIndexWithinComponent),
      InducedPackType(inducedPackType) {}

  void emit(SILGenFunction &SGF, CleanupLocation l,
            ForUnwind_t forUnwind) override {
    SGF.emitPartialDestroyRemainingTuple(l, Addr, InducedPackType,
                                         ComponentIndex,
                                         CurrentIndexWithinComponent);
  }

  void dump(SILGenFunction &) const override {
#ifndef NDEBUG
    llvm::errs() << "PartialDestroyRemainingTupleCleanup\n"
                 << "State:" << getState() << "\n"
                 << "Addr:" << Addr << "\n"
                 << "InducedPackType:" << InducedPackType << "\n"
                 << "ComponentIndex:" << ComponentIndex << "\n"
                 << "CurrentIndexWithinComponent:"
                 << CurrentIndexWithinComponent << "\n";
#endif
  }
};

/// An ASTWalker to emit tuple values in `MaterializePackExpr` nodes.
///
/// Materialized packs are emitted inside a pack expansion context before
/// entering the dynamic pack loop so that the values are only evaluated
/// once, rather than at each pack element iteration.
struct MaterializePackEmitter : public ASTWalker {
  SILGenFunction &SGF;

  MaterializePackEmitter(SILGenFunction &SGF) : SGF(SGF) {}

  ASTWalker::PreWalkResult<Expr *> walkToExprPre(Expr *expr) override {
    using Action = ASTWalker::Action;

    // Don't walk into nested pack expansions.
    if (isa<PackExpansionExpr>(expr))
      return Action::SkipChildren(expr);

    if (auto *packExpr = dyn_cast<MaterializePackExpr>(expr)) {
      auto *fromExpr = packExpr->getFromExpr();
      assert(fromExpr->getType()->is<TupleType>());

      auto &lowering = SGF.getTypeLowering(fromExpr->getType());
      auto loweredTy = lowering.getLoweredType();
      auto tupleAddr = SGF.emitTemporaryAllocation(fromExpr, loweredTy);
      auto init = SGF.useBufferAsTemporary(tupleAddr, lowering);
      SGF.emitExprInto(fromExpr, init.get());

      // Write the tuple value to a side table in the active pack expansion
      // to be projected later within the dynamic pack loop.
      auto *activeExpansion = SGF.getInnermostPackExpansion();
      activeExpansion->MaterializedPacks[packExpr] = tupleAddr;
    }

    return Action::Continue(expr);
  }
};

} // end anonymous namespace

void
SILGenFunction::prepareToEmitPackExpansionExpr(PackExpansionExpr *E) {
  MaterializePackEmitter tempPackEmission(*this);
  E->getPatternExpr()->walk(tempPackEmission);
}

CleanupHandle SILGenFunction::enterDeallocPackCleanup(SILValue temp) {
  assert(temp->getType().isAddress() &&  "dealloc must have an address type");
  assert(temp->getType().is<SILPackType>());
  Cleanups.pushCleanup<DeallocPackCleanup>(temp);
  return Cleanups.getTopCleanup();
}

CleanupHandle SILGenFunction::enterDestroyPackCleanup(SILValue addr,
                                                   CanPackType formalPackType) {
  Cleanups.pushCleanup<DestroyPackCleanup>(addr, formalPackType);
  return Cleanups.getTopCleanup();
}

CleanupHandle
SILGenFunction::enterPartialDestroyPackCleanup(SILValue addr,
                                               CanPackType formalPackType,
                                               unsigned packComponentIndex,
                                               SILValue limitWithinComponent) {
  Cleanups.pushCleanup<PartialDestroyPackCleanup>(addr, formalPackType,
                                                  packComponentIndex,
                                                  limitWithinComponent);
  return Cleanups.getTopCleanup();
}

CleanupHandle
SILGenFunction::enterPartialDestroyTupleCleanup(SILValue addr,
                                                CanPackType inducedPackType,
                                                unsigned componentIndex,
                                                SILValue limitWithinComponent) {
  Cleanups.pushCleanup<PartialDestroyTupleCleanup>(addr, inducedPackType,
                                                   componentIndex,
                                                   limitWithinComponent);
  return Cleanups.getTopCleanup();
}

CleanupHandle
SILGenFunction::enterPartialDestroyRemainingTupleCleanup(SILValue addr,
                                                CanPackType inducedPackType,
                                                unsigned componentIndex,
                                                SILValue indexWithinComponent) {
  Cleanups.pushCleanup<PartialDestroyRemainingTupleCleanup>(addr,
                                                   inducedPackType,
                                                   componentIndex,
                                                   indexWithinComponent);
  return Cleanups.getTopCleanup();
}

void SILGenFunction::emitDestroyPack(SILLocation loc, SILValue packAddr,
                                     CanPackType formalPackType) {
  auto packTy = packAddr->getType().castTo<SILPackType>();

  // Destroy each of the elements of the pack.
  for (auto componentIndex : indices(packTy->getElementTypes())) {
    auto eltTy = packTy->getSILElementType(componentIndex);

    // We can skip this if the whole thing is trivial.
    auto &eltTL = getTypeLowering(eltTy);
    if (eltTL.isTrivial()) continue;

    // If it's an expansion component, emit a "partial"-destroy loop.
    if (auto expansion = eltTy.getAs<PackExpansionType>()) {
      emitPartialDestroyPack(loc, packAddr, formalPackType, componentIndex,
                             /*limit*/ nullptr);

    // If it's a scalar component, project and destroy it.
    } else {
      auto packIndex =
        B.createScalarPackIndex(loc, componentIndex, formalPackType);
      auto eltAddr =
        B.createPackElementGet(loc, packIndex, packAddr, eltTy);
      B.createDestroyAddr(loc, eltAddr);
    }
  }
}

ManagedValue
SILGenFunction::emitManagedPackWithCleanup(SILValue addr,
                                           CanPackType formalPackType) {
  // If the pack type is trivial, we're done.
  if (getTypeLowering(addr->getType()).isTrivial())
    return ManagedValue::forTrivialAddressRValue(addr);

  // If we weren't given a formal pack type, construct one induced from
  // the lowered pack type.
  auto packType = addr->getType().castTo<SILPackType>();
  if (!formalPackType)
    formalPackType = packType->getApproximateFormalPackType();

  // Enter a cleanup for the pack.
  auto cleanup = enterDestroyPackCleanup(addr, formalPackType);
  return ManagedValue::forOwnedAddressRValue(addr, cleanup);
}

static bool isPatternInvariantToExpansion(CanType patternType,
                                          CanPackArchetypeType countArchetype) {
  return !patternType.findIf([&](CanType type) {
    if (auto archetype = dyn_cast<PackArchetypeType>(type)) {
      return archetype == countArchetype ||
             archetype->getReducedShape() == countArchetype->getReducedShape();
    }
    return false;
  });
}

std::pair<GenericEnvironment*, SILType>
SILGenFunction::createOpenedElementValueEnvironment(SILType expansionTy) {
  auto expansion = expansionTy.castTo<PackExpansionType>();

  // If the pattern type is invariant to the expansion, we don't need
  // to open anything.
  auto countArchetype = cast<PackArchetypeType>(expansion.getCountType());
  auto patternType = expansion.getPatternType();
  if (isPatternInvariantToExpansion(patternType, countArchetype)) {
    return std::make_pair(nullptr,
                          SILType::getPrimitiveAddressType(patternType));
  }

  // Otherwise, make a new opened element environment for the signature
  // of the archetype we're expanding over.
  // TODO: consider minimizing this signature down to only what we need to
  // destroy the elements
  auto context =
    OpenedElementContext::createForContextualExpansion(SGM.getASTContext(),
                                                       expansion);
  auto elementType =
    context.environment->mapContextualPackTypeIntoElementContext(patternType);
  return std::make_pair(context.environment,
                        SILType::getPrimitiveAddressType(elementType));
}

void SILGenFunction::emitPartialDestroyPack(SILLocation loc, SILValue packAddr,
                                            CanPackType formalPackType,
                                            unsigned componentIndex,
                                            SILValue limitWithinComponent) {
  auto packTy = packAddr->getType().castTo<SILPackType>();

  auto result = createOpenedElementValueEnvironment(
                                  packTy->getSILElementType(componentIndex));
  auto elementEnv = result.first;
  auto elementTy = result.second;

  emitDynamicPackLoop(loc, formalPackType, componentIndex,
                      /*startAfter*/ SILValue(), limitWithinComponent,
                      elementEnv, /*reverse*/ true,
                      [&](SILValue indexWithinComponent,
                          SILValue packExpansionIndex,
                          SILValue packIndex) {
    auto eltAddr = B.createPackElementGet(loc, packIndex, packAddr, elementTy);
    B.createDestroyAddr(loc, eltAddr);
  });
}

void SILGenFunction::emitPartialDestroyTuple(SILLocation loc,
                                             SILValue tupleAddr,
                                             CanPackType inducedPackType,
                                             unsigned componentIndex,
                                             SILValue limitWithinComponent) {
  auto result = createOpenedElementValueEnvironment(
                    tupleAddr->getType().getTupleElementType(componentIndex));
  auto elementEnv = result.first;
  auto elementTy = result.second;

  emitDynamicPackLoop(loc, inducedPackType, componentIndex,
                      /*startAfter*/ SILValue(), limitWithinComponent,
                      elementEnv, /*reverse*/ true,
                      [&](SILValue indexWithinComponent,
                          SILValue packExpansionIndex,
                          SILValue packIndex) {
    auto eltAddr =
      B.createTuplePackElementAddr(loc, packIndex, tupleAddr, elementTy);
    B.createDestroyAddr(loc, eltAddr);
  });
}

void SILGenFunction::emitPartialDestroyRemainingTuple(SILLocation loc,
                                                      SILValue tupleAddr,
                                                      CanPackType inducedPackType,
                                                      unsigned componentIndex,
                                        SILValue currentIndexWithinComponent) {
  auto result = createOpenedElementValueEnvironment(
                    tupleAddr->getType().getTupleElementType(componentIndex));
  auto elementEnv = result.first;
  auto elementTy = result.second;

  emitDynamicPackLoop(loc, inducedPackType, componentIndex,
                      /*startAfter*/ currentIndexWithinComponent,
                      /*limit*/ SILValue(), elementEnv, /*reverse*/ false,
                      [&](SILValue indexWithinComponent,
                          SILValue packExpansionIndex,
                          SILValue packIndex) {
    auto eltAddr =
      B.createTuplePackElementAddr(loc, packIndex, tupleAddr, elementTy);
    B.createDestroyAddr(loc, eltAddr);
  });
}

void SILGenFunction::emitDynamicPackLoop(SILLocation loc,
                                         CanPackType formalPackType,
                                         unsigned componentIndex,
                                         GenericEnvironment *openedElementEnv,
                      llvm::function_ref<void(SILValue indexWithinComponent,
                                              SILValue packExpansionIndex,
                                              SILValue packIndex)> emitBody) {
  return emitDynamicPackLoop(loc, formalPackType, componentIndex,
                             /*startAfter*/ SILValue(), /*limit*/ SILValue(),
                             openedElementEnv, /*reverse*/false, emitBody);
}

void SILGenFunction::emitDynamicPackLoop(SILLocation loc,
                                         CanPackType formalPackType,
                                         unsigned componentIndex,
                                         SILValue startingAfterIndexInComponent,
                                         SILValue limitWithinComponent,
                                         GenericEnvironment *openedElementEnv,
                                         bool reverse,
                      llvm::function_ref<void(SILValue indexWithinComponent,
                                              SILValue packExpansionIndex,
                                              SILValue packIndex)> emitBody) {
  assert(isa<PackExpansionType>(formalPackType.getElementType(componentIndex)));
  assert((!startingAfterIndexInComponent || !reverse) &&
         "cannot reverse with a starting index");
  ASTContext &ctx = SGM.getASTContext();

  // Save and restore the innermost pack expansion.
  ActivePackExpansion activeExpansionRecord = {
    openedElementEnv
  };

  llvm::SaveAndRestore<ActivePackExpansion*>
    packExpansionScope(InnermostPackExpansion, &activeExpansionRecord);

  if (auto *expansion = loc.getAsASTNode<PackExpansionExpr>())
    prepareToEmitPackExpansionExpr(expansion);

  auto wordTy = SILType::getBuiltinWordType(ctx);
  auto boolTy = SILType::getBuiltinIntegerType(1, ctx);

  SILValue zero;
  if (!startingAfterIndexInComponent) {
    zero = B.createIntegerLiteral(loc, wordTy, 0);
  }

  auto one = B.createIntegerLiteral(loc, wordTy, 1);

  // The formal type of the component of the pack that we're iterating over.
  // If this isn't the entire pack, we'll dynamically index into just the
  // expansion component and then compose that into an index into the larger
  // pack.
  CanPackType formalDynamicPackType = formalPackType;
  bool needsSlicing = formalPackType->getNumElements() != 1;
  if (needsSlicing) {
    formalDynamicPackType =
      CanPackType::get(ctx, formalPackType.getElementType(componentIndex));
  }

  // If the caller didn't give us a limit, use the full length of the
  // pack expansion.
  if (!limitWithinComponent) {
    limitWithinComponent = B.createPackLength(loc, formalDynamicPackType);
  }

  // The initial index value: the limit if iterating in reverse,
  // otherwise the start-after index + 1 if we have one, otherwise 0.
  SILValue startingIndex;
  if (reverse) {
    startingIndex = limitWithinComponent;
  } else if (startingAfterIndexInComponent) {
    startingIndex = B.createBuiltinBinaryFunction(loc, "add", wordTy, wordTy,
                                      { startingAfterIndexInComponent, one });
  } else {
    startingIndex = zero;
  }

  // Branch to the loop condition block, passing the initial index value.
  auto condBB = createBasicBlock();
  B.createBranch(loc, condBB, { startingIndex });

  // Condition block:
  B.emitBlock(condBB);
  auto incomingIndex = condBB->createPhiArgument(wordTy, OwnershipKind::None);

  // Branch to the end block if the incoming index value is equal to the
  // end index (the limit if forward, 0 if reverse).
  auto atEnd =
    B.createBuiltinBinaryFunction(loc, "cmp_eq", wordTy, boolTy,
                                  { incomingIndex,
                                    reverse ? zero : limitWithinComponent });
  auto bodyBB = createBasicBlock();
  auto endBB = createBasicBlockAfter(bodyBB);
  B.createCondBranch(loc, atEnd, endBB, bodyBB);

  // Body block:
  B.emitBlock(bodyBB);

  // The index to use in this iteration (the incoming index if forward,
  // the incoming index - 1 if reverse)
  SILValue curIndex = incomingIndex;
  if (reverse) {
    curIndex = B.createBuiltinBinaryFunction(loc, "sub", wordTy, wordTy,
                                             { incomingIndex, one });
  }

  // Construct the dynamic pack index into the component.
  SILValue packExpansionIndex =
    B.createDynamicPackIndex(loc, curIndex, formalDynamicPackType);
  getInnermostPackExpansion()->ExpansionIndex = packExpansionIndex;

  // If there's an opened element environment, open it here.
  if (openedElementEnv) {
    B.createOpenPackElement(loc, packExpansionIndex, openedElementEnv);
  }

  // If there are multiple pack components in the overall pack, construct
  // the overall pack index.
  SILValue packIndex = packExpansionIndex;
  if (needsSlicing) {
    packIndex = B.createPackPackIndex(loc, componentIndex, packIndex,
                                      formalPackType);
  }

  // Emit the loop body in a scope as a convenience, since it's necessary
  // to avoid dominance problems anyway.
  {
    FullExpr scope(Cleanups, CleanupLocation(loc));
    emitBody(curIndex, packExpansionIndex, packIndex);
  }

  // The index to pass to the loop condition block (the current index + 1
  // if forward, the current index if reverse)
  SILValue outgoingIndex = curIndex;
  if (!reverse) {
    outgoingIndex = B.createBuiltinBinaryFunction(loc, "add", wordTy, wordTy,
                                                  { curIndex, one });
  }
  B.createBranch(loc, condBB, {outgoingIndex});

  // End block:
  B.emitBlock(endBB);
}

/// Given that we're within a dynamic pack loop with the same expansion
/// shape as a pack expansion component of the given formal pack type,
/// produce a pack index for the current component within the formal pack.
///
/// Note that the *outer* pack index for the dynamic pack loop
/// isn't necessarily correct for the given pack, just the *expansion*
/// pack index.
static SILValue emitPackPackIndexForActiveExpansion(SILGenFunction &SGF,
                                                    SILLocation loc,
                                                    CanPackType formalPackType,
                                                    unsigned componentIndex) {
  auto activeExpansion = SGF.getInnermostPackExpansion();
  auto packIndex = activeExpansion->ExpansionIndex;
  if (formalPackType->getNumElements() != 1) {
    packIndex = SGF.B.createPackPackIndex(loc, componentIndex, packIndex,
                                          formalPackType);
  }
  return packIndex;
}

void InPlacePackExpansionInitialization::
       performPackExpansionInitialization(SILGenFunction &SGF,
                                          SILLocation loc,
                                          SILValue indexWithinComponent,
                        llvm::function_ref<void(Initialization *into)> fn) {
  // Enter a cleanup to destroy elements of the expansion up to the
  // current index.  We only need to do this if the elements are
  // non-trivial, which we've already checked in order to decide whether
  // to set up the dormant full-expansion cleanup.  So we can just check
  // that instead of looking at type properties again.
  bool needCleanups = ExpansionCleanup.isValid();

  CleanupHandle packCleanup = CleanupHandle::invalid();
  if (needCleanups)
    packCleanup = enterPartialDestroyCleanup(SGF, indexWithinComponent);

  // The pack index from the active pack expansion is just into the
  // expansion component; wrap it as necessary to index into the larger
  // pack/tuple element list.
  auto packIndex = emitPackPackIndexForActiveExpansion(SGF, loc,
                                                       FormalPackType,
                                                       ComponentIndex);

  // Translate the pattern type into the environment of the innermost
  // pack expansion.
  auto loweredPatternTy = getLoweredExpansionType().getPatternType();
  if (auto env = SGF.getInnermostPackExpansion()->OpenedElementEnv) {
    // This AST-level transformation is fine on lowered types because
    // we're just replacing pack archetypes with element archetypes.
    loweredPatternTy =
      env->mapContextualPackTypeIntoElementContext(loweredPatternTy);
  }
  auto eltAddrTy = SILType::getPrimitiveAddressType(loweredPatternTy);

  // Project the element address.
  auto eltAddr = getElementAddress(SGF, loc, packIndex, eltAddrTy);

  // Enter a dormant address for the element, under the same condition
  // as above.
  CleanupHandle eltCleanup = CleanupHandle::invalid();
  if (needCleanups) {
    eltCleanup = SGF.enterDestroyCleanup(eltAddr);
    SGF.Cleanups.setCleanupState(eltCleanup, CleanupState::Dormant);
  }

  // Emit the initialization into the temporary.
  TemporaryInitialization eltInit(eltAddr, eltCleanup);
  fn(&eltInit);

  // Deactivate the cleanups before continuing the loop.
  if (needCleanups) {
    SGF.Cleanups.forwardCleanup(packCleanup);
    SGF.Cleanups.forwardCleanup(eltCleanup);
  }
}

bool InPlacePackExpansionInitialization::
       canPerformInPlacePackInitialization(GenericEnvironment *env,
                                           SILType eltAddrTy) const {
  auto loweredPatternTy = getLoweredExpansionType().getPatternType();
  if (env) {
    loweredPatternTy =
      env->mapContextualPackTypeIntoElementContext(loweredPatternTy);
  }

  return loweredPatternTy == eltAddrTy.getASTType();
}

SILValue InPlacePackExpansionInitialization::
           getAddressForInPlacePackInitialization(SILGenFunction &SGF,
                                                  SILLocation loc,
                                                  SILType eltAddrTy) {
  auto packIndex = emitPackPackIndexForActiveExpansion(SGF, loc,
                                                       FormalPackType,
                                                       ComponentIndex);
  return getElementAddress(SGF, loc, packIndex, eltAddrTy);
}

void InPlacePackExpansionInitialization::
       finishInitialization(SILGenFunction &SGF) {
  if (ExpansionCleanup.isValid())
    SGF.Cleanups.setCleanupState(ExpansionCleanup, CleanupState::Active);
}

void InPlacePackExpansionInitialization::
       enterDormantExpansionCleanup(SILGenFunction &SGF) {
  assert(!ExpansionCleanup.isValid());
  auto loweredExpansionTy = getLoweredExpansionType();
  auto loweredPatternTy = loweredExpansionTy.getPatternType();

  // Enter a dormant cleanup to destroy the pack expansion elements
  // if they're non-trivial.
  if (!SGF.getTypeLowering(loweredPatternTy).isTrivial()) {
    ExpansionCleanup = enterPartialDestroyCleanup(SGF, /*limit*/SILValue());
    SGF.Cleanups.setCleanupState(ExpansionCleanup, CleanupState::Dormant);
  }
}

std::unique_ptr<PackExpansionInitialization>
PackExpansionInitialization::create(SILGenFunction &SGF, SILValue packAddr,
                                    CanPackType formalPackType,
                                    unsigned componentIndex) {
  auto init =
    std::make_unique<PackExpansionInitialization>(packAddr, formalPackType,
                                                  componentIndex);
  init->enterDormantExpansionCleanup(SGF);
  return init;
}

CanPackExpansionType
PackExpansionInitialization::getLoweredExpansionType() const {
  auto loweredPackTy = PackAddr->getType().castTo<SILPackType>();
  auto loweredComponentTy = loweredPackTy->getElementType(ComponentIndex);
  return cast<PackExpansionType>(loweredComponentTy);
}

CleanupHandle
PackExpansionInitialization::enterPartialDestroyCleanup(SILGenFunction &SGF,
                                              SILValue limitWithinComponent) {
  return SGF.enterPartialDestroyPackCleanup(PackAddr, FormalPackType,
                                            ComponentIndex,
                                            limitWithinComponent);
}

SILValue PackExpansionInitialization::getElementAddress(SILGenFunction &SGF,
                                                        SILLocation loc,
                                                        SILValue packIndex,
                                                        SILType eltAddrTy) {
  return SGF.B.createPackElementGet(loc, packIndex, PackAddr, eltAddrTy);
}

std::unique_ptr<TuplePackExpansionInitialization>
TuplePackExpansionInitialization::create(SILGenFunction &SGF,
                                         SILValue tupleAddr,
                                         CanPackType inducedPackType,
                                         unsigned componentIndex) {
  auto init = std::make_unique<TuplePackExpansionInitialization>(tupleAddr,
                                                            inducedPackType,
                                                            componentIndex);
  init->enterDormantExpansionCleanup(SGF);
  return init;
}

CanPackExpansionType
TuplePackExpansionInitialization::getLoweredExpansionType() const {
  auto loweredTupleTy = TupleAddr->getType().castTo<TupleType>();
  auto loweredComponentTy = loweredTupleTy.getElementType(ComponentIndex);
  return cast<PackExpansionType>(loweredComponentTy);
}

CleanupHandle TuplePackExpansionInitialization::
                enterPartialDestroyCleanup(SILGenFunction &SGF,
                                           SILValue limitWithinComponent) {
  return SGF.enterPartialDestroyTupleCleanup(TupleAddr, FormalPackType,
                                             ComponentIndex,
                                             limitWithinComponent);
}

SILValue
TuplePackExpansionInitialization::getElementAddress(SILGenFunction &SGF,
                                                    SILLocation loc,
                                                    SILValue packIndex,
                                                    SILType eltAddrTy) {
  return SGF.B.createTuplePackElementAddr(loc, packIndex, TupleAddr, eltAddrTy);
}