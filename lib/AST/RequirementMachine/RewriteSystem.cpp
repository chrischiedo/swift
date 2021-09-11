//===--- RewriteSystem.cpp - Generics with term rewriting -----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Decl.h"
#include "swift/AST/Types.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <vector>
#include "ProtocolGraph.h"
#include "RewriteContext.h"
#include "RewriteSystem.h"

using namespace swift;
using namespace rewriting;

void Rule::dump(llvm::raw_ostream &out) const {
  out << LHS << " => " << RHS;
  if (deleted)
    out << " [deleted]";
}

void RewritePath::invert() {
  std::reverse(Steps.begin(), Steps.end());

  for (auto &step : Steps)
    step.invert();
}

/// Dumps the rewrite step that was applied to \p term. Mutates \p term to
/// reflect the application of the rule.
void RewriteStep::dump(llvm::raw_ostream &out,
                       MutableTerm &term,
                       const RewriteSystem &system) const {
  const auto &rule = system.getRule(RuleID);

  auto lhs = (Inverse ? rule.getRHS() : rule.getLHS());
  auto rhs = (Inverse ? rule.getLHS() : rule.getRHS());

  assert(std::equal(term.begin() + Offset,
                    term.begin() + Offset + lhs.size(),
                    lhs.begin()));

  MutableTerm prefix(term.begin(), term.begin() + Offset);
  MutableTerm suffix(term.begin() + Offset + lhs.size(), term.end());

  if (!prefix.empty()) {
    out << prefix;
    out << ".";
  }
  out << "(" << rule.getLHS();
  out << (Inverse ? " <= " : " => ");
  out << rule.getRHS() << ")";
  if (!suffix.empty()) {
    out << ".";
    out << suffix;
  }

  term = prefix;
  term.append(rhs);
  term.append(suffix);
}

/// Dumps a series of rewrite steps applied to \p term.
void RewritePath::dump(llvm::raw_ostream &out,
                       MutableTerm term,
                       const RewriteSystem &system) const {
  bool first = true;

  for (const auto &step : Steps) {
    if (!first) {
      out << " ⊗ ";
    } else {
      first = false;
    }

    step.dump(out, term, system);
  }
}

RewriteSystem::RewriteSystem(RewriteContext &ctx)
    : Context(ctx), Debug(ctx.getDebugOptions()) {}

RewriteSystem::~RewriteSystem() {
  Trie.updateHistograms(Context.RuleTrieHistogram,
                        Context.RuleTrieRootHistogram);
}

void RewriteSystem::initialize(
    std::vector<std::pair<MutableTerm, MutableTerm>> &&rules,
    ProtocolGraph &&graph) {
  Protos = graph;

  for (const auto &rule : rules)
    addRule(rule.first, rule.second);
}

Symbol RewriteSystem::simplifySubstitutionsInSuperclassOrConcreteSymbol(
    Symbol symbol) const {
  return symbol.transformConcreteSubstitutions(
    [&](Term term) -> Term {
      MutableTerm mutTerm(term);
      if (!simplify(mutTerm))
        return term;

      return Term::get(mutTerm, Context);
    }, Context);
}

