#pragma once
// Harness configuration for local-model agents (docs/plans/local-harness/README.md §4).
// The tunable half of the local harness (system prompt, tool subset, step limits) lives in
// an extension pack's harness.json; the deterministic half (transport, protocol parser,
// propose gate, step loop) stays in core. A missing, disabled or invalid pack falls back
// to the built-in defaults below, so the feature always works.

#include <string>
#include <vector>

namespace chat
{
   enum class HarnessMode { Protocol, Tools };

   struct HarnessConfig
   {
      HarnessMode mode = HarnessMode::Protocol;
      std::string systemPrompt;  // empty: the agent's own briefing
      std::string source;        // "pack:<id>" when loaded from a pack, "builtin" otherwise
      std::vector<std::string> tools; // enabled tool names (subset of the full surface)
      int maxSteps = 12;
      int maxParseErrors = 2;
      bool allowMultiAction = false;
      bool probeProtocol = false;
      std::string warning;       // last problem with the pack's harness.json (best-effort)
   };

   // Load <PackDir(packId)>/harness.json (schema 1). Falls back to built-in defaults when
   // the pack is absent, disabled, unreadable or invalid; `warning` carries what went wrong.
   HarnessConfig LoadHarnessConfig(const std::string& packId);

   // The built-in default tool subset for local models: fewer tools than the full surface
   // raises the chance a weak model picks the right one.
   const std::vector<std::string>& DefaultHarnessTools();
}
