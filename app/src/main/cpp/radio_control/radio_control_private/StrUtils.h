#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

inline bool startsWith(const std::string& str, const char* match)
{
    return (str.rfind(match, 0) == 0);
}

inline std::string& rightTrim(std::string& str)
{
    str.erase(str.find_last_not_of(" \t\n\r\f\v") + 1);
    return str;
}

inline std::string& leftTrim(std::string& str)
{
    str.erase(0, str.find_first_not_of(" \t\n\r\f\v"));
    return str;
}

inline std::string& trim(std::string& str)
{
    return leftTrim(rightTrim(str));
}

inline std::string& lower(std::string& str)
{
    std::transform(str.begin(), str.end(), str.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return str;
}

inline std::string& upper(std::string& str)
{
    std::transform(str.begin(), str.end(), str.begin(),
        [](unsigned char c) { return std::toupper(c); });
    return str;
}

inline std::string quote(const std::string& str)
{
    return "\"" + str + "\"";
}

std::string asprintf(const char* msg, ...);

std::vector<std::string> split(const std::string& str, char seperator);
