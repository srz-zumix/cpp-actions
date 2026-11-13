#import <Foundation/Foundation.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
  @autoreleasepool {
    const char* home = getenv("HOME");
    const char* fixed_home = getenv("CFFIXED_USER_HOME");
    const char* foundation_home = NSHomeDirectory().fileSystemRepresentation;
    char* resolved_home = home == NULL ? NULL : realpath(home, NULL);
    char* resolved_fixed_home = fixed_home == NULL ? NULL : realpath(fixed_home, NULL);
    char* resolved_foundation_home = foundation_home == NULL ? NULL : realpath(foundation_home, NULL);
    char* resolved_expected = argc == 2 ? realpath(argv[1], NULL) : NULL;
    if (argc != 2 || home == NULL || fixed_home == NULL || foundation_home == NULL ||
        resolved_home == NULL || resolved_fixed_home == NULL || resolved_foundation_home == NULL ||
        resolved_expected == NULL || strcmp(resolved_home, resolved_expected) != 0 ||
        strcmp(resolved_fixed_home, resolved_expected) != 0 || strcmp(resolved_foundation_home, resolved_expected) != 0) {
      fprintf(stderr, "HOME=%s CFFIXED_USER_HOME=%s NSHomeDirectory=%s expected=%s\n",
              home == NULL ? "(unset)" : home, fixed_home == NULL ? "(unset)" : fixed_home,
              foundation_home == NULL ? "(unset)" : foundation_home, argc == 2 ? argv[1] : "(missing)");
      free(resolved_home);
      free(resolved_fixed_home);
      free(resolved_foundation_home);
      free(resolved_expected);
      return 1;
    }
    free(resolved_home);
    free(resolved_fixed_home);
    free(resolved_foundation_home);
    free(resolved_expected);
    return 0;
  }
}
