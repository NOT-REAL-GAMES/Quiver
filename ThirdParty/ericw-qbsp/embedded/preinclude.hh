/* Preinclude the standard and system headers at global scope before placing
 * each legacy tool translation unit in its own C++ namespace. */
#include <algorithm>
#include <array>
#include <assert.h>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits.h>
#include <limits>
#include <list>
#include <map>
#include <math.h>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <pthread.h>
#include <unistd.h>
#endif
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#include <xmmintrin.h>
#endif
