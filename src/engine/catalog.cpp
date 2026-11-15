#include "engine/catalog.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

#include "expr/template.hpp"
#include "util/fs.hpp"
#include "util/strings.hpp"
#include "workflow/matrix.hpp"
#include "workflow/parser.hpp"

namespace cpa::engine {

namespace {

std::string display_path(const fs::path& path, const fs::path& root) {
  const auto rel = path.lexically_normal().lexically_relative(root.lexically_normal());
  if (!rel.empty() && *rel.begin() != "..") return rel.generic_string();
  return path.generic_string();
}

bool has_expression(const yaml::Node* n) {
  if (n == nullptr) return false;
  if (n->is_string()) return expr::contains_expression(n->text);
  if (n->is_sequence()) {
    return std::any_of(n->items.begin(), n->items.end(), [](const yaml::Node* i) { return has_expression(i); });
  }
  if (n->is_mapping()) {
    return std::any_of(n->entries.begin(), n->entries.end(),
                       [](const yaml::Entry& e) { return has_expression(e.value); });
  }
  return false;
}

std::string dot_quote(std::string_view s) {
  std::string r = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') r.push_back('\\');
    if (c == '\n') {
      r += "\\n";
      continue;
    }
    r.push_back(c);
  }
  return r + "\"";
}

std::string job_label(const workflow::Job& job) {
  if (job.name != nullptr && job.name->is_scalar() && !job.name->is_null() &&
      !expr::contains_expression(job.name->scalar_string()) && job.name->scalar_string() != job.id) {
    return job.id + " (" + job.name->scalar_string() + ")";
  }
  return job.id;
}

}  // namespace

std::vector<WorkflowFile> load_workflow_files(const fs::path& repo_root, const std::vector<fs::path>& inputs) {
  std::vector<fs::path> paths;
  auto add_dir = [&](const fs::path& dir) {
    std::error_code ec;
    std::vector<fs::path> found;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      const auto ext = entry.path().extension().string();
      if (entry.is_regular_file(ec) && (ext == ".yml" || ext == ".yaml")) found.push_back(entry.path());
    }
    std::sort(found.begin(), found.end());
    paths.insert(paths.end(), found.begin(), found.end());
  };
  if (inputs.empty()) {
    for (auto& p : workflow::discover_workflows(repo_root)) paths.push_back(p);
  } else {
    for (const auto& in : inputs) {
      std::error_code ec;
      if (fs::is_directory(in, ec)) {
        add_dir(in);
      } else {
        paths.push_back(in);
      }
    }
  }
  std::vector<WorkflowFile> out;
  std::set<fs::path> seen;
  for (const auto& p : paths) {
    const auto abs = util::real_path(p);
    if (!seen.insert(abs).second) continue;
    WorkflowFile file;
    file.path = abs;
    file.display = display_path(abs, repo_root);
    try {
      file.workflow =
          std::make_shared<const workflow::Workflow>(workflow::load_workflow(abs, file.display, &file.warnings));
    } catch (const workflow::WorkflowError& e) {
      file.errors = e.diagnostics();
    } catch (const std::exception& e) {
      file.errors.push_back({file.display, {}, e.what(), false});
    }
    out.push_back(std::move(file));
  }
  return out;
}

bool workflow_matches(const WorkflowFile& file, std::string_view selector) {
  if (selector.empty()) return false;
  if (file.display == selector) return true;
  const fs::path sel(selector);
  const auto name = file.path.filename().string();
  if (name == selector || file.path.stem().string() == selector) return true;
  std::error_code ec;
  if (fs::exists(sel, ec) && util::real_path(sel) == file.path) return true;
  if (file.workflow != nullptr) {
    const auto* n = file.workflow->name;
    if (n != nullptr && n->is_scalar() && !n->is_null() && n->scalar_string() == selector) return true;
  }
  return false;
}

std::vector<Selection> select_workflows(const std::vector<WorkflowFile>& files, const workflow::EventInfo& event,
                                        const std::vector<std::string>& selectors) {
  for (const auto& sel : selectors) {
    const bool found =
        std::any_of(files.begin(), files.end(), [&](const WorkflowFile& f) { return workflow_matches(f, sel); });
    if (!found) throw std::runtime_error("No workflow matches '" + sel + "'");
  }
  std::vector<Selection> out;
  for (const auto& f : files) {
    if (!selectors.empty() && std::none_of(selectors.begin(), selectors.end(),
                                           [&](const std::string& s) { return workflow_matches(f, s); })) {
      continue;
    }
    Selection s;
    s.file = &f;
    if (!f.valid()) {
      s.match = {false, "the workflow is invalid"};
    } else {
      s.match = workflow::match_trigger(*f.workflow, event);
    }
    out.push_back(std::move(s));
  }
  return out;
}

std::string render_validation(const std::vector<WorkflowFile>& files) {
  std::string out;
  std::size_t errors = 0;
  std::size_t warnings = 0;
  std::size_t invalid = 0;
  for (const auto& f : files) {
    for (const auto& d : f.errors) out += d.to_string() + "\n";
    for (const auto& d : f.warnings) out += d.to_string() + "\n";
    errors += f.errors.size();
    warnings += f.warnings.size();
    if (!f.valid()) ++invalid;
  }
  out += std::to_string(files.size()) + " workflow file(s) checked: " + std::to_string(files.size() - invalid) +
         " valid, " + std::to_string(invalid) + " invalid (" + std::to_string(errors) + " error(s), " +
         std::to_string(warnings) + " warning(s))\n";
  return out;
}

