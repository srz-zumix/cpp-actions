#include <CLI/CLI.hpp>

#include <iostream>

int main(int argc, char** argv) {
  CLI::App app{"cpp-actions - run GitHub Actions workflows locally in a sandbox"};
  app.set_version_flag("--version", std::string(CPA_VERSION));
  app.require_subcommand(1);
  app.add_subcommand("run", "Run workflows");
  app.add_subcommand("list", "List workflows and jobs");
  app.add_subcommand("validate", "Validate workflow files");
  app.add_subcommand("graph", "Show the job dependency graph");
  CLI11_PARSE(app, argc, argv);
  std::cerr << "not implemented yet\n";
  return 2;
}
