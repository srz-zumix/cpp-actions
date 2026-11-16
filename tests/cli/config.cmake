if(NOT DEFINED CLI OR NOT DEFINED WORK)
  message(FATAL_ERROR "CLI and WORK must be provided")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../cleanup_workspaces.cmake")
cleanup_workspaces("${WORK}")
file(REMOVE_RECURSE "${WORK}")
set(repo "${WORK}/repo")
set(user_dir "${WORK}/config/cpp-actions")
set(user_config "${user_dir}/config.yml")
set(repo_config "${repo}/.cpp-actions.yml")
file(MAKE_DIRECTORY "${repo}/.github/workflows" "${repo}/nested" "${user_dir}" "${WORK}/home" "${WORK}/tmp")
find_program(GIT git REQUIRED)
set(git_command "${CMAKE_COMMAND}" -E env "GIT_CONFIG_NOSYSTEM=1" "GIT_CONFIG_GLOBAL=/dev/null"
  "${GIT}" -c commit.gpgsign=false -c user.name=config-test -c user.email=fixture-user@example.test)
execute_process(COMMAND ${git_command} init --quiet -b main "${repo}" RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "could not initialize configuration fixture")
endif()
execute_process(COMMAND ${git_command} -C "${repo}" commit --quiet --allow-empty -m fixture
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "could not commit configuration fixture")
endif()

set(command "${CMAKE_COMMAND}" -E env
  "HOME=${WORK}/home" "XDG_CONFIG_HOME=${WORK}/config" "TMPDIR=${WORK}/tmp"
  "GIT_CONFIG_NOSYSTEM=1" "GIT_CONFIG_GLOBAL=/dev/null" "${CLI}")
set(sandbox_args)
if(NOT HOST_IS_APPLE)
  list(APPEND sandbox_args --no-sandbox)
endif()

