//===--- CSSolver.cpp - Constraint Solver ---------------------------------===//
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
// This file implements the constraint solver used in the type checker.
//
//===----------------------------------------------------------------------===//
#include "ConstraintSystem.h"
#include "ConstraintGraph.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SaveAndRestore.h"
#include <memory>
#include <tuple>
using namespace swift;
using namespace constraints;

//===--------------------------------------------------------------------===//
// Constraint solver statistics
//===--------------------------------------------------------------------===//
#define DEBUG_TYPE "Constraint solver overall"
#define JOIN(X,Y) JOIN2(X,Y)
#define JOIN2(X,Y) X##Y
STATISTIC(NumSolutionAttempts, "# of solution attempts");

#define CS_STATISTIC(Name, Description) \
  STATISTIC(JOIN2(Overall,Name), Description);
#include "ConstraintSolverStats.def"

#undef DEBUG_TYPE
#define DEBUG_TYPE "Constraint solver largest system"
#define CS_STATISTIC(Name, Description) \
  STATISTIC(JOIN2(Largest,Name), Description);
#include "ConstraintSolverStats.def"
STATISTIC(LargestSolutionAttemptNumber, "# of the largest solution attempt");

/// \brief Check whether the given type can be used as a binding for the given
/// type variable.
///
/// \returns the type to bind to, if the binding is okay.
static Optional<Type> checkTypeOfBinding(ConstraintSystem &cs, 
                                         TypeVariableType *typeVar, Type type) {
  if (!type)
    return Nothing;

  // Simplify the type.
  type = cs.simplifyType(type);

  // If the type references the type variable, don't permit the binding.
  SmallVector<TypeVariableType *, 4> referencedTypeVars;
  type->getTypeVariables(referencedTypeVars);
  if (std::count(referencedTypeVars.begin(), referencedTypeVars.end(), typeVar))
    return Nothing;

  // If the type is a type variable itself, don't permit the binding.
  // FIXME: This is a hack. We need to be smarter about whether there's enough
  // structure in the type to produce an interesting binding, or not.
  if (type->getRValueType()->is<TypeVariableType>())
    return Nothing;

  // Okay, allow the binding (with the simplified type).
  return type;
}

Solution ConstraintSystem::finalize(
           FreeTypeVariableBinding allowFreeTypeVariables) {
  // Create the solution.
  Solution solution(*this, CurrentScore);

  // Update the best score we've seen so far.
  if (solverState) {
    assert(!solverState->BestScore || CurrentScore <= *solverState->BestScore);
    solverState->BestScore = CurrentScore;
  }

  // For any of the type variables that has no associated fixed type, assign a
  // fresh generic type parameters.
  // FIXME: We could gather the requirements on these as well.
  unsigned index = 0;
  for (auto tv : TypeVariables) {
    if (getFixedType(tv))
      continue;

    switch (allowFreeTypeVariables) {
    case FreeTypeVariableBinding::Disallow:
      llvm_unreachable("Solver left free type variables");

    case FreeTypeVariableBinding::Allow:
      break;

    case FreeTypeVariableBinding::GenericParameters:
      assignFixedType(tv, GenericTypeParamType::get(0, index++, TC.Context));
      break;
    }
  }

  // For each of the type variables, get its fixed type.
  for (auto tv : TypeVariables) {
    solution.typeBindings[tv] = simplifyType(tv);
  }

  // For each of the overload sets, get its overload choice.
  for (auto resolved = resolvedOverloadSets;
       resolved; resolved = resolved->Previous) {
    solution.overloadChoices[resolved->Locator]
      = { resolved->Choice, resolved->OpenedFullType, resolved->ImpliedType };
  }

  // For each of the constraint restrictions, record it with simplified,
  // canonical types.
  if (solverState) {
    for (auto &restriction : solverState->constraintRestrictions) {
      using std::get;
      CanType first = simplifyType(get<0>(restriction))->getCanonicalType();
      CanType second = simplifyType(get<1>(restriction))->getCanonicalType();
      solution.constraintRestrictions[{first, second}] = get<2>(restriction);
    }
  }

  return std::move(solution);
}

void ConstraintSystem::applySolution(const Solution &solution) {
  // Update the score.
  CurrentScore += solution.getFixedScore();

  // Assign fixed types to the type variables solved by this solution.
  llvm::SmallPtrSet<TypeVariableType *, 4> 
    knownTypeVariables(TypeVariables.begin(), TypeVariables.end());
  for (auto binding : solution.typeBindings) {
    // If we haven't seen this type variable before, record it now.
    if (knownTypeVariables.insert(binding.first))
      TypeVariables.push_back(binding.first);

    // If we don't already have a fixed type for this type variable,
    // assign the fixed type from the solution.
    if (!getFixedType(binding.first) && !binding.second->hasTypeVariable())
      assignFixedType(binding.first, binding.second, /*updateScore=*/false);
  }

  // Register overload choices.
  // FIXME: Copy these directly into some kind of partial solution?
  for (auto overload : solution.overloadChoices) {
    resolvedOverloadSets
      = new (*this) ResolvedOverloadSetListItem{resolvedOverloadSets,
                                                Type(),
                                                overload.second.choice,
                                                overload.first,
                                                overload.second.openedFullType,
                                                overload.second.openedType};    
  }

  // Register constraint restrictions.
  // FIXME: Copy these directly into some kind of partial solution?
  for (auto restriction : solution.constraintRestrictions) {
    solverState->constraintRestrictions.push_back({restriction.first.first,
                                                   restriction.first.second,
                                                   restriction.second});
  }
}

/// \brief Restore the type variable bindings to what they were before
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

