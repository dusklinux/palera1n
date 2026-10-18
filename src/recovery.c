#ifdef __linux__
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern unsigned char palera1n_recovery_script[];
extern unsigned int palera1n_recovery_script_len;

int recovery_main(int argc, char **argv)
{
    char executable[4096];
    ssize_t size = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (size < 0 || (size_t)size >= sizeof(executable) - 1) {
        perror("Cannot locate this palera1n executable");
        return 1;
    }
    executable[size] = '\0';
    if (setenv("PALERA1N_EXECUTABLE", executable, 1)) return 1;
    char *script = malloc((size_t)palera1n_recovery_script_len + 1);
    char **args = calloc((size_t)argc + 3, sizeof(*args));
    if (!script || !args) { free(script); free(args); return 1; }
    memcpy(script, palera1n_recovery_script, palera1n_recovery_script_len);
    script[palera1n_recovery_script_len] = '\0';
    args[0] = "python3";
    args[1] = "-c";
    args[2] = script;
    for (int i = 1; i < argc; ++i) args[i + 2] = argv[i];
    execvp(args[0], args);
    fprintf(stderr, "Cannot run recovery with python3: %s\n", strerror(errno));
    free(script);
    free(args);
    return 1;
}
#else
typedef int recovery_linux_only;
#endif
