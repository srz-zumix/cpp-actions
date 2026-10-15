#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "expr/value.hpp"

namespace cpa::engine {

struct Annotation {
  std::string level;  // error, warning, notice
  std::string message;
  std::string title;
  std::string file;
  int line = 0;
  int end_line = 0;
  int column = 0;
  int end_column = 0;
};

struct StepReport {
  std::string id;
  std::string name;
  std::string outcome = "skipped";     // success, failure, cancelled, skipped
  std::string conclusion = "skipped";  // after continue-on-error
  std::int64_t duration_ms = 0;
  std::map<std::string, std::string> outputs;
  std::string error;
  bool post = false;
};

struct JobReport {
  std::string key;  // unique key (job id, matrix suffix, reusable workflow path)
  std::string job_id;
  std::string name;
  std::string result = "skipped";  // success, failure, cancelled, skipped
  std::string reason;              // why the job was skipped or failed before running steps
  bool continue_on_error = false;  // a failure does not fail the workflow
  expr::Value matrix;
  std::map<std::string, std::string> outputs;
  std::vector<StepReport> steps;
  std::vector<Annotation> annotations;
  std::int64_t duration_ms = 0;
  std::string log_file;
  std::string summary;  // step summaries (markdown)
};

struct WorkflowReport {
  std::string file;
  std::string name;
  std::string run_id;
  std::string result = "skipped";
  std::vector<JobReport> jobs;
};

struct RunReport {
  std::string run_dir;
  std::string event;
  std::string ref;
  std::string sha;
  std::string result = "success";
  std::int64_t duration_ms = 0;
  std::vector<WorkflowReport> workflows;
};

// JSON report (schema version 1).
std::string report_to_json(const RunReport& report);

// Aggregated result: failure > cancelled > success > skipped.
std::string combine_results(const std::vector<std::string>& results);

}  // namespace cpa::engine
