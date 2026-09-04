#ifndef SHIM_GECKO_H
#define SHIM_GECKO_H
#include <stdio.h>
#define gprintf(...) fprintf(stderr, __VA_ARGS__)
#endif
