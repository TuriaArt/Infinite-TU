// Node-search helpers: folding, match test and incremental ranking for the
// node picker and the Modules panel. Kept together and self-contained (no GUI,
// no engine) so the ranking logic can be exercised headless - the UI surfaces
// (StagePopupsB.cpp, StageSidePanels.cpp) call these and own the display.
#include "app/AppShared.h"

#include <string>

namespace app
{
   // Lowercases (incl. Cyrillic) and strips accents so the dropdown search finds
   // "cafe" from "café". Covers the Latin-1 supplement and Latin Extended-A
   // (U+00C0..U+017F), which is every accented letter that shows up in node,
   // device and preset names; anything else passes through unchanged.
   std::string FoldForSearch(const std::string& in)
   {
      static const char* const kFold[0x180 - 0xC0] = {
         "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
         "d", "n", "o", "o", "o", "o", "o", "", "o", "u", "u", "u", "u", "y", "th", "ss",
         "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
         "d", "n", "o", "o", "o", "o", "o", "", "o", "u", "u", "u", "u", "y", "th", "y",
         "a", "a", "a", "a", "a", "a", "c", "c", "c", "c", "c", "c", "c", "c", "d", "d",
         "d", "d", "e", "e", "e", "e", "e", "e", "e", "e", "e", "e", "g", "g", "g", "g",
         "g", "g", "g", "g", "h", "h", "h", "h", "i", "i", "i", "i", "i", "i", "i", "i",
         "i", "i", "ij", "ij", "j", "j", "k", "k", "k", "l", "l", "l", "l", "l", "l", "l",
         "l", "l", "l", "n", "n", "n", "n", "n", "n", "n", "n", "o", "o", "o", "o",
         "o", "o", "oe", "oe", "r", "r", "r", "r", "r", "r", "s", "s", "s", "s", "s", "s",
         "s", "s", "t", "t", "t", "t", "t", "t", "u", "u", "u", "u", "u", "u", "u", "u",
         "u", "u", "u", "u", "w", "w", "y", "y", "y", "z", "z", "z", "z", "z", "z", "s"
      };
      std::string out;
      out.reserve(in.size());
      for (size_t i = 0; i < in.size(); i++)
      {
         const unsigned char c = (unsigned char)in[i];
         if (c < 0x80)
            out += (char)std::tolower(c);
         else if ((c == 0xC3 || c == 0xC4 || c == 0xC5) && i + 1 < in.size())
         {
            const unsigned cp = ((c & 0x1Fu) << 6) | ((unsigned char)in[i + 1] & 0x3Fu);
            out += kFold[cp - 0xC0];
            i++;
         }
         else if ((c == 0xD0 || c == 0xD1) && i + 1 < in.size())
         {
            // Cyrillic: U+0410..042F -> 0430..044F, U+0400..040F -> 0450..045F, yo -> ye.
            const unsigned cp = ((c & 0x1Fu) << 6) | ((unsigned char)in[i + 1] & 0x3Fu);
            unsigned lo = cp;
            if (cp >= 0x410 && cp <= 0x42F)
               lo = cp + 0x20;
            else if (cp >= 0x400 && cp <= 0x40F)
               lo = cp + 0x50;
            if (lo == 0x451)
               lo = 0x435;
            out += (char)(0xC0 | (lo >> 6));
            out += (char)(0x80 | (lo & 0x3F));
            i++;
         }
         else
            out += (char)c;
      }
      return out;
   }

   bool NodeSearchMatches(std::string hay, std::string q)
   {
      auto norm = [](std::string& t) {
         t = FoldForSearch(t);
         for (char& c : t)
            if (c == '-' || c == '_')
               c = ' ';
      };
      auto strip = [](const std::string& t) {
         std::string o;
         for (char c : t)
            if (c != ' ')
               o += c;
         return o;
      };
      norm(hay);
      norm(q);
      if (hay.find(q) != std::string::npos)
         return true;
      const std::string qs = strip(q);
      return !qs.empty() && strip(hay).find(qs) != std::string::npos;
   }

   int NodeSearchRank(const std::string& name, const std::string& category, std::string q)
   {
      // Same fold as NodeSearchMatches: FoldForSearch lowercases/strips accents,
      // then -/_ become spaces. The query arrives already lowered by the caller,
      // but folding again keeps it idempotent and accent-safe.
      auto fold = [](std::string& t) {
         t = FoldForSearch(t);
         for (char& c : t)
            if (c == '-' || c == '_')
               c = ' ';
      };
      auto strip = [](const std::string& t) {
         std::string o;
         for (char c : t)
            if (c != ' ')
               o += c;
         return o;
      };
      if (q.empty())
         return 0; // no query: browse order, everything is a rank-0 match

      fold(q);
      if (q.empty())
         return 0;

      // 0: the folded display name is a prefix match (the strongest signal - the
      // type the user is spelling out, e.g. "samp" -> "sample player").
      std::string disp = DisplayName(name);
      fold(disp);
      if (disp.find(q) == 0)
         return 0;

      // 1: the query is a substring anywhere in the haystack (display name,
      // category, or help prose for non-English UIs).
      std::string hay = NodeSearchHaystack(name, category);
      fold(hay);
      if (hay.find(q) != std::string::npos)
         return 1;
      const std::string qs = strip(q);
      return !qs.empty() && strip(hay).find(qs) != std::string::npos ? 1 : -1;
   }
}
