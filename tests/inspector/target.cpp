#include "app/frame/InspectorTarget.h"
#include <cassert>

int main()
{
   app::InspectorTarget target;
   assert(target.Update(1, 10, false) == 10);
   target.TogglePin();
   assert(target.Pinned() == 10);
   assert(target.Update(1, 20, true) == 10);
   assert(target.Update(1, 0, true) == 10);
   target.TogglePin();
   assert(target.Update(1, 20, false) == 20);
   target.TogglePin();
   assert(target.Update(1, 30, false) == 30); // pinned node deleted
   target.TogglePin();
   assert(target.Update(2, 40, true) == 40); // new patch, even if UID was reused
   assert(target.Pinned() == 0);
   assert(target.Update(2, 0, false) == 0); // no / multiple nodes selected
   target.TogglePin();
   assert(target.Pinned() == 0);
}
