#include "workflow/parser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <map>
#include <set>

#include "expr/template.hpp"
#include "util/strings.hpp"
#include "workflow/scopes.hpp"

namespace cpa::workflow {

namespace fs = std::filesystem;

std::string Diagnostic::to_string() const {
  std::string s = file;
  if (mark.line > 0) s += ":" + std::to_string(mark.line) + ":" + std::to_string(mark.column);
  s += warning ? ": warning: " : ": error: ";
  s += message;
  return s;
}

namespace {

std::string summarize(const std::vector<Diagnostic>& diagnostics) {
  std::string s;
  for (const auto& d : diagnostics) {
    if (!s.empty()) s += "\n";
    s += d.to_string();
  }
  return s;
}

}  // namespace

WorkflowError::WorkflowError(std::vector<Diagnostic> diagnostics)
    : std::runtime_error(summarize(diagnostics)), diagnostics_(std::move(diagnostics)) {}

bool is_valid_identifier(std::string_view id) {
  if (id.empty() || id.size() >= 100) return false;
  const auto c0 = static_cast<unsigned char>(id[0]);
  if (!(std::isalpha(c0) || c0 == '_')) return false;
  return std::all_of(id.begin(), id.end(), [](char ch) {
    const auto c = static_cast<unsigned char>(ch);
    return std::isalnum(c) || c == '_' || c == '-';
  });
}

const Job* Workflow::find_job(std::string_view id) const {
  for (const auto& j : jobs) {
    if (j.id == id) return &j;
  }
  return nullptr;
}

const Trigger* Workflow::find_trigger(std::string_view event) const {
  for (const auto& t : triggers) {
    if (t.event == event) return &t;
  }
  return nullptr;
}

std::string Workflow::display_name() const {
  if (name != nullptr && name->is_scalar() && !name->is_null()) return name->scalar_string();
  return file;
}

std::string Step::default_display_name() const {
  if (run != nullptr) {
    const auto text = run->scalar_string();
    for (const auto& line : util::split_lines(text)) {
      const auto t = util::trim(line);
      if (!t.empty()) return "Run " + std::string(t);
    }
    return "Run ";
  }
  return "Run " + uses;
}

bool is_known_event(std::string_view event) {
  static const std::set<std::string_view> events = {
      "branch_protection_rule",
      "check_run",
      "check_suite",
      "create",
      "delete",
      "deployment",
      "deployment_status",
      "discussion",
      "discussion_comment",
      "fork",
      "gollum",
      "image_version",
      "issue_comment",
      "issues",
      "label",
      "merge_group",
      "milestone",
      "page_build",
      "project",
      "project_card",
      "project_column",
      "public",
      "pull_request",
      "pull_request_review",
      "pull_request_review_comment",
      "pull_request_target",
      "push",
      "registry_package",
      "release",
      "repository_dispatch",
      "schedule",
      "status",
      "watch",
      "workflow_call",
      "workflow_dispatch",
      "workflow_run",
  };
  return events.count(event) != 0;
}

namespace {

using yaml::Node;

class Parser {
 public:
  explicit Parser(std::string file) : file_(std::move(file)) {}

  Workflow parse(std::shared_ptr<yaml::Document> document) {
    Workflow wf;
    wf.document = document;
    wf.file = file_;
    const Node* root = document->root;
    if (root == nullptr || root->is_null()) {
      error(root, "The workflow is empty");
      return wf;
    }
    if (!root->is_mapping()) {
      error(root, std::string("A ") + root->kind_name() + " was not expected; the workflow must be a mapping");
      return wf;
    }
    check_keys(*root, {"name", "run-name", "on", "env", "defaults", "jobs", "permissions", "concurrency"});

    wf.name = root->get("name");
    if (wf.name != nullptr) {
      if (!wf.name->is_scalar()) {
        error(wf.name, "'name' must be a string");
      } else if (expr::contains_expression(wf.name->scalar_string())) {
        error(wf.name, "A template expression is not allowed in 'name'");
      }
    }
    wf.run_name = root->get("run-name");
    if (wf.run_name != nullptr) check_string_template(wf.run_name, Site::RunName, "run-name");

    const Node* on = root->get("on");
    if (on == nullptr) {
      error(root, "Required property is missing: on");
    } else {
      parse_triggers(*on, wf);
    }

    wf.env = root->get("env");
    if (wf.env != nullptr) check_env(wf.env, Site::WorkflowEnv, false);
    wf.defaults = root->get("defaults");
    if (wf.defaults != nullptr) check_defaults(wf.defaults, Site::JobDefaultsRun);
    wf.permissions = root->get("permissions");
    if (wf.permissions != nullptr) check_permissions(wf.permissions);
    wf.concurrency = root->get("concurrency");
    if (wf.concurrency != nullptr) check_concurrency(wf.concurrency, Site::WorkflowConcurrency);

    const Node* jobs = root->get("jobs");
    if (jobs == nullptr) {
      error(root, "Required property is missing: jobs");
    } else if (!jobs->is_mapping()) {
      error(jobs, "'jobs' must be a mapping");
    } else if (jobs->entries.empty()) {
      error(jobs, "The workflow must contain at least one job");
    } else {
      for (const auto& e : jobs->entries) wf.jobs.push_back(parse_job(*e.key, e.value));
      check_needs(wf);
    }
    return wf;
  }

