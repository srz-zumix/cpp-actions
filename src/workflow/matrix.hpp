#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "expr/value.hpp"

namespace cpa::workflow {

class MatrixError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

constexpr std::size_t kMaxMatrixJobs = 256;

// Expands an evaluated `strategy.matrix` object into job combinations
// (objects), following the GitHub rules for vectors, `exclude` (partial
// matches) and `include` (merged into matching original combinations or
// appended). A null matrix yields a single empty combination.
std::vector<expr::Value> expand_matrix(const expr::Value& matrix);

// "(v1, v2, ...)" for display names.
std::string matrix_display_suffix(const expr::Value& combination);

}  // namespace cpa::workflow
