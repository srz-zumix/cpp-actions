#include "engine/event.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>

#include "util/fs.hpp"
#include "util/strings.hpp"
#include "workflow/parser.hpp"

namespace cpa::engine {

using expr::Value;

namespace {

const std::string kZeroSha(40, '0');

bool is_pull_request_event(std::string_view name) {
  return name == "pull_request" || name == "pull_request_target" || name == "pull_request_review" ||
         name == "pull_request_review_comment";
}

std::string default_activity(std::string_view name) {
  if (name == "pull_request" || name == "pull_request_target") return "opened";
  if (name == "pull_request_review") return "submitted";
  if (name == "pull_request_review_comment" || name == "issue_comment") return "created";
  if (name == "release") return "published";
  if (name == "issues") return "opened";
  if (name == "workflow_run") return "completed";
  return {};
}

std::string string_at(const Value& v, std::initializer_list<std::string_view> path) {
  Value cur = v;
  for (auto key : path) {
    if (!cur.is_object()) return {};
    cur = cur.get(key);
  }
  return cur.is_string() ? cur.as_string() : std::string();
}

std::string iso_now() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

Value repository_object(const workspace::GitInfo& git) {
  Value repo = Value::object();
  const std::string owner = git.github ? git.github->owner : "local";
  const std::string name = git.github ? git.github->name : git.root.filename().string();
  repo.set("id", 0);
  repo.set("name", name);
  repo.set("full_name", owner + "/" + name);
  Value o = Value::object();
  o.set("login", owner);
  o.set("id", 0);
  repo.set("owner", o);
  repo.set("private", false);
  repo.set("fork", false);
  repo.set("default_branch", git.default_branch);
  repo.set("html_url", "https://github.com/" + owner + "/" + name);
  repo.set("clone_url", "https://github.com/" + owner + "/" + name + ".git");
  return repo;
}

Value user_object(const std::string& login) {
  Value u = Value::object();
  u.set("login", login);
  u.set("id", 0);
  u.set("type", "User");
  return u;
}

Value generate_payload(const Event& ev, const workspace::GitInfo& git, int number) {
  Value p = Value::object();
  const auto& name = ev.info.name;
  if (!ev.info.action.empty()) p.set("action", ev.info.action);
  if (name == "push") {
    p.set("ref", ev.info.ref);
    p.set("before", kZeroSha);
    p.set("after", ev.sha);
    p.set("created", false);
    p.set("deleted", false);
    p.set("forced", false);
    p.set("base_ref", nullptr);
    Value commit = Value::object();
    commit.set("id", ev.sha);
    commit.set("message", git.head_message);
    commit.set("timestamp", iso_now());
    Value author = Value::object();
    author.set("name", ev.actor);
    author.set("username", ev.actor);
    commit.set("author", author);
    commit.set("committer", author);
    p.set("commits", Value::array({commit}));
    p.set("head_commit", commit);
    Value pusher = Value::object();
    pusher.set("name", ev.actor);
    p.set("pusher", pusher);
  } else if (is_pull_request_event(name)) {
    p.set("number", number);
    Value pr = Value::object();
    pr.set("number", number);
    pr.set("state", "open");
    pr.set("title", git.head_message);
    pr.set("draft", false);
    pr.set("merged", false);
    pr.set("user", user_object(ev.actor));
    pr.set("labels", Value::array());
    Value head = Value::object();
    head.set("ref", ev.head_ref);
    head.set("sha", ev.sha);
    head.set("label", ev.head_ref);
    head.set("repo", repository_object(git));
    pr.set("head", head);
    Value base = Value::object();
    base.set("ref", ev.info.base_ref);
    base.set("label", ev.info.base_ref);
    base.set("repo", repository_object(git));
    pr.set("base", base);
    p.set("pull_request", pr);
  } else if (name == "workflow_dispatch") {
    p.set("ref", ev.info.ref);
    p.set("inputs", Value::object());
  } else if (name == "release") {
    Value release = Value::object();
    release.set("tag_name", ref_short_name(ev.info.ref));
    release.set("name", ref_short_name(ev.info.ref));
    release.set("draft", false);
    release.set("prerelease", false);
    p.set("release", release);
  } else if (name == "create" || name == "delete") {
    p.set("ref", ref_short_name(ev.info.ref));
    p.set("ref_type", ref_type(ev.info.ref));
  }
  p.set("repository", repository_object(git));
  p.set("sender", user_object(ev.actor));
  return p;
}

std::optional<std::string> merge_base(const std::filesystem::path& root, const std::string& other) {
  for (const auto& candidate : {"origin/" + other, other}) {
    if (auto mb = workspace::git_output(root, {"merge-base", "HEAD", candidate})) {
      if (!mb->empty()) return *mb;
    }
  }
  return std::nullopt;
}

void compute_changed_files(Event& ev, const EventOptions& o, const workspace::GitInfo& git) {
  if (!git.is_repo || git.head_sha.empty()) {
    ev.changes_basis = "not a git repository; 'paths' filters are not evaluated";
    return;
  }
  const auto& name = ev.info.name;
  const bool pr = name == "pull_request" || name == "pull_request_target";
  if (name != "push" && !pr) return;
  std::string base = o.changes_base;
  bool automatic = base.empty();
  if (automatic) {
    if (pr) {
      if (auto mb = merge_base(git.root, ev.info.base_ref)) {
        base = *mb;
        ev.changes_basis = "changes since the merge base with '" + ev.info.base_ref + "'";
      }
    } else if (!git.upstream.empty()) {
      if (auto mb = workspace::git_output(git.root, {"merge-base", "HEAD", git.upstream})) {
        base = *mb;
        ev.changes_basis = "changes not pushed to '" + git.upstream + "'";
      }
    }
    if (base.empty()) {
      base = "HEAD";
      ev.changes_basis = "uncommitted changes";
    }
  } else {
    ev.changes_basis = "changes since '" + base + "'";
  }
  auto files = workspace::changed_files_since(git.root, base);
  if (!files) throw EventError("Cannot resolve revision '" + base + "'");
  if (files->empty() && automatic) {
    files = workspace::files_of_commit(git.root, "HEAD");
    ev.changes_basis = "no pending changes; files of the HEAD commit";
  }
  ev.info.changed_files = std::move(*files);
}

}  // namespace

std::string ref_short_name(std::string_view ref) {
  for (std::string_view prefix : {"refs/heads/", "refs/tags/", "refs/pull/", "refs/remotes/"}) {
    if (ref.starts_with(prefix)) return std::string(ref.substr(prefix.size()));
  }
  return std::string(ref);
}

std::string ref_type(std::string_view ref) {
  if (ref.starts_with("refs/heads/")) return "branch";
  if (ref.starts_with("refs/tags/")) return "tag";
  return {};
}

std::string normalize_ref(std::string_view ref) {
  if (ref.starts_with("refs/")) return std::string(ref);
  return "refs/heads/" + std::string(ref);
}

Event build_event(const EventOptions& o, const workspace::GitInfo& git) {
  Event ev;
  ev.info.name = o.name.empty() ? "push" : o.name;
  const auto& name = ev.info.name;
  if (!workflow::is_known_event(name)) throw EventError("Unknown event '" + name + "'");
  if (!o.actor.empty()) {
    ev.actor = o.actor;
  } else if (const char* user = std::getenv("USER"); user != nullptr && *user != '\0') {
    ev.actor = user;
  } else {
    ev.actor = "local";
  }
  ev.sha = git.head_sha.empty() ? kZeroSha : git.head_sha;

  Value payload;
  const bool from_file = !o.payload_file.empty();
  if (from_file) {
    try {
      payload = Value::parse_json(util::read_file(o.payload_file));
    } catch (const std::exception& e) {
      throw EventError("Cannot read the event payload '" + o.payload_file.string() + "': " + e.what());
    }
    if (!payload.is_object()) throw EventError("The event payload must be a JSON object");
  }

  const std::string current_branch = !git.branch.empty() ? git.branch : git.default_branch;
  int number = 1;
  if (is_pull_request_event(name)) {
    ev.info.base_ref = !o.base_ref.empty() ? o.base_ref : string_at(payload, {"pull_request", "base", "ref"});
    if (ev.info.base_ref.empty()) ev.info.base_ref = git.default_branch;
    ev.head_ref = !o.head_ref.empty() ? o.head_ref : string_at(payload, {"pull_request", "head", "ref"});
    if (ev.head_ref.empty()) ev.head_ref = current_branch;
    for (const auto& v : {payload.get("number"), payload.get("pull_request").get("number")}) {
      if (v.is_number() && v.as_number() >= 1) {
        number = static_cast<int>(v.as_number());
        break;
      }
    }
    if (!o.ref.empty()) {
      ev.info.ref = normalize_ref(o.ref);
    } else if (name == "pull_request_target") {
      ev.info.ref = "refs/heads/" + ev.info.base_ref;
    } else {
      ev.info.ref = "refs/pull/" + std::to_string(number) + "/merge";
    }
  } else if (!o.ref.empty()) {
    ev.info.ref = normalize_ref(o.ref);
  } else if (const auto r = string_at(payload, {"ref"}); !r.empty() && r.starts_with("refs/")) {
    ev.info.ref = r;
  } else if (name == "schedule") {
    ev.info.ref = "refs/heads/" + git.default_branch;
  } else if ((name == "push" || name == "release") && git.branch.empty() && !git.tags.empty()) {
    ev.info.ref = "refs/tags/" + git.tags.front();
  } else {
    ev.info.ref = "refs/heads/" + current_branch;
  }
  if (!o.activity_type.empty()) {
    ev.info.action = o.activity_type;
  } else if (const auto a = string_at(payload, {"action"}); !a.empty()) {
    ev.info.action = a;
  } else {
    ev.info.action = default_activity(name);
  }

  if (from_file) {
    if (!o.activity_type.empty()) payload.set("action", o.activity_type);
    ev.info.payload = payload;
  } else {
    ev.info.payload = generate_payload(ev, git, number);
  }
  compute_changed_files(ev, o, git);
  return ev;
}

std::string render_event(const Event& ev) {
  std::string out = "Event: " + ev.info.name;
  if (!ev.info.action.empty()) out += " (" + ev.info.action + ")";
  out += "\n";
  if (!ev.info.ref.empty()) out += "  ref: " + ev.info.ref + "\n";
  if (!ev.info.base_ref.empty()) out += "  base: " + ev.info.base_ref + "\n";
  if (!ev.head_ref.empty()) out += "  head: " + ev.head_ref + "\n";
  if (!ev.sha.empty()) out += "  sha: " + ev.sha + "\n";
  out += "  actor: " + ev.actor + "\n";
  if (ev.info.changed_files) {
    out += "  changed files: " + std::to_string(ev.info.changed_files->size());
    if (!ev.changes_basis.empty()) out += " (" + ev.changes_basis + ")";
    out += "\n";
  } else if (!ev.changes_basis.empty()) {
    out += "  changed files: " + ev.changes_basis + "\n";
  }
  return out;
}

expr::Value dispatch_inputs(const workflow::Workflow& wf, const std::map<std::string, std::string>& given) {
  auto find_declared = [&](std::string_view key) -> const workflow::DispatchInput* {
    for (const auto& in : wf.dispatch_inputs) {
      if (util::iequals(in.name, key)) return &in;
    }
    return nullptr;
  };
  for (const auto& [key, value] : given) {
    (void)value;
    if (find_declared(key) == nullptr) {
      throw EventError("Unexpected input '" + key + "' for workflow '" + wf.display_name() + "'");
    }
  }
  Value out = Value::object();
  for (const auto& in : wf.dispatch_inputs) {
    std::optional<std::string> raw;
    for (const auto& [key, value] : given) {
      if (util::iequals(key, in.name)) raw = value;
    }
    if (!raw && in.default_value != nullptr && !in.default_value->is_null()) raw = in.default_value->scalar_string();
    if (!raw && in.required) {
      throw EventError("Required input '" + in.name + "' of workflow '" + wf.display_name() + "' was not provided");
    }
    Value v;
    if (in.type == "boolean") {
      if (!raw || util::iequals(*raw, "false")) {
        v = false;
      } else if (util::iequals(*raw, "true")) {
        v = true;
      } else {
        throw EventError("Input '" + in.name + "' must be 'true' or 'false', got '" + *raw + "'");
      }
    } else if (in.type == "number") {
      if (raw && !util::trim(*raw).empty()) {
        const double d = util::parse_number(*raw);
        if (d != d) throw EventError("Input '" + in.name + "' must be a number, got '" + *raw + "'");
        v = d;
      } else {
        v = "";
      }
    } else if (in.type == "choice") {
      if (raw && std::find(in.options.begin(), in.options.end(), *raw) == in.options.end()) {
        throw EventError("Input '" + in.name + "' must be one of " + util::join(in.options, ", ") + ", got '" +
                         *raw + "'");
      }
      v = raw.value_or("");
    } else {
      v = raw.value_or("");
    }
    out.set(in.name, v);
  }
  return out;
}

expr::Value dispatch_event_inputs(const expr::Value& typed) {
  Value out = Value::object();
  if (!typed.is_object()) return out;
  for (const auto& [k, v] : typed.object_data().entries()) out.set(k, v.to_string());
  return out;
}

}  // namespace cpa::engine
