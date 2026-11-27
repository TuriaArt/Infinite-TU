#pragma once
// Text protocol parser for the local-model harness (docs/plans/local-harness/README.md §3).
// Local models (Ollama /v1, custom base URLs) often cannot emit native tool_calls reliably,
// so the harness asks them to answer in a plain text protocol that this parser turns into
// deterministic tool calls (PROPOSE / CHECK). The model never edits the graph directly:
// every action block goes through the existing propose gate, exactly like native tools.

#include <string>
#include <vector>

#include "json.hpp"

namespace chat
{
   enum class ProtocolKind { Text, Propose, Check, Tool, Unknown };

   // One block of a parsed reply.
   struct ProtocolBlock
   {
      ProtocolKind kind = ProtocolKind::Text;
      std::string tag;    // normalized tag name ("TEXT", "PROPOSE", "CHECK", or the unknown one)
      std::string raw;    // Text: its content; action blocks: the raw JSON text
      bool validJson = false; // action blocks: did the JSON parse as an object?
      std::string err;         // action blocks: why not, when validJson is false
      nlohmann::json json;     // action blocks: the parsed object (object(), or discarded)
   };

   // The parsed reply. `text` is everything the model wrote for the user (concatenated
   // TEXT blocks, plus any non-tag lines); `action` is the first action block.
   struct ProtocolReply
   {
      std::string text;
      ProtocolBlock action;
      bool hasAction = false;
      bool multipleActions = false; // more than one action block: extras are ignored
      std::vector<std::string> unknownTags; // tag-like lines we don't know; content stays in text
   };

   // Parse a model reply into a ProtocolReply. Deterministic, never throws: a broken JSON
   // action block is reported (validJson == false, err set) instead of failing the parse.
   ProtocolReply ParseProtocol(const std::string& reply);

   // Propose reference gate: every $N.field in a propose's calls must point at an earlier call
   // in the same propose. Empty string = the propose is valid (see the executor's resolution).
   std::string ValidateProposeRefs(const nlohmann::json& propose);

   // The TOOL: block is read-only by design; edit methods exist only inside a propose's calls.
   bool IsReadOnlyToolName(const std::string& name);

   // Repair a model's over-escaped JSON text: drops the backslash before structural quotes
   // (a quote the model escaped at the token level) and collapses runs of backslashes inside
   // strings (a double-escaped quote is three backslashes before a quote, which is one too
   // many). Applied only when a direct parse fails, so valid JSON is never touched.
   std::string NormalizeActionText(const std::string& raw);

   // Parse an action block's raw text as a JSON object. Returns false with `err` set when
   // the text is not a JSON object. Identifier strings (method names, parameter keys) are
   // cleaned of stray backslashes, and free-text labels of a backslash before a quote.
   bool ParseActionJson(const std::string& raw, nlohmann::json& outObject, std::string& err);
}
