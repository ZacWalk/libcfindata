#include "fg.h"

int fg_cli_run(int argc, char **argv, const fg_transport *transport);

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int wmain(int argc, wchar_t **wide_argv)
{
    char **argv = NULL;
    int i, result = FG_NOMEM;
    if (argc < 1 || (size_t)argc > SIZE_MAX / sizeof(*argv) - 1) {
        result = FG_INVALID;
        goto failed;
    }
    argv = (char **)calloc((size_t)argc + 1, sizeof(*argv));
    if (!argv) goto failed;
    for (i = 0; i < argc; ++i) {
        int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                         wide_argv[i], -1, NULL, 0, NULL, NULL);
        if (!length) { result = FG_INVALID; goto failed; }
        argv[i] = (char *)malloc((size_t)length);
        if (!argv[i]) goto failed;
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                wide_argv[i], -1, argv[i], length, NULL, NULL)) {
            result = FG_INVALID;
            goto failed;
        }
    }
    result = fg_cli_run(argc, argv, NULL);
    goto done;
failed:
    if (fprintf(stderr, "libcfindata: %s; command-line conversion failed\n",
                fg_status_name((fg_status)result)) < 0 ||
        fflush(stderr) == EOF || ferror(stderr)) result = FG_IO;
done:
    if (argv) {
        for (i = 0; i < argc; ++i) free(argv[i]);
        free(argv);
    }
    return result;
}
#else
int main(int argc, char **argv)
{
    return fg_cli_run(argc, argv, NULL);
}
#endif
