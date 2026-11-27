// The timeline palette requests theme colors in a global initializer. On MSVC
// the map in CategoryColors.cpp may otherwise still be unconstructed.
#include "core/CategoryColors.h"
#include <cstdio>

#ifdef _MSC_VER
#pragma init_seg(lib) // before normal user-segment globals, independently of link order
#endif
struct EarlyThemeProbe
{
   bool light;
   EarlyThemeProbe() : light(CategoryColors::IsThemeLight()) {}
};
static EarlyThemeProbe early;

int main()
{
   if (early.light != CategoryColors::IsThemeLight())
      return 1;
   std::puts("Early theme access OK");
   return 0;
}
