// Settings > Agents & Terminal > "Agents" (docs/plans/terminal/cli-setup.md). Draws what
// CliSetup::Describe() says; reads only status.json, never terminal text, and spawns nothing itself:
// the buttons hand one line to a new Terminal tab.
#include "app/AppShared.h"
#include "app/ui/design/UiFmt.h"
#include "app/ui/design/components/Tip.h"
#include "app/ui/design/GlyphDraw.h"
#include "app/ui/design/TokenColors.h"
#include "app/ui/design/Tokens.gen.h"
#include "app/ui/design/components/ActionButton.h"
#include "app/ui/design/components/ChipButton.h"
#include "app/ui/design/components/CodeBlock.h"
#include "app/ui/design/components/EmptyState.h"
#include "app/ui/design/components/MenuParts.h"
#include "app/ui/design/components/PillGroup.h"
#include "app/ui/design/components/Switch.h"
#include "app/ui/design/components/TextButton.h"
#include "app/ui/design/components/FormParts.h"
#include "app/ui/design/UiScope.h"
#include "core/CliSetup.h"
#include "core/chat/ChatThread.h"
#include "core/chat/Onboarding.h"
#include "core/chat/OpenAIAgent.h"
#include "platform/AppPaths.h"
#include "platform/Platform.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <vector>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace app
{
   namespace
   {
      std::string ReadFile(const std::string& path)
      {
         std::ifstream f(AppPaths::FsPath(path), std::ios::binary);
         std::stringstream ss;
         ss << f.rdbuf();
         return ss.str();
      }

      [[maybe_unused]] void DrawStepIcon(const CliSetup::StepStatus& s, float size)
      {
         const ImVec2 p = ImGui::GetCursorScreenPos();
         ImDrawList* dl = ImGui::GetWindowDrawList();
         const bool light = CategoryColors::IsThemeLight();
         const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
         const float r = size * tok::cli_setup_section_r_ratio;
         using S = CliSetup::StepState;
         // Semantic roles, the same everywhere: go = green, learn = amber, record = red. A glyph or label
         // always rides with the colour.
         if (s.Green())
         {
            const ImU32 col = tok::U32(tok::action_go, light);
            dl->AddPolyline(std::array<ImVec2, tok::cli_setup_section_line_num_points>{ ImVec2(c.x - r, c.y), ImVec2(c.x - r * tok::cli_setup_section_step_icon_ratio, c.y + r * tok::cli_setup_section_read_file_y), ImVec2(c.x + r, c.y - r * tok::cli_setup_section_line_x) }.data(), tok::cli_setup_section_line_polyline_lg, col, 0, tok::cli_setup_section_line_polyline_sm);
         }
         else if (s.state == S::kFailed)
            glyph::Draw(dl, c, size, tok::U32(tok::action_record, light), IconsInfinite::Error);
         else if (s.state == S::kRepair)
            glyph::Draw(dl, c, size, tok::U32(tok::action_learn, light), IconsInfinite::Warning);
         else if (s.state == S::kRunning)
            dl->AddCircleFilled(c, r * tok::cli_setup_section_line_x, tok::U32(tok::action_learn, light));
         else
            dl->AddLine(ImVec2(c.x - r * tok::cli_setup_section_line_x, c.y), ImVec2(c.x + r * tok::cli_setup_section_line_x, c.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), tok::cli_setup_section_step_icon_stroke);
         Ui::Claim(ImVec2(size, size));
      }

      // True when the base URL points at the local machine, where a local Ollama would listen
      // (127.0.0.1, localhost, ::1). Used to turn a generic "connection refused" into the
      // actionable "Ollama is not running" diagnosis.
      bool IsLocalBaseUrl(const std::string& url)
      {
         return url.find("127.0.0.1") != std::string::npos ||
                url.find("localhost") != std::string::npos ||
                url.find("[::1]") != std::string::npos;
      }
   }

   void DrawCliSetupSection()
   {
      using namespace CliSetup;
      const Os os = CurrentOs();
      const Paths paths = PathsFor(AppPaths::AppSupportDir(), os);
      Config& prefs = Prefs();
      ChatAgentsTick();

      FormParts::Section(T("Agents"));

      // Every row the same grid: the label column, the value after it, and one button column on the right edge whose
      // buttons all share one width and the row's height.
      const float rowH = ImGui::GetFrameHeight();
      float bw = 84.0f;
      for (const char* l : { "Sign in", "Sign out", "Set up", "Cancel", "Repair", "Choose…", "Clear all", "Add folder…", "Remove" })
         bw = std::max(bw, ImGui::CalcTextSize(T(l)).x + tok::cli_setup_section_ratio * tok::space_3);
      // Label first, then this: the value text, cut to what fits before the button column.
      auto value = [&](const std::string& text, bool dim)
      {
         const float room = std::max(0.0f, Ui::AvailW() - bw - tok::space_3);
         std::string shown = text;
         if (ImGui::CalcTextSize(shown.c_str()).x > room)
         {
            while (!shown.empty() && ImGui::CalcTextSize(("…" + shown).c_str()).x > room)
               shown.erase(0, 1);
            shown = "…" + shown;
         }
         ImGui::AlignTextToFramePadding();
         if (dim)
            ImGui::TextDisabled("%s", shown.c_str());
         else
            ImGui::TextUnformatted(shown.c_str());
         if (shown != text && ImGui::IsItemHovered())
            Tip::Show("%s", text.c_str());
      };
      // The row's button, in the right column.
      auto button = [&](const char* label, bool primary)
      {
         // Measured without the "###id" a translated label carries, so every button is the column's width and
         // none reaches past the card (that overflow let the page scroll sideways).
         const float w = std::max(bw, ImGui::CalcTextSize(label, nullptr, true).x + tok::cli_setup_section_ratio * tok::space_3);
         FormParts::Inline(0.0f, 0.0f);
         Ui::AlignRight(w);
         return ChipButton::Draw(label, primary, rowH, w);
      };

      // One row per agent, all the same: its mark, its name, where it stands, and the one button that moves it on.
      for (const Agent& a : Agents())
      {
         const ChatAgentInfo info = ChatAgentStatus(a.id);
         ImGui::PushID(a.id);
         FormParts::Label(L(a.name));
         const float rowW = Ui::AvailW();
         const ImVec2 at = ImGui::GetCursorScreenPos();
         ChatDrawAvatar(ImGui::GetWindowDrawList(), ImVec2(at.x + rowH * 0.5f, at.y + rowH * 0.5f), rowH * tok::cli_setup_section_chat_avatar_r, a.id);
         Ui::Claim(ImVec2(rowH, rowH));
         FormParts::Inline(0.0f, tok::space_2);
         value(info.status, !info.ready);
         if (!info.action.empty())  // Cancel shows while a sign-in waits
            if (button(info.action.c_str(), !info.ready))
               ChatAgentAct(a.id, false);
         ChatAgentCodeEntry(a.id, rowW);
         if (!info.hint.empty() && info.problem.empty())
         {
            FormParts::Label("");
            FormParts::TintedText(ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                  [&] { ImGui::TextWrapped("%s", info.hint.c_str()); });
         }
         if (!info.problem.empty())
         {
            FormParts::Label("");
            FormParts::TintedText(tok::U32(tok::action_record, CategoryColors::IsThemeLight()),
                                  [&] { ImGui::TextWrapped("%s", info.problem.c_str()); });
         }
         ImGui::PopID();
      }

      // The model each connected agent answers with; the chat's picker sets the same thing. An agent that is not
      // installed and signed in has no model to pick yet.
      for (const Agent& a : Agents())
      {
         if (!ChatAgentStatus(a.id).ready)
            continue;
         const auto models = ChatAgentModels(a.id);
         if (models.empty())
            continue;
         const std::string face = ChatAgentModelFace(a.id);
         std::string items;
         int cur = 0;
         for (size_t i = 0; i < models.size(); i++)
         {
            items += models[i].second + '\0';
            if (models[i].second == face)
               cur = (int)i;
         }
         items.push_back('\0');
         const std::string label = std::string(a.name) + " " + T("model");
         ImGui::PushID(a.id);
         if (FormParts::Combo(label.c_str(), &cur, items.c_str()))
         {
            prefs.models[a.id] = models[(size_t)cur].first;
            SaveConfig(paths.config, prefs);
         }
         ImGui::PopID();
      }

      // What each agent used through the chat, so a long chat's cost is never a surprise. Cached input is read
      // again on every step of a turn and costs about a tenth of fresh input.
      {
         bool any = false;
         for (const Agent& a : Agents())
         {
            const auto it = prefs.usage.find(a.id);
            if (it == prefs.usage.end() || it->second.turns == 0)
               continue;
            const Config::Usage& u = it->second;
            const long long total = u.input + u.cached + u.output;
            std::string line = std::to_string(u.turns) + " " + T(u.turns == 1 ? "turn" : "turns") + " · " + chat::TokenCount(total) + " " + T("tokens");
            if (total > 0)
               line += " · " + std::to_string((int)(tok::cli_setup_section_line_scale * (double)u.cached / (double)total)) + "% " + T("cached");
            if (u.cost > 0.0)
            {
               char c[32];
               std::snprintf(c, sizeof c, " · $%s", UiFmt::F2(u.cost).c);
               line += c;
            }
            const std::string label = std::string(a.name) + " " + T("usage");
            ImGui::PushID(a.id);
            FormParts::Label(label.c_str());
            value(line, false);
            const bool reset = !any && button(L("Reset"), false);
            ImGui::PopID();
            any = true;
            if (reset)
            {
               prefs.usage.clear();
               SaveConfig(paths.config, prefs);
               break;
            }
         }
      }

      // Only a fault in the plumbing (the infinite command, the MCP link, the skills) shows here, with its fix.
      {
         const std::string exe = RealExecutable();
         const Agent* agent = FindAgent(prefs.agent);
         if (agent == nullptr)
            agent = &Agents()[0];
         const HealthReport health = LatestHealth();
         const View v = Describe(os, ParseStatus("", 0), *agent, AgentInstalledHere(*agent), exe, paths, &health);
         std::string broken;
         for (const StepStatus& st : v.steps)
            if (st.state == StepState::kRepair || st.state == StepState::kFailed)
               broken += (broken.empty() ? "" : ", ") + std::string(T(st.title));
         const ChatAgentInfo cur = ChatAgentStatus(agent->id);
         if (!broken.empty() && cur.ready && !cur.busy)
         {
            FormParts::Label(L("Connection"));
            value(broken, true);
            if (button(L("Repair"), true))
               ChatAgentAct(agent->id, true);
         }
      }

      {
         const Paths wpaths = PathsFor(AppPaths::AppSupportDir(), CurrentOs());
         FormParts::Label(L("Agent folder"));
         value(prefs.workspace.empty() ? std::string(T("The open patch's folder")) : HomeTilde(prefs.workspace), prefs.workspace.empty());
         if (button(L("Choose…"), false))
         {
            const std::string dir = Platform::OpenFolderDialog("Folder for agents", prefs.workspace);
            if (!dir.empty())
            {
               prefs.workspace = dir;
               SaveConfig(wpaths.config, prefs);
            }
         }
         // More folders agents may open, and the commands they may run besides `infinite`: what an Allow under a
         // "Not allowed" note in the chat adds, and where it is taken back.
         for (size_t i = 0; i < prefs.folders.size(); i++)
         {
            ImGui::PushID((int)i);
            FormParts::Label(i == 0 ? L("More folders") : "");
            value(HomeTilde(prefs.folders[i]), false);
            const bool remove = button(L("Remove"), false);
            ImGui::PopID();
            if (remove)
            {
               prefs.folders.erase(prefs.folders.begin() + (long)i);
               SaveConfig(wpaths.config, prefs);
               break;
            }
         }
         FormParts::Label(prefs.folders.empty() ? L("More folders") : "");
         value(prefs.folders.empty() ? std::string(T("None")) : std::string(), true);
         if (button(L("Add folder…"), false))
         {
            const std::string dir = Platform::OpenFolderDialog("Folder for agents", prefs.workspace);
            if (!dir.empty() && std::find(prefs.folders.begin(), prefs.folders.end(), dir) == prefs.folders.end())
            {
               prefs.folders.push_back(dir);
               SaveConfig(wpaths.config, prefs);
            }
         }
         if (FormParts::Check(T("Edit files in them"), &prefs.folderAccess, true, "##agentaccess"))
            SaveConfig(wpaths.config, prefs);
         for (size_t i = 0; i < prefs.commands.size(); i++)
         {
            ImGui::PushID(1000 + (int)i);
            FormParts::Label(i == 0 ? L("Allowed commands") : "");
            value(prefs.commands[i], false);
            const bool remove = button(L("Remove"), false);
            ImGui::PopID();
            if (remove)
            {
               prefs.commands.erase(prefs.commands.begin() + (long)i);
               SaveConfig(wpaths.config, prefs);
               break;
            }
         }
      }

      {
         const int count = ChatSavedCount();
         const std::string label = std::string(T("Saved chats")) + " (" + std::to_string(count) + ")";
         FormParts::Label(label.c_str());
         Ui::Claim(ImVec2(0.0f, rowH));
         ImGui::BeginDisabled(count == 0 || ChatBusy());
         if (button(L("Clear all"), false))
            ImGui::OpenPopup("##clearchats");
         ImGui::EndDisabled();
         ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(tok::space_3, tok::space_3));
         if (MenuParts::BeginPopup("##clearchats"))
         {
            ImGui::TextUnformatted(T("Delete every saved chat?"));
            Ui::Spacer(Ui::Space::S2);
            const float cancelW = std::max(tok::cli_setup_section_cancel_w_min, ImGui::CalcTextSize(T("Cancel")).x + tok::cli_setup_section_ratio * tok::space_2);
            const float deleteW = std::max(tok::cli_setup_section_cancel_w_min, ImGui::CalcTextSize(T("Delete")).x + tok::cli_setup_section_ratio * tok::space_2);
            const float total = cancelW + tok::space_2 + deleteW;
            const float avail = Ui::AvailW();
            Ui::ColumnAt(ImGui::GetCursorPosX() + avail - total);   // only when there is room: never left of the cursor
            if (ChipButton::Draw(L("Cancel"), false, tok::control_h, cancelW))
               ImGui::CloseCurrentPopup();
            FormParts::Inline(0.0f, tok::space_2);
            if (ChipButton::Draw(L("Delete"), true, tok::control_h, deleteW))
            {
               ChatClearHistory();
               ImGui::CloseCurrentPopup();
            }
            MenuParts::EndPopup();
         }
         ImGui::PopStyleVar();
      }

      Ui::Spacer(Ui::Space::S1);
      SectionCard::End();

      // Custom OpenAI-compatible agent (Ollama, DeepSeek, OpenRouter, custom base URL).
      // API key is stored only in Platform::SecretStore (Keychain / Credential Manager), never in config.json.
      SectionCard::Begin(T("Custom agent"));
      {
         const Paths cpaths = PathsFor(AppPaths::AppSupportDir(), os);
         const auto& presets = chat::CustomPresets();
         static bool sKeyLoaded = false;
         static char sUrlBuf[512] = {};
         static char sModelBuf[256] = {};
         static char sKeyBuf[512] = {};
         static std::string sLoadedUrl, sLoadedModel;
         static std::mutex sTestMutex;
         static std::atomic<bool> sTesting{ false };
         [[maybe_unused]] static bool sTestOk = false;
         static std::string sTestStatus;
         static std::vector<std::string> sModelList;      // ids the endpoint listed, most recent check
         static std::atomic<bool> sModelListLoading{ false };
         static std::chrono::steady_clock::time_point sModelListStart;  // watchdog start, see Browse handler
         static std::atomic<bool> sModelListWantPopup{ false };  // result arrived after the popup was closed: reopen it

         if (sLoadedUrl != prefs.customBaseUrl)
         {
            sLoadedUrl = prefs.customBaseUrl;
            std::snprintf(sUrlBuf, sizeof sUrlBuf, "%s", prefs.customBaseUrl.c_str());
         }
         if (sLoadedModel != prefs.customModel)
         {
            sLoadedModel = prefs.customModel;
            std::snprintf(sModelBuf, sizeof sModelBuf, "%s", prefs.customModel.c_str());
         }
         if (!sKeyLoaded)
         {
            sKeyLoaded = true;
            std::string sec;
            if (Platform::SecretLoad("custom_agent_api_key", sec))
               std::snprintf(sKeyBuf, sizeof sKeyBuf, "%s", sec.c_str());
         }

         FormParts::Label(L("Preset"));
         const chat::CustomPreset* curPreset = chat::FindCustomPreset(prefs.customPreset);
         const char* preview = curPreset ? curPreset->name : "Custom";
         ImGui::SetNextItemWidth(-1.0f);
         if (MenuParts::BeginCombo("##custompreset", preview))
         {
            for (const chat::CustomPreset& p : presets)
            {
               const bool sel = (prefs.customPreset == p.id);
               if (MenuParts::Choice(p.name, sel))
               {
                  prefs.customPreset = p.id;
                  if (p.baseUrl[0] != '\0')
                     prefs.customBaseUrl = p.baseUrl;
                  if (p.defaultModel[0] != '\0')
                     prefs.customModel = p.defaultModel;
                  sLoadedUrl.clear();
                  sLoadedModel.clear();
                  SaveConfig(cpaths.config, prefs);
               }
            }
            MenuParts::EndCombo();
         }

         FormParts::Label(L("Base URL"));
         ImGui::SetNextItemWidth(-1.0f);
         ImGui::InputText("##customurl", sUrlBuf, sizeof sUrlBuf);
         if (ImGui::IsItemDeactivatedAfterEdit())
         {
            prefs.customBaseUrl = sUrlBuf;
            sLoadedUrl = prefs.customBaseUrl;
            SaveConfig(cpaths.config, prefs);
         }

         FormParts::Label(L("Model ID"));
         ImGui::SetNextItemWidth(-1.0f);
         ImGui::InputText("##custommodel", sModelBuf, sizeof sModelBuf);
         if (ImGui::IsItemDeactivatedAfterEdit())
         {
            prefs.customModel = sModelBuf;
            sLoadedModel = prefs.customModel;
            SaveConfig(cpaths.config, prefs);
         }
         // Browse the endpoint's own model list (Ollama and the hosted providers all answer /models).
         // A pick fills the field above, like choosing from any other agent's menu.
         FormParts::Inline(0.0f, tok::space_1);
         if (button(L("Browse models"), false))
         {
            const std::string url = sUrlBuf;
            std::string key;
            if (sKeyBuf[0] != '\0')
               Platform::SecretLoad("custom_agent_api_key", key);
            if (!sModelListLoading.exchange(true))
            {
               sModelListStart = std::chrono::steady_clock::now();
               // Detached worker, value-captured url/key only. It never calls ImGui; the only
               // lifetime risk is the app shutting down mid-request, which is safe here: the
               // statics outlive the window, the worker only writes them under sTestMutex, and
               // the flag is always cleared (try/catch + the render watchdog below), so a
               // crashed request can never leave sModelListLoading stuck and the button dead.
               std::thread([url, key]() {
                  std::vector<std::string> models;
                  std::string msg;
                  bool ok = false;
                  try
                  {
                     ok = chat::ListOpenAIModels(url, key, models, msg);
                  }
                  catch (const std::exception& e)
                  {
                     models.clear();
                     msg = std::string(T("The request failed.")) + " (" + e.what() + ")";
                  }
                  {
                     std::lock_guard<std::mutex> lock(sTestMutex);
                     sModelList = std::move(models);
                     sTestStatus = ok ? msg : (msg.empty() ? std::string(T("No models listed")) : msg);
                  }
                  sModelListLoading.store(false);
                  // The popup may have been closed (click-outside) while the request was in
                  // flight: ask the render loop to open it again with the finished result.
                  sModelListWantPopup.store(true);
               }).detach();
            }
            // Re-open on every click, even while a load is already running - it may hold a
            // result the user has not seen yet.
            ImGui::OpenPopup("##custommodels");
         }
         // A result that arrived after the user closed the popup (click-outside while the
         // request was in flight) is re-shown by opening it again - otherwise the finished
         // list or error would only live in the status row below, out of view.
         if (sModelListWantPopup.load() && !ImGui::IsPopupOpen("##custommodels"))
            ImGui::OpenPopup("##custommodels");
         if (MenuParts::BeginPopup("##custommodels"))
         {
            // Watchdog: a worker that died without clearing the flag (or a request that hung
            // past the shared timeout) would leave the button silently dead and the popup
            // stuck on "Loading…". Force-finish and report the failure instead.
            if (sModelListLoading.load() &&
                std::chrono::steady_clock::now() - sModelListStart >
                   std::chrono::seconds(chat::kOpenAIRequestTimeoutSec + 10))
            {
               std::lock_guard<std::mutex> lock(sTestMutex);
               sModelList.clear();
               sTestStatus = T("Model list request timed out.");
               sModelListLoading.store(false);
            }
            if (sModelListLoading.load())
            {
               ImGui::TextUnformatted(T("Connecting…"));
            }
            else
            {
               for (const std::string& m : sModelList)
               {
                  const bool sel = (m == std::string(sModelBuf));
                  if (MenuParts::Choice(m.c_str(), sel))
                  {
                     std::snprintf(sModelBuf, sizeof sModelBuf, "%s", m.c_str());
                     prefs.customModel = sModelBuf;
                     sLoadedModel.clear();
                     SaveConfig(cpaths.config, prefs);
                     ImGui::CloseCurrentPopup();
                  }
               }
               if (sModelList.empty())
               {
                  // Surface the actual failure here, not only "No models listed": the status
                  // row below may be scrolled out of view. TextUnformatted + concatenation, not
                  // a printf-style Text: sTestStatus can carry a raw server error body that may
                  // itself contain format specifiers.
                  if (!sTestStatus.empty())
                     ImGui::TextUnformatted((std::string(T("Error:")) + " " + sTestStatus).c_str());
                  else
                     ImGui::TextUnformatted(T("No models listed"));
               }
            }
            MenuParts::EndPopup();
         }
         // The popup is closed again (click-outside / Esc): stop requesting reopens until the
         // next Browse click or a new result.
         if (!ImGui::IsPopupOpen("##custommodels"))
            sModelListWantPopup.store(false);

         FormParts::Label(L("API key"));
         ImGui::SetNextItemWidth(-1.0f);
         ImGui::InputTextWithHint("##customkey", T("Optional for local Ollama; stored in Keychain"), sKeyBuf, sizeof sKeyBuf, ImGuiInputTextFlags_Password);
         if (ImGui::IsItemDeactivatedAfterEdit())
         {
            if (sKeyBuf[0] == '\0')
               Platform::SecretClear("custom_agent_api_key");
            else
               Platform::SecretStore("custom_agent_api_key", sKeyBuf);
         }

         FormParts::Label(L("Active"));
         const bool isActive = (prefs.agent == "custom");
         value(isActive ? std::string(T("Selected in chat")) : std::string(T("Not selected")), !isActive);
         if (!isActive && button(L("Use in chat"), false))
         {
            prefs.agent = "custom";
            SaveConfig(cpaths.config, prefs);
         }

         FormParts::Label(L("Connection"));
         {
            std::lock_guard<std::mutex> lock(sTestMutex);
            std::string statusText = sTesting.load() ? std::string(T("Testing…")) : (sTestStatus.empty() ? std::string(T("Not tested")) : sTestStatus);
            // A refused connection to a local base URL means the local Ollama is not running:
            // say so instead of the generic diagnosis (the Start Ollama button sits right below).
            if (!statusText.empty() && !sTesting.load() &&
                chat::ClassifyHttpError(statusText) == chat::HttpErrorKind::kConnectionRefused &&
                IsLocalBaseUrl(prefs.customBaseUrl))
               statusText = T("Ollama is not running");
            value(statusText, sTestStatus.empty() && !sTesting.load());
         }
         ImGui::BeginDisabled(sTesting.load());
         if (button(L("Test connection"), false))
         {
            prefs.customBaseUrl = sUrlBuf;
            prefs.customModel = sModelBuf;
            SaveConfig(cpaths.config, prefs);
            if (sKeyBuf[0] == '\0')
               Platform::SecretClear("custom_agent_api_key");
            else
               Platform::SecretStore("custom_agent_api_key", sKeyBuf);

            sTesting.store(true);
            const std::string url = prefs.customBaseUrl;
            const std::string model = prefs.customModel;
            const std::string key = sKeyBuf;
            std::thread([url, model, key]() {
               std::string msg;
               const bool ok = chat::TestOpenAIConnection(url, model, key, msg);
               // A working endpoint also answers /models: list them for the Browse button, and
               // an empty model field can take the first one the endpoint offers.
               std::vector<std::string> models;
               if (ok)
               {
                  std::string listMsg;
                  if (chat::ListOpenAIModels(url, key, models, listMsg))
                     msg += " " + listMsg;
               }
               {
                  std::lock_guard<std::mutex> lock(sTestMutex);
                  sTestOk = ok;
                  sTestStatus = msg;
                  sModelList = std::move(models);
               }
               sTesting.store(false);
            }).detach();
         }

         // Start the local Ollama server in the background (only meaningful for a local base
         // URL). The probe first: if a server is already answering, never launch a second
         // instance - just report that it is running and refresh the model list.
         if (button(L("Start Ollama"), false))
         {
            const std::string url = sUrlBuf;
            std::string key;
            if (sKeyBuf[0] != '\0')
               Platform::SecretLoad("custom_agent_api_key", key);
            // Detached worker, value-captured url/key only; same lifetime caveat as the Browse
            // worker above - no ImGui calls from the worker, statics outlive the window.
            std::thread([url, key]() {
               std::vector<std::string> models;
               std::string msg;
               if (chat::ListOpenAIModels(url, key, models, msg))
               {
                  std::lock_guard<std::mutex> lock(sTestMutex);
                  sModelList = std::move(models);
                  sTestStatus = T("Ollama is already running.");
                  sModelListWantPopup.store(true);
                  return;
               }
               const std::string exe = Platform::FindOllamaExecutable();
               if (exe.empty())
               {
                  std::lock_guard<std::mutex> lock(sTestMutex);
                  sTestStatus = T("Ollama not found. Install it and restart the app.");
                  return;
               }
               if (!Platform::StartDetachedProcess(exe, { "serve" }))
               {
                  std::lock_guard<std::mutex> lock(sTestMutex);
                  sTestStatus = T("Failed to start Ollama");
                  return;
               }
               {
                  // Lock only around the status write: the mutex must be free while we sleep
                  // and probe, or the second lock below would deadlock on the same thread.
                  std::lock_guard<std::mutex> lock(sTestMutex);
                  sTestStatus = T("Starting Ollama…");
               }
               // Give the server a moment to bind its port, then probe again so the outcome is
               // visible without a second click.
               std::this_thread::sleep_for(std::chrono::seconds(2));
               std::vector<std::string> models2;
               std::string msg2;
               if (chat::ListOpenAIModels(url, key, models2, msg2))
               {
                  std::lock_guard<std::mutex> lock2(sTestMutex);
                  sModelList = std::move(models2);
                  sTestStatus = T("Ollama started.");
                  sModelListWantPopup.store(true);
               }
               else
               {
                  std::lock_guard<std::mutex> lock2(sTestMutex);
                  sTestStatus = msg2;
               }
            }).detach();
         }
         ImGui::EndDisabled();

         Ui::Spacer(Ui::Space::S1);
         SectionCard::End();
      }
   }

   // First open of the Terminal: what it is, which agent to connect, one button to set it up. Returns true
   // while the welcome is showing (the panel draws nothing else then). "Skip" or any setup ends it for good.
   bool DrawTerminalWelcome()
   {
      using namespace CliSetup;
      Config& prefs = Prefs();
      if (prefs.welcomed)
         return false;
      // A clipped or collapsing panel submits no items; moving the cursor then would trip ImGui's boundary check.
      if (ImGui::GetCurrentWindow()->SkipItems)
         return true;
      const Os os = CurrentOs();
      const Paths paths = PathsFor(AppPaths::AppSupportDir(), os);
      const Agent* agent = FindAgent(prefs.agent);
      if (agent == nullptr)
         agent = &Agents()[0];
      const std::string exe = RealExecutable();
      const View v = Describe(os, ParseStatus(ReadFile(paths.status), (long long)std::time(nullptr)), *agent, AgentInstalledHere(*agent), exe, paths, nullptr);

      auto finish = [&]()
      {
         prefs.welcomed = true;
         SaveConfig(paths.config, prefs);
      };

      // One centred column, never wider than reads well; every control is a design component.
      const ImVec2 origin = ImGui::GetCursorScreenPos();
      const float availW = Ui::AvailW();
      const float colW = std::min(tok::cli_setup_section_col_w_max, std::max(tok::cli_setup_section_col_w_min, availW - tok::cli_setup_section_terminal_welcome_ratio * tok::space_4));
      const float x = origin.x + (availW - colW) * 0.5f;
      const float rowH = ImGui::GetFrameHeight() + tok::space_1;
      float y = origin.y + tok::space_5;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
      const float cx = x + colW * 0.5f;
      auto centred = [&](UiType::Size size, UiType::Weight weight, ImU32 col, const char* text)
      {
         UiType::Scope scope(size, weight);
         y = EmptyState::DrawWrapped(dl, cx, y, colW, col, text);
      };

      centred(UiType::Size::Body, UiType::Weight::Semibold, ImGui::GetColorU32(ImGuiCol_Text), T("Welcome to the Terminal"));
      y += tok::space_2;
      centred(UiType::Size::Body, UiType::Weight::Regular, dim, T("Connect a coding agent to build with Infinite."));
      y += tok::space_5;

      {
         const auto& all = Agents();
         std::vector<PillGroup::Segment> segs;
         for (const Agent& a : all)
            segs.push_back({ a.id, a.name });
         int cur = 0;
         for (size_t i = 0; i < all.size(); i++)
            if (&all[i] == agent)
               cur = (int)i;
         const int hit = PillGroup::Draw("welcome.agent", UiLayout::Rect{ x, y, colW, rowH }, segs.data(), (int)segs.size(), cur);
         if (hit >= 0 && hit != cur)
         {
            prefs.agent = all[(size_t)hit].id;
            SaveConfig(paths.config, prefs);
         }
         y += rowH + tok::space_3;
      }
      y += tok::space_1;

      static std::string sError;
      auto perform = [&](Action a)
      {
         sError.clear();
         Perform(a, os, *agent, exe, paths, [](const std::string&, const std::string& line) { TerminalRunInNewTab(line); }, &sError, std::string(), nullptr);
         finish();
      };
      // What the button does, said first and in plain words; the agent starts only when the person starts it.
      centred(UiType::Size::Caption, UiType::Weight::Regular, dim, chat::SetUpNote(*agent, os == Os::kWindows, AgentInstalledHere(*agent)).c_str());
      y += tok::space_2;
      const std::string connect = std::string(AgentInstalledHere(*agent) ? T("Connect") : T("Install")) + " " + agent->name;
      if (TextButton::Draw("welcome.connect", UiLayout::Rect{ x, y, colW, rowH }, connect.c_str(), TextButton::Kind::Primary))
         perform(Action::kSetupAgent);
      y += rowH + tok::space_2;
      if (TextButton::Draw("welcome.skip", UiLayout::Rect{ x, y, colW, rowH }, T("Skip, just a terminal"), TextButton::Kind::Quiet))
         finish();
      y += rowH;
      if (!sError.empty())
      {
         y += tok::space_2;
         centred(UiType::Size::Caption, UiType::Weight::Regular, tok::U32(tok::action_record, CategoryColors::IsThemeLight()), sError.c_str());
      }
      ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
      Ui::Claim(ImVec2(availW, tok::space_4));
      return true;
   }
}
