//===--- TypeChecker.cpp - Type Checking ----------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements the swift::performTypeChecking entry point for
// semantic analysis.
//
//===----------------------------------------------------------------------===//

#include "swift/Subsystems.h"
#include "TypeChecker.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/Attr.h"
#include "swift/AST/ExprHandle.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Sema/CodeCompletionTypeChecking.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/ADT/Twine.h"

using namespace swift;

TypeChecker::TypeChecker(ASTContext &Ctx, DiagnosticEngine &Diags)
  : Context(Ctx), Diags(Diags)
{
  Context.addMutationListener(*this);

  // Add any external definitions already part of the translation unit.
  // Note: the underlying vector can get reallocated during this traversal.
  // We don't want to pick up the new external definitions.
  unsigned n = Context.ExternalDefinitions.size();
  for (unsigned i = Context.LastCheckedExternalDefinition; i != n; ++i)
    handleExternalDecl(Context.ExternalDefinitions[i]);
}

TypeChecker::~TypeChecker() {
  Context.removeMutationListener(*this);
}

void TypeChecker::handleExternalDecl(Decl *decl) {
  if (auto structDecl = dyn_cast<StructDecl>(decl)) {
    addImplicitConstructors(structDecl);
  }
  if (auto CD = dyn_cast<ClassDecl>(decl)) {
    addImplicitDestructor(CD);
  }
}

void TypeChecker::addedExternalDecl(Decl *decl) {
  handleExternalDecl(decl);
  Context.ExternalDefinitions.insert(decl);
}

ProtocolDecl *TypeChecker::getProtocol(SourceLoc loc, KnownProtocolKind kind) {
  auto protocol = Context.getProtocol(kind);
  if (!protocol && loc.isValid()) {
    diagnose(loc, diag::missing_protocol,
             Context.getIdentifier(getProtocolName(kind)));
  }

  return protocol;
}

ProtocolDecl *TypeChecker::getLiteralProtocol(Expr *expr) {
  if (isa<ArrayExpr>(expr))
    return getProtocol(expr->getLoc(),
                       KnownProtocolKind::ArrayLiteralConvertible);

  if (isa<DictionaryExpr>(expr))
    return getProtocol(expr->getLoc(),
                       KnownProtocolKind::DictionaryLiteralConvertible);

  if (!isa<LiteralExpr>(expr))
    return nullptr;
  
  if (isa<IntegerLiteralExpr>(expr))
    return getProtocol(expr->getLoc(),
                       KnownProtocolKind::IntegerLiteralConvertible);

  if (isa<FloatLiteralExpr>(expr))
    return getProtocol(expr->getLoc(),
                       KnownProtocolKind::FloatLiteralConvertible);

  if (isa<CharacterLiteralExpr>(expr))
    return getProtocol(expr->getLoc(),
                       KnownProtocolKind::CharacterLiteralConvertible);

  if (isa<StringLiteralExpr>(expr))
    return getProtocol(expr->getLoc(),
                       KnownProtocolKind::StringLiteralConvertible);

  if (isa<InterpolatedStringLiteralExpr>(expr))
    return getProtocol(expr->getLoc(),
                       KnownProtocolKind::StringInterpolationConvertible);
  if (auto E = dyn_cast<MagicIdentifierLiteralExpr>(expr)) {
    switch (E->getKind()) {
    case MagicIdentifierLiteralExpr::File:
      return getProtocol(expr->getLoc(),
                         KnownProtocolKind::StringLiteralConvertible);

    case MagicIdentifierLiteralExpr::Line:
    case MagicIdentifierLiteralExpr::Column:
      return getProtocol(expr->getLoc(),
                         KnownProtocolKind::IntegerLiteralConvertible);
    }
  }
  
  llvm_unreachable("Unhandled literal kind");
}