  std::vector<Diagnostic>& errors() { return errors_; }
  std::vector<Diagnostic>& warnings() { return warnings_; }

 private:
  void error(const Node* n, std::string message) {
    errors_.push_back({file_, n != nullptr ? n->mark : yaml::Mark{}, std::move(message), false});
  }
  void warning(const Node* n, std::string message) {
    warnings_.push_back({file_, n != nullptr ? n->mark : yaml::Mark{}, std::move(message), true});
  }

  void check_keys(const Node& mapping, std::initializer_list<std::string_view> allowed) {
    for (const auto& e : mapping.entries) {
      const auto& key = e.key->text;
      if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
        error(e.key, "Unexpected value '" + key + "'");
      }
    }
  }

  // Validates all expressions below a node (keys are literal).
  void check_template(const Node* n, Site site) {
    if (n == nullptr) return;
    if (n->is_string()) {
      try {
        expr::validate_template(n->text, scope_for(site));
      } catch (const expr::ParseError& e) {
        error(n, e.what());
      }
    } else if (n->is_sequence()) {
      for (const auto* item : n->items) check_template(item, site);
    } else if (n->is_mapping()) {
      for (const auto& e : n->entries) {
        if (expr::contains_expression(e.key->text)) {
          error(e.key, "A template expression is not allowed in a mapping key");
        }
        check_template(e.value, site);
      }
    }
  }

  void check_string_template(const Node* n, Site site, std::string_view what) {
    if (!n->is_scalar() || n->is_null()) {
      error(n, "'" + std::string(what) + "' must be a string");
      return;
    }
    check_template(n, site);
  }

  void check_no_expression(const Node* n, std::string_view what) {
    if (n->is_scalar() && expr::contains_expression(n->scalar_string())) {
      error(n, "A template expression is not allowed in '" + std::string(what) + "'");
    }
  }

  void check_call_value(const Node* n, Site site, std::string_view what, std::string_view type = {},
                        bool allow_null = false) {
    auto valid_type = [&](const expr::Value& value) {
      if (value.is_null()) return allow_null;
      if (type == "string") return value.is_string();
      if (type == "boolean") return value.is_bool();
      if (type == "number") return value.is_number();
      return value.is_string() || value.is_bool() || value.is_number();
    };
    const std::string expected = type.empty() ? "a string, boolean or number" : "a " + std::string(type);
    if (!n->is_scalar()) {
      error(n, "'" + std::string(what) + "' must be " + expected);
      return;
    }
    if (n->is_string() && expr::contains_expression(n->text)) {
      try {
        const auto expression = expr::Expression::parse(
            expr::template_to_expression(expr::split_template(n->text)), scope_for(site));
        // Context-dependent results can only be type-checked when the caller runs.
        if (expression.root().type == expr::NodeType::NamedValue ||
            (expression.referenced_named_values().empty() &&
             !valid_type(expr::evaluate(expression, expr::EvalContext{})))) {
          error(n, "'" + std::string(what) + "' must be " + expected);
        }
      } catch (const std::exception& e) {
        error(n, e.what());
      }
    } else if (!valid_type(yaml::to_value(*n))) {
      error(n, "'" + std::string(what) + "' must be " + expected);
    }
  }

  void check_call_arguments(const Node* n, bool secrets) {
    if (secrets && n->is_string() && n->text == "inherit") return;
    const std::string what = secrets ? "secrets" : "with";
    if (!n->is_mapping()) {
      error(n, "'" + what + "' must be " + (secrets ? "a mapping or 'inherit'" : "a mapping"));
      return;
    }
    for (const auto& e : n->entries) {
      if (expr::contains_expression(e.key->text)) {
        error(e.key, "A template expression is not allowed in a mapping key");
      }
      check_call_value(e.value, secrets ? Site::JobEnv : Site::JobGeneral,
                       what + "." + e.key->text, secrets ? "string" : "", !secrets);
    }
  }

  void check_workflow_uses(const Node* uses) {
    if (!uses->is_string() || uses->text.empty()) {
      error(uses, "'uses' must be a non-empty string");
      return;
    }
    const auto& value = uses->text;
    if (expr::contains_expression(value)) {
      check_no_expression(uses, "uses");
      return;
    }
    const bool local = value.starts_with("./");
    const auto at = value.find('@');
    const auto path = local ? value.substr(2) : value.substr(0, at);
    const auto parts = util::split(path, '/');
    const auto offset = local ? 0u : 2u;
    bool valid = parts.size() == offset + 3;
    if (valid) {
      valid = parts[offset] == ".github" && parts[offset + 1] == "workflows" &&
              (parts.back().ends_with(".yml") || parts.back().ends_with(".yaml"));
      for (const auto& part : parts) {
        if (part.empty() || part == "." || part == ".." ||
            part.find_first_of("\\@ \t\r\n") != std::string::npos) valid = false;
      }
    }
    valid = valid && (local ? at == std::string::npos
                           : at != std::string::npos && at + 1 < value.size());
    if (!valid) {
      error(uses, "Expected reusable workflow format ./.github/workflows/{file}.yml or "
                  "{owner}/{repo}/.github/workflows/{file}.yml@ref. Actual '" + value + "'");
    }
  }