/// Adds a rewrite rule, returning true if the new rule was non-trivial.
///
/// If both sides simplify to the same term, the rule is trivial and discarded,
/// and this method returns false.
///
/// If \p path is non-null, the new rule is derived from existing rules in the
/// rewrite system; the path records a series of rewrite steps which transform
/// \p lhs to \p rhs.
bool RewriteSystem::addRule(MutableTerm lhs, MutableTerm rhs,
                            const RewritePath *path) {
  assert(!lhs.empty());
  assert(!rhs.empty());

  if (Debug.contains(DebugFlags::Add)) {
    llvm::dbgs() << "# Adding rule " << lhs << " == " << rhs << "\n\n";
  }

  // Now simplify both sides as much as possible with the rules we have so far.
  //
  // This avoids unnecessary work in the completion algorithm.
  RewritePath lhsPath;
  RewritePath rhsPath;

  simplify(lhs, &lhsPath);
  simplify(rhs, &rhsPath);

  RewritePath loop;
  if (path) {
    // Produce a path from the simplified lhs to the simplified rhs.

    // (1) First, apply lhsPath in reverse to produce the original lhs.
    lhsPath.invert();
    loop.append(lhsPath);

    // (2) Now, apply the path from the original lhs to the original rhs
    // given to us by the completion procedure.
    loop.append(*path);

    // (3) Finally, apply rhsPath to produce the simplified rhs, which
    // is the same as the simplified lhs.
    loop.append(rhsPath);
  }

  // If the left hand side and right hand side are already equivalent, we're
  // done.
  int result = lhs.compare(rhs, Protos);
  if (result == 0) {
    // If this rule is a consequence of existing rules, add a homotopy
    // generator.
    if (path) {
      // We already have a loop, since the simplified lhs is identical to the
      // simplified rhs.
      HomotopyGenerators.emplace_back(lhs, loop);

      if (Debug.contains(DebugFlags::Add)) {
        llvm::dbgs() << "## Recorded trivial loop at " << lhs << ": ";
        loop.dump(llvm::dbgs(), lhs, *this);
        llvm::dbgs() << "\n\n";
      }
    }

    return false;
  }

  // Orient the two terms so that the left hand side is greater than the
  // right hand side.
  if (result < 0) {
    std::swap(lhs, rhs);
    loop.invert();
  }

  assert(lhs.compare(rhs, Protos) > 0);

  if (Debug.contains(DebugFlags::Add)) {
    llvm::dbgs() << "## Simplified and oriented rule " << lhs << " => " << rhs << "\n\n";
  }

  unsigned newRuleID = Rules.size();

  auto uniquedLHS = Term::get(lhs, Context);
  auto uniquedRHS = Term::get(rhs, Context);
  Rules.emplace_back(uniquedLHS, uniquedRHS);

  if (path) {
    // We have a rewrite path from the simplified lhs to the simplified rhs;
    // add a rewrite step applying the new rule in reverse to close the loop.
    loop.add(RewriteStep(/*offset=*/0, newRuleID, /*inverse=*/true));
    HomotopyGenerators.emplace_back(lhs, loop);

    if (Debug.contains(DebugFlags::Add)) {
      llvm::dbgs() << "## Recorded non-trivial loop at " << lhs << ": ";
      loop.dump(llvm::dbgs(), lhs, *this);
      llvm::dbgs() << "\n\n";
    }
  }

  auto oldRuleID = Trie.insert(lhs.begin(), lhs.end(), newRuleID);
  if (oldRuleID) {
    llvm::errs() << "Duplicate rewrite rule!\n";
    const auto &oldRule = getRule(*oldRuleID);
    llvm::errs() << "Old rule #" << *oldRuleID << ": ";
    oldRule.dump(llvm::errs());
    llvm::errs() << "\nTrying to replay what happened when I simplified this term:\n";
    Debug |= DebugFlags::Simplify;
    MutableTerm term = lhs;
    simplify(lhs);

    abort();
  }

  checkMergedAssociatedType(uniquedLHS, uniquedRHS);

  // Tell the caller that we added a new rule.
  return true;
}

/// Reduce a term by applying all rewrite rules until fixed point.
///
/// If \p path is non-null, records the series of rewrite steps taken.
bool RewriteSystem::simplify(MutableTerm &term, RewritePath *path) const {
  bool changed = false;

  MutableTerm original;
  RewritePath forDebug;
  if (Debug.contains(DebugFlags::Simplify)) {

    original = term;
    if (!path)
      path = &forDebug;
  }

  while (true) {
    bool tryAgain = false;

    auto from = term.begin();
    auto end = term.end();
    while (from < end) {
      auto ruleID = Trie.find(from, end);
      if (ruleID) {
        const auto &rule = getRule(*ruleID);
        if (!rule.isDeleted()) {
          auto to = from + rule.getLHS().size();
          assert(std::equal(from, to, rule.getLHS().begin()));

          term.rewriteSubTerm(from, to, rule.getRHS());

          if (path) {
            unsigned offset = (unsigned)(from - term.begin());
            path->add(RewriteStep(offset, *ruleID, /*inverse=*/false));
          }

          changed = true;
          tryAgain = true;
          break;
        }
      }

      ++from;
    }

    if (!tryAgain)
      break;
  }

  if (Debug.contains(DebugFlags::Simplify)) {
    if (changed) {
      llvm::dbgs() << "= Simplified " << term << ": ";
      forDebug.dump(llvm::dbgs(), original, *this);
      llvm::dbgs() << "\n";
    } else {
      llvm::dbgs() << "= Irreducible term: " << term << "\n";
    }
  }

  assert(path == nullptr || changed != path->empty());
  return changed;
}

