#include "core/chat/LocalProtocol.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace chat
{
namespace
{
   std::string UpperAscii(std::string s)
   {
      for (char& c : s)
         c = (char)std::toupper((unsigned char)c);
      return s;
   }

   std::string Trim(const std::string& s)
   {
      size_t a = 0, b = s.size();
      while (a < b && std::isspace((unsigned char)s[a]))
         ++a;
      while (b > a && std::isspace((unsigned char)s[b - 1]))
         --b;
      return s.substr(a, b - a);
   }

   // A tag line starts with a name (letters, digits, underscore) and a colon; the name is
   // uppercased. Returns the tag and the text after the colon with leading spaces trimmed.
   // Plain prose is never a tag: "Note: something" matches the shape but is not a known
   // tag, so only the three known tags trigger protocol handling (see ParseProtocol).
   bool SplitTagLine(const std::string& line, std::string& tag, std::string& rest)
   {
      size_t i = 0;
      if (i >= line.size() || !std::isalpha((unsigned char)line[i]))
         return false;
      size_t nameEnd = i;
      while (nameEnd < line.size() && (std::isalnum((unsigned char)line[nameEnd]) || line[nameEnd] == '_'))
         ++nameEnd;
      size_t j = nameEnd;
      while (j < line.size() && std::isspace((unsigned char)line[j]))
         ++j;
      if (j >= line.size() || line[j] != ':')
         return false;
      ++j;
      while (j < line.size() && std::isspace((unsigned char)line[j]))
         ++j;
      tag = UpperAscii(line.substr(0, nameEnd));
      rest = line.substr(j);
      return true;
   }

   // Second repair candidate: drop every backslash that directly precedes a quote, whatever the
   // state. Handles a document where the model escaped every structural quote at the token level
   // (the state-machine repair cannot tell an over-escaped closing quote from an escaped content
   // quote, so both candidates are tried and the first that parses wins).
   std::string NormalizeActionTextDropQuotes(const std::string& raw)
   {
      std::string out;
      out.reserve(raw.size());
      for (size_t i = 0; i < raw.size(); ++i)
      {
         if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '"')
            ++i; // skip the backslash, keep the quote
         out += raw[i];
      }
      return out;
   }

   void RemoveBackslashes(std::string& s)
   {
      std::string out;
      out.reserve(s.size());
      for (char c : s)
         if (c != '\\')
            out += c;
      s = std::move(out);
   }

   void RemoveBackslashBeforeQuotes(std::string& s)
   {
      std::string out;
      out.reserve(s.size());
      for (size_t i = 0; i < s.size(); ++i)
      {
         if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '"')
            ++i; // drop the backslash, keep the quote
         out += s[i];
      }
      s = std::move(out);
   }

   // Strip the escape artifacts a weak model leaves in identifier strings: method names and
   // parameter keys never carry a backslash, and a label's backslash before a quote is a
   // double-escape. Values (paths, code) are left alone.
   nlohmann::json SanitizeEscapes(const nlohmann::json& j, const std::string& parentKey)
   {
      if (j.is_object())
      {
         nlohmann::json out = nlohmann::json::object();
         for (auto it = j.begin(); it != j.end(); ++it)
         {
            std::string key = it.key();
            RemoveBackslashes(key);
            out[key] = SanitizeEscapes(it.value(), key);
         }
         return out;
      }
      if (j.is_array())
      {
         nlohmann::json out = nlohmann::json::array();
         for (const nlohmann::json& v : j)
            out.push_back(SanitizeEscapes(v, parentKey));
         return out;
      }
      if (j.is_string())
      {
         std::string s = j.get<std::string>();
         if (parentKey == "method")
            RemoveBackslashes(s);
         else if (parentKey == "label")
            RemoveBackslashBeforeQuotes(s);
         return s;
      }
      return j;
   }
}