SmallVector<Type, 4> ConstraintSystem::enumerateDirectSupertypes(Type type) {
  SmallVector<Type, 4> result;

  if (auto tupleTy = type->getAs<TupleType>()) {
    // A tuple that can be constructed from a scalar has a value of that
    // scalar type as its supertype.
    // FIXME: There is a way more general property here, where we can drop
    // one label from the tuple, maintaining the rest.
    int scalarIdx = tupleTy->getFieldForScalarInit();
    if (scalarIdx >= 0) {
      auto &elt = tupleTy->getFields()[scalarIdx];
      if (elt.isVararg()) // FIXME: Should we keep the name?
        result.push_back(elt.getVarargBaseTy());
      else if (!elt.getName().empty())
        result.push_back(elt.getType());
    }
  }

  if (auto functionTy = type->getAs<FunctionType>()) {
    // FIXME: Can weaken input type, but we really don't want to get in the
    // business of strengthening the result type.

    // An [auto_closure] function type can be viewed as scalar of the result
    // type.
    if (functionTy->isAutoClosure())
      result.push_back(functionTy->getResult());
  }

  if (type->mayHaveSuperclass()) {
    // FIXME: Can also weaken to the set of protocol constraints, but only
    // if there are any protocols that the type conforms to but the superclass
    // does not.

    // If there is a superclass, it is a direct supertype.
    if (auto superclass = TC.getSuperClassOf(type))
      result.push_back(superclass);
  }

  if (auto lvalue = type->getAs<LValueType>()) {
    if (lvalue->getQualifiers().isImplicit()) {
      result.push_back(lvalue->getObjectType());
    }
  }

  // FIXME: lots of other cases to consider!
  return result;
}

/// Determine whether the type variables in the two given sets intersect.
static bool
typeVariablesIntersect(ConstraintSystem &cs,
                       SmallVectorImpl<TypeVariableType *> &typeVars1,
                       SmallVectorImpl<TypeVariableType *> &typeVars2) {
  if (typeVars1.empty() || typeVars2.empty())
    return false;

  // Put the representations of the type variables from the first set into
  // a pointer set.
  llvm::SmallPtrSet<TypeVariableType *, 4> typeVars1Set;
  for (auto tv : typeVars1) {
    typeVars1Set.insert(cs.getRepresentative(tv));
  }

  // Check if there are any intersections.
  for (auto tv : typeVars2) {
    if (typeVars1Set.count(cs.getRepresentative(tv)))
      return true;
  }

  return false;
}

void ConstraintSystem::collectConstraintsForTypeVariables(
       SmallVectorImpl<TypeVariableConstraints> &typeVarConstraints,
       SmallVectorImpl<Constraint *> &disjunctions) {
  typeVarConstraints.clear();

  // Provide a mapping from type variable to its constraints. The getTVC
  // function ties together the SmallVector and the DenseMap.
  llvm::SmallDenseMap<TypeVariableType *, unsigned, 8> typeVarConstraintsMap;
  auto getTVC = [&](TypeVariableType *tv) -> TypeVariableConstraints& {
    tv = getRepresentative(tv);
    unsigned& constraintsIdx = typeVarConstraintsMap[tv];
    if (!constraintsIdx) {
      typeVarConstraints.push_back(TypeVariableConstraints(tv));
      constraintsIdx = typeVarConstraints.size();
    }
    return typeVarConstraints[constraintsIdx - 1];
  };

  // First, collect all of the constraints that relate directly to a
  // type variable.
  SmallVector<TypeVariableType *, 8> referencedTypeVars;
  for (auto &constraint : Constraints) {
    Type first;
    if (constraint.getKind() != ConstraintKind::Conjunction &&
        constraint.getKind() != ConstraintKind::Disjunction)
      first = simplifyType(constraint.getFirstType());

    switch (constraint.getClassification()) {
    case ConstraintClassification::Relational:
      // Store conformance constraints separately.
      if (constraint.getKind() == ConstraintKind::ConformsTo ||
          constraint.getKind() == ConstraintKind::SelfObjectOfProtocol) {
        if (auto firstTV = dyn_cast<TypeVariableType>(first.getPointer())) {
          // Record this constraint on the type variable.
          getTVC(firstTV).ConformsToConstraints.push_back(&constraint);
        }
        
        continue;
      }

      if (constraint.getKind() == ConstraintKind::ApplicableFunction) {
        // Applicable function constraints fully bind the type variables on
        // the left-hand side.
        SmallVector<TypeVariableType *, 4> lhsTypeVars;
        first->getTypeVariables(lhsTypeVars);
        for (auto typeVar : lhsTypeVars)
          getTVC(typeVar).FullyBound = true;

        simplifyType(constraint.getSecondType())
          ->getTypeVariables(referencedTypeVars);
        continue;
      }

      // Handle this interesting case below.
      break;

    case ConstraintClassification::TypeProperty:
      if (!isa<TypeVariableType>(first.getPointer())) {
        // Simply mark any type variables in the type as referenced.
        first->getTypeVariables(referencedTypeVars);
      }
      continue;

    case ConstraintClassification::Member: {
      // Collect the type variables from the base type (first) and member
      // type (second).
      SmallVector<TypeVariableType *, 4> baseTypeVars;
      first->getTypeVariables(baseTypeVars);

      SmallVector<TypeVariableType *, 4> memberTypeVars;
      simplifyType(constraint.getSecondType())
        ->getTypeVariables(memberTypeVars);

      // If the set of type variables in the base type does not intersect with
      // the set of the type variables in the member type, the type variables
      // in the member type are fully bound.
      if (!typeVariablesIntersect(*this, baseTypeVars, memberTypeVars)) {
        for (auto typeVar : memberTypeVars)
          getTVC(typeVar).FullyBound = true;
      } else {
        referencedTypeVars.append(memberTypeVars.begin(), memberTypeVars.end());
      }
      continue;
    }

    case ConstraintClassification::Conjunction:
      llvm_unreachable("Conjunction constraints should have been broken apart");

    case ConstraintClassification::Disjunction:
      // Record this disjunction.
      disjunctions.push_back(&constraint);

      // Reference type variables in all of the constraints.
      for (auto dis : constraint.getNestedConstraints()) {
        ArrayRef<Constraint *> innerConstraints;
        if (dis->getKind() == ConstraintKind::Conjunction)
          innerConstraints = dis->getNestedConstraints();
        else
          innerConstraints = dis;

        for (auto inner : innerConstraints) {
          simplifyType(inner->getFirstType())
            ->getTypeVariables(referencedTypeVars);
          if (auto second = inner->getSecondType()) {
            simplifyType(second)->getTypeVariables(referencedTypeVars);
          }
        }
      }
      continue;
    }

    auto second = simplifyType(constraint.getSecondType());

    auto firstTV = first->getAs<TypeVariableType>();
    if (firstTV) {
      // Record the constraint.
      getTVC(firstTV).Above.push_back(std::make_pair(&constraint, second));
    } else {
      // Collect any type variables represented in the first type.
      first->getTypeVariables(referencedTypeVars);
    }

    auto secondTV = second->getAs<TypeVariableType>();
    if (secondTV) {
      // Record the constraint.
      getTVC(secondTV).Below.push_back(std::make_pair(&constraint, first));
    } else {
      // Collect any type variables represented in the second type.
      second->getTypeVariables(referencedTypeVars);
    }

    // If both types are type variables, mark both as referenced.
    if (firstTV && secondTV) {
      referencedTypeVars.push_back(firstTV);
      referencedTypeVars.push_back(secondTV);
    }
  }

  // Mark any referenced type variables as having non-concrete constraints.
  llvm::SmallPtrSet<TypeVariableType *, 8> SeenVars;
  for (auto tv : referencedTypeVars) {
    // If this type variable is redundantly in the list, skip it.
    if (!SeenVars.insert(tv)) continue;

    tv = getRepresentative(tv);
    auto known = typeVarConstraintsMap.find(tv);
    if (known == typeVarConstraintsMap.end())
      continue;

    typeVarConstraints[known->second-1].HasNonConcreteConstraints = true;
  }
}

