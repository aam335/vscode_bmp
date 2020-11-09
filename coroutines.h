#pragma once

// coRoutines
#define _APPEND(x, y) x##y

#define crBegin          \
    switch (crStaticVar) \
    {                    \
    case 0:

#define crReturn(x)             \
    do                          \
    {                           \
        crStaticVar = __LINE__; \
        return x;               \
    case __LINE__:;             \
    } while (0)
#define crFinish }

#define crPoll(NAME)                                                   \
    do                                                                 \
    {                                                                  \
        if (system_millis - _APPEND(NAME, _last) > _APPEND(NAME, _next)) \
        {                                                              \
            _APPEND(NAME, _next) = NAME();                              \
            _APPEND(NAME, _last) = system_millis;                       \
        }                                                              \
    } while (0)