ProtocolReply ParseProtocol(const std::string& reply)
{
   ProtocolReply out;
   std::vector<ProtocolBlock> actions;  // every action block in the reply, in order
   std::vector<std::string> textParts;
   bool inAction = false;

   // Split into lines, dropping trailing '\r' (SSE and CRLF both reach here).
   size_t pos = 0;
   while (pos <= reply.size())
   {
      size_t nl = reply.find('\n', pos);
      std::string line = reply.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
      if (!line.empty() && line.back() == '\r')
         line.pop_back();
      if (nl == std::string::npos)
         pos = reply.size() + 1;
      else
         pos = nl + 1;

      std::string tag, rest;
      if (SplitTagLine(line, tag, rest))
      {
         if (inAction)
            inAction = false;
         if (tag == "TEXT")
         {
            textParts.push_back(rest);
         }
         else if (tag == "PROPOSE")
         {
            actions.push_back(ProtocolBlock{ ProtocolKind::Propose, "PROPOSE", rest, false, {}, {} });
            inAction = true;
         }
         else if (tag == "CHECK")
         {
            actions.push_back(ProtocolBlock{ ProtocolKind::Check, "CHECK", rest, false, {}, {} });
            inAction = true;
         }
         else if (tag == "TOOL")
         {
            actions.push_back(ProtocolBlock{ ProtocolKind::Tool, "TOOL", rest, false, {}, {} });
            inAction = true;
         }
         else
         {
            // An unknown tag-like line: keep its content as text, but note the tag so the
            // harness can show the user what the model tried to do.
            out.unknownTags.push_back(tag);
            textParts.push_back(line);
         }
      }
      else if (inAction)
      {
         actions.back().raw += "\n";
         actions.back().raw += line;
      }
      else
      {
         textParts.push_back(line);
      }

      if (nl == std::string::npos)
         break;
   }

   // Tolerant selection: a reply may carry several blocks (a weak model often sends TEXT +
   // TOOL + PROPOSE at once). One action runs - the highest-priority kind present, and among
   // equals the LAST one (the model's most recent intent wins). Priority: PROPOSE > CHECK > TOOL.
   if (!actions.empty())
   {
      int bestPri = -1;
      size_t best = 0;
      for (size_t i = 0; i < actions.size(); ++i)
      {
         const int pri = actions[i].kind == ProtocolKind::Propose ? 3 :
                         actions[i].kind == ProtocolKind::Check ? 2 : 1;
         if (pri >= bestPri)
            bestPri = pri, best = i;
      }
      out.action = actions[best];
      out.hasAction = true;
      out.multipleActions = actions.size() > 1;
      out.action.validJson = ParseActionJson(out.action.raw, out.action.json, out.action.err);
   }

   for (const std::string& part : textParts)
      if (!part.empty())
         out.text = out.text.empty() ? part : out.text + "\n" + part;

   return out;
}

// Propose reference gate. Every $N.field string in a call's params is resolved by the executor
// against the results of the calls before it, so a reference must point at an EARLIER call in
// the same propose - never at the current call, a later one, or nothing at all. Weak models
// emit "$0...." with no prior call (dry-run 2); rejecting that here, before anything runs, gives
// the model an actionable error instead of a confusing downstream failure. Empty string = valid.
std::string ValidateProposeRefs(const nlohmann::json& propose)
{
   if (!propose.is_object())
      return {};
   const nlohmann::json& calls = propose.value("calls", nlohmann::json::array());
   if (!calls.is_array())
      return {};
   for (size_t i = 0; i < calls.size(); ++i)
   {
      const nlohmann::json& call = calls[i];
      if (!call.is_object())
         continue;
      const nlohmann::json params = call.value("params", nlohmann::json::object());
      if (!params.is_object())
         continue;
      for (const auto& [name, value] : params.items())
      {
         if (!value.is_string())
            continue;
         const std::string& text = value.get<std::string>();
         // The executor resolves "$N.field" (digits, a dot, a field name); anything else is
         // passed through untouched.
         if (text.size() < 3 || text[0] != '$')
            continue;
         const size_t dot = text.find('.');
         if (dot == std::string::npos || dot == 1)
            continue;
         const std::string num = text.substr(1, dot - 1);
         if (num.find_first_not_of("0123456789") != std::string::npos)
            continue;
         const long at = std::strtol(num.c_str(), nullptr, 10);
         if (at < 0 || (size_t)at >= i)
            return "call " + std::to_string(i) + " references $" + num + ", but every $N must point at an earlier call in the same propose";
      }
   }
   return {};
}