bool ConstraintSystem::simplify() {
  // If there is a constraint graph, use the worklist implementation.
  if (CG) {
    // The set of constraints that we retired.
    llvm::SetVector<Constraint *> retiredConstraints;

    // While we have a constraint in the worklist, process it.
    while (!Worklist.empty()) {
      // Grab the next constraint from the worklist.
      auto constraint = Worklist.front();
      Worklist.pop_front();
      assert(constraint->isActive() && "Worklist constraint is not active?");

      // Simplify this constraint.
      switch (simplifyConstraint(*constraint)) {
      case SolutionKind::Error:
        if (!failedConstraint) {
          failedConstraint = constraint;
        }
        break;

      case SolutionKind::Solved:
        ++solverState->NumSimplifiedConstraints;

        // This constraint has already been solved; retire it.
        retiredConstraints.insert(constraint);

        // Remove the constraint from the constraint graph.
        CG->removeConstraint(constraint);
        break;

      case SolutionKind::Unsolved:
        ++solverState->NumUnsimplifiedConstraints;
        break;
      }

      // This constraint is not active. We delay this operation until
      // after simplification to avoid re-insertion.
      constraint->setActive(false);

      // Check whether a constraint failed. If so, we're done.
      if (failedConstraint) {
        // Mark all of the remaining constraints in the worklist inactive.
        while (!Worklist.empty()) {
          auto constraint = Worklist.front();
          Worklist.pop_front();
          assert(constraint->isActive()&&"Worklist constraint is not active?");
          constraint->setActive(false);
        }

        // Retire all of the constraints.
        if (solverState)
          solverState->retiredConstraints.splice(
            solverState->retiredConstraints.begin(),
            Constraints);
        else
          Constraints.clear();

        // Clear out the worklist. There's nothing to do now.
        return true;
      }

      // If the current score is worse than the best score we've seen so far,
      // there's no point in continuing. So don't.
      if (worseThanBestSolution())
        return true;
    }

    // Transfer any retired constraints to the retired list.
    for (auto i = Constraints.begin(), end = Constraints.end();
         i != end; /*increment in loop*/) {
      // If it's not retired, do nothing.
      if (retiredConstraints.count(&*i) == 0) {
        ++i;
        continue;
      }

      // If there is no list of retired constraints, just erase it.
      // FIXME: This is weird.
      if (!solverState) {
        i = Constraints.erase(i);
        continue;
      }

      // If we have a list of retired constraints, move it there.
      auto victim = i++;
      solverState->retiredConstraints.splice(solverState->retiredConstraints.begin(),
                                             Constraints,
                                             victim);
    }
    return false;
  }


  bool solvedAny;
  do {
    // Loop through all of the thus-far-unsolved constraints, attempting to
    // simplify each one.
    ConstraintList existingConstraints;
    existingConstraints.splice(existingConstraints.end(), Constraints);
    solvedAny = false;
    while (!existingConstraints.empty()) {
      // Pull the next constraint off the beginning of the list.
      auto constraint = &existingConstraints.front();
      existingConstraints.pop_front();

      if (addConstraint(constraint, false, true)) {
        solvedAny = true;
        ++solverState->NumSimplifiedConstraints;
      } else if (!failedConstraint) {
        ++solverState->NumUnsimplifiedConstraints;
      }

      if (failedConstraint) {
        if (solverState) {
          solverState->retiredConstraints.splice(
            solverState->retiredConstraints.begin(),
            existingConstraints);
        }
        return true;
      }
    }

    ++solverState->NumSimplifyIterations;
  } while (solvedAny);

  // We've simplified all of the constraints we can.
  return false;
}