  void check_condition(const Node* n, Site site) {
    if (!n->is_scalar()) {
      error(n, "'if' must be a string, boolean or number");
      return;
    }
    try {
      condition_expression(n, scope_for(site));
    } catch (const expr::ParseError& e) {
      error(n, e.what());
    }
  }

  // A boolean or an expression.
  void check_bool_or_expression(const Node* n, Site site, std::string_view what) {
    if (n->kind == yaml::NodeKind::Boolean) return;
    if (n->is_string() && expr::contains_expression(n->text)) {
      check_template(n, site);
      return;
    }
    error(n, "Unexpected value '" + n->scalar_string() + "' for '" + std::string(what) +
                 "'; a boolean or an expression is expected");
  }

  // A number or an expression.
  void check_number_or_expression(const Node* n, Site site, std::string_view what) {
    if (n->kind == yaml::NodeKind::Number) return;
    if (n->is_string() && expr::contains_expression(n->text)) {
      check_template(n, site);
      return;
    }
    error(n, "Unexpected value '" + n->scalar_string() + "' for '" + std::string(what) +
                 "'; a number or an expression is expected");
  }

  void check_env(const Node* n, Site site, bool allow_expression) {
    if (n->is_mapping()) {
      for (const auto& e : n->entries) {
        if (!e.value->is_scalar()) {
          error(e.value, "The value of environment variable '" + e.key->text + "' must be a string");
        }
        if (expr::contains_expression(e.key->text)) {
          error(e.key, "A template expression is not allowed in an environment variable name");
        }
        check_template(e.value, site);
      }
    } else if (allow_expression && n->is_string() && expr::contains_expression(n->text)) {
      check_template(n, site);
    } else if (!n->is_null()) {
      error(n, "'env' must be a mapping");
    }
  }

  void check_defaults(const Node* n, Site site) {
    if (!n->is_mapping()) {
      error(n, "'defaults' must be a mapping");
      return;
    }
    check_keys(*n, {"run"});
    if (const Node* run = n->get("run")) {
      if (!run->is_mapping()) {
        error(run, "'defaults.run' must be a mapping");
        return;
      }
      check_keys(*run, {"shell", "working-directory"});
      for (const auto& e : run->entries) check_string_template(e.value, site, e.key->text);
    }
  }

  void check_permissions(const Node* n) {
    static const std::set<std::string_view> scopes = {
        "actions",       "attestations", "checks",   "contents",     "deployments", "discussions", "id-token",
        "issues",        "models",       "packages", "pages",        "pull-requests", "repository-projects",
        "security-events", "statuses",   "artifact-metadata"};
    if (n->is_string()) {
      if (n->text != "read-all" && n->text != "write-all") error(n, "Unexpected value '" + n->text + "'");
      return;
    }
    if (n->is_mapping()) {
      for (const auto& e : n->entries) {
        if (scopes.count(e.key->text) == 0) warning(e.key, "Unknown permission scope '" + e.key->text + "'");
        const auto v = e.value->scalar_string();
        if (v != "read" && v != "write" && v != "none") error(e.value, "Unexpected value '" + v + "'");
      }
      return;
    }
    if (!(n->is_mapping() && n->entries.empty())) error(n, "'permissions' must be a string or a mapping");
  }

  void check_concurrency(const Node* n, Site site) {
    if (n->is_string()) {
      check_template(n, site);
      return;
    }
    if (!n->is_mapping()) {
      error(n, "'concurrency' must be a string or a mapping");
      return;
    }
    check_keys(*n, {"group", "cancel-in-progress"});
    if (const Node* g = n->get("group")) {
      check_string_template(g, site, "group");
    } else {
      error(n, "Required property is missing: group");
    }
    if (const Node* c = n->get("cancel-in-progress")) check_bool_or_expression(c, site, "cancel-in-progress");
  }

  void parse_triggers(const Node& on, Workflow& wf) {
    auto add = [&](const Node* key, const Node* config) {
      const auto& event = key->text;
      if (!is_known_event(event)) {
        error(key, "Unexpected value '" + event + "'");
        return;
      }
      wf.triggers.push_back({event, config});
      if (config != nullptr) check_trigger_config(event, *config, wf);
    };
    if (on.is_string()) {
      add(&on, nullptr);
    } else if (on.is_sequence()) {
      for (const auto* item : on.items) {
        if (!item->is_string()) {
          error(item, "An event name is expected");
          continue;
        }
        add(item, nullptr);
      }
    } else if (on.is_mapping()) {
      for (const auto& e : on.entries) add(e.key, e.value);
    } else {
      error(&on, "'on' must be a string, a sequence or a mapping");
    }
    if (wf.triggers.empty() && errors_.empty()) error(&on, "At least one event is required in 'on'");
  }

