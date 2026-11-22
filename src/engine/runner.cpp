#include "engine/runner.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>

#include "actions/action.hpp"
#include "engine/cleanup.hpp"
#include "engine/evaluate.hpp"
#include "engine/job.hpp"
#include "expr/template.hpp"
#include "sandbox/sandbox.hpp"
#include "util/fs.hpp"
#include "util/stoppable_thread.hpp"
#include "util/strings.hpp"
#include "workflow/matrix.hpp"
#include "workflow/parser.hpp"
#include "workflow/scopes.hpp"

namespace cpa::engine {

namespace {

using expr::Value;
using workflow::Site;
using workflow::scope_for;
using Clock = std::chrono::steady_clock;
using StringMap = std::map<std::string, std::string>;

constexpr int kMaxCallDepth = 10;

std::int64_t elapsed_ms(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

std::string timestamp() {
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  ::localtime_r(&now, &tm);
  std::ostringstream os;
  os << std::put_time(&tm, "%Y%m%d-%H%M%S");
  return os.str();
}

std::string random_run_id() {
  std::string id = "1";
  for (char c : util::random_hex(5)) id += static_cast<char>('0' + (c % 10));
  return id;
}

void create_private_directory(const fs::path& path, bool must_be_new) {
  if (path.has_parent_path()) fs::create_directories(path.parent_path());
  if (::mkdir(path.c_str(), 0700) == 0) return;
  const int error = errno;
  if (error == EEXIST && !must_be_new && fs::is_directory(path) && !fs::is_symlink(fs::symlink_status(path))) {
    return;
  }
  throw std::system_error(error, std::generic_category(), "Cannot create private directory: " + path.string());
}

// Result of a job as seen by the jobs that need it.
std::string effective_result(const JobReport& r) {
  return r.result == "failure" && r.continue_on_error ? "success" : r.result;
}

Value convert_call_input(const workflow::CallInput& in, const Value& v, const std::string& where) {
  if (in.type == "string" && v.is_null()) return Value("");
  if ((in.type == "boolean" && v.is_bool()) || (in.type == "number" && v.is_number()) ||
      (in.type == "string" && v.is_string())) return v;
  throw EvaluationError(where + ": the input '" + in.name + "' must be a " + in.type);
}

struct CalledWorkflow {
  std::shared_ptr<const workflow::Workflow> workflow;
  std::string display;
  fs::path root;
  fs::path path;
};

bool valid_workflow_path(const std::string& path) {
  const fs::path p(path);
  return path.starts_with(".github/workflows/") && p.parent_path() == ".github/workflows" &&
         !p.filename().empty() && (p.extension() == ".yml" || p.extension() == ".yaml") &&
         path.find('\\') == std::string::npos && path.find('@') == std::string::npos;
}

bool valid_repository_component(const std::string& text) {
  return !text.empty() && text != "." && text != ".." &&
         std::all_of(text.begin(), text.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_' || c == '.';
         });
}

bool valid_workflow_ref(const std::string& ref) {
  if (ref.empty() || ref.front() == '-' || ref.front() == '/' || ref.back() == '/' || ref.back() == '.' ||
      ref.find("..") != std::string::npos || ref.find("//") != std::string::npos ||
      ref.find("@{") != std::string::npos || ref == "@") return false;
  for (unsigned char c : ref) {
    if (c <= ' ' || c == 127 || std::string_view("~^:?*[\\").find(c) != std::string_view::npos) return false;
  }
  for (const auto& part : util::split(ref, '/')) {
    if (part.empty() || part.front() == '.' || part.ends_with(".lock")) return false;
  }
  return true;
}

class WorkflowExecutor;

struct JobNode {
  const workflow::Job* job = nullptr;
  bool selected = true;
  bool done = false;
  std::string result;  // effective result
  StringMap outputs;
};

class WorkflowExecutor {
 public:
  WorkflowExecutor(RunState& run, WorkflowRun& wr, std::vector<JobReport>& reports, std::mutex& reports_mutex,
                  const std::set<std::string>* selected, JobInstance* parent = nullptr)
      : run_(run), wr_(wr), wf_(*wr.workflow), reports_(reports), reports_mutex_(reports_mutex), parent_(parent) {
    for (const auto& job : wf_.jobs) {
      JobNode node;
      node.job = &job;
      node.selected = selected == nullptr || selected->count(job.id) != 0;
      nodes_.emplace(job.id, node);
    }
  }