Module *TypeChecker::getStdlibModule() {
  if (StdlibModule)
    return StdlibModule;

  if (!StdlibModule)
    StdlibModule = Context.LoadedModules.lookup(Context.StdlibModuleName.str());
  if (!StdlibModule) {
    // FIXME: Hack. There should just be a pointer on ASTContext to the main
    // module.
    for (auto &entry : Context.LoadedModules) {
      if (auto TU = dyn_cast_or_null<TranslationUnit>(entry.getValue()))
        if (TU->getImportBufferID() == -1)
          StdlibModule = TU;
    }
    assert(StdlibModule && "no main module found");
  }

  Context.recordKnownProtocols(StdlibModule);
  return StdlibModule;
}

Type TypeChecker::lookupBoolType() {
  return boolType.cache([&]{
    UnqualifiedLookup boolLookup(Context.getIdentifier("Bool"),
                                 getStdlibModule(), nullptr,
                                 SourceLoc(),
                                 /*IsTypeLookup=*/true);
    if (!boolLookup.isSuccess()) {
      diagnose(SourceLoc(), diag::bool_type_broken);
      return Type();
    }
    TypeDecl *tyDecl = boolLookup.getSingleTypeResult();
    
    if (!tyDecl) {
      diagnose(SourceLoc(), diag::bool_type_broken);
      return Type();
    }
    
    return tyDecl->getDeclaredType();
  });
}

static void overrideDecl(TypeChecker &TC,
                         llvm::SmallPtrSet<ValueDecl*, 16> &OverriddenDecls,
                         ValueDecl *MemberVD, ValueDecl *OverriddenDecl) {
  if (!OverriddenDecls.insert(OverriddenDecl)) {
    TC.diagnose(MemberVD->getLoc(),
                diag::override_multiple_decls_derived);
    return;
  }
  if ((OverriddenDecl->getDeclContext()->isExtensionContext() ||
       MemberVD->getDeclContext()->isExtensionContext()) &&
      !OverriddenDecl->isObjC()) {
    TC.diagnose(MemberVD->getLoc(), diag::override_decl_extension,
                OverriddenDecl->getDeclContext()->isExtensionContext());
    TC.diagnose(OverriddenDecl->getLoc(), diag::overridden_here);
    return;
  }
  if (auto FD = dyn_cast<FuncDecl>(MemberVD)) {
    FD->setOverriddenDecl(cast<FuncDecl>(OverriddenDecl));
  } else if (auto VD = dyn_cast<VarDecl>(MemberVD)) {
    VD->setOverriddenDecl(cast<VarDecl>(OverriddenDecl));
  } else if (auto SD = dyn_cast<SubscriptDecl>(MemberVD)) {
    SD->setOverriddenDecl(cast<SubscriptDecl>(OverriddenDecl));
  } else {
    llvm_unreachable("Unexpected decl");
  }
}

static Type getBaseMemberDeclType(TypeChecker &TC,
                                  ValueDecl *D, Type Base) {
  ClassDecl *BaseDecl =
      D->getDeclContext()->getDeclaredTypeInContext()
      ->getClassOrBoundGenericClass();
  while (Base->getClassOrBoundGenericClass() != BaseDecl)
    Base = TC.getSuperClassOf(Base);
  return TC.substMemberTypeWithBase(D->getModuleContext(),
                                    D->getType(), D, Base);
}