// The TOOL: block is read-only by design: inspect the patch, never change it. Edit methods
// (create_node, set_param, connect, ...) exist only inside a propose's calls.
bool IsReadOnlyToolName(const std::string& name)
{
   static const char* const kReadOnly[] = {
      "get_graph", "get_params", "list_node_types", "describe",
      "code_errors", "status", "attention", "selection",
   };
   for (const char* t : kReadOnly)
      if (name == t)
         return true;
   return false;
}

std::string NormalizeActionText(const std::string& raw)
{
   // Walk the text with a JSON-string state machine. Outside a string a backslash directly
   // before a quote is a structural quote the model escaped at the token level: drop the
   // backslash. Inside a string:
   //  - before a quote: a valid escape always ends with one backslash before the quote, so an
   //    odd run is a correct escape and an even run is broken - keep 1 for odd, n-1 for even
   //    (a double-escaped quote is three backslashes + quote, one too many);
   //  - before a character that is not a valid JSON escape target (" \ / b f n r t u): the
   //    backslash is the model's stray escape (e.g. set\_param) and is dropped whole;
   //  - otherwise keep at most two backslashes (a literal backslash, and a newline escape).
   std::string out;
   out.reserve(raw.size());
   bool inString = false;
   size_t i = 0;
   while (i < raw.size())
   {
      const char c = raw[i];
      if (!inString)
      {
         if (c == '\\' && i + 1 < raw.size() && raw[i + 1] == '"')
         {
            ++i; // skip the backslash; the quote opens the string
            out += '"';
            inString = true;
            continue;
         }
         out += c;
         if (c == '"')
            inString = true;
         ++i;
         continue;
      }
      if (c == '\\')
      {
         size_t run = i;
         while (run < raw.size() && raw[run] == '\\')
            ++run;
         const size_t n = run - i;
         size_t keep = std::min(n, size_t(2));
         if (run < raw.size())
         {
            const char next = raw[run];
            if (next == '"')
               keep = (n % 2 == 1) ? 1 : (n - 1);
            else if (std::string("\"\\/bfnrtu").find(next) == std::string::npos)
               keep = 0; // a backslash before a non-escape character: the model's stray escape
         }
         out.append(keep, '\\');
         i = run;
         continue;
      }
      if (c == '"')
         inString = false;
      out += c;
      ++i;
   }
   return out;
}

bool ParseActionJson(const std::string& raw, nlohmann::json& outObject, std::string& err)
{
   outObject = nlohmann::json();
   err.clear();
   const std::string text = Trim(raw);
   if (text.empty())
   {
      err = "the action block is empty";
      return false;
   }
   auto j = nlohmann::json::parse(text, nullptr, false);
   if (j.is_discarded() || !j.is_object())
   {
      // The model over-escaped its JSON (double-escaped quotes, backslashes before structural
      // quotes). Repair the raw text and try again before reporting a failure: first the
      // state-machine repair, then the blunt drop of every backslash before a quote.
      const std::string fixed = NormalizeActionText(text);
      if (fixed != text)
         j = nlohmann::json::parse(fixed, nullptr, false);
      if (j.is_discarded() || !j.is_object())
      {
         const std::string fixed2 = NormalizeActionTextDropQuotes(text);
         if (fixed2 != text && fixed2 != fixed)
            j = nlohmann::json::parse(fixed2, nullptr, false);
      }
   }
   if (j.is_discarded())
   {
      err = "the action block is not valid JSON";
      return false;
   }
   if (!j.is_object())
   {
      err = "the action block must be a JSON object";
      return false;
   }
   outObject = SanitizeEscapes(std::move(j), std::string());
   return true;
}
}