function(check_run name expected)
  set(workflow_args --workflow config.yml)
  list(FIND ARGN "--workflow" workflow_option_index)
  if(NOT workflow_option_index EQUAL -1)
    set(workflow_args)
  endif()
  execute_process(COMMAND ${command} run ${workflow_args}
    --run-dir "${WORK}/runs/${name}" ${sandbox_args} ${ARGN}
    WORKING_DIRECTORY "${repo}/nested"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
  if(NOT "${status}" STREQUAL "${expected}")
    message(FATAL_ERROR "${name}: expected ${expected}, got ${status}\n${output}\n${error}")
  endif()
  set(cli_output "${output}" PARENT_SCOPE)
  set(cli_error "${error}" PARENT_SCOPE)
endfunction()

function(check_clean expected)
  execute_process(COMMAND ${command} clean ${ARGN}
    WORKING_DIRECTORY "${repo}/nested"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
  if(NOT "${status}" STREQUAL "${expected}")
    message(FATAL_ERROR "clean: expected ${expected}, got ${status}\n${output}\n${error}")
  endif()
  set(cli_output "${output}" PARENT_SCOPE)
  set(cli_error "${error}" PARENT_SCOPE)
endfunction()

file(WRITE "${user_config}" [=[
host-labels: [config-host]
allow-read: user-readable
allow-write: user-written
env-pass: [CPA_VALUE=user, CPA_USER_ONLY=kept]
parallel: 2
cache-dir: user-cache
node: missing-user-node
secrets-file: missing-user-secrets.env
vars-file: missing-user-vars.env
vars: {USER_ONLY: kept, SHARED: user}
environments:
  Prod:
    secrets-file: missing-user-environment.env
    vars-file: environment-vars.env
    vars: {ENV_USER: kept, ENV_SHARED: user}
setup-steps:
  - id: user_setup
    name: User setup
    run: |
      echo SETUP_VALUE=user >> "$GITHUB_ENV"
      echo value=user >> "$GITHUB_OUTPUT"
  - run: test "$SETUP_VALUE" = user
]=])
file(WRITE "${user_dir}/user-readable" "user\n")
file(WRITE "${user_dir}/environment-vars.env" "ENV_FILE=kept\n")
file(WRITE "${WORK}/repository-readable" "repository\n")
file(WRITE "${repo}/repository-secrets.env" "TEST_TOKEN=repository-secret\n")
file(WRITE "${repo}/repository-vars.env" "FILE_VALUE=repository\n")
file(WRITE "${repo}/environment-secrets.env" "TEST_TOKEN=environment-secret\n")
file(WRITE "${repo_config}" [=[
host-labels: [repository-host]
allow-read: ../repository-readable
allow-write: ../repository-written
env-pass: [CPA_VALUE=repository, CPA_REPO_ONLY=added]
parallel: 1
cache-dir: repository-cache
node: ""
secrets-file: repository-secrets.env
vars-file: repository-vars.env
vars: {REPO_ONLY: added, SHARED: repository}
environments:
  prod:
    secrets-file: environment-secrets.env
    vars: {ENV_REPO: added, ENV_SHARED: repository}
setup-steps:
  - id: repository_setup
    name: Repository setup
    run: |
      test "$SETUP_VALUE" = user
      test "${{ steps.user_setup.outputs.value }}" = user
      echo SETUP_VALUE=repository >> "$GITHUB_ENV"
  - run: test "$SETUP_VALUE" = repository
]=])
set(workflow [=[
on: push
jobs:
  smoke:
    runs-on: [config-host, repository-host]
    environment: PROD
    steps:
      - run: |
          test "$CPA_VALUE" = "${EXPECTED_VALUE:-repository}"
          test "$CPA_USER_ONLY" = kept
          test "$CPA_REPO_ONLY" = added
          test "$SETUP_VALUE" = repository
          test "${{ vars.USER_ONLY }}" = kept
          test "${{ vars.REPO_ONLY }}" = added
          test "${{ vars.SHARED }}" = repository
          test "${{ vars.FILE_VALUE }}" = repository
          test "${{ vars.ENV_USER }}" = kept
          test "${{ vars.ENV_REPO }}" = added
          test "${{ vars.ENV_SHARED }}" = repository
          test "${{ vars.ENV_FILE }}" = kept
          test "${{ secrets.TEST_TOKEN }}" = environment-secret
          test "$(cat '@user_dir@/user-readable')" = user
          test "$(cat '@WORK@/repository-readable')" = repository
          echo user-written > '@user_dir@/user-written'
          echo repository-written > '@WORK@/repository-written'
          echo CONFIG_MERGED
]=])
string(CONFIGURE "${workflow}" workflow @ONLY)
file(WRITE "${repo}/.github/workflows/config.yml" "${workflow}")
file(WRITE "${repo}/.github/workflows/plain.yml" [=[
on: push
jobs:
  smoke:
    runs-on: config-host
    steps:
      - name: Plain step
        run: echo PLAIN_EXECUTED
]=])
file(READ "${user_config}" saved_user_config)
file(READ "${repo_config}" saved_repo_config)

check_run(merged 0 -C "${repo}" --pass-env CPA_VALUE=cli --pass-env EXPECTED_VALUE=cli)
if(NOT cli_output MATCHES "CONFIG_MERGED"
    OR NOT cli_error MATCHES "automatically loading repository configuration:.*\\.cpp-actions.yml")
  message(FATAL_ERROR "repository merge or automatic-load warning missing\n${cli_output}\n${cli_error}")
endif()
string(FIND "${cli_output}" "cpp-actions setup / User setup" user_position)
string(FIND "${cli_output}" "cpp-actions setup / Repository setup" repo_position)
if(user_position LESS 0 OR repo_position LESS user_position)
  message(FATAL_ERROR "setup steps were not executed in user-then-repository order")
endif()
file(READ "${WORK}/runs/merged/report.json" report)
string(JSON user_generated_id GET "${report}" workflows 0 jobs 0 steps 1 id)
string(JSON repo_generated_id GET "${report}" workflows 0 jobs 0 steps 3 id)
if(user_generated_id STREQUAL repo_generated_id)
  message(FATAL_ERROR "merged unnamed setup steps have duplicate report IDs")
endif()
file(READ "${user_dir}/user-written" user_written)
file(READ "${WORK}/repository-written" repo_written)
if(NOT user_written STREQUAL "user-written\n" OR NOT repo_written STREQUAL "repository-written\n")
  message(FATAL_ERROR "per-file relative write grants resolved incorrectly")
endif()
check_run(selected-subdirectory 0 -C "${repo}/nested")
check_run(current-subdirectory 0)

execute_process(COMMAND ${command} run --workflow config.yml --keep-workdir ${sandbox_args}
  WORKING_DIRECTORY "${repo}/nested"
  RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 30)
file(GLOB reports "${repo}/repository-cache/runs/*/report.json")
list(LENGTH reports report_count)
if(NOT status EQUAL 0 OR NOT report_count EQUAL 1)
  message(FATAL_ERROR "repository-relative cache was not selected\n${output}\n${error}")
endif()
list(GET reports 0 report_path)
get_filename_component(retained_run "${report_path}" DIRECTORY)
get_filename_component(run_name "${retained_run}" NAME)
check_clean(0 --dry-run "${run_name}")
if(NOT cli_output MATCHES "would remove" OR NOT cli_error MATCHES "automatically loading repository configuration")
  message(FATAL_ERROR "clean did not discover repository cache from a subdirectory")
endif()
if(NOT IS_DIRECTORY "${retained_run}/jobs")
  message(FATAL_ERROR "clean dry-run removed retained data")
endif()

file(REMOVE "${user_config}")
file(WRITE "${repo_config}" "host-labels: [config-host]\nvars: {REPOSITORY_ONLY: selected}\n")
check_run(repository-only 0 --workflow plain.yml)
if(NOT cli_output MATCHES "PLAIN_EXECUTED" OR NOT cli_error MATCHES "automatically loading")
  message(FATAL_ERROR "repository configuration without user configuration was not loaded")
endif()
file(WRITE "${user_config}" "${saved_user_config}")

file(WRITE "${repo_config}" [=[
secrets-file: ""
vars-file: ""
node: ""
cache-dir: ""
]=])
check_run(empty-overrides 0 --workflow plain.yml)
set(clear_paths "secrets-file: \"\"\nvars-file: \"\"\nnode: \"\"\ncache-dir: \"\"\n")
file(WRITE "${repo_config}" "${clear_paths}setup-steps:\n  - name: Repository failure\n    run: exit 9\n")
check_run(merged-setup-failure 1 --workflow plain.yml)
if(NOT cli_output MATCHES "cpp-actions setup failed" OR cli_output MATCHES "PLAIN_EXECUTED")
  message(FATAL_ERROR "merged setup failure did not block workflow steps")
endif()
file(WRITE "${user_config}" [=[
host-labels: [config-host]
setup-steps:
  - name: User failure
    run: exit 9
]=])
file(WRITE "${repo_config}" [=[
setup-steps:
  - name: Repository default condition
    run: echo REPOSITORY_DEFAULT_EXECUTED
  - name: Repository always condition
    if: always()
    run: echo REPOSITORY_ALWAYS_EXECUTED
]=])
check_run(user-setup-failure 1 --workflow plain.yml)
if(NOT cli_output MATCHES "cpp-actions setup failed"
    OR NOT cli_output MATCHES "REPOSITORY_ALWAYS_EXECUTED"
    OR cli_output MATCHES "REPOSITORY_DEFAULT_EXECUTED|PLAIN_EXECUTED")
  message(FATAL_ERROR "setup status was reset between user and repository groups\n${cli_output}")
endif()
file(WRITE "${user_config}" "${saved_user_config}")
file(WRITE "${repo_config}" "${clear_paths}setup-steps:\n  - id: user_setup\n    run: echo duplicate\n")
check_run(duplicate-setup-id 2 --workflow plain.yml)
if(EXISTS "${WORK}/runs/duplicate-setup-id" OR NOT cli_error MATCHES "Duplicate setup step id")
  message(FATAL_ERROR "duplicate explicit setup ID was not rejected before execution")
endif()
set(source_diagnostic [=[
setup-steps:
  - run: echo "${{ fromJSON('invalid-json') }}"
]=])
file(WRITE "${repo_config}" "${clear_paths}${source_diagnostic}")
check_run(setup-source-diagnostic 1 --workflow plain.yml)
if(NOT cli_output MATCHES "\\.cpp-actions.yml")
  message(FATAL_ERROR "merged setup diagnostic lost its repository source file\n${cli_output}")
endif()

file(WRITE "${user_config}" "invalid-user-key: true\n")
file(WRITE "${repo_config}" "invalid-repository-key: true\n")
file(WRITE "${WORK}/explicit.yml" "host-labels: [config-host]\n")
check_run(explicit 0 --workflow plain.yml --config "${WORK}/explicit.yml")
if(cli_error MATCHES "automatically loading" OR NOT cli_output MATCHES "PLAIN_EXECUTED")
  message(FATAL_ERROR "explicit --config did not bypass both automatic configurations")
endif()
check_run(explicit-missing 2 --config "${WORK}/missing.yml")
if(NOT cli_error MATCHES "missing.yml" OR cli_error MATCHES "invalid-user-key")
  message(FATAL_ERROR "explicit missing configuration fell back to automatic settings")
endif()
check_run(dry-run 0 --dry-run)
if(cli_error MATCHES "automatically loading" OR EXISTS "${WORK}/runs/dry-run")
  message(FATAL_ERROR "dry-run loaded configuration")
endif()
check_run(no-trigger 0 --event workflow_dispatch)
if(cli_error MATCHES "automatically loading" OR EXISTS "${WORK}/runs/no-trigger"
    OR EXISTS "${WORK}/runs/dry-run")
  message(FATAL_ERROR "preview or unmatched event loaded configuration")
endif()
check_clean(0 --all --dry-run --cache-dir "${repo}/repository-cache")
if(NOT cli_output MATCHES "would remove" OR cli_error MATCHES "automatically loading")
  message(FATAL_ERROR "clean --cache-dir loaded automatic configuration")
endif()
check_clean(0 --dry-run "${retained_run}")
if(NOT cli_output MATCHES "would remove" OR cli_error MATCHES "automatically loading")
  message(FATAL_ERROR "clean of an explicit path loaded configuration")
endif()
file(WRITE "${WORK}/explicit-clean.yml" "cache-dir: repo/repository-cache\n")
check_clean(0 --all --dry-run --config "${WORK}/explicit-clean.yml")
if(NOT cli_output MATCHES "would remove" OR cli_error MATCHES "automatically loading")
  message(FATAL_ERROR "clean --config did not bypass automatic configuration")
endif()

file(WRITE "${user_config}" "${saved_user_config}")
file(WRITE "${repo_config}" "invalid-repository-key: true\n")
check_run(invalid-repository 2)
if(EXISTS "${WORK}/runs/invalid-repository" OR NOT cli_error MATCHES "\\.cpp-actions.yml")
  message(FATAL_ERROR "invalid repository configuration was silently ignored")
endif()
file(REMOVE "${repo_config}")
file(MAKE_DIRECTORY "${repo_config}")
check_run(directory-repository 2)
if(NOT cli_error MATCHES "not a regular file")
  message(FATAL_ERROR "directory repository configuration was accepted")
endif()
file(REMOVE_RECURSE "${repo_config}")
file(CREATE_LINK "${repo}/missing-config.yml" "${repo_config}" SYMBOLIC RESULT link_status)
if(NOT link_status STREQUAL "0")
  message(FATAL_ERROR "could not create dangling configuration symlink: ${link_status}")
endif()
check_run(dangling-repository 2)
if(NOT cli_error MATCHES "not a regular file")
  message(FATAL_ERROR "dangling repository configuration was silently ignored")
endif()

cleanup_workspaces("${WORK}")
file(REMOVE_RECURSE "${WORK}")