/// \brief Check the given set of members for any members that override a member
/// of a superclass (or its extensions).
///
/// \param TC The type checker.
///
/// \param CD The class for which we're checking overrides.
///
/// \param Members The set of members for which we are checking overrides.
static void checkClassOverrides(TypeChecker &TC, ClassDecl *CD,
                                ArrayRef<Decl *> Members) {
  if (!CD->hasSuperclass())
    return;

  // The overall process of checking a method which might override
  // a superclass method with the same name has three phases:
  // 1. If there's an exact match, the method overrides
  // the superclass method.
  // 2. If there's a subtyping relationship with a single method not yet
  // overriden, the method overrides that superclass method.  It's an error
  // if there are multiple potential superclass methods.
  // 3. If all the superclass methods with a given name have been overridden,
  // the method introduces a new overload.

  Type superclassTy = CD->getSuperclass();
  auto superclassMetaTy = MetaTypeType::get(superclassTy, TC.Context);
  llvm::DenseMap<Identifier, std::vector<ValueDecl*>> FoundDecls;
  llvm::SmallPtrSet<ValueDecl*, 16> ExactOverriddenDecls;
  SmallVector<ValueDecl*, 16> PossiblyOverridingDecls;
  for (Decl *MemberD : Members) {
    ValueDecl *MemberVD = dyn_cast<ValueDecl>(MemberD);
    if (!MemberVD)
      continue;
    if (isa<DestructorDecl>(MemberVD) || isa<ConstructorDecl>(MemberVD))
      continue;
    if (MemberVD->getName().empty())
      continue;
    auto FoundDeclResult = FoundDecls.insert({ MemberVD->getName(),
                                               std::vector<ValueDecl*>() });
    auto& CurDecls = FoundDeclResult.first->second;
    if (FoundDeclResult.second) {
      for (auto BaseMember : TC.lookupMember(superclassMetaTy,
                                             MemberVD->getName(),
                                             CD->getDeclContext(),
                                             /*allowDynamicLookup=*/false))
        CurDecls.push_back(BaseMember);
    }
    if (!CurDecls.empty()) {
      if (isa<TypeDecl>(MemberVD)) {
        // Duplicate type declaration; this gets diagnosed earlier.
        // FIXME: That doesn't actually happen at the moment.
        continue;
      }

      ValueDecl *OverriddenDecl = nullptr;

      // First, check for an exact type match.
      for (unsigned i = 0, e = CurDecls.size(); i != e; ++i) {
        if (isa<TypeDecl>(CurDecls[i])) {
          // Overriding type declaration; this gets diagnosed earlier.
          // FIXME: That doesn't actually happen at the moment.
          break;
        }
        if (MemberVD->getKind() != CurDecls[i]->getKind())
          continue;
        bool isTypeEqual = false;
        Type BaseMemberTy = getBaseMemberDeclType(TC, CurDecls[i], superclassTy);
        if (isa<FuncDecl>(MemberVD)) {
          AnyFunctionType *MemberFTy =
              MemberVD->getType()->getAs<AnyFunctionType>();
          AnyFunctionType *OtherFTy = BaseMemberTy->castTo<AnyFunctionType>();
          if (MemberFTy && OtherFTy)
            isTypeEqual = MemberFTy->getResult()->isEqual(OtherFTy->getResult());
        } else {
          isTypeEqual = MemberVD->getType()->isEqual(BaseMemberTy);
        }
        if (isTypeEqual) {
          if (CurDecls[i]->getDeclContext() == MemberVD->getDeclContext()) {
            // Duplicate declaration, diagnosed elsewhere
            // FIXME: That doesn't actually happen at the moment.
            continue;
          }
          if (OverriddenDecl) {
            // Two decls from the superclass have the same type; this will
            // trigger an error elsewhere.
            // FIXME: That doesn't actually happen at the moment.
            continue;
          }
          OverriddenDecl = CurDecls[i];
        }
      }

      if (OverriddenDecl) {
        overrideDecl(TC, ExactOverriddenDecls, MemberVD, OverriddenDecl);
      } else {
        PossiblyOverridingDecls.push_back(MemberVD);
      }
    }
  }

  SmallVector<ValueDecl*, 16> PossiblyOverloadingDecls;
  llvm::SmallPtrSet<ValueDecl*, 16> SubtypeOverriddenDecls;
  for (auto MemberVD : PossiblyOverridingDecls) {
    // Then, check for a subtyping relationship.
    auto& CurDecls = FoundDecls[MemberVD->getName()];
    ValueDecl *OverriddenDecl = nullptr;
    for (unsigned i = 0, e = CurDecls.size(); i != e; ++i) {
      if (CurDecls[i]->getDeclContext() == MemberVD->getDeclContext()) {
        // We ignore sub-typing on the same class.
        continue;
      }
      if (ExactOverriddenDecls.count(CurDecls[i]))
        continue;
      if (MemberVD->getKind() != CurDecls[i]->getKind())
        continue;
      bool isSubtype = false;
      Type BaseMemberTy = getBaseMemberDeclType(TC, CurDecls[i], superclassTy);
      if (isa<FuncDecl>(MemberVD)) {
        AnyFunctionType *MemberFTy =
            MemberVD->getType()->getAs<AnyFunctionType>();
        AnyFunctionType *OtherFTy = BaseMemberTy->getAs<AnyFunctionType>();
        if (MemberFTy && OtherFTy) {
          bool Trivial;
          isSubtype = TC.isSubtypeOf(MemberFTy->getResult(),
                                     OtherFTy->getResult(),
                                     CD->getDeclContext(),
                                     Trivial) && Trivial;
        }
      } else {
        bool Trivial;
        isSubtype = TC.isSubtypeOf(MemberVD->getType(),
                                   BaseMemberTy,
                                   CD->getDeclContext(),
                                   Trivial) && Trivial;
      }
      if (isSubtype) {
        if (OverriddenDecl) {
          TC.diagnose(MemberVD->getLoc(),
                      diag::override_multiple_decls_base);
          continue;
        }
        OverriddenDecl = CurDecls[i];
      }
      if (OverriddenDecl) {
        overrideDecl(TC, SubtypeOverriddenDecls, MemberVD, OverriddenDecl);
      } else {
        PossiblyOverloadingDecls.push_back(MemberVD);
      }
    }
  }

  for (auto MemberVD : PossiblyOverloadingDecls) {
    // Check if this is a new overload.
    auto& CurDecls = FoundDecls[MemberVD->getName()];
    for (unsigned i = 0, e = CurDecls.size(); i != e; ++i) {
      if (CurDecls[i]->getDeclContext() == MemberVD->getDeclContext()) {
        // We ignore sub-typing on the same class.
        continue;
      }
      if (ExactOverriddenDecls.count(CurDecls[i]))
        continue;
      if (SubtypeOverriddenDecls.count(CurDecls[i]))
        continue;
      // Give [objc] decls a pass, because 'foo:' and 'foo:bar:' can coexist.
      if (MemberVD->isObjC() || CurDecls[i]->isObjC())
        continue;
      TC.diagnose(MemberVD->getLoc(), diag::overload_base_decl);
      break;
    }
  }
}

