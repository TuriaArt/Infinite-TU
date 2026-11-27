#include "core/chat/HarnessConfig.h"

#include "core/Extensions.h"
#include "core/chat/OpenAIAgent.h"
#include "platform/AppPaths.h"

#include <cstdio>
#include <fstream>
#include <iterator>

namespace chat
{
namespace
{
   constexpr size_t kMaxPromptBytes = 8192;
   constexpr int kMinSteps = 1, kMaxSteps = 32;
   constexpr int kMaxParseErrors = 5;

   const char* const kDefaultProtocolPrompt =
      "You are the built-in assistant inside Infinite, a modular audiovisual patching app.\n"
      "The user asks you to change the patch; you act with the tools below, one action per reply.\n"
      "Reply in the same language as the user.\n"
      "\n"
      "Protocol:\n"
      "- Write your answer for the user under a TEXT: line (plain text).\n"
      "- To inspect the patch, write one TOOL: line holding a JSON object:\n"
      "  {\"name\": \"get_graph\", \"params\": {}}. The read-only tools are get_graph, get_params,\n"
      "  list_node_types, describe, code_errors, status, attention, selection.\n"
      "- Example: the user asks \"what nodes are in the graph?\" - reply with exactly one block, nothing else:\n"
      "  TOOL: {\"name\": \"get_graph\", \"params\": {}}\n"
      "- To change the patch, write one PROPOSE: line holding a JSON object:\n"
      "  {\"label\": \"short description\", \"calls\": [{\"method\": \"...\", \"params\": {...}}, ...]}\n"
      "  All edits go through propose: they are validated and applied atomically, and later calls\n"
      "  may reference earlier results with $0.index etc. Never use $0 or $N when no call has\n"
      "  come before it in this same propose: every reference must point at an earlier call.\n"
      "- Edit methods (create_node, set_param, connect, ...) exist only inside a propose's calls:\n"
      "  never call them as tools on their own.\n"
      "- After a PROPOSE that changes sound or wiring, call CHECK: with {\"reset\": false} and fix\n"
      "  any E_ problem in one more PROPOSE.\n"
      "- One action block per reply: never a TOOL: and a PROPOSE: in the same reply. Never write JSON\n"
      "  outside a PROPOSE: or CHECK: line.\n"
      "\n"
      "Tools you can call: propose, check, get_graph, get_params, list_node_types, describe,\n"
      "code_errors, status, attention, selection. Before proposing, inspect the patch when useful.\n";

   std::string ReadFile(const std::string& path, size_t cap)
   {
      std::ifstream in(AppPaths::FsPath(path), std::ios::binary);
      if (!in)
         return {};
      std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      if (body.size() > cap)
         body.resize(cap);
      return body;
   }

   // The canonical tool names the app actually offers, straight from the tool definitions.
   std::vector<std::string> KnownToolNames()
   {
      std::vector<std::string> names;
      for (const nlohmann::json& t : OpenAIAgentTools())
         if (t.is_object() && t.contains("function") && t["function"].is_object() &&
             t["function"].contains("name") && t["function"]["name"].is_string())
            names.push_back(t["function"]["name"].get<std::string>());
      return names;
   }
}

const std::vector<std::string>& DefaultHarnessTools()
{
   static const std::vector<std::string> kDefaults = {
      "propose", "check", "get_graph", "get_params", "list_node_types",
      "describe", "code_errors", "status", "attention", "selection",
   };
   return kDefaults;
}

HarnessConfig LoadHarnessConfig(const std::string& packId)
{
   HarnessConfig out;
   out.source = "builtin";
   out.systemPrompt = kDefaultProtocolPrompt;
   out.tools = DefaultHarnessTools();

   if (!Extensions::IsValidId(packId))
      return out;

   const std::string dir = Extensions::PackDir(packId);
   if (dir.empty())
      return out; // absent or disabled: built-in defaults

   const std::string body = ReadFile(dir + "/harness.json", 1ull << 20);
   if (body.empty())
   {
      out.warning = "harness.json could not be read";
      return out;
   }

   nlohmann::json j;
   try
   {
      j = nlohmann::json::parse(body);
   }
   catch (...)
   {
      out.warning = "harness.json is not valid JSON";
      return out;
   }
   if (!j.is_object())
   {
      out.warning = "harness.json is not an object";
      return out;
   }

   if (j.contains("schema") && (j["schema"].is_number_integer() ? j["schema"].get<int>() != 1
                                                    : !(j["schema"].is_string() && j["schema"].get<std::string>() == "1")))
   {
      out.warning = "harness.json uses an unsupported schema";
      return out;
   }

   // mode: "protocol" or "tools"; anything else stays protocol with a note.
   if (j.contains("mode") && j["mode"].is_string())
   {
      const std::string m = j["mode"].get<std::string>();
      if (m == "tools")
         out.mode = HarnessMode::Tools;
      else if (m != "protocol")
         out.warning = "harness.json names an unknown mode";
   }

   // Optional readable prompt file; a prompt longer than the cap is dropped in favour of
   // the built-in one (protection against a corrupt or hostile pack).
   std::string prompt;
   if (j.contains("promptFile") && j["promptFile"].is_string())
   {
      const std::string file = j["promptFile"].get<std::string>();
      if (file.find("..") != std::string::npos)
         out.warning = "harness.json promptFile escapes the pack";
      else
      {
         const std::string text = ReadFile(dir + "/" + file, kMaxPromptBytes);
         if (!text.empty())
            prompt = text;
      }
   }
   if (prompt.empty() && j.contains("systemPrompt") && j["systemPrompt"].is_string())
   {
      const std::string text = j["systemPrompt"].get<std::string>();
      if (text.size() <= kMaxPromptBytes)
         prompt = text;
      else
         out.warning = "harness.json systemPrompt is too long";
   }
   if (!prompt.empty())
      out.systemPrompt = prompt;

   // Tool subset: keep only names the app actually offers; an empty subset falls back.
   std::vector<std::string> tools;
   if (j.contains("tools") && j["tools"].is_object())
   {
      const auto known = KnownToolNames();
      for (auto it = j["tools"].begin(); it != j["tools"].end(); ++it)
      {
         const bool enabled = !it.value().is_boolean() || it.value().get<bool>();
         if (!enabled)
            continue;
         if (std::find(known.begin(), known.end(), it.key()) == known.end())
            continue; // unknown name: cannot be called, so it is not offered
         tools.push_back(it.key());
      }
   }
   if (tools.empty())
   {
      out.tools = DefaultHarnessTools();
      out.warning = out.warning.empty() ? "harness.json offers no usable tools" : out.warning + "; no usable tools";
   }
   else
      out.tools = std::move(tools);

   // Limits, clamped to sane bounds.
   if (j.contains("limits") && j["limits"].is_object())
   {
      const auto& l = j["limits"];
      if (l.contains("maxSteps") && l["maxSteps"].is_number_integer())
         out.maxSteps = std::clamp(l["maxSteps"].get<int>(), kMinSteps, kMaxSteps);
      if (l.contains("maxParseErrors") && l["maxParseErrors"].is_number_integer())
         out.maxParseErrors = std::clamp(l["maxParseErrors"].get<int>(), 0, kMaxParseErrors);
      if (l.contains("allowMultiAction") && l["allowMultiAction"].is_boolean())
         out.allowMultiAction = l["allowMultiAction"].get<bool>();
   }
   if (j.contains("probeProtocol") && j["probeProtocol"].is_boolean())
      out.probeProtocol = j["probeProtocol"].get<bool>();

   out.source = "pack:" + packId;
   return out;
}
}
