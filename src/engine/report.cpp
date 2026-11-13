#include "engine/report.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace cpa::engine {

namespace {

nlohmann::ordered_json string_map(const std::map<std::string, std::string>& m) {
  auto j = nlohmann::ordered_json::object();
  for (const auto& [k, v] : m) j[k] = v;
  return j;
}

nlohmann::ordered_json annotation_json(const Annotation& a) {
  nlohmann::ordered_json j;
  j["level"] = a.level;
  j["message"] = a.message;
  if (!a.title.empty()) j["title"] = a.title;
  if (!a.file.empty()) j["file"] = a.file;
  if (a.line > 0) j["line"] = a.line;
  if (a.end_line > 0) j["end_line"] = a.end_line;
  if (a.column > 0) j["column"] = a.column;
  if (a.end_column > 0) j["end_column"] = a.end_column;
  return j;
}

}  // namespace

std::string combine_results(const std::vector<std::string>& results) {
  auto has = [&](std::string_view r) { return std::find(results.begin(), results.end(), r) != results.end(); };
  if (has("failure")) return "failure";
  if (has("cancelled")) return "cancelled";
  if (has("success")) return "success";
  return "skipped";
}

std::string report_to_json(const RunReport& report) {
  nlohmann::ordered_json j;
  j["version"] = 1;
  j["run_dir"] = report.run_dir;
  j["event"] = report.event;
  j["ref"] = report.ref;
  j["sha"] = report.sha;
  j["result"] = report.result;
  j["duration_ms"] = report.duration_ms;
  auto workflows = nlohmann::ordered_json::array();
  for (const auto& w : report.workflows) {
    nlohmann::ordered_json wj;
    wj["file"] = w.file;
    wj["name"] = w.name;
    wj["run_id"] = w.run_id;
    wj["result"] = w.result;
    auto jobs = nlohmann::ordered_json::array();
    for (const auto& job : w.jobs) {
      nlohmann::ordered_json jj;
      jj["key"] = job.key;
      jj["id"] = job.job_id;
      jj["name"] = job.name;
      jj["result"] = job.result;
      if (!job.reason.empty()) jj["reason"] = job.reason;
      if (job.continue_on_error) jj["continue_on_error"] = true;
      if (!job.matrix.is_null()) jj["matrix"] = job.matrix.to_json();
      jj["outputs"] = string_map(job.outputs);
      jj["duration_ms"] = job.duration_ms;
      if (!job.log_file.empty()) jj["log"] = job.log_file;
      auto steps = nlohmann::ordered_json::array();
      for (const auto& s : job.steps) {
        nlohmann::ordered_json sj;
        sj["id"] = s.id;
        sj["name"] = s.name;
        sj["outcome"] = s.outcome;
        sj["conclusion"] = s.conclusion;
        sj["duration_ms"] = s.duration_ms;
        sj["outputs"] = string_map(s.outputs);
        if (!s.error.empty()) sj["error"] = s.error;
        if (s.post) sj["post"] = true;
        steps.push_back(std::move(sj));
      }
      jj["steps"] = std::move(steps);
      auto annotations = nlohmann::ordered_json::array();
      for (const auto& a : job.annotations) annotations.push_back(annotation_json(a));
      jj["annotations"] = std::move(annotations);
      if (!job.summary.empty()) jj["summary"] = job.summary;
      jobs.push_back(std::move(jj));
    }
    wj["jobs"] = std::move(jobs);
    workflows.push_back(std::move(wj));
  }
  j["workflows"] = std::move(workflows);
  return j.dump(2) + "\n";
}

}  // namespace cpa::engine
