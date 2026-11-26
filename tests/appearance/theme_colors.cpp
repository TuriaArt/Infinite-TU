// Standalone regression: run with an isolated HOME and XDG_CONFIG_HOME.
#include "core/CategoryColors.h"
#include <cassert>
#include <cmath>
#include <limits>

using namespace CategoryColors;
static bool Same(Color a, Color b)
{
   return std::fabs(a.r - b.r) < 0.0001f && std::fabs(a.g - b.g) < 0.0001f && std::fabs(a.b - b.b) < 0.0001f;
}
int main()
{
   LoadPreference();
   assert(PresetNames().size() > 1);
   SetPreset(0);
   const Color original = CurrentUiTheme().accent;
   const Color first{0.11f, 0.22f, 0.33f};
   const Color second{0.44f, 0.55f, 0.66f};
   SetUiColor(UiColorRole::Accent, first);
   SetPreset(1);
   const Color originalSecond = CurrentUiTheme().accent;
   SetUiColor(UiColorRole::Accent, second);
   assert(Same(UiThemeForPreset(0).accent, first));
   LoadPreference();
   assert(CurrentPreset() == 1 && Same(CurrentUiTheme().accent, second));
   SetPreset(0);
   assert(Same(CurrentUiTheme().accent, first));
   SetUiColor(UiColorRole::Accent, {std::numeric_limits<float>::quiet_NaN(), 0, 0});
   assert(Same(CurrentUiTheme().accent, first));
   ResetUiColors();
   assert(Same(CurrentUiTheme().accent, original));
   assert(Same(UiThemeForPreset(1).accent, second));
   SetPreset(1);
   ResetAllAppearanceBoth();
   assert(Same(CurrentUiTheme().accent, originalSecond));
   LoadPreference();
   assert(Same(CurrentUiTheme().accent, originalSecond));
}
