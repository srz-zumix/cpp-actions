#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  const std::string_view expected = argv[2];
  if (expected != "allow" && expected != "deny") return 2;
  mach_port_t service = MACH_PORT_NULL;
  const auto result = bootstrap_look_up(bootstrap_port, argv[1], &service);
  if (result == KERN_SUCCESS) mach_port_deallocate(mach_task_self(), service);
  std::cout << argv[1] << ": " << result << '\n';
  if (expected == "allow") return result == KERN_SUCCESS ? 0 : 1;
  return result == BOOTSTRAP_NOT_PRIVILEGED || result == BOOTSTRAP_UNKNOWN_SERVICE ? 0 : 1;
}
