#pragma once
#include <cstdint>

namespace app
{
// Session-only selection. A pinned UID is meaningful only in the patch where
// it was picked; no node addresses are retained across deletion or undo.
class InspectorTarget
{
public:
   uint64_t Update(uint64_t generation, uint64_t selectedUid, bool pinnedExists)
   {
      if (generation != generation_)
      {
         generation_ = generation;
         pinned_ = target_ = 0;
      }
      if (pinned_ && !pinnedExists)
         pinned_ = target_ = 0;
      if (!pinned_)
         target_ = selectedUid;
      return pinned_ ? pinned_ : target_;
   }
   void TogglePin()
   {
      if (pinned_)
         pinned_ = 0;
      else if (target_)
         pinned_ = target_;
   }
   uint64_t Pinned() const { return pinned_; }
private:
   uint64_t generation_ = 0;
   uint64_t target_ = 0;
   uint64_t pinned_ = 0;
};
}