  // Runs all jobs; returns the effective results and outputs of the jobs.
  void execute() {
    if (wf_.concurrency != nullptr) {
      const auto ctx = job_level_context(Value::object(), Value(), Value());
      run_.console.line(run_.console.paint(
          run_.masker.mask("[" + wr_.file + "] " +
                           describe_concurrency(wf_.concurrency, ctx, scope_for(Site::WorkflowConcurrency), wf_.file)),
          Color::Gray));
    }
    std::optional<util::StoppableThread> cancellation;
    if (parent_ != nullptr) {
      cancellation.emplace([this](const std::atomic<bool>& stop) {
        while (!stop.load(std::memory_order_relaxed)) {
          {
            std::lock_guard lock(active_mutex_);
            for (auto* inst : active_) inherit_cancellation(*inst);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      });
    }
    std::vector<std::thread> threads;
    for (const auto& job : wf_.jobs) {
      if (!nodes_.at(job.id).selected) continue;
      threads.emplace_back([this, &job] { run_node(job); });
    }
    for (auto& t : threads) t.join();
  }

  const std::map<std::string, JobNode>& nodes() const { return nodes_; }

 private:
  void inherit_cancellation(JobInstance& inst) const {
    if (parent_ == nullptr) return;
    if (parent_->cancel) inst.cancel = true;
    if (parent_->force) inst.force = true;
  }

  void add_report(JobReport report) {
    std::lock_guard lock(reports_mutex_);
    reports_.push_back(std::move(report));
  }

  void finish_node(const std::string& id, std::string result, StringMap outputs) {
    {
      std::lock_guard lock(mutex_);
      auto& n = nodes_.at(id);
      n.done = true;
      n.result = std::move(result);
      n.outputs = std::move(outputs);
    }
    cv_.notify_all();
  }

  // Waits for the needed jobs; returns false when the run must stop.
  void wait_needs(const workflow::Job& job) {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [&] {
      return std::all_of(job.needs.begin(), job.needs.end(), [&](const std::string& need) {
        const auto& n = nodes_.at(need);
        return !n.selected || n.done;
      });
    });
  }

  std::string need_result(const std::string& id) const {
    const auto& n = nodes_.at(id);
    return n.selected ? n.result : "success";
  }

  JobStatus status_of(const workflow::Job& job) const {
    JobStatus st;
    std::set<std::string> seen;
    std::vector<std::string> stack(job.needs.begin(), job.needs.end());
    while (!stack.empty()) {
      const auto id = stack.back();
      stack.pop_back();
      if (!seen.insert(id).second) continue;
      const auto r = need_result(id);
      if (r != "success") st.success = false;
      if (r == "failure") st.failure = true;
      for (const auto& n : nodes_.at(id).job->needs) stack.push_back(n);
    }
    st.cancelled = run_.cancelled || (parent_ != nullptr && parent_->cancel);
    return st;
  }

  Value needs_context(const workflow::Job& job) const {
    auto needs = Value::object();
    for (const auto& id : job.needs) {
      auto entry = Value::object();
      entry.set("result", need_result(id));
      entry.set("outputs", to_object(nodes_.at(id).outputs));
      needs.set(id, entry);
    }
    return needs;
  }

  expr::EvalContext job_level_context(const Value& needs, const Value& matrix, const Value& strategy) const {
    expr::EvalContext ctx;
    auto c = Value::object();
    auto github = Value::object();
    // github context without the job specific fields.
    const auto g = github_context(run_, wr_, "", fs::path{}, fs::path{});
    c.set("github", g);
    c.set("needs", needs);
    c.set("vars", wr_.vars);
    c.set("inputs", wr_.inputs);
    c.set("matrix", matrix);
    c.set("strategy", strategy);
    c.set("secrets", to_object(wr_.secrets));
    c.set("env", Value::object());
    ctx.contexts = c;
    return ctx;
  }

  std::string job_display_name(const workflow::Job& job, const expr::EvalContext& ctx, const Value& matrix) const {
    const auto suffix = matrix.is_null() ? std::string() : workflow::matrix_display_suffix(matrix);
    if (job.name == nullptr) return job.id + (suffix.empty() ? "" : " " + suffix);
    try {
      const auto name = evaluate_string(job.name, ctx, scope_for(Site::JobGeneral), wf_.file);
      if (job.name->is_scalar() && expr::contains_expression(job.name->text)) return name;
      return name + (suffix.empty() ? "" : " " + suffix);
    } catch (const std::exception&) {
      return job.id + (suffix.empty() ? "" : " " + suffix);
    }
  }

  void run_node(const workflow::Job& job) {
    wait_needs(job);
    const auto status = status_of(job);
    const auto needs = needs_context(job);
    auto ctx = job_level_context(needs, Value(), Value());
    ctx.status = job_status_functions(status);
    const auto key_base = wr_.key_prefix + job.id;

    bool run_it = false;
    std::string error;
    try {
      run_it = evaluate_condition(job.condition, ctx, scope_for(Site::JobIf), wf_.file);
    } catch (const std::exception& e) {
      error = e.what();
    }
    if (!error.empty() || !run_it) {
      JobReport r;
      r.key = key_base;
      r.job_id = job.id;
      r.name = job_display_name(job, ctx, Value());
      if (!error.empty()) {
        r.result = "failure";
        r.reason = error;
        run_.console.line(run_.console.paint(run_.masker.mask("[" + r.name + "] " + error), Color::Red));
      } else {
        r.result = status.cancelled ? "cancelled" : "skipped";
        r.reason = status.cancelled ? "the run was cancelled" : "the job condition is false";
        run_.console.line(run_.console.paint(
            run_.masker.mask("[" + r.name + "] Job " + r.result + " (" + r.reason + ")"), Color::Gray));
      }
      const auto result = r.result;
      add_report(std::move(r));
      finish_node(job.id, result, {});
      return;
    }

    // Strategy
    std::vector<Value> combinations{Value()};
    bool fail_fast = true;
    int max_parallel = 0;
    try {
      if (job.strategy != nullptr && job.strategy->is_mapping()) {
        const auto site = scope_for(Site::JobStrategy);
        if (const auto* m = job.strategy->get("matrix")) {
          const auto matrix = evaluate_token(m, ctx, site, wf_.file);
          if (!matrix.is_object()) throw EvaluationError(location(wf_.file, m) + ": the matrix must be a mapping");
          combinations = workflow::expand_matrix(matrix);
        }
        if (const auto* ff = job.strategy->get("fail-fast")) fail_fast = evaluate_token(ff, ctx, site, wf_.file).truthy();
        if (const auto* mp = job.strategy->get("max-parallel")) {
          const auto value = evaluate_token(mp, ctx, site, wf_.file).to_number();
          if (!std::isfinite(value) || value <= 0 || std::trunc(value) != value ||
              value > std::numeric_limits<int>::max()) {
            throw EvaluationError(location(wf_.file, mp) + ": 'max-parallel' must be a positive integer");
          }
          max_parallel = static_cast<int>(value);
        }
      }
    } catch (const std::exception& e) {
      JobReport r;
      r.key = key_base;
      r.job_id = job.id;
      r.name = job.id;
      r.result = "failure";
      r.reason = e.what();
      run_.console.line(run_.console.paint(run_.masker.mask("[" + job.id + "] " + e.what()), Color::Red));
      add_report(std::move(r));
      finish_node(job.id, "failure", {});
      return;
    }
    const bool has_matrix = job.strategy != nullptr && job.strategy->get("matrix") != nullptr;
    if (has_matrix && combinations.empty()) {
      JobReport r;
      r.key = key_base;
      r.job_id = job.id;
      r.name = job.id;
      r.result = "failure";
      r.reason = "The matrix is empty";
      add_report(std::move(r));
      finish_node(job.id, "failure", {});
      return;
    }

    // --matrix filters
    std::vector<std::pair<std::size_t, Value>> selected;
    for (std::size_t i = 0; i < combinations.size(); ++i) {
      const auto& combination = combinations[i];
      bool keep = true;
      if (combination.is_object()) {
        for (const auto& [k, v] : run_.options.matrix) {
          if (combination.has(k) && combination.get(k).to_string() != v) keep = false;
        }
      }
      if (keep) selected.emplace_back(i, combination);
    }
    if (selected.empty()) {
      JobReport r;
      r.key = key_base;
      r.job_id = job.id;
      r.name = job.id;
      r.result = "skipped";
      r.reason = "no matrix combination matches --matrix";
      add_report(std::move(r));
      finish_node(job.id, "skipped", {});
      return;
    }

    const int total = static_cast<int>(combinations.size());
    Semaphore limiter(max_parallel > 0 ? max_parallel : total);
    std::mutex group_mutex;
    std::vector<std::string> results;
    StringMap outputs;
    std::vector<std::unique_ptr<JobInstance>> instances;
    std::atomic<bool> fail_fast_triggered{false};
    for (std::size_t i = 0; i < selected.size(); ++i) {
      auto inst = std::make_unique<JobInstance>();
      inst->run = &run_;
      inst->workflow = &wr_;
      inst->job = &job;
      inst->needs = needs;
      inst->matrix = has_matrix ? selected[i].second : Value();
      auto strategy = Value::object();
      strategy.set("fail-fast", fail_fast);
      strategy.set("job-index", static_cast<double>(selected[i].first));
      strategy.set("job-total", static_cast<double>(total));
      strategy.set("max-parallel", static_cast<double>(max_parallel > 0 ? max_parallel : total));
      inst->strategy = strategy;
      auto ictx = job_level_context(needs, inst->matrix, strategy);
      inst->name = wr_.key_prefix + job_display_name(job, ictx, inst->matrix);
      const auto suffix = has_matrix ? " " + workflow::matrix_display_suffix(selected[i].second) : std::string();
      inst->key = key_base + suffix;
      instances.push_back(std::move(inst));
    }

    std::vector<std::thread> threads;
    for (auto& inst_ptr : instances) {
      auto* inst = inst_ptr.get();
      threads.emplace_back([&, inst] {
        SemaphoreGuard limit(&limiter);
        run_.register_cancel(&inst->cancel, &inst->force);
        {
          std::lock_guard lock(active_mutex_);
          inherit_cancellation(*inst);
          active_.push_back(inst);
        }
        if (fail_fast_triggered) inst->cancel = true;
        std::vector<JobReport> produced;
        if (!job.uses.empty()) {
          produced = call_workflow(*inst);
        } else {
          SemaphoreGuard slot(&run_.slots);
          produced.push_back(run_job_instance(*inst));
        }
        run_.unregister_cancel(&inst->cancel);
        {
          std::lock_guard lock(active_mutex_);
          std::erase(active_, inst);
        }
        const auto& main = produced.front();
        const auto effective = effective_result(main);
        {
          std::lock_guard lock(group_mutex);
          results.push_back(effective);
          for (const auto& [k, v] : main.outputs) {
            if (job.uses.empty() || (main.result == "success" && !v.empty())) outputs[k] = v;
          }
          if (fail_fast && effective == "failure" && !fail_fast_triggered.exchange(true)) {
            for (auto& other : instances) {
              if (other.get() != inst) other->cancel = true;
            }
          }
        }
        for (auto& r : produced) add_report(std::move(r));
      });
    }
    for (auto& t : threads) t.join();
    finish_node(job.id, combine_results(results), outputs);
  }

  CalledWorkflow load_called(const std::string& uses, const std::string& where) {
    CalledWorkflow called;
    std::string path;
    if (uses.starts_with("./")) {
      path = uses.substr(2);
      called.root = wr_.repo_root;
      called.display = uses.substr(2);
    } else {
      // owner/repo/.github/workflows/workflow.yml@ref
      const auto at = uses.rfind('@');
      if (at == std::string::npos) throw EvaluationError(where + ": the reusable workflow needs a ref: " + uses);
      const auto ref = uses.substr(at + 1);
      const auto parts = util::split(uses.substr(0, at), '/');
      if (parts.size() != 5 || !valid_repository_component(parts[0]) ||
          !valid_repository_component(parts[1]) || !valid_workflow_ref(ref)) {
        throw EvaluationError(where + ": invalid reusable workflow reference: " + uses);
      }
      path = parts[2] + "/" + parts[3] + "/" + parts[4];
      if (!valid_workflow_path(path)) throw EvaluationError(where + ": invalid workflow path " + uses);
      const auto entry = run_.repositories.fetch(parts[0], parts[1], ref, [this](std::string_view m) {
        run_.console.line(run_.console.paint(run_.masker.mask(m), Color::Gray));
      });
      called.root = entry.dir;
      called.display = uses;
    }
    if (!valid_workflow_path(path)) throw EvaluationError(where + ": invalid workflow path " + uses);
    called.root = fs::canonical(called.root);
    called.path = fs::canonical(called.root / path);
    if (!util::is_subpath(called.path, called.root)) {
      throw EvaluationError(where + ": invalid workflow path " + uses);
    }
    called.workflow = std::make_shared<workflow::Workflow>(workflow::load_workflow(called.path, called.display));
    return called;
  }

  std::vector<JobReport> call_workflow(JobInstance& inst) {
    const auto& job = *inst.job;
    const auto started = Clock::now();
    JobReport caller;
    caller.key = inst.key;
    caller.job_id = job.id;
    caller.name = inst.name;
    caller.matrix = inst.matrix;
    caller.reason = "calls " + job.uses;
    std::vector<JobReport> nested_reports;
    try {
      if (inst.cancel) {
        caller.result = "cancelled";
        caller.reason = "the calling job was cancelled";
        return {std::move(caller)};
      }
      auto ctx = job_level_context(inst.needs, inst.matrix, inst.strategy);
      if (job.permissions != nullptr) {
        run_.console.line(run_.console.paint(
            run_.masker.mask("[" + inst.name +
                             "] 'permissions' are not emulated; explicitly supplied tokens retain their original permissions"),
            Color::Yellow));
      }
      if (job.concurrency != nullptr) {
        run_.console.line(run_.console.paint(
            run_.masker.mask("[" + inst.name + "] " +
                             describe_concurrency(job.concurrency, ctx, scope_for(Site::JobGeneral), wf_.file)),
            Color::Gray));
      }
      if (wr_.depth + 1 >= kMaxCallDepth) {
        throw EvaluationError("Reusable workflows can be nested up to " + std::to_string(kMaxCallDepth) + " levels");
      }
      const auto where = location(wf_.file, job.node);
      auto called = load_called(job.uses, where);
      if (std::find(wr_.call_stack.begin(), wr_.call_stack.end(), called.path) != wr_.call_stack.end()) {
        throw EvaluationError(where + ": reusable workflow call cycle: " + called.display);
      }
      const auto& cwf = *called.workflow;
      if (cwf.find_trigger("workflow_call") == nullptr) {
        throw EvaluationError(where + ": " + called.display + " is not a reusable workflow (no 'workflow_call' trigger)");
      }
      const auto site = scope_for(Site::JobEnv);
      // inputs
      std::map<std::string, Value> given;
      if (job.with != nullptr && job.with->is_mapping()) {
        for (const auto& e : job.with->entries) {
          given[e.key->text] = evaluate_token(e.value, ctx, scope_for(Site::JobGeneral), wf_.file);
        }
      }
      auto inputs = Value::object();
      for (const auto& [k, v] : given) {
        const auto it = std::find_if(cwf.call_inputs.begin(), cwf.call_inputs.end(),
                                     [&](const workflow::CallInput& in) { return util::iequals(in.name, k); });
        if (it == cwf.call_inputs.end()) {
          throw EvaluationError(where + ": invalid input, " + k + " is not defined in the referenced workflow");
        }
        inputs.set(it->name, convert_call_input(*it, v, where));
      }
      expr::Scope default_scope{{"github", "inputs", "vars"}, false, false, false};
      for (const auto& in : cwf.call_inputs) {
        if (inputs.has(in.name)) continue;
        if (in.default_value != nullptr) {
          auto dctx = ctx;
          dctx.contexts.set("inputs", inputs);
          inputs.set(in.name, convert_call_input(in, evaluate_token(in.default_value, dctx, default_scope, cwf.file),
                                                 cwf.file));
        } else if (in.required) {
          throw EvaluationError(where + ": input required and not supplied: " + in.name);
        } else {
          inputs.set(in.name, in.type == "boolean" ? Value(false) : in.type == "number" ? Value(0.0) : Value(""));
        }
      }
      // secrets
      StringMap secrets;
      if (job.secrets != nullptr && job.secrets->is_scalar() && job.secrets->text == "inherit") {
        secrets = wr_.secrets;
      } else if (job.secrets != nullptr && job.secrets->is_mapping()) {
        for (const auto& e : job.secrets->entries) {
          const auto value = evaluate_string(e.value, ctx, site, wf_.file);
          run_.masker.add(value);
          const auto it = std::find_if(cwf.call_secrets.begin(), cwf.call_secrets.end(),
                                      [&](const auto& s) { return util::iequals(s.name, e.key->text); });
          if (it == cwf.call_secrets.end()) {
            throw EvaluationError(where + ": invalid secret, " + e.key->text +
                                  " is not defined in the referenced workflow");
          }
          secrets[it->name] = value;
        }
      }
      for (const auto& s : cwf.call_secrets) {
        const auto found = std::find_if(secrets.begin(), secrets.end(),
                                        [&](const auto& e) { return util::iequals(e.first, s.name); });
        if (s.required && found == secrets.end()) {
          throw EvaluationError(where + ": secret required and not supplied: " + s.name);
        }
      }
      if (cwf.permissions != nullptr) {
        run_.console.line(run_.console.paint(
            run_.masker.mask("[" + inst.name + "] 'permissions' are not emulated"), Color::Gray));
      }
      WorkflowRun child;
      child.workflow = called.workflow;
      child.file = called.display;
      child.run_id = wr_.run_id;
      child.run_number = wr_.run_number;
      child.name = wr_.name;
      child.workflow_ref = wr_.workflow_ref;
      child.payload = wr_.payload;
      child.inputs = inputs;
      child.secrets = secrets;
      child.vars = wr_.vars;
      child.key_prefix = inst.key + " / ";
      child.depth = wr_.depth + 1;
      child.artifacts = wr_.artifacts;
      child.repo_root = called.root;
      child.call_stack = wr_.call_stack;
      child.call_stack.push_back(called.path);
      std::mutex nested_mutex;
      WorkflowExecutor nested(run_, child, nested_reports, nested_mutex, nullptr, &inst);
      nested.execute();
      std::vector<std::string> results;
      auto jobs = Value::object();
      for (const auto& [id, node] : nested.nodes()) {
        results.push_back(node.result);
        auto entry = Value::object();
        entry.set("result", node.result);
        entry.set("outputs", to_object(node.outputs));
        jobs.set(id, entry);
      }
      caller.result = combine_results(results);
      expr::EvalContext octx;
      auto c = Value::object();
      c.set("github", github_context(run_, child, "", fs::path{}, fs::path{}));
      c.set("jobs", jobs);
      c.set("vars", child.vars);
      c.set("inputs", child.inputs);
      octx.contexts = c;
      expr::Scope output_scope{{"github", "jobs", "vars", "inputs"}, false, false, false};
      for (const auto& out : cwf.call_outputs) {
        if (out.value == nullptr) continue;
        caller.outputs[out.name] = evaluate_string(out.value, octx, output_scope, cwf.file);
      }
    } catch (const std::exception& e) {
      caller.result = "failure";
      caller.reason = e.what();
      run_.console.line(run_.console.paint(run_.masker.mask("[" + inst.name + "] " + e.what()), Color::Red));
    }
    if ((run_.cancelled || inst.cancel) && caller.result == "success") caller.result = "cancelled";
    caller.duration_ms = elapsed_ms(started);
    std::vector<JobReport> out;
    out.push_back(std::move(caller));
    for (auto& r : nested_reports) out.push_back(std::move(r));
    return out;
  }

  RunState& run_;
  WorkflowRun& wr_;
  const workflow::Workflow& wf_;
  std::vector<JobReport>& reports_;
  std::mutex& reports_mutex_;
  std::map<std::string, JobNode> nodes_;
  std::mutex mutex_;
  std::condition_variable cv_;
  JobInstance* parent_;
  std::mutex active_mutex_;
  std::vector<JobInstance*> active_;
};

// Jobs to run: the selected ids and (optionally) their transitive needs.
std::optional<std::set<std::string>> job_selection(const workflow::Workflow& wf, const RunOptions& options) {
  if (options.jobs.empty()) return std::nullopt;
  std::set<std::string> out;
  std::vector<std::string> stack;
  for (const auto& id : options.jobs) {
    if (wf.find_job(id) != nullptr) stack.push_back(id);
  }
  while (!stack.empty()) {
    const auto id = stack.back();
    stack.pop_back();
    if (!out.insert(id).second) continue;
    if (!options.with_needs) continue;
    if (const auto* job = wf.find_job(id)) {
      for (const auto& n : job->needs) stack.push_back(n);
    }
  }
  return out;
}

std::string render_summary(const RunReport& report) {
  std::string out;
  for (const auto& w : report.workflows) {
    for (const auto& j : w.jobs) {
      if (j.summary.empty()) continue;
      out += "## " + w.name + " / " + j.name + "\n\n" + j.summary + "\n";
    }
  }
  return out;
}

Value masked_value(const Value& value, const Masker& masker) {
  if (value.is_string()) return masker.mask(value.as_string());
  if (value.is_array()) {
    auto result = Value::array();
    for (const auto& item : value.items()) result.push(masked_value(item, masker));
    return result;
  }
  if (value.is_object()) {
    auto result = Value::object();
    for (const auto& [key, item] : value.object_data().entries()) {
      result.set(masker.mask(key), masked_value(item, masker));
    }
    return result;
  }
  return value;
}

void mask_report(RunReport& report, const Masker& masker) {
  auto mask_map = [&](StringMap& values) {
    StringMap masked;
    for (const auto& [key, value] : values) masked[masker.mask(key)] = masker.mask(value);
    values = std::move(masked);
  };
  report.run_dir = masker.mask(report.run_dir);
  report.ref = masker.mask(report.ref);
  report.sha = masker.mask(report.sha);
  for (auto& workflow : report.workflows) {
    workflow.file = masker.mask(workflow.file);
    workflow.name = masker.mask(workflow.name);
    for (auto& job : workflow.jobs) {
      job.key = masker.mask(job.key);
      job.job_id = masker.mask(job.job_id);
      job.name = masker.mask(job.name);
      job.reason = masker.mask(job.reason);
      job.matrix = masked_value(job.matrix, masker);
      job.log_file = masker.mask(job.log_file);
      job.summary = masker.mask(job.summary);
      mask_map(job.outputs);
      for (auto& step : job.steps) {
        step.id = masker.mask(step.id);
        step.name = masker.mask(step.name);
        step.error = masker.mask(step.error);
        mask_map(step.outputs);
      }
      for (auto& annotation : job.annotations) {
        annotation.message = masker.mask(annotation.message);
        annotation.title = masker.mask(annotation.title);
        annotation.file = masker.mask(annotation.file);
      }
    }
  }
}

}  // namespace

struct Runner::Impl {
  Impl(RunOptions o, Console& c) : options(std::move(o)), console(c) {}
  RunOptions options;
  Console& console;
  std::mutex mutex;
  RunState* state = nullptr;
  int cancel_requests = 0;
};

Runner::Runner(RunOptions options, Console& console) : impl_(std::make_unique<Impl>(std::move(options), console)) {}
Runner::~Runner() = default;

void Runner::cancel() {
  std::lock_guard lock(impl_->mutex);
  ++impl_->cancel_requests;
  if (impl_->state != nullptr) impl_->state->cancel_all(impl_->cancel_requests > 1);
}

RunReport Runner::run(const std::vector<Selection>& selection, const Event& event, const workspace::GitInfo& git) {
  const auto started = Clock::now();
  auto options = impl_->options;
  for (const auto& selector : options.sandbox.allow_apple_security_for)
    actions::validate_action_permission_selector(selector);
  for (const auto& selector : options.sandbox.allow_docker_for)
    actions::validate_action_permission_selector(selector);
  for (const auto& selector : options.sandbox.allow_msbuild_for)
    actions::validate_action_permission_selector(selector);
  for (const auto& selector : options.sandbox.allow_unity_for)
    actions::validate_action_permission_selector(selector);
  for (const auto& selector : options.sandbox.allow_xcode_for)
    actions::validate_action_permission_selector(selector);
  const auto valid_selector_file = [](const std::string& file) {
    constexpr std::string_view prefix = ".github/workflows/";
    return file.starts_with(prefix) && file.substr(prefix.size()).find('/') == std::string::npos &&
           (file.ends_with(".yml") || file.ends_with(".yaml")) && file.find("..") == std::string::npos;
  };
  const auto valid_selector_id = [](const std::string& id) {
    return !id.empty() && id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") ==
                              std::string::npos &&
           (id.front() == '_' || (id.front() >= 'A' && id.front() <= 'Z') ||
            (id.front() >= 'a' && id.front() <= 'z'));
  };
  const auto validate_step_selector = [&](const std::string& selector, std::string_view permission) {
    const auto first = selector.find("::");
    const auto second = first == std::string::npos ? first : selector.find("::", first + 2);
    const auto file = selector.substr(0, first);
    if (first == std::string::npos || second == std::string::npos || !valid_selector_file(file) ||
        selector.find_first_of("*?[]{}\r\n\t") != std::string::npos ||
        !valid_selector_id(selector.substr(first + 2, second - first - 2)) ||
        !valid_selector_id(selector.substr(second + 2))) {
      throw std::invalid_argument("Invalid " + std::string(permission) + " step selector '" + selector +
                                  "': expected .github/workflows/file.yml::job-id::step-id");
    }
  };
  for (const auto& selector : options.sandbox.allow_docker_step) validate_step_selector(selector, "Docker");
  for (const auto& selector : options.sandbox.allow_msbuild_step) validate_step_selector(selector, "MSBuild");
  for (const auto& selector : options.sandbox.allow_unity_step) validate_step_selector(selector, "Unity");
  for (const auto& selector : options.sandbox.allow_xcode_simulator_job) {
    const auto separator = selector.find("::");
    const auto file = selector.substr(0, separator);
    if (separator == std::string::npos || selector.find("::", separator + 2) != std::string::npos ||
        !valid_selector_file(file) || selector.find_first_of("*?[]{}\r\n\t") != std::string::npos ||
        !valid_selector_id(selector.substr(separator + 2))) {
      throw std::invalid_argument("Invalid Xcode/Simulator job selector '" + selector +
                                  "': expected .github/workflows/file.yml::job-id");
    }
  }
  if (options.sandbox.enabled) {
    std::string reason;
    if (!sandbox::supported(&reason)) throw std::runtime_error("sandbox unavailable: " + reason);
  }
  if (options.cache_dir.empty()) options.cache_dir = default_cache_dir();
  create_private_directory(options.cache_dir, false);
  options.cache_dir = util::real_path(options.cache_dir);
  if (options.run_dir.empty()) options.run_dir = options.cache_dir / "runs" / (timestamp() + "-" + util::random_hex(3));
  create_private_directory(options.run_dir, true);
  options.run_dir = util::real_path(options.run_dir);
  create_private_directory(options.run_dir / "logs", true);
  const RunLock run_lock(options.run_dir);
  if (options.artifact_dir.empty()) options.artifact_dir = options.run_dir / "artifacts";
  options.artifact_dir = fs::weakly_canonical(fs::absolute(options.artifact_dir));
  for (const auto& protected_path : {options.run_dir / "jobs", options.run_dir / "control",
                                      options.run_dir / "snapshot", options.run_dir / "logs"}) {
    if (util::is_subpath(options.artifact_dir, protected_path) ||
        util::is_subpath(protected_path, options.artifact_dir)) {
      throw std::invalid_argument("Artifact directory overlaps a job/control/snapshot/logs directory: " +
                                  options.artifact_dir.string());
    }
  }
  create_private_directory(options.artifact_dir, false);

  for (const auto& id : options.jobs) {
    const bool found = std::any_of(selection.begin(), selection.end(), [&](const Selection& s) {
      return s.match.matched && s.file->workflow->find_job(id) != nullptr;
    });
    if (!found) throw std::runtime_error("No triggered workflow has a job '" + id + "'");
  }

  RunState state(options, impl_->console, event, git);
  struct StateGuard {
    Impl& impl;
    RunState& run;
    ~StateGuard() {
      {
        std::lock_guard lock(impl.mutex);
        impl.state = nullptr;
      }
      if (!run.options.keep_workdir) {
        for (const auto* name : {"snapshot", "jobs"}) {
          try {
            util::remove_tree(run.options.run_dir / name);
          } catch (const std::exception& e) {
            try {
              impl.console.line(run.masker.mask(std::string("Failed to remove run working directory: ") + e.what()));
            } catch (...) {
            }
          }
        }
      }
    }
  } state_guard{*impl_, state};
  {
    std::lock_guard lock(impl_->mutex);
    impl_->state = &state;
    if (impl_->cancel_requests > 0) state.cancel_all(impl_->cancel_requests > 1);
  }

  RunReport report;
  report.run_dir = options.run_dir.string();
  report.event = event.info.name;
  report.ref = event.info.ref;
  report.sha = event.sha;

  struct Item {
    const Selection* selection;
    WorkflowRun run;
    std::optional<std::set<std::string>> jobs;
    std::vector<JobReport> reports;
    std::mutex mutex;
    std::string error;
  };
  std::vector<std::unique_ptr<Item>> items;
  const auto repository = state.repository;
  for (const auto& s : selection) {
    if (!s.match.matched) continue;
    auto item = std::make_unique<Item>();
    item->selection = &s;
    const auto& wf = *s.file->workflow;
    auto& wr = item->run;
    wr.workflow = s.file->workflow;
    wr.file = s.file->display;
    wr.run_id = random_run_id();
    wr.name = wf.display_name();
    wr.workflow_ref = repository + "/" + s.file->display + "@" + event.info.ref;
    wr.payload = event.info.payload.deep_copy();
    wr.inputs = Value::object();
    if (event.info.name == "workflow_dispatch") {
      try {
        wr.inputs = dispatch_inputs(wf, options.inputs);
        wr.payload.set("inputs", dispatch_event_inputs(wr.inputs));
      } catch (const std::exception& e) {
        item->error = e.what();
      }
    }
    wr.secrets = options.secrets;
    wr.vars = to_object(options.vars);
    wr.artifacts = options.artifact_dir / wr.run_id;
    create_private_directory(wr.artifacts, true);
    wr.repo_root = git.root.empty() ? fs::current_path() : git.root;
    wr.call_stack.push_back(fs::weakly_canonical(s.file->path.empty() ? wr.repo_root / wr.file : s.file->path));
    item->jobs = job_selection(wf, options);
    items.push_back(std::move(item));
  }

  std::vector<std::thread> threads;
  for (auto& item_ptr : items) {
    auto* item = item_ptr.get();
    if (!item->error.empty()) continue;
    threads.emplace_back([&state, item, this] {
      const auto& wf = *item->run.workflow;
      auto& console = impl_->console;
      std::string title = item->run.name;
      if (wf.run_name != nullptr) {
        expr::EvalContext ctx;
        auto c = Value::object();
        c.set("github", github_context(state, item->run, "", fs::path{}, fs::path{}));
        c.set("inputs", item->run.inputs);
        c.set("vars", item->run.vars);
        ctx.contexts = c;
        try {
          title = evaluate_string(wf.run_name, ctx, scope_for(Site::RunName), wf.file);
        } catch (const std::exception& e) {
          console.line(console.paint(state.masker.mask(item->run.file + ": " + e.what()), Color::Yellow));
        }
      }
      console.line(console.paint(state.masker.mask("Workflow: " + title + " (" + item->run.file + ")"), Color::Bold));
      if (wf.permissions != nullptr) {
        console.line(console.paint(state.masker.mask(item->run.file + ": 'permissions' are not emulated"), Color::Gray));
      }
      try {
        WorkflowExecutor executor(state, item->run, item->reports, item->mutex,
                                  item->jobs ? &*item->jobs : nullptr);
        executor.execute();
      } catch (const std::exception& e) {
        item->error = e.what();
      }
    });
  }
  for (auto& t : threads) t.join();

  std::vector<std::string> workflow_results;
  for (auto& item : items) {
    WorkflowReport w;
    w.file = item->run.file;
    w.name = item->run.name;
    w.run_id = item->run.run_id;
    std::vector<std::string> results;
    for (auto& r : item->reports) results.push_back(effective_result(r));
    w.result = item->error.empty() ? combine_results(results) : "failure";
    if (!item->error.empty()) {
      impl_->console.line(impl_->console.paint(state.masker.mask(w.file + ": " + item->error), Color::Red));
      JobReport r;
      r.key = r.job_id = r.name = "(workflow)";
      r.result = "failure";
      r.reason = item->error;
      item->reports.push_back(std::move(r));
    }
    w.jobs = std::move(item->reports);
    workflow_results.push_back(w.result);
    report.workflows.push_back(std::move(w));
  }
  report.result = workflow_results.empty() ? "skipped" : combine_results(workflow_results);
  if (state.cancelled && report.result != "failure") report.result = "cancelled";
  report.duration_ms = elapsed_ms(started);

  mask_report(report, state.masker);
  util::write_file(options.run_dir / "report.json", report_to_json(report));
  const auto summary = render_summary(report);
  if (!summary.empty()) util::write_file(options.run_dir / "summary.md", summary);
  return report;
}

std::vector<std::string> runs_on_labels(const Value& runs_on) {
  std::vector<std::string> labels;
  auto add = [&](const Value& v) {
    if (v.is_array()) {
      for (const auto& item : v.items()) labels.push_back(item.to_string());
    } else if (!v.is_null()) {
      labels.push_back(v.to_string());
    }
  };
  if (runs_on.is_object()) {
    if (runs_on.has("group")) labels.push_back("group:" + runs_on.get("group").to_string());
    add(runs_on.get("labels"));
  } else {
    add(runs_on);
  }
  return labels;
}

RunnerMatch match_runner(const std::vector<std::string>& labels, const std::vector<std::string>& host_labels,
                         bool macos) {
  RunnerMatch m;
  if (labels.empty()) {
    m.reason = "runs-on is empty";
    return m;
  }
  for (const auto& l : labels) {
    for (const auto& h : host_labels) {
      if (util::iequals(l, h)) {
        m.host = true;
        m.reason = "--host-label " + h;
        return m;
      }
    }
  }
  bool self_hosted = false;
  bool mac_label = false;
  for (const auto& l : labels) {
    const auto lower = util::to_lower(l);
    if (lower == "self-hosted") self_hosted = true;
    if (lower.starts_with("macos") || lower == "macos" || lower == "osx") mac_label = true;
  }
  if (mac_label && macos) {
    m.host = true;
    m.reason = self_hosted ? "self-hosted macOS runner" : "macOS runner";
    return m;
  }
  if (mac_label) {
    m.reason = "macOS jobs run only on macOS hosts (runs-on: " + util::join(labels, ", ") + ")";
  } else {
    m.reason = "runs-on '" + util::join(labels, ", ") +
               "' is not supported (only macos-* runners run on the host; use --host-label to force)";
  }
  return m;
}

bool host_is_macos() {
#if defined(__APPLE__)
  return true;
#else
  return false;
#endif
}

fs::path default_cache_dir() { return util::user_cache_dir() / "cpp-actions"; }

}  // namespace cpa::engine
