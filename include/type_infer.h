#ifndef CALCLANG_TYPE_INFER_H
#define CALCLANG_TYPE_INFER_H
#include "ast.h"

/* Forward dataflow analysis over the whole program. Walks each
   function body (and the top-level statement sequence) maintaining a
   type environment per program point. The result lands in each
   NODE_VAR's `inferred_type` field: when the analysis can prove a
   variable has a single concrete type at a use site, that TypeAnnot
   ends up on the node; otherwise the field stays 0 (TYPE_ANY) and the
   call-site / codegen logic falls back to its existing behavior.

   Handles:
     - let/assign as refinement
     - if/else by merging the two branches' environments at the join
     - while/for by iterating to a fixpoint over the body
     - calls by using the callee's declared return type when known
     - nested functions / closures: walked recursively, params seeded
       from declared types (TYPE_ANY when not annotated)

   Effects are purely annotation — the AST shape is unchanged. Codegen
   may consult the new annotations but is not required to.

   Must be called *after* parsing and *before* codegen. */
void infer_program_types(const Program *prog);

#endif