  void check_filter_list(const Node* n, std::string_view what) {
    if (n->is_string()) return;
    if (!n->is_sequence()) {
      error(n, "'" + std::string(what) + "' must be a sequence of strings");
      return;
    }
    for (const auto* item : n->items) {
      if (!item->is_scalar() || item->is_null()) error(item, "'" + std::string(what) + "' must contain strings");
    }
  }

  void check_trigger_config(const std::string& event, const Node& config, Workflow& wf) {
    if (config.is_null()) return;
    if (event == "schedule") {
      if (!config.is_sequence()) {
        error(&config, "'schedule' must be a sequence of mappings with 'cron'");
        return;
      }
      for (const auto* item : config.items) {
        if (!item->is_mapping() || item->get("cron") == nullptr) {
          error(item, "Required property is missing: cron");
        } else {
          check_keys(*item, {"cron", "timezone"});
        }
      }
      return;
    }
    if (!config.is_mapping()) {
      error(&config, "The configuration of '" + event + "' must be a mapping");
      return;
    }
    if (event == "push" || event == "pull_request" || event == "pull_request_target") {
      if (event == "push") {
        check_keys(config, {"branches", "branches-ignore", "tags", "tags-ignore", "paths", "paths-ignore"});
      } else {
        check_keys(config, {"types", "branches", "branches-ignore", "paths", "paths-ignore"});
      }
      for (const auto& e : config.entries) check_filter_list(e.value, e.key->text);
      const std::array<std::pair<const char*, const char*>, 3> exclusive = {
          {{"branches", "branches-ignore"}, {"tags", "tags-ignore"}, {"paths", "paths-ignore"}}};
      for (const auto& [a, b] : exclusive) {
        if (config.get(a) != nullptr && config.get(b) != nullptr) {
          error(config.get(b), std::string("'") + a + "' and '" + b + "' cannot be used together for '" + event +
                                   "'. Use '" + a + "' with '!' patterns instead");
        }
      }
      return;
    }
    if (event == "workflow_dispatch") {
      check_keys(config, {"inputs"});
      if (const Node* inputs = config.get("inputs")) parse_dispatch_inputs(*inputs, wf);
      return;
    }
    if (event == "workflow_call") {
      check_keys(config, {"inputs", "outputs", "secrets"});
      parse_call_config(config, wf);
      return;
    }
    if (event == "workflow_run") {
      check_keys(config, {"workflows", "types", "branches", "branches-ignore"});
      return;
    }
    // Activity types for the other events.
    for (const auto& e : config.entries) {
      if (e.key->text != "types") error(e.key, "Unexpected value '" + e.key->text + "'");
      check_filter_list(e.value, "types");
    }
  }

  void parse_call_config(const Node& config, Workflow& wf) {
    if (const Node* inputs = config.get("inputs"); inputs != nullptr && !inputs->is_null()) {
      if (!inputs->is_mapping()) {
        error(inputs, "'inputs' must be a mapping");
      } else {
        for (const auto& e : inputs->entries) {
          CallInput in;
          in.name = e.key->text;
          const Node* def = e.value;
          if (!def->is_mapping()) {
            error(def, "The definition of input '" + in.name + "' must be a mapping with 'type'");
            continue;
          }
          check_keys(*def, {"description", "required", "default", "type", "deprecationMessage"});
          if (const Node* d = def->get("description")) in.description = d->scalar_string();
          if (const Node* r = def->get("required")) {
            if (r->kind != yaml::NodeKind::Boolean) error(r, "'required' must be a boolean");
            in.required = r->boolean;
          }
          in.default_value = def->get("default");
          const Node* t = def->get("type");
          if (t == nullptr) {
            error(def, "Required property is missing: type");
          } else {
            in.type = t->scalar_string();
            if (in.type != "string" && in.type != "boolean" && in.type != "number") {
              error(t, "Unexpected input type '" + in.type + "'. Expected string, boolean or number");
            } else if (in.default_value != nullptr) {
              check_call_value(in.default_value, Site::RunName, "default", in.type, in.type == "string");
            }
          }
          wf.call_inputs.push_back(std::move(in));
        }
      }
    }
    if (const Node* outputs = config.get("outputs"); outputs != nullptr && !outputs->is_null()) {
      if (!outputs->is_mapping()) {
        error(outputs, "'outputs' must be a mapping");
      } else {
        for (const auto& e : outputs->entries) {
          CallOutput out;
          out.name = e.key->text;
          if (!e.value->is_mapping() || e.value->get("value") == nullptr) {
            error(e.value, "The output '" + out.name + "' must be a mapping with 'value'");
            continue;
          }
          check_keys(*e.value, {"description", "value"});
          if (const Node* d = e.value->get("description")) out.description = d->scalar_string();
          out.value = e.value->get("value");
          wf.call_outputs.push_back(std::move(out));
        }
      }
    }
    if (const Node* secrets = config.get("secrets"); secrets != nullptr && !secrets->is_null()) {
      if (!secrets->is_mapping()) {
        error(secrets, "'secrets' must be a mapping");
      } else {
        for (const auto& e : secrets->entries) {
          CallSecret sec;
          sec.name = e.key->text;
          if (e.value->is_mapping()) {
            check_keys(*e.value, {"description", "required"});
            if (const Node* r = e.value->get("required")) {
              if (r->kind != yaml::NodeKind::Boolean) error(r, "'required' must be a boolean");
              sec.required = r->boolean;
            }
          } else if (!e.value->is_null()) {
            error(e.value, "The definition of secret '" + sec.name + "' must be a mapping");
          }
          wf.call_secrets.push_back(std::move(sec));
        }
      }
    }
  }

