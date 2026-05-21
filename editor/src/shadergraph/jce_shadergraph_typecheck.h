/*
 * jce_shadergraph_typecheck.h — link validation for shader graphs.
 *
 * Verifies that every link's producer dtype matches its consumer dtype
 * and that no consumer socket receives more than one link.  Reports
 * warnings for unconnected Output inputs (codegen still produces a
 * valid shader using sensible defaults).
 *
 * Pure model utility; no ImGui / bgfx dependency.
 */

#pragma once

#include "shadergraph/jce_shadergraph_types.h"

#include <string>
#include <vector>

namespace jce_sg {

struct TypeDiag {
    int         link_index;  /* -1 if not link-scoped */
    int         node_id;     /* -1 if not node-scoped */
    std::string message;
};

struct TypeCheckResult {
    std::vector<TypeDiag> errors;
    std::vector<TypeDiag> warnings;
    bool ok() const { return errors.empty(); }
};

TypeCheckResult typecheck(const Graph &g);

} /* namespace jce_sg */
