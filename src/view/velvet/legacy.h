// Folders made under the app's old name are taken over under the new one, once.
#pragma once
#include <cstdio>
#include <string>
#include <sys/stat.h>

inline void adopt_old_dir(const std::string& old_path, const std::string& new_path) {
    struct stat a, b;
    if (stat(new_path.c_str(), &b) != 0 && stat(old_path.c_str(), &a) == 0) std::rename(old_path.c_str(), new_path.c_str());
}