std::string describe_token(const yaml::Node* n) {
  if (n == nullptr) return {};
  if (n->is_scalar()) return n->scalar_string();
  if (n->is_sequence()) {
    std::vector<std::string> parts;
    for (const auto* item : n->items) parts.push_back(describe_token(item));
    return util::join(parts, ", ");
  }
  std::vector<std::string> parts;
  for (const auto& e : n->entries) parts.push_back(e.key->text + ": " + describe_token(e.value));
  return util::join(parts, ", ");
}

std::vector<int> job_levels(const workflow::Workflow& wf) {
  std::map<std::string, std::size_t> index;
  for (std::size_t i = 0; i < wf.jobs.size(); ++i) index[wf.jobs[i].id] = i;
  std::vector<int> level(wf.jobs.size(), 0);
  // The parser rejects cycles; the iteration bound guards against them anyway.
  for (std::size_t round = 0; round <= wf.jobs.size(); ++round) {
    bool changed = false;
    for (std::size_t i = 0; i < wf.jobs.size(); ++i) {
      int l = 1;
      for (const auto& need : wf.jobs[i].needs) {
        const auto it = index.find(need);
        if (it != index.end()) l = std::max(l, level[it->second] + 1);
      }
      if (l != level[i]) {
        level[i] = l;
        changed = true;
      }
    }
    if (!changed) break;
  }
  return level;
}

std::string render_list(const std::vector<WorkflowFile>& files) {
  std::string out;
  for (const auto& f : files) {
    if (!f.valid()) {
      out += f.display + " (invalid: " + std::to_string(f.errors.size()) + " error(s); run 'validate')\n\n";
      continue;
    }
    const auto& wf = *f.workflow;
    out += wf.display_name();
    if (wf.display_name() != f.display) out += " (" + f.display + ")";
    out += "\n";
    std::vector<std::string> events;
    for (const auto& t : wf.triggers) events.push_back(t.event);
    out += "  on: " + util::join(events, ", ") + "\n";
    out += "  jobs:\n";
    for (const auto& job : wf.jobs) {
      out += "    " + job_label(job) + "\n";
      if (!job.needs.empty()) out += "      needs: " + util::join(job.needs, ", ") + "\n";
      if (!job.uses.empty()) out += "      uses: " + job.uses + "\n";
      if (job.runs_on != nullptr) out += "      runs-on: " + describe_token(job.runs_on) + "\n";
      if (job.condition != nullptr) out += "      if: " + describe_token(job.condition) + "\n";
      const yaml::Node* matrix = job.strategy != nullptr ? job.strategy->get("matrix") : nullptr;
      if (matrix != nullptr) {
        if (has_expression(matrix)) {
          out += "      matrix: (evaluated at run time)\n";
        } else {
          try {
            const auto combos = workflow::expand_matrix(yaml::to_value(*matrix));
            out += "      matrix: " + std::to_string(combos.size()) + " combination(s)\n";
            for (const auto& c : combos) out += "        " + workflow::matrix_display_suffix(c) + "\n";
          } catch (const std::exception& e) {
            out += std::string("      matrix: error: ") + e.what() + "\n";
          }
        }
      }
      if (!job.steps.empty()) out += "      steps: " + std::to_string(job.steps.size()) + "\n";
    }
    out += "\n";
  }
  if (out.size() >= 2 && out.ends_with("\n\n")) out.pop_back();
  return out;
}

std::string render_graph_text(const workflow::Workflow& wf) {
  std::string out = wf.display_name();
  if (wf.display_name() != wf.file) out += " (" + wf.file + ")";
  out += "\n";
  const auto levels = job_levels(wf);
  const int max_level = levels.empty() ? 0 : *std::max_element(levels.begin(), levels.end());
  for (int l = 1; l <= max_level; ++l) {
    out += "  stage " + std::to_string(l) + ":\n";
    for (std::size_t i = 0; i < wf.jobs.size(); ++i) {
      if (levels[i] != l) continue;
      const auto& job = wf.jobs[i];
      out += "    " + job_label(job);
      if (!job.needs.empty()) out += " <- " + util::join(job.needs, ", ");
      out += "\n";
    }
  }
  return out;
}

std::string render_graph_dot(const std::vector<const workflow::Workflow*>& workflows) {
  std::string out = "digraph workflows {\n  rankdir=LR;\n  node [shape=box];\n";
  int cluster = 0;
  for (const auto* wf : workflows) {
    const std::string prefix = wf->file + "/";
    out += "  subgraph " + dot_quote("cluster_" + std::to_string(cluster++)) + " {\n";
    out += "    label=" + dot_quote(wf->display_name() == wf->file ? wf->file
                                                                    : wf->display_name() + " (" + wf->file + ")") +
           ";\n";
    for (const auto& job : wf->jobs) {
      out += "    " + dot_quote(prefix + job.id) + " [label=" + dot_quote(job_label(job)) + "];\n";
    }
    for (const auto& job : wf->jobs) {
      for (const auto& need : job.needs) {
        out += "    " + dot_quote(prefix + need) + " -> " + dot_quote(prefix + job.id) + ";\n";
      }
    }
    out += "  }\n";
  }
  out += "}\n";
  return out;
}

}  // namespace cpa::engine
