// Folder listing for the Scratch Everywhere! PAPP.
//
// newlib's <dirent.h> includes <sys/dirent.h>, which upstream newlib leaves
// unsupported (ESP-IDF supplies its own in its vfs component). This one is
// found first on the include path; opendir/readdir/closedir are implemented
// with the loader's file_list_dir in papp_syscalls.c.
#pragma once

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DT_UNKNOWN 0
#define DT_REG 1
#define DT_DIR 2

struct dirent {
    ino_t d_ino;
    unsigned char d_type;
    char d_name[256];
};

typedef struct papp_dir DIR;

DIR *opendir(const char *path);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);

#ifdef __cplusplus
}
#endif