/// Delete any rules whose left hand sides can be reduced by other rules,
/// and reduce the right hand sides of all remaining rules as much as
/// possible.
///
/// Must be run after the completion procedure, since the deletion of
/// rules is only valid to perform if the rewrite system is confluent.
void RewriteSystem::simplifyRewriteSystem() {
  for (unsigned ruleID = 0, e = Rules.size(); ruleID < e; ++ruleID) {
    auto &rule = getRule(ruleID);
    if (rule.isDeleted())
      continue;

    // First, see if the left hand side of this rule can be reduced using
    // some other rule.
    auto lhs = rule.getLHS();
    auto begin = lhs.begin();
    auto end = lhs.end();
    while (begin < end) {
      if (auto otherRuleID = Trie.find(begin++, end)) {
        // A rule does not obsolete itself.
        if (*otherRuleID == ruleID)
          continue;

        // Ignore other deleted rules.
        if (getRule(*otherRuleID).isDeleted())
          continue;

        if (Debug.contains(DebugFlags::Completion)) {
          const auto &otherRule = getRule(ruleID);
          llvm::dbgs() << "$ Deleting rule " << rule << " because "
                       << "its left hand side contains " << otherRule
                       << "\n";
        }

        rule.markDeleted();
        break;
      }
    }

    // If the rule was deleted above, skip the rest.
    if (rule.isDeleted())
      continue;

    // Now, try to reduce the right hand side.
    RewritePath rhsPath;
    MutableTerm rhs(rule.getRHS());
    if (!simplify(rhs, &rhsPath))
      continue;

    // We're adding a new rule, so the old rule won't apply anymore.
    rule.markDeleted();

    unsigned newRuleID = Rules.size();

    // Add a new rule with the simplified right hand side.
    Rules.emplace_back(lhs, Term::get(rhs, Context));
    auto oldRuleID = Trie.insert(lhs.begin(), lhs.end(), newRuleID);
    assert(oldRuleID == ruleID);
    (void) oldRuleID;

    // Produce a loop at the simplified rhs.
    RewritePath loop;

    // (1) First, apply rhsPath in reverse to produce the original rhs.
    rhsPath.invert();
    loop.append(rhsPath);

    // (2) Next, apply the original rule in reverse to produce the
    // original lhs.
    loop.add(RewriteStep(/*offset=*/0, ruleID, /*inverse=*/true));

    // (3) Finally, apply the new rule to produce the simplified rhs.
    loop.add(RewriteStep(/*offset=*/0, newRuleID, /*inverse=*/false));

    if (Debug.contains(DebugFlags::Completion)) {
      llvm::dbgs() << "$ Right hand side simplification recorded a loop: ";
      loop.dump(llvm::dbgs(), rhs, *this);
    }

    HomotopyGenerators.emplace_back(rhs, loop);
  }
}

void RewriteSystem::verify() const {
#ifndef NDEBUG

#define ASSERT_RULE(expr) \
  if (!(expr)) { \
    llvm::errs() << "&&& Malformed rewrite rule: " << rule << "\n"; \
    llvm::errs() << "&&& " << #expr << "\n\n"; \
    dump(llvm::errs()); \
    assert(expr); \
  }

  for (const auto &rule : Rules) {
    if (rule.isDeleted())
      continue;

    const auto &lhs = rule.getLHS();
    const auto &rhs = rule.getRHS();

    for (unsigned index : indices(lhs)) {
      auto symbol = lhs[index];

      if (index != lhs.size() - 1) {
        ASSERT_RULE(symbol.getKind() != Symbol::Kind::Layout);
        ASSERT_RULE(!symbol.isSuperclassOrConcreteType());
      }

      if (index != 0) {
        ASSERT_RULE(symbol.getKind() != Symbol::Kind::GenericParam);
      }

      if (index != 0 && index != lhs.size() - 1) {
        ASSERT_RULE(symbol.getKind() != Symbol::Kind::Protocol);
      }
    }

    for (unsigned index : indices(rhs)) {
      auto symbol = rhs[index];

      // FIXME: This is only true if the input requirements were valid.
      // On invalid code, we'll need to skip this assertion (and instead
      // assert that we diagnosed an error!)
      ASSERT_RULE(symbol.getKind() != Symbol::Kind::Name);

      ASSERT_RULE(symbol.getKind() != Symbol::Kind::Layout);
      ASSERT_RULE(!symbol.isSuperclassOrConcreteType());

      if (index != 0) {
        ASSERT_RULE(symbol.getKind() != Symbol::Kind::GenericParam);
        ASSERT_RULE(symbol.getKind() != Symbol::Kind::Protocol);
      }
    }

    auto lhsDomain = lhs.getRootProtocols();
    auto rhsDomain = rhs.getRootProtocols();

    ASSERT_RULE(lhsDomain == rhsDomain);
  }

#undef ASSERT_RULE
#endif
}

void RewriteSystem::dump(llvm::raw_ostream &out) const {
  out << "Rewrite system: {\n";
  for (const auto &rule : Rules) {
    out << "- " << rule << "\n";
  }
  out << "}\n";
  out << "Homotopy generators: {\n";
  for (const auto &loop : HomotopyGenerators) {
    out << "- " << loop.first << ": ";
    loop.second.dump(out, loop.first, *this);
    out << "\n";
  }
  out << "}\n";
}