  void parse_dispatch_inputs(const Node& inputs, Workflow& wf) {
    if (inputs.is_null()) return;
    if (!inputs.is_mapping()) {
      error(&inputs, "'inputs' must be a mapping");
      return;
    }
    for (const auto& e : inputs.entries) {
      DispatchInput in;
      in.name = e.key->text;
      const Node* def = e.value;
      if (!def->is_mapping()) {
        if (!def->is_null()) error(def, "The definition of input '" + in.name + "' must be a mapping");
        wf.dispatch_inputs.push_back(in);
        continue;
      }
      check_keys(*def, {"description", "required", "default", "type", "options", "deprecationMessage"});
      if (const Node* d = def->get("description")) in.description = d->scalar_string();
      if (const Node* r = def->get("required")) {
        if (r->kind != yaml::NodeKind::Boolean) error(r, "'required' must be a boolean");
        in.required = r->boolean;
      }
      in.default_value = def->get("default");
      if (const Node* t = def->get("type")) {
        in.type = t->scalar_string();
        static const std::set<std::string> types = {"string", "boolean", "choice", "number", "environment"};
        if (types.count(in.type) == 0) error(t, "Unexpected value '" + in.type + "'");
      }
      if (const Node* o = def->get("options")) {
        if (!o->is_sequence()) {
          error(o, "'options' must be a sequence");
        } else {
          for (const auto* item : o->items) in.options.push_back(item->scalar_string());
        }
      }
      if (in.type == "choice" && in.options.empty()) error(def, "Input '" + in.name + "' of type choice needs options");
      wf.dispatch_inputs.push_back(std::move(in));
    }
  }

  void check_runs_on(const Node* n) {
    if (n->is_string()) {
      check_template(n, Site::JobGeneral);
    } else if (n->is_sequence()) {
      if (n->items.empty()) error(n, "'runs-on' must not be empty");
      for (const auto* item : n->items) check_string_template(item, Site::JobGeneral, "runs-on");
    } else if (n->is_mapping()) {
      check_keys(*n, {"group", "labels"});
      check_template(n, Site::JobGeneral);
    } else {
      error(n, "'runs-on' must be a string, a sequence or a mapping");
    }
  }

  void check_strategy(const Node* n) {
    if (!n->is_mapping()) {
      error(n, "'strategy' must be a mapping");
      return;
    }
    check_keys(*n, {"matrix", "fail-fast", "max-parallel"});
    if (const Node* ff = n->get("fail-fast")) check_bool_or_expression(ff, Site::JobStrategy, "fail-fast");
    if (const Node* mp = n->get("max-parallel")) check_number_or_expression(mp, Site::JobStrategy, "max-parallel");
    const Node* matrix = n->get("matrix");
    if (matrix == nullptr) return;
    if (matrix->is_string()) {
      if (!expr::contains_expression(matrix->text)) {
        error(matrix, "'matrix' must be a mapping or an expression");
      } else {
        check_template(matrix, Site::JobStrategy);
      }
      return;
    }
    if (!matrix->is_mapping()) {
      error(matrix, "'matrix' must be a mapping or an expression");
      return;
    }
    if (matrix->entries.empty()) error(matrix, "Matrix must define at least one vector");
    for (const auto& e : matrix->entries) {
      const auto& key = e.key->text;
      const Node* v = e.value;
      if (v->is_string() && expr::contains_expression(v->text)) {
        check_template(v, Site::JobStrategy);
        continue;
      }
      if (key == "include" || key == "exclude") {
        if (!v->is_sequence()) {
          error(v, "'" + key + "' must be a sequence of mappings");
          continue;
        }
        for (const auto* item : v->items) {
          if (item->is_string() && expr::contains_expression(item->text)) {
            check_template(item, Site::JobStrategy);
          } else if (!item->is_mapping()) {
            error(item, "'" + key + "' must contain mappings");
          } else {
            check_template(item, Site::JobStrategy);
          }
        }
        continue;
      }
      if (!v->is_sequence()) {
        error(v, "Matrix vector '" + key + "' must be a sequence");
        continue;
      }
      if (v->items.empty()) error(v, "Matrix vector '" + key + "' does not contain any values");
      check_template(v, Site::JobStrategy);
    }
  }