namespace {

/// \brief Truncate the given small vector to the given new size.
template<typename T>
void truncate(SmallVectorImpl<T> &vec, unsigned newSize) {
  assert(newSize <= vec.size() && "Not a truncation!");
  vec.erase(vec.begin() + newSize, vec.end());
}

} // end anonymous namespace

ConstraintSystem::SolverState::SolverState(ConstraintSystem &cs) : CS(cs) {
  ++NumSolutionAttempts;
  SolutionAttempt = NumSolutionAttempts;

  // If we're supposed to debug a specific constraint solver attempt,
  // turn on debugging now.
  ASTContext &ctx = CS.getTypeChecker().Context;
  LangOptions &langOpts = ctx.LangOpts;
  OldDebugConstraintSolver = langOpts.DebugConstraintSolver;
  if (langOpts.DebugConstraintSolverAttempt &&
      langOpts.DebugConstraintSolverAttempt == SolutionAttempt) {
    langOpts.DebugConstraintSolver = true;
    llvm::raw_ostream &dbgOut = ctx.TypeCheckerDebug->getStream();
    dbgOut << "---Constraint system #" << SolutionAttempt << "---\n";
    CS.dump(dbgOut);
  }
}

ConstraintSystem::SolverState::~SolverState() {
  // Restore debugging state.
  LangOptions &langOpts = CS.getTypeChecker().Context.LangOpts;
  langOpts.DebugConstraintSolver = OldDebugConstraintSolver;

  // Write our local statistics back to the overall statistics.
  #define CS_STATISTIC(Name, Description) JOIN2(Overall,Name) += Name;
  #include "ConstraintSolverStats.def"

  // Update the "largest" statistics if this system is larger than the
  // previous one.  
  // FIXME: This is not at all thread-safe.
  if (NumStatesExplored > LargestNumStatesExplored.Value) {
    LargestSolutionAttemptNumber.Value = SolutionAttempt-1;
    ++LargestSolutionAttemptNumber;
    #define CS_STATISTIC(Name, Description) \
      JOIN2(Largest,Name).Value = Name-1; \
      ++JOIN2(Largest,Name);
    #include "ConstraintSolverStats.def"
  }
}

ConstraintSystem::SolverScope::SolverScope(ConstraintSystem &cs)
  : cs(cs)
{
  ++cs.solverState->depth;

  resolvedOverloadSets = cs.resolvedOverloadSets;
  numTypeVariables = cs.TypeVariables.size();
  numSavedBindings = cs.solverState->savedBindings.size();
  firstRetired = cs.solverState->retiredConstraints.begin();
  numConstraintRestrictions = cs.solverState->constraintRestrictions.size();
  oldGeneratedConstraints = cs.solverState->generatedConstraints;
  cs.solverState->generatedConstraints = &generatedConstraints;
  PreviousScore = cs.CurrentScore;

  ++cs.solverState->NumStatesExplored;

  if (cs.CG)
    CGScope.emplace(*cs.CG);
}

ConstraintSystem::SolverScope::~SolverScope() {
  --cs.solverState->depth;

  // Erase the end of various lists.
  cs.resolvedOverloadSets = resolvedOverloadSets;
  truncate(cs.TypeVariables, numTypeVariables);

  // Restore bindings.
  cs.restoreTypeVariableBindings(cs.solverState->savedBindings.size() -
                                   numSavedBindings);

  // Add the retired constraints back into circulation.
  cs.Constraints.splice(cs.Constraints.end(), 
                        cs.solverState->retiredConstraints,
                        cs.solverState->retiredConstraints.begin(),
                        firstRetired);

  // Remove any constraints that were generated here.
  cs.Constraints.erase_if([&](Constraint &c) {
                            return generatedConstraints.count(&c) > 0;
                          });

  // Remove any constraint restrictions.
  truncate(cs.solverState->constraintRestrictions, numConstraintRestrictions);

  // Reset the prior generated-constraints pointer.
  cs.solverState->generatedConstraints = oldGeneratedConstraints;

  // Reset the previous score.
  cs.CurrentScore = PreviousScore;

  // Clear out other "failed" state.
  cs.failedConstraint = nullptr;
}

namespace {
  struct PotentialBindings {
    /// The set of potential bindings.
    SmallVector<std::pair<Type, bool>, 4> Bindings;

    /// Whether this type variable is fully bound by one of its constraints.
    bool FullyBound = false;

    /// Whether the bindings of this type involve other type variables.
    bool InvolvesTypeVariables = false;

    /// Whether this type variable has literal bindings.
    bool HasLiteralBindings = false;

    /// Determine whether the set of bindings is non-empty.
    explicit operator bool() const {
      return !Bindings.empty();
    }

    /// Compare two sets of bindings, where \c x < y indicates that
    /// \c x is a better set of bindings that \c y.
    friend bool operator<(const PotentialBindings &x, 
                          const PotentialBindings &y) {
      return std::make_tuple(x.FullyBound, x.InvolvesTypeVariables, 
                             x.HasLiteralBindings, -x.Bindings.size())
        < std::make_tuple(y.FullyBound, y.InvolvesTypeVariables, 
                          y.HasLiteralBindings, -y.Bindings.size());
    }
  };
}

