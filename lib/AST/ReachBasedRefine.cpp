/*
 * Copyright (c) 2021-present, Trail of Bits, Inc.
 * All rights reserved.
 *
 * This source code is licensed in accordance with the terms specified in
 * the LICENSE file found in the root directory of this source tree.
 */

#include "rellic/AST/ReachBasedRefine.h"

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "rellic/AST/Util.h"

namespace rellic {

ReachBasedRefine::ReachBasedRefine(Provenance &provenance, clang::ASTUnit &unit)
    : TransformVisitor<ReachBasedRefine>(provenance, unit),
      z3_ctx(new z3::context()),
      z3_gen(new rellic::Z3ConvVisitor(unit, z3_ctx.get())) {}

z3::expr ReachBasedRefine::GetZ3Cond(clang::IfStmt *ifstmt) {
  auto cond = ifstmt->getCond();
  auto expr = z3_gen->Z3BoolCast(z3_gen->GetOrCreateZ3Expr(cond));
  return expr.simplify();
}

bool ReachBasedRefine::VisitCompoundStmt(clang::CompoundStmt *compound) {
  std::vector<clang::Stmt *> body{compound->body_begin(), compound->body_end()};
  std::vector<clang::IfStmt *> ifs;
  z3::expr_vector conds{*z3_ctx};

  auto ResetChain = [&]() {
    ifs.clear();
    conds.resize(0);
  };

  bool done_something{false};
  for (size_t i{0}; i < body.size() && !done_something; ++i) {
    auto if_stmt{clang::dyn_cast<clang::IfStmt>(body[i])};
    if (!if_stmt) {
      ResetChain();
      continue;
    }

    ifs.push_back(if_stmt);
    auto cond{GetZ3Cond(if_stmt)};

    if (if_stmt->getElse()) {
      // We cannot link `if` statements that contain `else` branches
      ResetChain();
      continue;
    }

    // Is the current `if` statement unreachable from all the others?
    bool is_unreachable{Prove(*z3_ctx, !(cond && z3::mk_or(conds)))};

    if (!is_unreachable) {
      ResetChain();
      continue;
    }

    conds.push_back(GetZ3Cond(if_stmt));

    // Do the collected statements cover all possibilities?
    auto is_complete{Prove(*z3_ctx, z3::mk_or(conds))};

    if (ifs.size() <= 2 || !is_complete) {
      // We need to collect more statements
      continue;
    }

    /*
    `body` will look like this at this point:

      ...
      i - n    : ...
      i - n + 1: if(cond_1) { }
      i - n + 2: if(cond_2) { }
      ...
      i - 1    : if(cond_n-1) { }
      i        : if(cond_n) { }
      ...

    and we want to chain all of the statements together:
      ...
      i - n    : ...
      i - n + 1: if(cond_1) { } else if(cond_2) { } ... else if(cond_n) { }
      i - n + 2: if(cond_2) { } else if(cond_3) { } ... else if(cond_n) { }
      ...
      i - 1    : if(cond_n-1) { } else if(cond_n) { }
      i        : if(cond_n) { }
      ...
    */
    auto last_if{ifs[0]};
    for (auto stmt : ifs) {
      if (stmt == ifs.front()) {
        continue;
      }
      if (stmt == ifs.back()) {
        last_if->setElse(stmt->getThen());
      } else {
        last_if->setElse(stmt);
        last_if = stmt;
      }
    }

    /*
    `body` will look like this at this point:

      ...
      i - n    : ...
      i - n + 1: if(cond_1) { } else if(cond_2) { } ... else if(cond_n) { }
      i - n + 2: if(cond_2) { } else if(cond_3) { } ... else if(cond_n) { }
      ...
      i - 1    : if(cond_n-1) { } else { }
      i        : if(cond_n) { }
      ...

    but since we chained all of the statements into the first, we want to remove
    the others from the body:

      ...
      i - n    : ...
      i - n + 1: if(cond_1) { } else if(cond_2) { } else if ...
      ...
    */
    size_t start_delete{i - (ifs.size() - 2)};
    size_t end_delete{i};
    body.erase(body.erase(std::next(body.begin(), start_delete),
                          std::next(body.begin(), end_delete)));
    done_something = true;
  }

  if (done_something) {
    substitutions[compound] = ast.CreateCompoundStmt(body);
  }
  return !Stopped();
}

void ReachBasedRefine::RunImpl() {
  LOG(INFO) << "Reachability-based refinement";
  TransformVisitor<ReachBasedRefine>::RunImpl();
  TraverseDecl(ast_ctx.getTranslationUnitDecl());
}

}  // namespace rellic