  void check_environment(const Node* n) {
    if (n->is_string()) {
      check_template(n, Site::JobGeneral);
    } else if (n->is_mapping()) {
      check_keys(*n, {"name", "url", "deployment"});
      if (const Node* name = n->get("name")) {
        check_string_template(name, Site::JobGeneral, "name");
      } else {
        error(n, "Required property is missing: name");
      }
      if (const Node* url = n->get("url")) check_string_template(url, Site::JobOutputs, "url");
    } else {
      error(n, "'environment' must be a string or a mapping");
    }
  }

  Job parse_job(const Node& key, const Node* n) {
    Job job;
    job.id = key.text;
    job.node = n;
    if (!is_valid_identifier(job.id)) {
      error(&key, "The identifier '" + job.id +
                      "' is invalid. IDs may only contain alphanumeric characters, '_', and '-'. IDs must start "
                      "with a letter or '_' and must be less than 100 characters.");
    }
    if (!n->is_mapping()) {
      error(n, "The definition of job '" + job.id + "' must be a mapping");
      return job;
    }
    const Node* uses = n->get("uses");
    if (uses != nullptr) {
      check_keys(*n, {"name", "uses", "with", "secrets", "needs", "if", "permissions", "strategy", "concurrency"});
      check_workflow_uses(uses);
      job.uses = uses->scalar_string();
    } else {
      check_keys(*n, {"name", "needs", "runs-on", "permissions", "environment", "concurrency", "outputs", "env",
                      "defaults", "if", "steps", "timeout-minutes", "strategy", "continue-on-error", "container",
                      "services", "snapshot"});
    }

    job.name = n->get("name");
    if (job.name != nullptr) check_string_template(job.name, Site::JobGeneral, "name");

    if (const Node* needs = n->get("needs")) {
      if (needs->is_string()) {
        job.needs.push_back(needs->text);
      } else if (needs->is_sequence()) {
        for (const auto* item : needs->items) {
          if (!item->is_string()) {
            error(item, "'needs' must contain job ids");
            continue;
          }
          job.needs.push_back(item->text);
        }
      } else {
        error(needs, "'needs' must be a string or a sequence");
      }
      for (const auto& id : job.needs) {
        if (expr::contains_expression(id)) error(needs, "A template expression is not allowed in 'needs'");
      }
    }

    job.condition = n->get("if");
    if (job.condition != nullptr) check_condition(job.condition, Site::JobIf);
    job.strategy = n->get("strategy");
    if (job.strategy != nullptr) check_strategy(job.strategy);
    job.permissions = n->get("permissions");
    if (job.permissions != nullptr) check_permissions(job.permissions);
    job.concurrency = n->get("concurrency");
    if (job.concurrency != nullptr) check_concurrency(job.concurrency, Site::JobGeneral);

    if (uses != nullptr) {
      job.with = n->get("with");
      if (job.with != nullptr) check_call_arguments(job.with, false);
      job.secrets = n->get("secrets");
      if (job.secrets != nullptr) check_call_arguments(job.secrets, true);
      return job;
    }

    job.runs_on = n->get("runs-on");
    if (job.runs_on == nullptr) {
      error(n, "Required property is missing: runs-on");
    } else {
      check_runs_on(job.runs_on);
    }
    job.environment = n->get("environment");
    if (job.environment != nullptr) check_environment(job.environment);
    job.outputs = n->get("outputs");
    if (job.outputs != nullptr) {
      if (!job.outputs->is_mapping()) {
        error(job.outputs, "'outputs' must be a mapping");
      } else {
        for (const auto& e : job.outputs->entries) check_string_template(e.value, Site::JobOutputs, e.key->text);
      }
    }
    job.env = n->get("env");
    if (job.env != nullptr) check_env(job.env, Site::JobEnv, true);
    job.defaults = n->get("defaults");
    if (job.defaults != nullptr) check_defaults(job.defaults, Site::JobDefaultsRun);
    job.timeout_minutes = n->get("timeout-minutes");
    if (job.timeout_minutes != nullptr) {
      check_number_or_expression(job.timeout_minutes, Site::JobGeneral, "timeout-minutes");
    }
    job.continue_on_error = n->get("continue-on-error");
    if (job.continue_on_error != nullptr) {
      check_bool_or_expression(job.continue_on_error, Site::JobGeneral, "continue-on-error");
    }
    job.container = n->get("container");
    if (job.container != nullptr) check_template(job.container, Site::JobGeneral);
    job.services = n->get("services");
    if (job.services != nullptr) check_template(job.services, Site::JobGeneral);

    const Node* steps = n->get("steps");
    if (steps == nullptr) {
      error(n, "Required property is missing: steps");
    } else if (!steps->is_sequence()) {
      error(steps, "'steps' must be a sequence");
    } else if (steps->items.empty()) {
      error(steps, "Job '" + job.id + "' must contain at least one step");
    } else {
      parse_steps(*steps, job);
    }
    return job;
  }