/// \brief Retrieve the set of potential type bindings for the given type
/// variable, along with flags indicating whether those types should be
/// opened.
static PotentialBindings getPotentialBindings(ConstraintSystem &cs,
                                              TypeVariableConstraints &tvc) {
  PotentialBindings result;
  result.FullyBound = tvc.FullyBound;
  result.InvolvesTypeVariables = tvc.HasNonConcreteConstraints;
  result.HasLiteralBindings = false;

  llvm::SmallPtrSet<CanType, 4> exactTypes;
  SmallVector<std::pair<Type, bool>, 4> bindings;

  // Add the types below this type variable.
  for (auto arg : tvc.Below) {
    // Make sure we can perform this binding.
    auto type = arg.second;
    if (auto boundTy = checkTypeOfBinding(cs, tvc.TypeVar, type)) {
      type = *boundTy;

      // Check whether the type involves type variables.
      if (type->hasTypeVariable())
        result.InvolvesTypeVariables = true;
    } else {
      // If it's recursive, obviously it involves type variables.
      result.InvolvesTypeVariables = true;
      continue;
    }

    if (exactTypes.insert(type->getCanonicalType()))
      result.Bindings.push_back({type, false});
  }

  // Add the types above this type variable.
  for (auto arg : tvc.Above) {
    // Make sure we can perform this binding.
    auto type = arg.second;
    if (auto boundTy = checkTypeOfBinding(cs, tvc.TypeVar, type)) {
      type = *boundTy;

      // Anything with a type variable in it is not definitive.
      if (type->hasTypeVariable())
        result.InvolvesTypeVariables = true;
    } else {
      // If it's recursive, obviously it involves type variables.
      result.InvolvesTypeVariables = true;
      continue;
    }

    // If this is a conversion to a single-element, non-variadic labelled
    // tuple, just grab the element type.
    if (arg.first->getKind() == ConstraintKind::Conversion ||
        arg.first->getKind() == ConstraintKind::Subtype ||
        arg.first->getKind() == ConstraintKind::TrivialSubtype) {
      if (auto tupleTy = type->getAs<TupleType>())
        if (tupleTy->getNumElements() == 1 &&
            !tupleTy->getFields()[0].isVararg())
          type = tupleTy->getElementType(0);
    }

    if (exactTypes.insert(type->getCanonicalType()))
      result.Bindings.push_back({type, false});
  }

  // When we see conformance to a known protocol, add the default type for
  // that protocol.
  auto &tc = cs.getTypeChecker();
  for (auto constraint : tvc.ConformsToConstraints) {
    if (auto type = tc.getDefaultType(constraint->getProtocol(), cs.DC)) {
      // For non-generic literal types, just check for exact types.
      if (!type->isUnspecializedGeneric()) {
        if (exactTypes.insert(type->getCanonicalType())) {
          result.HasLiteralBindings = true;
          result.Bindings.push_back({type, true});
        }
        continue;
      }

      // For generic literal types, check whether we already have a
      // specialization of this generic within our list.
      auto nominal = type->getAnyNominal();
      bool matched = false;
      for (auto exactType : exactTypes) {
        if (auto exactNominal = exactType->getAnyNominal()) {
          // FIXME: Check parents?
          if (nominal == exactNominal) {
            matched = true;
            break;
          }
        }
      }

      if (!matched) {
        result.HasLiteralBindings = true;
        exactTypes.insert(type->getCanonicalType());
        result.Bindings.push_back({type, true});
      }
    }
  }

  // FIXME: Minimize type bindings here by removing types that are supertypes
  // of others in the list.

  return result;
}

/// \brief Try each of the given type variable bindings to find solutions
/// to the given constraint system.
///
/// \param cs The constraint system we're solving in.
/// \param depth The depth of the solution stack.
/// \param tvc The type variable and its constraints that we're solving for.
/// \param bindings The initial set of bindings to explore.
/// \param solutions The set of solutions.
///
/// \returns true if there are no solutions.
static bool tryTypeVariableBindings(
              ConstraintSystem &cs,
              unsigned depth,
              TypeVariableConstraints &tvc,
              ArrayRef<std::pair<Type, bool>> bindings,
              SmallVectorImpl<Solution> &solutions,
              FreeTypeVariableBinding allowFreeTypeVariables) {
  auto typeVar = tvc.TypeVar;
  bool anySolved = false;
  llvm::SmallPtrSet<CanType, 4> exploredTypes;

  SmallVector<std::pair<Type, bool>, 4> storedBindings;
  auto &tc = cs.getTypeChecker();
  ++cs.solverState->NumTypeVariablesBound;

  for (unsigned tryCount = 0; !anySolved && !bindings.empty(); ++tryCount) {
    // Try each of the bindings in turn.
    ++cs.solverState->NumTypeVariableBindings;
    bool sawFirstLiteralConstraint = false;
    for (auto binding : bindings) {
      auto type = binding.first;

      // If the type variable can't bind to an lvalue, make sure the
      // type we pick isn't an lvalue.
      if (!typeVar->getImpl().canBindToLValue())
        type = type->getRValueType();

      if (tc.getLangOpts().DebugConstraintSolver) {
        auto &log = cs.getASTContext().TypeCheckerDebug->getStream();
        log.indent(depth * 2)
          << "(trying " << typeVar->getString()
          << " := " << type->getString() << "\n";
      }

      // Try to solve the system with typeVar := type
      ConstraintSystem::SolverScope scope(cs);
      if (binding.second) {
        // FIXME: If we were able to solve this without considering
        // default literals, don't bother looking at default literals.
        if (!sawFirstLiteralConstraint) {
          sawFirstLiteralConstraint = true;
          if (anySolved)
            break;
        }
        type = cs.openBindingType(type);
      }

      cs.addConstraint(ConstraintKind::Bind, typeVar, type);
      if (!cs.solve(solutions, allowFreeTypeVariables))
        anySolved = true;

      if (tc.getLangOpts().DebugConstraintSolver) {
        auto &log = cs.getASTContext().TypeCheckerDebug->getStream();
        log.indent(depth * 2) << ")\n";
      }
    }

    // If we found any solution, we're done.
    if (anySolved)
      break;

    // None of the children had solutions, enumerate supertypes and
    // try again.
    SmallVector<std::pair<Type, bool>, 4> newBindings;

    // Check whether this was our first attempt.
    if (tryCount == 0) {
      // Note which bindings we already visited.
      for (auto binding : bindings)
        exploredTypes.insert(binding.first->getCanonicalType());

      // Find types that conform to each of the protocols to which this
      // type variable must conform.
      // FIXME: We don't want to visit the supertypes of this type.
      for (auto constraint : tvc.ConformsToConstraints) {
        auto proto = constraint->getProtocol();

        // Only do this for protocols that have default types, i.e., protocols
        // for literals.
        if (!tc.getDefaultType(proto, cs.DC))
          continue;

        KnownProtocolKind knownKind = *proto->getKnownProtocolKind();
        for (auto type : cs.getAlternativeLiteralTypes(knownKind)) {
          if (exploredTypes.insert(type->getCanonicalType()))
            newBindings.push_back({type, true});
        }
      }

      // If we found any new bindings, try them now.
      if (!newBindings.empty()) {
        // We have a new set of bindings; use them for our next loop.
        storedBindings = std::move(newBindings);
        bindings = storedBindings;
        continue;
      }
    }

    // Enumerate the supertypes of each of the types we tried.
    for (auto binding : bindings) {
      auto type = binding.first;

      for (auto supertype : cs.enumerateDirectSupertypes(type)) {
        // If we're not allowed to try this binding, skip it.
        auto simpleSuper = checkTypeOfBinding(cs, typeVar, supertype);
        if (!simpleSuper)
          continue;

        // If we haven't seen this supertype, add it.
        if (exploredTypes.insert((*simpleSuper)->getCanonicalType()))
          newBindings.push_back({*simpleSuper, false});
      }
    }

    // If we didn't compute any new bindings, we're done.
    if (newBindings.empty())
      break;

    // We have a new set of bindings; use them for our next loop.
    storedBindings = std::move(newBindings);
    bindings = storedBindings;
  }

  return !anySolved;
}

