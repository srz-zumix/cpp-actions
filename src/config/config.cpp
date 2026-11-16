#include "config/config.hpp"

#include <cstdlib>
#include <set>

#include "sandbox/proxy.hpp"
#include "util/fs.hpp"
#include "util/strings.hpp"
#include "workflow/yaml.hpp"
#include "workflow/parser.hpp"

namespace cpa::config {

namespace {

bool valid_name(std::string_view name) {
  if (name.empty()) return false;
  for (std::size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    const bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    const bool digit = c >= '0' && c <= '9';
    if (!(alpha || (digit && i > 0) || (c == '-' || c == '.'))) return false;
  }
  return true;
}

[[noreturn]] void fail(std::string_view file, int line, const std::string& message) {
  std::string where = file.empty() ? std::string("<dotenv>") : std::string(file);
  throw ConfigError(where + ":" + std::to_string(line) + ": " + message);
}

std::string node_string(const yaml::Node* n, const std::string& file, std::string_view key) {
  if (n == nullptr || n->is_null()) return {};
  if (!n->is_scalar()) {
    throw ConfigError(file + ":" + std::to_string(n->mark.line) + ": '" + std::string(key) + "' must be a scalar");
  }
  return n->scalar_string();
}

std::vector<std::string> node_strings(const yaml::Node* n, const std::string& file, std::string_view key) {
  std::vector<std::string> out;
  if (n == nullptr || n->is_null()) return out;
  if (n->is_scalar()) {
    out.push_back(n->scalar_string());
    return out;
  }
  if (!n->is_sequence()) {
    throw ConfigError(file + ":" + std::to_string(n->mark.line) + ": '" + std::string(key) +
                      "' must be a string or a list");
  }
  for (const auto* item : n->items) out.push_back(node_string(item, file, key));
  return out;
}

std::map<std::string, std::string> node_map(const yaml::Node* n, const std::string& file, std::string_view key) {
  std::map<std::string, std::string> out;
  if (n == nullptr || n->is_null()) return out;
  if (!n->is_mapping()) {
    throw ConfigError(file + ":" + std::to_string(n->mark.line) + ": '" + std::string(key) + "' must be a mapping");
  }
  for (const auto& e : n->entries) out[e.key->text] = node_string(e.value, file, e.key->text);
  return out;
}

}  // namespace

fs::path default_config_path() {
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
    return fs::path(xdg) / "cpp-actions" / "config.yml";
  }
  return util::home_dir() / ".config" / "cpp-actions" / "config.yml";
}

fs::path expand_user(std::string_view path) {
  if (path == "~") return util::home_dir();
  if (path.starts_with("~/")) return util::home_dir() / std::string(path.substr(2));
  return fs::path(std::string(path));
}

std::pair<std::string, std::optional<std::string>> split_assignment(std::string_view text) {
  const auto eq = text.find('=');
  if (eq == std::string_view::npos) return {std::string(text), std::nullopt};
  return {std::string(text.substr(0, eq)), std::string(text.substr(eq + 1))};
}