  static std::string generated_id_base(const Step& s) {
    if (s.is_run()) return "__run";
    std::string name = s.uses.substr(0, s.uses.find('@'));
    std::string id = "__";
    for (char c : name) {
      id += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    }
    // Collapse the leading separators of local paths ("./x" -> "__x").
    std::string collapsed = "__";
    std::size_t i = 2;
    while (i < id.size() && id[i] == '_') ++i;
    collapsed += id.substr(i);
    return collapsed == "__" ? "__self" : collapsed;
  }

  void parse_steps(const Node& steps, Job& job) {
    std::set<std::string> ids;
    for (std::size_t i = 0; i < steps.items.size(); ++i) {
      const Node* n = steps.items[i];
      Step step;
      step.index = static_cast<int>(i);
      step.node = n;
      if (!n->is_mapping()) {
        error(n, "A step must be a mapping");
        job.steps.push_back(step);
        continue;
      }
      step.run = n->get("run");
      const Node* uses = n->get("uses");
      if (step.run != nullptr && uses != nullptr) {
        error(n, "A step cannot have both the 'uses' and 'run' keys");
      } else if (step.run == nullptr && uses == nullptr) {
        error(n, "Every step must define a 'uses' or 'run' key");
      }
      if (step.run != nullptr) {
        if (composite_) {
          check_keys(*n, {"id", "if", "name", "run", "shell", "env", "continue-on-error", "working-directory"});
          if (n->get("shell") == nullptr) error(n, "Required property is missing: shell");
        } else {
          check_keys(*n, {"id", "if", "name", "run", "shell", "env", "continue-on-error", "timeout-minutes",
                          "working-directory"});
        }
        check_string_template(step.run, general_site_, "run");
      } else if (composite_) {
        check_keys(*n, {"id", "if", "name", "uses", "with", "env", "continue-on-error"});
      } else {
        check_keys(*n, {"id", "if", "name", "uses", "with", "env", "continue-on-error", "timeout-minutes"});
      }
      if (uses != nullptr) {
        if (!uses->is_string() || uses->text.empty()) {
          error(uses, "'uses' must be a non-empty string");
        } else {
          step.uses = uses->text;
          if (expr::contains_expression(step.uses)) {
            check_no_expression(uses, "uses");
          } else {
            check_uses(uses);
          }
        }
      }
      step.name = n->get("name");
      if (step.name != nullptr) check_string_template(step.name, general_site_, "name");
      step.condition = n->get("if");
      if (step.condition != nullptr) check_condition(step.condition, if_site_);
      step.shell = n->get("shell");
      if (step.shell != nullptr) check_string_template(step.shell, general_site_, "shell");
      step.working_directory = n->get("working-directory");
      if (step.working_directory != nullptr) {
        check_string_template(step.working_directory, general_site_, "working-directory");
      }
      step.env = n->get("env");
      if (step.env != nullptr) check_env(step.env, general_site_, true);
      step.with = n->get("with");
      if (step.with != nullptr) {
        if (!step.with->is_mapping()) {
          if (!step.with->is_null()) error(step.with, "'with' must be a mapping");
        } else {
          for (const auto& e : step.with->entries) {
            if (!e.value->is_scalar()) error(e.value, "The value of input '" + e.key->text + "' must be a string");
          }
          check_template(step.with, general_site_);
        }
      }
      step.continue_on_error = n->get("continue-on-error");
      if (step.continue_on_error != nullptr) {
        check_bool_or_expression(step.continue_on_error, general_site_, "continue-on-error");
      }
      step.timeout_minutes = n->get("timeout-minutes");
      if (step.timeout_minutes != nullptr) {
        check_number_or_expression(step.timeout_minutes, general_site_, "timeout-minutes");
      }

      if (const Node* id = n->get("id")) {
        step.id = id->scalar_string();
        step.explicit_id = true;
        if (!is_valid_identifier(step.id)) {
          error(id, "The identifier '" + step.id +
                        "' is invalid. IDs may only contain alphanumeric characters, '_', and '-'. IDs must start "
                        "with a letter or '_' and must be less than 100 characters.");
        } else if (!ids.insert(step.id).second) {
          error(id, "The identifier '" + step.id + "' may not be used more than once within the same scope.");
        }
      }
      job.steps.push_back(std::move(step));
    }
    // Generated ids for steps without an explicit id.
    for (auto& step : job.steps) {
      if (step.explicit_id || step.node == nullptr || !step.node->is_mapping()) continue;
      if (!step.is_run() && step.uses.empty()) continue;
      const auto base = generated_id_base(step);
      std::string id = base;
      for (int n = 2; ids.count(id) != 0; ++n) id = base + "_" + std::to_string(n);
      ids.insert(id);
      step.id = id;
    }
  }

  void check_uses(const Node* uses) {
    const auto& u = uses->text;
    if (u.starts_with("./") || u.starts_with("docker://")) return;
    const auto at = u.find('@');
    const auto slash = u.find('/');
    if (at == std::string::npos || slash == std::string::npos || slash == 0 || slash > at || at + 1 == u.size() ||
        slash + 1 == at) {
      error(uses, "Expected format {org}/{repo}[/path]@ref. Actual '" + u + "'");
    }
  }