bool ConstraintSystem::solve(SmallVectorImpl<Solution> &solutions,
                             FreeTypeVariableBinding allowFreeTypeVariables) {
  // If there is no solver state, this is the top-level call. Create solver
  // state and begin recursion.
  if (!solverState) {
    // Set up solver state.
    SolverState state(*this);
    this->solverState = &state;

    // Solve the system.
    solve(solutions, allowFreeTypeVariables);

    // If there is more than one viable system, attempt to pick the best
    // solution.
    if (solutions.size() > 1) {
      if (auto best = findBestSolution(solutions, /*minimize=*/false)) {
        if (*best != 0)
          solutions[0] = std::move(solutions[*best]);
        solutions.erase(solutions.begin() + 1, solutions.end());
      }
    }

    // Remove the solver state.
    this->solverState = nullptr;
    return solutions.size() != 1;
  }

  // If we already failed, or simplification fails, we're done.
  if (failedConstraint || simplify())
    return true;

  // If there are no constraints remaining, we're done. Save this solution.
  if (Constraints.empty()) {
    // If this solution is worse than the best solution we've seen so far,
    // skipt it.
    if (worseThanBestSolution())
      return true;

    // If any free type variables remain and we're not allowed to have them,
    // fail.
    if (allowFreeTypeVariables == FreeTypeVariableBinding::Disallow &&
        hasFreeTypeVariables())
      return true;

    auto solution = finalize(allowFreeTypeVariables);
    if (TC.getLangOpts().DebugConstraintSolver) {
      auto &log = getASTContext().TypeCheckerDebug->getStream();
      log.indent(solverState->depth * 2)
        << "(found solution " << CurrentScore << ")\n";
    }

    solutions.push_back(std::move(solution));
    return false;
  }

  // If there's no global constraint graph, just simplify all of the
  // constraints.
  Optional<ConstraintGraph> localCG;
  if (!CG) {
    return solveSimplified(solutions, allowFreeTypeVariables);
  }

  // Compute the connected components of the constraint graph.
  // FIXME: We're seeding typeVars with TypeVariables so that the
  // connected-components algorithm only considers those type variables within
  // our component. There are clearly better ways to do this.
  SmallVector<TypeVariableType *, 16> typeVars(TypeVariables);
  SmallVector<unsigned, 16> components;
  unsigned numComponents = CG->computeConnectedComponents(typeVars, components);

  // If we don't have more than one component, just solve the whole
  // system.
  if (numComponents < 2) {
    return solveSimplified(solutions, allowFreeTypeVariables);
  }

  if (TC.Context.LangOpts.DebugConstraintSolver) {
    auto &log = getASTContext().TypeCheckerDebug->getStream();

    // Verify that the constraint graph is valid.
    CG->verify();

    log << "---Constraint graph---\n";
    CG->print(log);

    log << "---Connected components---\n";
    CG->printConnectedComponents(log);
  }

  // Construct a mapping from type variables and constraints to their
  // owning component.
  llvm::DenseMap<TypeVariableType *, unsigned> typeVarComponent;
  llvm::DenseMap<Constraint *, unsigned> constraintComponent;
  for (unsigned i = 0, n = typeVars.size(); i != n; ++i) {
    // Record the component of this type variable.
    typeVarComponent[typeVars[i]] = components[i];

    // Record the component of each of the constraints.
    for (auto constraint : (*CG)[typeVars[i]].getConstraints())
      constraintComponent[constraint] = components[i];
  }

  // Sort the constraints into buckets based on component number.
  std::unique_ptr<ConstraintList[]> constraintBuckets(
                                      new ConstraintList[numComponents]);
  while (!Constraints.empty()) {
    auto constraint = &Constraints.front();
    Constraints.pop_front();
    constraintBuckets[constraintComponent[constraint]].push_back(constraint);
  }

  // Function object that returns all constraints placed into buckets
  // back to the list of constraints.
  auto returnAllConstraints = [&] {
    assert(Constraints.empty() && "Already have constraints?");
    for (unsigned component = 0; component != numComponents; ++component) {
      Constraints.splice(Constraints.end(), constraintBuckets[component]);
    }
  };

  // Compute the partial solutions produced for each connected component.
  std::unique_ptr<SmallVector<Solution, 4>[]> 
    partialSolutions(new SmallVector<Solution, 4>[numComponents]);
  Optional<Score> PreviousBestScore = solverState->BestScore;
  for (unsigned component = 0; component != numComponents; ++component) {
    assert(Constraints.empty() && "Some constraints were not transferred?");
    ++solverState->NumComponentsSplit;

    // Collect the constraints for this component.
    Constraints.splice(Constraints.end(), constraintBuckets[component]);

    // Collect the type variables that are not part of a different
    // component; this includes type variables that are part of the
    // component as well as already-resolved type variables.
    // FIXME: The latter could be avoided if we had already
    // substituted all of those other type variables through.
    llvm::SmallVector<TypeVariableType *, 16> allTypeVariables 
      = std::move(TypeVariables);
    for (auto typeVar : allTypeVariables) {
      auto known = typeVarComponent.find(typeVar);
      if (known != typeVarComponent.end() && known->second != component)
        continue;

      TypeVariables.push_back(typeVar);
    }
    
    // Solve for this component. If it fails, we're done.
    bool failed;
    if (TC.getLangOpts().DebugConstraintSolver) {
      auto &log = getASTContext().TypeCheckerDebug->getStream();
      log.indent(solverState->depth * 2) << "(solving component #" 
                                         << component << "\n";
    }
    {
      SolverScope scope(*this);
      failed = solveSimplified(partialSolutions[component], 
                               allowFreeTypeVariables);
    }

    // Put the constraints back into their original bucket.
    auto &bucket = constraintBuckets[component];
    bucket.splice(bucket.end(), Constraints);
    
    if (failed) {
      if (TC.getLangOpts().DebugConstraintSolver) {
        auto &log = getASTContext().TypeCheckerDebug->getStream();
        log.indent(solverState->depth * 2) << "failed component #" 
                                           << component << ")\n";
      }
      
      TypeVariables = std::move(allTypeVariables);
      returnAllConstraints();
      return true;
    }

    if (TC.getLangOpts().DebugConstraintSolver) {
      auto &log = getASTContext().TypeCheckerDebug->getStream();
      log.indent(solverState->depth * 2) << "finised component #" 
                                         << component << ")\n";
    }
    
    assert(!partialSolutions[component].empty() &&" No solutions?");

    // Move the type variables back, clear out constraints; we're
    // ready for the next component.
    TypeVariables = std::move(allTypeVariables);

    // For each of the partial solutions, substract off the current score.
    // It doesn't contribute.
    for (auto &solution : partialSolutions[component])
      solution.getFixedScore() -= CurrentScore;

    // Restore the previous best score.
    solverState->BestScore = PreviousBestScore;
  }

  // Move the constraints back. The system is back in a normal state.
  returnAllConstraints();

  // When there are multiple partial solutions for a given connected component,
  // rank those solutions to pick the best ones. This limits the number of
  // combinations we need to produce; in the common case, down to a single
  // combination.
  for (unsigned component = 0; component != numComponents; ++component) {
    auto &solutions = partialSolutions[component];
    // If there's a single best solution, keep only that one.
    // Otherwise, the set of solutions will at least have been minimized.
    if (auto best = findBestSolution(solutions, /*minimize=*/true)) {
      if (*best > 0)
        solutions[0] = std::move(solutions[*best]);
      solutions.erase(solutions.begin() + 1, solutions.end());
    }
  }

  // Produce all combinations of partial solutions.
  SmallVector<unsigned, 2> indices(numComponents, 0);
  bool done = false;
  bool anySolutions = false;
  do {
    // Create a new solver scope in which we apply all of the partial
    // solutions.
    SolverScope scope(*this);
    for (unsigned i = 0; i != numComponents; ++i)
      applySolution(partialSolutions[i][indices[i]]);

    // This solution might be worse than the best solution found so far. If so,
    // skip it.
    if (!worseThanBestSolution()) {
      // Finalize this solution.
      auto solution = finalize(allowFreeTypeVariables);
      if (TC.getLangOpts().DebugConstraintSolver) {
        auto &log = getASTContext().TypeCheckerDebug->getStream();
        log.indent(solverState->depth * 2)
          << "(composed solution " << CurrentScore << ")\n";
      }

      // Save this solution.
      solutions.push_back(std::move(solution));

      anySolutions = true;
    }
    
    // Find the next combination.
    for (unsigned n = numComponents; n > 0; --n) {
      ++indices[n-1];

      // If we haven't run out of solutions yet, we're done.
      if (indices[n-1] < partialSolutions[n-1].size())
        break;

      // If we ran out of solutions at the first position, we're done.
      if (n == 1) {
        done = true;
        break;
      } 

      // Zero out the indices from here to the end.
      for (unsigned i = n-1; i != numComponents; ++i)
        indices[i] = 0;
    }
  } while (!done);

  return !anySolutions;
}