std::map<std::string, std::string> parse_dotenv(std::string_view text, std::string_view file) {
  std::map<std::string, std::string> out;
  const std::size_t n = text.size();
  std::size_t i = 0;
  int line = 1;
  auto skip_blank = [&] {
    while (i < n && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r')) ++i;
  };
  auto skip_line = [&] {
    const auto eol = text.find('\n', i);
    i = eol == std::string_view::npos ? n : eol;
  };
  while (i < n) {
    skip_blank();
    if (i >= n) break;
    if (text[i] == '\n') {
      ++line;
      ++i;
      continue;
    }
    if (text[i] == '#') {
      skip_line();
      continue;
    }
    auto eol = text.find('\n', i);
    if (eol == std::string_view::npos) eol = n;
    const auto head = text.substr(i, eol - i);
    const auto eq = head.find('=');
    if (eq == std::string_view::npos) fail(file, line, "expected NAME=VALUE");
    auto name_part = util::trim(head.substr(0, eq));
    if (name_part.starts_with("export ")) name_part = util::trim(name_part.substr(7));
    const std::string name(name_part);
    if (!valid_name(name)) fail(file, line, "invalid name '" + name + "'");
    i += eq + 1;
    while (i < n && (text[i] == ' ' || text[i] == '\t')) ++i;
    std::string value;
    if (i < n && text[i] == '\'') {
      const auto close = text.find('\'', i + 1);
      if (close == std::string_view::npos) fail(file, line, "unterminated single quote");
      value = std::string(text.substr(i + 1, close - i - 1));
      for (char c : value) line += c == '\n' ? 1 : 0;
      i = close + 1;
    } else if (i < n && text[i] == '"') {
      ++i;
      bool closed = false;
      while (i < n) {
        const char c = text[i++];
        if (c == '"') {
          closed = true;
          break;
        }
        if (c == '\\' && i < n) {
          const char e = text[i++];
          switch (e) {
            case 'n':
              value.push_back('\n');
              break;
            case 'r':
              value.push_back('\r');
              break;
            case 't':
              value.push_back('\t');
              break;
            default:
              if (e == '\n') ++line;
              value.push_back(e);
              break;
          }
          continue;
        }
        if (c == '\n') ++line;
        value.push_back(c);
      }
      if (!closed) fail(file, line, "unterminated double quote");
    } else {
      auto v = text.substr(i, eol - i);
      const auto hash = v.find(" #");
      if (hash != std::string_view::npos) v = v.substr(0, hash);
      value = std::string(util::trim(v));
      i = eol;
    }
    skip_blank();
    if (i < n && text[i] == '#') skip_line();
    if (i < n && text[i] != '\n') fail(file, line, "unexpected characters after the value of '" + name + "'");
    out[name] = std::move(value);
  }
  return out;
}

std::map<std::string, std::string> load_dotenv(const fs::path& path) {
  const auto text = util::try_read_file(path);
  if (!text) throw ConfigError("cannot read " + path.string());
  return parse_dotenv(*text, path.string());
}

Config load_config(const fs::path& path) {
  const std::string file = path.string();
  std::shared_ptr<yaml::Document> doc;
  try {
    doc = yaml::load_file(path, file);
  } catch (const yaml::LoadError& e) {
    throw ConfigError(e.what());
  }
  Config c;
  const auto* root = doc->root;
  if (root == nullptr || root->is_null()) return c;
  if (!root->is_mapping()) throw ConfigError(file + ": the configuration must be a mapping");
  for (const auto& e : root->entries) {
    const std::string& key = e.key->text;
    const auto* v = e.value;
    if (key == "setup-steps") {
      try {
        auto steps = workflow::parse_setup_steps(*v, file);
        if (!steps.empty()) c.setup_steps.push_back({doc, std::move(steps)});
      } catch (const workflow::WorkflowError& error) {
        throw ConfigError(error.what());
      }
    } else if (key == "allow-hosts") {
      c.allow_hosts = node_strings(v, file, key);
    } else if (key == "allow-apple-security-for") {
      c.allow_apple_security_for = node_strings(v, file, key);
    } else if (key == "allow-docker-for") {
      c.allow_docker_for = node_strings(v, file, key);
    } else if (key == "allow-docker-step") {
      c.allow_docker_step = node_strings(v, file, key);
    } else if (key == "allow-msbuild-for") {
      c.allow_msbuild_for = node_strings(v, file, key);
    } else if (key == "allow-msbuild-step") {
      c.allow_msbuild_step = node_strings(v, file, key);
    } else if (key == "allow-unity-for") {
      c.allow_unity_for = node_strings(v, file, key);
    } else if (key == "allow-unity-step") {
      c.allow_unity_step = node_strings(v, file, key);
    } else if (key == "allow-xcode-for") {
      c.allow_xcode_for = node_strings(v, file, key);
    } else if (key == "allow-read") {
      c.allow_read = node_strings(v, file, key);
    } else if (key == "allow-write") {
      c.allow_write = node_strings(v, file, key);
    } else if (key == "allow-localhost") {
      if (v == nullptr || v->kind != yaml::NodeKind::Boolean) {
        throw ConfigError(file + ":" + std::to_string(e.key->mark.line) + ": 'allow-localhost' must be a boolean");
      }
      c.allow_localhost = v->boolean;
    } else if (key == "allow-xcode") {
      if (v == nullptr || v->kind != yaml::NodeKind::Boolean) {
        throw ConfigError(file + ":" + std::to_string(e.key->mark.line) + ": 'allow-xcode' must be a boolean");
      }
      c.allow_xcode = v->boolean;
    } else if (key == "allow-xcode-simulator") {
      if (v == nullptr || v->kind != yaml::NodeKind::Boolean) {
        throw ConfigError(file + ":" + std::to_string(e.key->mark.line) + ": 'allow-xcode-simulator' must be a boolean");
      }
      c.allow_xcode_simulator = v->boolean;
    } else if (key == "allow-unity-hosts") {
      if (v == nullptr || v->kind != yaml::NodeKind::Boolean) {
        throw ConfigError(file + ":" + std::to_string(e.key->mark.line) + ": 'allow-unity-hosts' must be a boolean");
      }
      c.allow_unity_hosts = v->boolean;
    } else if (key == "inherit-unity-editor-prefs") {
      if (v == nullptr || v->kind != yaml::NodeKind::Boolean) {
        throw ConfigError(file + ":" + std::to_string(e.key->mark.line) +
                          ": 'inherit-unity-editor-prefs' must be a boolean");
      }
      c.inherit_unity_editor_prefs = v->boolean;
    } else if (key == "ignore-interactive-shell") {
      if (v == nullptr || v->kind != yaml::NodeKind::Boolean) {
        throw ConfigError(file + ":" + std::to_string(e.key->mark.line) +
                          ": 'ignore-interactive-shell' must be a boolean");
      }
      c.ignore_interactive_shell = v->boolean;
    } else if (key == "env-pass") {
      c.pass_env = node_strings(v, file, key);
    } else if (key == "host-labels") {
      c.host_labels = node_strings(v, file, key);
    } else if (key == "proxy-max-connections") {
      const auto n = util::parse_int(node_string(v, file, key));
      if (!n || *n < 1 || *n > sandbox::kMaxProxyConnections) {
        throw ConfigError(file + ":" + std::to_string(e.key->mark.line) +
                          ": 'proxy-max-connections' must be an integer from 1 to " +
                          std::to_string(sandbox::kMaxProxyConnections));
      }
      c.proxy_max_connections = static_cast<int>(*n);
    } else if (key == "parallel") {
      const auto n = util::parse_int(node_string(v, file, key));
      if (!n || *n < 1) {
        throw ConfigError(file + ":" + std::to_string(e.key->mark.line) + ": 'parallel' must be a positive integer");
      }
      c.parallel = static_cast<int>(*n);
    } else if (key == "node") {
      c.node = node_string(v, file, key);
    } else if (key == "cache-dir") {
      c.cache_dir = node_string(v, file, key);
    } else if (key == "secrets-file") {
      c.secrets_file = node_string(v, file, key);
    } else if (key == "vars-file") {
      c.vars_file = node_string(v, file, key);
    } else if (key == "vars") {
      c.vars = node_map(v, file, key);
    } else if (key == "environments") {
      if (v == nullptr || !v->is_mapping()) {
        throw ConfigError(file + ":" + std::to_string(e.key->mark.line) + ": 'environments' must be a mapping");
      }
      for (const auto& env : v->entries) {
        EnvironmentConfig ec;
        if (env.value != nullptr && !env.value->is_null()) {
          if (!env.value->is_mapping()) {
            throw ConfigError(file + ":" + std::to_string(env.key->mark.line) + ": environment '" + env.key->text +
                              "' must be a mapping");
          }
          for (const auto& f : env.value->entries) {
            if (f.key->text == "secrets-file") {
              ec.secrets_file = node_string(f.value, file, f.key->text);
            } else if (f.key->text == "vars-file") {
              ec.vars_file = node_string(f.value, file, f.key->text);
            } else if (f.key->text == "vars") {
              ec.vars = node_map(f.value, file, f.key->text);
            } else {
              throw ConfigError(file + ":" + std::to_string(f.key->mark.line) + ": unknown key '" + f.key->text +
                                "' in environment '" + env.key->text + "'");
            }
          }
        }
        c.environments[env.key->text] = std::move(ec);
      }
    } else {
      throw ConfigError(file + ":" + std::to_string(e.key->mark.line) + ": unknown key '" + key + "'");
    }
  }
  return c;
}

std::vector<fs::path> configuration_paths(const fs::path& repo_root, const fs::path& explicit_path) {
  if (!explicit_path.empty()) return {expand_user(explicit_path.string())};
  std::vector<fs::path> paths;
  const auto user_path = default_config_path();
  if (fs::exists(fs::symlink_status(user_path))) {
    if (util::is_subpath(util::real_path(user_path), util::real_path(repo_root))) {
      throw ConfigError("automatic configuration is inside the repository; use --config to trust it explicitly");
    }
    paths.push_back(user_path);
  }
  const auto repo_path = repo_root / ".cpp-actions.yml";
  if (fs::exists(fs::symlink_status(repo_path))) {
    if (!fs::is_regular_file(repo_path)) {
      throw ConfigError("repository configuration is not a regular file: " + repo_path.string());
    }
    paths.push_back(repo_path);
  }
  return paths;
}

Config load_configs(const std::vector<fs::path>& paths) {
  Config merged;
  const auto append = [](auto& target, const auto& source) {
    target.insert(target.end(), source.begin(), source.end());
  };
  const auto override_value = [](auto& target, const auto& source) {
    if (source) target = source;
  };
  const auto merge_values = [](auto& target, const auto& source) {
    for (const auto& [key, value] : source) {
      // Variable names are case-insensitive in expressions; drop earlier
      // spellings so the later file keeps precedence.
      for (auto it = target.begin(); it != target.end();) {
        it = util::iequals(it->first, key) ? target.erase(it) : std::next(it);
      }
      target[key] = value;
    }
  };
  for (const auto& path : paths) {
    auto config = load_config(path);
    const auto directory = fs::absolute(path).parent_path();
    const auto resolve = [&](const std::string& value) {
      const auto expanded = expand_user(value);
      return (expanded.is_absolute() ? expanded : directory / expanded).lexically_normal().string();
    };
    const auto resolve_optional = [&](std::optional<std::string>& value) {
      if (value && !value->empty()) *value = resolve(*value);
    };
    for (auto& value : config.allow_read) value = resolve(value);
    for (auto& value : config.allow_write) value = resolve(value);
    resolve_optional(config.node);
    resolve_optional(config.cache_dir);
    resolve_optional(config.secrets_file);
    resolve_optional(config.vars_file);
    append(merged.setup_steps, config.setup_steps);
    append(merged.allow_hosts, config.allow_hosts);
    append(merged.allow_apple_security_for, config.allow_apple_security_for);
    append(merged.allow_docker_for, config.allow_docker_for);
    append(merged.allow_docker_step, config.allow_docker_step);
    append(merged.allow_msbuild_for, config.allow_msbuild_for);
    append(merged.allow_msbuild_step, config.allow_msbuild_step);
    append(merged.allow_unity_for, config.allow_unity_for);
    append(merged.allow_unity_step, config.allow_unity_step);
    append(merged.allow_xcode_for, config.allow_xcode_for);
    append(merged.allow_read, config.allow_read);
    append(merged.allow_write, config.allow_write);
    append(merged.pass_env, config.pass_env);
    append(merged.host_labels, config.host_labels);
    override_value(merged.allow_localhost, config.allow_localhost);
    override_value(merged.allow_xcode, config.allow_xcode);
    override_value(merged.allow_xcode_simulator, config.allow_xcode_simulator);
    override_value(merged.allow_unity_hosts, config.allow_unity_hosts);
    override_value(merged.ignore_interactive_shell, config.ignore_interactive_shell);
    override_value(merged.inherit_unity_editor_prefs, config.inherit_unity_editor_prefs);
    override_value(merged.proxy_max_connections, config.proxy_max_connections);
    override_value(merged.parallel, config.parallel);
    override_value(merged.node, config.node);
    override_value(merged.cache_dir, config.cache_dir);
    override_value(merged.secrets_file, config.secrets_file);
    override_value(merged.vars_file, config.vars_file);
    merge_values(merged.vars, config.vars);
    for (auto& [name, environment] : config.environments) {
      resolve_optional(environment.secrets_file);
      resolve_optional(environment.vars_file);
      auto& target = merged.environments[util::to_lower(name)];
      override_value(target.secrets_file, environment.secrets_file);
      override_value(target.vars_file, environment.vars_file);
      merge_values(target.vars, environment.vars);
    }
  }
  std::set<std::string> ids;
  for (const auto& group : merged.setup_steps) {
    for (const auto& step : group.steps) {
      if (step.explicit_id && !ids.insert(util::to_lower(step.id)).second) {
        throw ConfigError(workflow::Diagnostic{group.document->file, step.node->mark,
                                              "Duplicate setup step id '" + step.id + "' across configuration files"}
                              .to_string());
      }
    }
  }
  int index = 0;
  for (auto& group : merged.setup_steps) {
    for (auto& step : group.steps) {
      step.index = index++;
      if (step.explicit_id) continue;
      std::string id = "__run";
      for (int n = 2; ids.count(id) != 0; ++n) id = "__run_" + std::to_string(n);
      ids.insert(id);
      step.id = std::move(id);
    }
  }
  return merged;
}

}  // namespace cpa::config
