#include <cstdio>
#include <cstdarg>
#include "Trace.h"

/////////////////////////////////////////////////////////////////////////////

void Trace::trace(const char* msg, ...)
{
    va_list args;
    va_start(args, msg);
    vfprintf(stderr, msg, args);
    fprintf(stderr, "\n");
    va_end(args);
}

/////////////////////////////////////////////////////////////////////////////
