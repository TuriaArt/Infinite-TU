// The timeline palette requests theme colors in a global initializer. On MSVC
// the map in CategoryColors.cpp may otherwise still be unconstructed.
#include "core/CategoryColors.h"
#include <cstdio>

#ifdef _MSC_VER
#pragma init_seg(lib) // before normal user-segment globals, independently of link order
#endif
static const bool earlyIsLight = CategoryColors::IsThemeLight();
#ifdef _MSC_VER
#pragma init_seg(user)
#endif

int main()
{
   if (earlyIsLight != CategoryColors::IsThemeLight())
      return 1;
   std::puts("Early theme access OK");
   return 0;
}
