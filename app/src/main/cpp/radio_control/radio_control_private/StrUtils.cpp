#include <cstdarg>
#include <cstring>
#include <sstream>
#include "StrUtils.h"

std::string asprintf(const char* msg, ...)
{
    std::string str;

    size_t size = strlen(msg) * 2 + 50;
    while (1) {
        str.resize(size);

        va_list ap;
        va_start(ap, msg);
        int n = vsnprintf((char*)str.data(), size, msg, ap);
        va_end(ap);

        if (n >= 0 && n < int(size)) {
            str.resize(n);
            return str;
        }

        size *= 2;
    }

    return str;
}

std::vector<std::string> split(const std::string& str, char seperator)
{
    std::vector<std::string> strings;

    std::istringstream iss(str);
    std::string s;
    while (getline(iss, s, seperator))
        strings.push_back(s);

    return strings;
}