  void check_needs(const Workflow& wf) {
    std::map<std::string, const Job*> by_id;
    for (const auto& j : wf.jobs) by_id[j.id] = &j;
    for (const auto& j : wf.jobs) {
      std::set<std::string> seen;
      for (const auto& need : j.needs) {
        const Node* where = j.node != nullptr && j.node->is_mapping() ? j.node->get("needs") : j.node;
        if (by_id.count(need) == 0) error(where, "Job '" + j.id + "' depends on unknown job '" + need + "'.");
        if (!seen.insert(need).second) warning(where, "Job '" + j.id + "' lists '" + need + "' more than once");
      }
    }
    // Cycle detection (DFS).
    std::map<std::string, int> state;  // 0 = new, 1 = visiting, 2 = done
    bool reported = false;
    std::function<void(const Job&)> visit = [&](const Job& j) {
      state[j.id] = 1;
      for (const auto& need : j.needs) {
        auto it = by_id.find(need);
        if (it == by_id.end()) continue;
        if (state[need] == 1) {
          if (!reported) {
            const Node* where = j.node != nullptr && j.node->is_mapping() ? j.node->get("needs") : j.node;
            error(where, "Job '" + j.id + "' depends on job '" + need +
                             "' which creates a cycle in the dependency graph.");
            reported = true;
          }
        } else if (state[need] == 0) {
          visit(*it->second);
        }
      }
      state[j.id] = 2;
    };
    for (const auto& j : wf.jobs) {
      if (state[j.id] == 0) visit(j);
    }
  }

  std::string file_;
  std::vector<Diagnostic> errors_;
  std::vector<Diagnostic> warnings_;

 public:
  // Composite action steps (runs.steps of action.yml).
  void set_composite() {
    composite_ = true;
    general_site_ = Site::ActionStepGeneral;
    if_site_ = Site::ActionStepIf;
  }
  void parse_step_list(const Node& steps, Job& job) { parse_steps(steps, job); }

 private:
  bool composite_ = false;
  Site general_site_ = Site::StepGeneral;
  Site if_site_ = Site::StepIf;
};

}  // namespace

Workflow parse_workflow(std::shared_ptr<yaml::Document> document, std::string file,
                        std::vector<Diagnostic>* warnings) {
  Parser parser(file);
  auto wf = parser.parse(std::move(document));
  if (warnings != nullptr) {
    warnings->insert(warnings->end(), parser.warnings().begin(), parser.warnings().end());
  }
  if (!parser.errors().empty()) throw WorkflowError(parser.errors());
  return wf;
}

std::vector<Step> parse_action_steps(const yaml::Node& steps, std::string file, std::vector<Diagnostic>* warnings) {
  Parser parser(file);
  parser.set_composite();
  Job job;
  job.id = "composite";
  parser.parse_step_list(steps, job);
  if (warnings != nullptr) {
    warnings->insert(warnings->end(), parser.warnings().begin(), parser.warnings().end());
  }
  if (!parser.errors().empty()) throw WorkflowError(parser.errors());
  return std::move(job.steps);
}

std::vector<Step> parse_setup_steps(const yaml::Node& steps, const std::string& file) {
  if (!steps.is_sequence()) {
    throw WorkflowError({{file, steps.mark, "'setup-steps' must be a list", false}});
  }
  std::vector<Diagnostic> errors;
  for (const auto* step : steps.items) {
    if (!step->is_mapping()) continue;  // Reported by the regular step parser.
    if (step->get("run") == nullptr) {
      errors.push_back({file, step->mark, "Every setup step must define 'run'", false});
    }
    for (const auto* key : {"uses", "with", "continue-on-error"}) {
      if (const auto* value = step->get(key)) {
        errors.push_back({file, value->mark, "'" + std::string(key) + "' is not supported in setup-steps", false});
      }
    }
  }
  if (!errors.empty()) throw WorkflowError(std::move(errors));
  Parser parser(file);
  Job job;
  parser.parse_step_list(steps, job);
  if (!parser.errors().empty()) throw WorkflowError(parser.errors());
  return std::move(job.steps);
}

Workflow load_workflow(const fs::path& path, std::string display_name, std::vector<Diagnostic>* warnings) {
  if (display_name.empty()) display_name = path.string();
  std::shared_ptr<yaml::Document> doc;
  try {
    doc = yaml::load_file(path, display_name);
  } catch (const yaml::LoadError& e) {
    throw WorkflowError({{e.file(), e.mark(), e.message(), false}});
  }
  return parse_workflow(std::move(doc), std::move(display_name), warnings);
}

std::vector<fs::path> discover_workflows(const fs::path& repo_root) {
  std::vector<fs::path> files;
  const auto dir = repo_root / ".github" / "workflows";
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return files;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    const auto ext = entry.path().extension().string();
    if (ext == ".yml" || ext == ".yaml") files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());
  return files;
}

}  // namespace cpa::workflow