static void bindExtensionDecl(ExtensionDecl *ED, TypeChecker &TC) {
  if (ED->getExtendedTypeLoc().wasValidated())
    return;
  
  // FIXME: Should allow bound generics here as well.
  if (TC.validateType(ED->getExtendedTypeLoc(), ED->getDeclContext(),
                      /*allowUnboundGenerics=*/true)) {
    ED->setInvalid();
    return;
  }

  Type ExtendedTy = ED->getExtendedType()->getCanonicalType();
  if (!ExtendedTy->is<NominalType>() && !ExtendedTy->is<UnboundGenericType>() &&
      !ExtendedTy->is<ErrorType>()) {
    TC.diagnose(ED, diag::non_nominal_extension,
                false, ED->getExtendedType());
    ED->getExtendedTypeLoc().setInvalidType(TC.Context);
    return;
  }

  if (auto nominal = ExtendedTy->getAnyNominal())
    nominal->addExtension(ED);
}

/// Returns true if the given decl or extension conforms to a protocol whose
/// name matches a compiler-known protocol. This is a syntactic check; no type
/// resolution is performed.
template <typename DeclTy>
static bool mayConformToKnownProtocol(const DeclTy *D) {
  for (TypeLoc inherited : D->getInherited()) {
    auto identRepr = dyn_cast_or_null<IdentTypeRepr>(inherited.getTypeRepr());
    if (!identRepr)
      continue;

    const IdentTypeRepr::Component &lastID = identRepr->Components.back();
    if (!lastID.getGenericArgs().empty())
      continue;

    bool matchesKnownProtocol =
      llvm::StringSwitch<bool>(lastID.getIdentifier().str())
#define PROTOCOL(Name) \
        .Case(#Name, true)
#include "swift/AST/KnownProtocols.def"
        .Default(false);
    if (matchesKnownProtocol)
      return true;
  }

  return false;
}

static void checkBridgingFunctions(TypeChecker &tc, Module *mod,
                                   StringRef bridgedTypeName,
                                   StringRef forwardConversion,
                                   StringRef reverseConversion) {
  assert(mod);
  Module::AccessPathTy unscopedAccess = {};
  SmallVector<ValueDecl *, 4> results;

  mod->lookupValue(unscopedAccess, mod->Ctx.getIdentifier(bridgedTypeName),
                   NLKind::QualifiedLookup, results);
  mod->lookupValue(unscopedAccess, mod->Ctx.getIdentifier(forwardConversion),
                   NLKind::QualifiedLookup, results);
  mod->lookupValue(unscopedAccess, mod->Ctx.getIdentifier(reverseConversion),
                   NLKind::QualifiedLookup, results);

  for (auto D : results)
    tc.validateDecl(D);
}

static bool haveDifferentFixity(const ValueDecl *lhs, const ValueDecl *rhs) {
  auto lhsFn = dyn_cast<FuncDecl>(lhs);
  auto rhsFn = dyn_cast<FuncDecl>(rhs);
  if (!lhsFn || !rhsFn)
    return false;

  assert(lhs->getName() == rhs->getName());
  if (!lhs->getName().isOperator())
    return false;

  assert(lhs->getAttrs().isPrefix() ^ lhs->getAttrs().isPostfix());
  assert(rhs->getAttrs().isPrefix() ^ rhs->getAttrs().isPostfix());
  return lhs->getAttrs().isPrefix() != rhs->getAttrs().isPrefix();
}

/// performTypeChecking - Once parsing and namebinding are complete, these
/// walks the AST to resolve types and diagnose problems therein.
///
/// FIXME: This should be moved out to somewhere else.
void swift::performTypeChecking(TranslationUnit *TU, unsigned StartElem) {
  // Make sure that name binding has been completed before doing any type
  // checking.
  performNameBinding(TU, StartElem);
  
  TypeChecker TC(TU->Ctx);
  auto &DefinedFunctions = TC.definedFunctions;

  // Lookup the swift module.  This ensures that we record all known protocols
  // in the AST.
  (void) TC.getStdlibModule();

  // Resolve extensions. This has to occur first during type checking,
  // because the extensions need to be wired into the AST for name lookup
  // to work.
  // FIXME: We can have interesting ordering dependencies among the various
  // extensions, so we'll need to be smarter here.
  TU->forAllVisibleModules(Module::AccessPathTy(),
                           [&](Module::ImportedModule import) {
    auto importTU = dyn_cast<TranslationUnit>(import.second);
    if (!importTU)
      return;
    // FIXME: Respect the access path?
    for (auto D : importTU->Decls) {
      if (auto ED = dyn_cast<ExtensionDecl>(D)) {
        bindExtensionDecl(ED, TC);
        if (mayConformToKnownProtocol(ED))
          TC.validateDecl(ED->getExtendedType()->getAnyNominal());
      } else if (auto nominal = dyn_cast<NominalTypeDecl>(D)) {
        if (mayConformToKnownProtocol(nominal))
          TC.validateDecl(nominal);
      }
    }
  });

  // FIXME: Check for cycles in class inheritance here?

  // Type check the top-level elements of the translation unit.
  for (unsigned i = StartElem, e = TU->Decls.size(); i != e; ++i) {
    Decl *D = TU->Decls[i];
    if (isa<TopLevelCodeDecl>(D))
      continue;

    TC.typeCheckDecl(D, /*isFirstPass*/true);
  }

  // Check for correct overriding in classes.  We have to do this after the
  // first pass so we have valid types, but before the second pass because
  // it affects name lookup.  The loop here is a bit complicated because we
  // can't check a class before we've checked its base.
  {
    enum ClassState { CheckNone, CheckExtensions, CheckAll };
    llvm::DenseMap<ClassDecl *, ClassState> CheckedClasses;
    SmallVector<ClassDecl *, 16> QueuedClasses;
    for (unsigned i = 0, e = StartElem; i != e; ++i) {
      ClassDecl *CD = dyn_cast<ClassDecl>(TU->Decls[i]);
      if (CD)
        CheckedClasses[CD] = CheckNone;
    }

    // Find all of the classes and class extensions.
    llvm::DenseMap<ClassDecl *, SmallVector<ExtensionDecl *, 2>>
      Extensions;
    for (unsigned i = StartElem, e = TU->Decls.size(); i != e; ++i) {
      // We found a class; record it.
      if (auto classDecl = dyn_cast<ClassDecl>(TU->Decls[i])) {
        if (CheckedClasses.find(classDecl) == CheckedClasses.end()) {
          QueuedClasses.push_back(classDecl);
          CheckedClasses[classDecl] = CheckAll;
        }
        continue;
      }

      auto ext = dyn_cast<ExtensionDecl>(TU->Decls[i]);
      if (!ext || ext->isInvalid())
        continue;

      auto classDecl = ext->getExtendedType()->getClassOrBoundGenericClass();
      if (!classDecl)
        continue;

      Extensions[classDecl].push_back(ext);
      if (CheckedClasses[classDecl] == CheckNone) {
        QueuedClasses.push_back(classDecl);
        ClassState &State = CheckedClasses[classDecl];
        State = std::max(State, CheckExtensions);
      }
    }

    // Check overrides in all of the classes and their extensions.
    while (!QueuedClasses.empty()) {
      auto classDecl = QueuedClasses.back();
      if (classDecl->hasSuperclass()) {
        ClassDecl *Base
          = classDecl->getSuperclass()->getClassOrBoundGenericClass();
        if (CheckedClasses[Base] != CheckNone) {
          QueuedClasses.push_back(Base);
          continue;
        }
      }
      if (CheckedClasses[classDecl] == CheckAll) {
        checkClassOverrides(TC, classDecl, classDecl->getMembers());
      }
      CheckedClasses[classDecl] = CheckNone;
      
      QueuedClasses.pop_back();

      // Check overrides in extensions of this class.
      auto knownExts = Extensions.find(classDecl);
      if (knownExts == Extensions.end())
        continue;

      for (auto ext : knownExts->second) {
        checkClassOverrides(TC, classDecl, ext->getMembers());
      }
    }
  }

  // At this point, we can perform general name lookup into any type.

  // We don't know the types of all the global declarations in the first
  // pass, which means we can't completely analyze everything. Perform the
  // second pass now.

  for (unsigned i = StartElem, e = TU->Decls.size(); i != e; ++i) {
    Decl *D = TU->Decls[i];
    if (TopLevelCodeDecl *TLCD = dyn_cast<TopLevelCodeDecl>(D)) {
      // Immediately perform global name-binding etc.
      TC.typeCheckTopLevelCodeDecl(TLCD);
    } else {
      TC.typeCheckDecl(D, /*isFirstPass*/false);
    }
  }

#define BRIDGE_TYPE(BRIDGED_MODULE, BRIDGED_TYPE, _, NATIVE_TYPE) \
  if (Module *module = TU->Ctx.LoadedModules.lookup(#BRIDGED_MODULE)) { \
    checkBridgingFunctions(TC, module, #BRIDGED_TYPE, \
                           "convert" #BRIDGED_TYPE "To" #NATIVE_TYPE, \
                           "convert" #NATIVE_TYPE "To" #BRIDGED_TYPE); \
  }
#include "swift/SIL/BridgedTypes.def"

  // Define any pending implicitly declarations.
  TC.definePendingImplicitDecls();
  DefinedFunctions.insert(DefinedFunctions.end(),
                          TC.implicitlyDefinedFunctions.begin(),
                          TC.implicitlyDefinedFunctions.end());
  TC.implicitlyDefinedFunctions.clear();

  // If we're in REPL mode, inject temporary result variables and other stuff
  // that the REPL needs to synthesize.
  if (TU->Kind == TranslationUnit::REPL && !TC.Context.hadError())
    TC.processREPLTopLevel(TU, StartElem);
  
  // Check overloaded vars/funcs.
  // FIXME: This is quadratic time for TUs with multiple chunks.
  // FIXME: Can we make this more efficient?
  // FIXME: This check should be earlier to avoid ambiguous overload
  // errors etc.
  llvm::DenseMap<Identifier, TinyPtrVector<ValueDecl*>> CheckOverloads;
  for (unsigned i = 0, e = TU->Decls.size(); i != e; ++i) {
    if (ValueDecl *VD = dyn_cast<ValueDecl>(TU->Decls[i])) {
      // FIXME: I'm not sure this check is really correct.
      if (VD->getName().empty())
        continue;
      if (VD->getType()->is<ErrorType>())
        continue;
      auto &PrevOv = CheckOverloads[VD->getName()];
      if (i >= StartElem) {
        for (ValueDecl *PrevD : PrevOv) {
          if (PrevD->getType()->isEqual(VD->getType()) &&
              !haveDifferentFixity(PrevD, VD)) {
            TC.diagnose(VD->getStartLoc(), diag::invalid_redecl);
            TC.diagnose(PrevD, diag::invalid_redecl_prev,
                        VD->getName());
          }
        }
      }
      PrevOv.push_back(VD);
    }
  }

  unsigned currentFunctionIdx = 0;
  unsigned currentExternalDef = TC.Context.LastCheckedExternalDefinition;
  do {
    // Type check the body of each of the function in turn.  Note that outside
    // functions must be visited before nested functions for type-checking to
    // work correctly.
    unsigned previousFunctionIdx = currentFunctionIdx;
    for (unsigned n = DefinedFunctions.size(); currentFunctionIdx != n;
         ++currentFunctionIdx) {
      auto *AFD = DefinedFunctions[currentFunctionIdx];
      PrettyStackTraceDecl StackEntry("type-checking", AFD);
      TC.typeCheckAbstractFunctionBody(AFD);
    }

    // Compute captures for functions we visited, in the opposite order of type
    // checking. i.e., the nested DefinedFunctions will be visited before the
    // outer DefinedFunctions.
    for (unsigned i = currentFunctionIdx; i > previousFunctionIdx; --i) {
      if (auto *FD = dyn_cast<AbstractFunctionDecl>(DefinedFunctions[i-1]))
        TC.computeCaptures(FD);
    }

    for (unsigned n = TC.Context.ExternalDefinitions.size();
                  currentExternalDef != n;
         ++currentExternalDef) {
      auto decl = TC.Context.ExternalDefinitions[currentExternalDef];

      if (auto *AFD = dyn_cast<AbstractFunctionDecl>(decl)) {
        PrettyStackTraceDecl StackEntry("type-checking", AFD);
        TC.typeCheckAbstractFunctionBody(AFD);
        continue;
      }
       if (isa<StructDecl>(decl) || isa<ClassDecl>(decl) ||
           isa<ProtocolDecl>(decl)) {
         // Type decls should already be typed by the ClangImporter and don't
         // need additional typechecking.
         continue;
      }
      llvm_unreachable("Unhandled external definition kind");
    }

    // Define any pending implicit declarations.
    TC.definePendingImplicitDecls();
    DefinedFunctions.insert(DefinedFunctions.end(),
                     TC.implicitlyDefinedFunctions.begin(),
                     TC.implicitlyDefinedFunctions.end());
    TC.implicitlyDefinedFunctions.clear();
  } while (currentFunctionIdx < DefinedFunctions.size() ||
           currentExternalDef < TC.Context.ExternalDefinitions.size());

  // FIXME: Horrible hack. Store this somewhere more sane.
  TC.Context.LastCheckedExternalDefinition = currentExternalDef;

  // Verify that we've checked types correctly.
  TU->ASTStage = TranslationUnit::TypeChecked;
  verify(TU);

  // Verify modules imported by Clang importer.
  if (auto ClangLoader = TC.Context.getClangModuleLoader())
    ClangLoader->verifyAllModules();
}

bool swift::performTypeLocChecking(TranslationUnit *TU, TypeLoc &T,
                                   DeclContext *DC,
                                   bool ProduceDiagnostics) {
  if (ProduceDiagnostics) {
    return TypeChecker(TU->Ctx).validateType(T, DC);
  } else {
    // Set up a diagnostics engine that swallows diagnostics.
    DiagnosticEngine Diags(TU->Ctx.SourceMgr);
    return TypeChecker(TU->Ctx, Diags).validateType(T, DC);
  }
}

bool swift::typeCheckCompletionDecl(TranslationUnit *TU, Decl *D) {
  // Set up a diagnostics engine that swallows diagnostics.
  DiagnosticEngine Diags(TU->Ctx.SourceMgr);
  TypeChecker TC(TU->Ctx, Diags);
  TC.typeCheckDecl(D, true);
  return true;
}

bool swift::typeCheckCompletionContextExpr(TranslationUnit *TU,
                                           Expr *&parsedExpr) {
  // Set up a diagnostics engine that swallows diagnostics.
  DiagnosticEngine diags(TU->Ctx.SourceMgr);
  
  TypeChecker TC(TU->Ctx, diags);
  TC.typeCheckExpression(parsedExpr, TU, Type(), /*discardedExpr=*/true);
  TU->ASTStage = TranslationUnit::TypeChecked;
  
  return parsedExpr && !isa<ErrorExpr>(parsedExpr)
                    && parsedExpr->getType()
                    && !parsedExpr->getType()->is<ErrorType>();
}

bool swift::typeCheckAbstractFunctionBodyUntil(TranslationUnit *TU,
                                               AbstractFunctionDecl *AFD,
                                               SourceLoc EndTypeCheckLoc) {
  if (AFD->isInvalid())
    return false;

  // Set up a diagnostics engine that swallows diagnostics.
  DiagnosticEngine Diags(TU->Ctx.SourceMgr);

  TypeChecker TC(TU->Ctx, Diags);
  return !TC.typeCheckAbstractFunctionBodyUntil(AFD, EndTypeCheckLoc);
}

static void deleteTypeCheckerAndDiags(LazyResolver *resolver) {
  DiagnosticEngine &diags = static_cast<TypeChecker*>(resolver)->Diags;
  delete resolver;
  delete &diags;
}

OwnedResolver swift::createLazyResolver(TranslationUnit *TU) {
  auto diags = new DiagnosticEngine(TU->Ctx.SourceMgr);
  return OwnedResolver(new TypeChecker(TU->Ctx, *diags),
                       &deleteTypeCheckerAndDiags);
}

void TypeChecker::diagnoseAmbiguousMemberType(Type baseTy,
                                              SourceRange baseRange,
                                              Identifier name,
                                              SourceLoc nameLoc,
                                              LookupTypeResult &lookup) {
  diagnose(nameLoc, diag::ambiguous_member_type, name, baseTy)
    .highlight(baseRange);
  for (const auto &member : lookup) {
    diagnose(member.first, diag::found_candidate_type,
             member.second);
  }
}