bool ConstraintSystem::solveSimplified(
       SmallVectorImpl<Solution> &solutions,
       FreeTypeVariableBinding allowFreeTypeVariables) {
  // Collect the type variable constraints.
  SmallVector<TypeVariableConstraints, 4> typeVarConstraints;
  SmallVector<Constraint *, 4> disjunctions;
  collectConstraintsForTypeVariables(typeVarConstraints, disjunctions);
  if (!typeVarConstraints.empty()) {
    // Look for the best type variable to bind.
    unsigned bestTypeVarIndex = 0;
    auto bestBindings = getPotentialBindings(*this, typeVarConstraints[0]);
    for (unsigned i = 1, n = typeVarConstraints.size(); i != n; ++i) {
      auto bindings = getPotentialBindings(*this, typeVarConstraints[i]);
      if (!bindings)
        continue;
      
      // Prefer type variables whose bindings don't involve type variables or,
      // if neither involves type variables, those with fewer bindings.
      if (!bestBindings || bindings < bestBindings) {
        bestTypeVarIndex = i;
        bestBindings = std::move(bindings);
      }
    }

    // If we have a binding that does not involve type variables, or we have
    // no other option, go ahead and try the bindings for this type variable.
    if (bestBindings && 
        (disjunctions.empty() ||
         (!bestBindings.InvolvesTypeVariables && !bestBindings.FullyBound))) {
      return tryTypeVariableBindings(*this,
                                     solverState->depth,
                                     typeVarConstraints[bestTypeVarIndex],
                                     bestBindings.Bindings,
                                     solutions,
                                     allowFreeTypeVariables);
    }

    // Fall through to resolve an overload set.
  }

  // If there are no disjunctions, we can't solve this system.
  if (disjunctions.empty()) {
    // If the only remaining constraints are conformance constraints
    // or member equality constraints, and we're allowed to have free
    // variables, we still have a solution.  FIXME: It seems like this
    // should be easier to detect. Aren't there other kinds of
    // constraints that could show up here?
    if (allowFreeTypeVariables != FreeTypeVariableBinding::Disallow &&
        hasFreeTypeVariables()) {
      bool anyNonConformanceConstraints = false;
      for (auto &constraint : Constraints) {
        if (constraint.getKind() == ConstraintKind::ConformsTo ||
            constraint.getKind() == ConstraintKind::SelfObjectOfProtocol)
          continue;

        if (constraint.getKind() == ConstraintKind::TypeMember)
          continue;

        anyNonConformanceConstraints = true;
        break;
      }

      // If this solution is worse than the best solution we've seen so far,
      // skipt it.
      if (worseThanBestSolution())
        return true;

      if (!anyNonConformanceConstraints) {
        auto solution = finalize(allowFreeTypeVariables);
        if (TC.getLangOpts().DebugConstraintSolver) {
          auto &log = getASTContext().TypeCheckerDebug->getStream();
          log.indent(solverState->depth * 2) << "(found solution)\n";
        }
        
        solutions.push_back(std::move(solution));
        return false;
      }
    }
    return true;
  }

  // Pick the smallest disjunction.
  // FIXME: This heuristic isn't great, but it helped somewhat for
  // overload sets.
  auto disjunction = disjunctions[0];
  auto bestSize = disjunction->getNestedConstraints().size();
  if (bestSize > 2) {
    for (auto contender : llvm::makeArrayRef(disjunctions).slice(1)) {
      unsigned newSize = contender->getNestedConstraints().size();
      if (newSize < bestSize) {
        bestSize = newSize;
        disjunction = contender;

        if (bestSize == 2)
          break;
      }
    }
  }

  // Remove this disjunction constraint from the list.
  auto afterDisjunction = Constraints.erase(disjunction);
  if (CG)
    CG->removeConstraint(disjunction);

  // Try each of the constraints within the disjunction.
  bool anySolved = false;
  ++solverState->NumDisjunctions;
  for (auto constraint : disjunction->getNestedConstraints()) {
    // These kinds of conversions should be avoided if we've already found a
    // solution.
    // FIXME: Generalize this!
    if (anySolved) {
      if (auto restriction = constraint->getRestriction()) {
        if (*restriction == ConversionRestrictionKind::OptionalToOptional)
          break;
      }
    }

    // Try to solve the system with this option in the disjunction.
    SolverScope scope(*this);
    ++solverState->NumDisjunctionTerms;
    if (TC.getLangOpts().DebugConstraintSolver) {
      auto &log = getASTContext().TypeCheckerDebug->getStream();
      log.indent(solverState->depth * 2)
        << "(assuming ";
      constraint->print(log, &TC.Context.SourceMgr);
      log << '\n';
    }

    // Simplify this term in the disjunction.
    switch (simplifyConstraint(*constraint)) {
    case SolutionKind::Error:
      if (!failedConstraint)
        failedConstraint = constraint;
      break;

    case SolutionKind::Solved:
      break;

    case SolutionKind::Unsolved:
      Constraints.push_back(constraint);
      if (CG)
        CG->addConstraint(constraint);
      break;
    }

    // Record this as a generated constraint.
    solverState->generatedConstraints->insert(constraint);

    if (!solve(solutions, allowFreeTypeVariables)) {
      anySolved = true;

      // If we see a tuple-to-tuple conversion that succeeded, we're done.
      // FIXME: This should be more general.
      if (auto restriction = constraint->getRestriction()) {
        if (*restriction == ConversionRestrictionKind::TupleToTuple)
          break;
      }

      // Or, if we see a conversion successfully applied to a string
      // interpolation argument, we're done.
      // FIXME: Probably should be more general, as mentioned above.
      if (auto locator = disjunction->getLocator()) {
        if (!locator->getPath().empty() &&
            locator->getPath().back().getKind()
              == ConstraintLocator::InterpolationArgument &&
            constraint->getKind() == ConstraintKind::Conversion)
          break;
      }
    }

    if (TC.getLangOpts().DebugConstraintSolver) {
      auto &log = getASTContext().TypeCheckerDebug->getStream();
      log.indent(solverState->depth * 2) << ")\n";
    }
  }

  // Put the disjunction constraint back in its place.
  Constraints.insert(afterDisjunction, disjunction);
  if (CG)
    CG->addConstraint(disjunction);

  return !anySolved;
}
