#pragma once

class Trace
{
public:
    void trace(const char* msg, ...);
    void silent(const char*, ...) {};
};

#if defined(_DEBUG) || defined(DEBUG)
#define TRACE Trace().trace
#else
#define TRACE while (false) Trace().silent
#endif
