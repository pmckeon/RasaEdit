#include "Editor.h"

#include "imgui_internal.h"   // DockBuilderGetNode

Editor::Editor(SDL_Window* window, SDL_GPUDevice* device)
    : context_(std::make_unique<EditorContext>(window, device)),
      client_(std::make_unique<ClientWorkspace>(*context_)),
      ui_(std::make_unique<UiWorkspace>(*context_)),
      server_(std::make_unique<ServerWorkspace>(*context_)),
      assets_(std::make_unique<AssetsWorkspace>(*context_)) {
    const std::string workspace = context_->setting("workspace");
    active_ = workspace == "client"   ? static_cast<Workspace*>(client_.get())
              : workspace == "ui"     ? static_cast<Workspace*>(ui_.get())
              : workspace == "assets" ? static_cast<Workspace*>(assets_.get())
                                      : static_cast<Workspace*>(server_.get());
    // Reopen the last game, server database and map on start-up.
    const std::string root = context_->setting("gameRoot");
    if (!root.empty()) context_->queueGameRoot(root);
    // A MySQL database is remembered without its password: with a remembered password it reconnects, else the connect dialog
    // opens pre-filled.
    const std::string database = context_->setting("database");
    if (isMySqlSpec(database)) {
        const std::string password = context_->setting("databasePassword");
        MySqlSpec m;
        if (!password.empty() && parseMySqlSpec(database, m)) {
            m.password = password;
            context_->queueDatabase("mysql://" + m.user + ":" + m.password + "@" + m.host + ":" + std::to_string(m.port) + "/" + m.database);
        } else server_->showMySqlDialog(database);
    } else if (!database.empty()) context_->queueDatabase(database);
    const std::string map = context_->setting("map");
    if (!map.empty()) context_->queueMap(map);
}

Editor::~Editor() = default;

void Editor::setActive(Workspace* workspace) {
    active_ = workspace;
    context_->setSetting("workspace", workspace == client_.get() ? "client" : workspace == ui_.get() ? "ui" : workspace == assets_.get() ? "assets" : "server");
}

// ------------------------------------------------------------------------------------------
// Between frames
// ------------------------------------------------------------------------------------------
void Editor::processPendingActions() {
    const EditorContext::Pending pending = context_->takePending();
    // Loading a game, map or database replaces the server content: ask first if it has unsaved edits.
    auto mayReplaceServerContent = [&](PromptAction action, const std::string& target) {
        // A new game or map also replaces the client workspace's map edits; a new database only the server content.
        const bool unsaved = server_->hasUnsavedChanges() || (action != PromptAction::OpenDatabase && client_->hasUnsavedChanges()) ||
                             (action == PromptAction::OpenGame && ui_->hasUnsavedChanges());
        if (!unsaved || skipUnsavedCheck_) {
            skipUnsavedCheck_ = false;
            return true;
        }
        promptAction_ = action;
        promptTarget_ = target;
        return false;
    };
    if (!pending.gameRoot.empty() && mayReplaceServerContent(PromptAction::OpenGame, pending.gameRoot) && context_->openGame(pending.gameRoot)) {
        client_->onGameOpened();
        ui_->onGameOpened();
        server_->onGameOpened();
        assets_->onGameOpened();
    }
    if (!pending.database.empty() && mayReplaceServerContent(PromptAction::OpenDatabase, pending.database)) server_->openDatabase(pending.database);
    if (!pending.map.empty() && mayReplaceServerContent(PromptAction::OpenMap, pending.map) && context_->openMap(pending.map)) {
        client_->onMapLoaded();
        server_->onMapLoaded();
    }
    if (context_->scene) context_->scene->loadPending(0.010);      // a few models per frame
}

// ------------------------------------------------------------------------------------------
// Frame
// ------------------------------------------------------------------------------------------
void Editor::drawUi() {
    if (!context_->showAssetRequest.empty()) {          // "Open in Assets" from another workspace
        setActive(assets_.get());
        assets_->showAsset(context_->showAssetRequest);
        context_->showAssetRequest.clear();
    }
    active_->beginFrame();
    active_->handleShortcuts();
    drawMenuBar();
    drawUnsavedPrompt();
    const ImGuiID dockspace = ImGui::GetID(active_->dockspaceName());
    if (ImGui::DockBuilderGetNode(dockspace) == nullptr) active_->buildDefaultLayout(dockspace);   // first use: no saved layout
    ImGui::DockSpaceOverViewport(dockspace, ImGui::GetMainViewport());
    active_->drawPanels();
}

void Editor::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    // Workspace tabs.
    for (Workspace* w : {static_cast<Workspace*>(client_.get()), static_cast<Workspace*>(ui_.get()), static_cast<Workspace*>(server_.get()),
                         static_cast<Workspace*>(assets_.get())}) {
        const bool current = w == active_;
        if (current) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_CheckMark));
        if (ImGui::MenuItem(w->name(), nullptr, current) && !current) setActive(w);
        if (current) ImGui::PopStyleColor();
    }
    ImGui::Separator();
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open Game Folder...")) context_->showGameFolderDialog();
        if (ImGui::MenuItem("Open Server Database...")) server_->showDatabaseDialog();
        if (ImGui::MenuItem("Connect to MySQL Database...")) server_->showMySqlDialog();
        if (ImGui::MenuItem("Save Server Changes", "Ctrl+S", false, server_->canEditServer() && server_->hasUnsavedChanges())) server_->saveChanges();
        if (ImGui::MenuItem("Save Map Changes", nullptr, false, client_->hasUnsavedChanges())) client_->saveMap();
        if (ImGui::MenuItem("Save UI Layout", nullptr, false, ui_->hasUnsavedChanges())) ui_->saveChanges();
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) {
            SDL_Event quit = {};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        active_->drawEditMenu();
        ImGui::EndMenu();
    }
    if (server_->hasUnsavedChanges()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "unsaved server changes");
    }
    if (client_->hasUnsavedChanges()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "unsaved map changes");
    }
    if (ui_->hasUnsavedChanges()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "unsaved UI layout changes");
    }
    if (!context_->statusText.empty()) {
        ImGui::Separator();
        if (context_->statusIsError) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", context_->statusText.c_str());
        else ImGui::TextDisabled("%s", context_->statusText.c_str());
    }
    ImGui::EndMainMenuBar();
}

void Editor::renderViewport(SDL_GPUCommandBuffer* commands) { active_->renderViewport(commands); }

void Editor::releaseGpuResources() {
    ui_->releaseGpuResources();
    assets_->releaseGpuResources();
    context_->releaseGpuResources();
}

// ------------------------------------------------------------------------------------------
// Unsaved changes
// ------------------------------------------------------------------------------------------
bool Editor::requestQuit() {
    if (!server_->hasUnsavedChanges() && !client_->hasUnsavedChanges() && !ui_->hasUnsavedChanges()) return true;
    promptAction_ = PromptAction::Quit;
    return false;
}

void Editor::drawUnsavedPrompt() {
    if (promptAction_ == PromptAction::None) return;
    if (!ImGui::IsPopupOpen("Unsaved server changes")) ImGui::OpenPopup("Unsaved server changes");
    if (!ImGui::BeginPopupModal("Unsaved server changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (server_->hasUnsavedChanges()) ImGui::Text("The server content of %s has unsaved changes.", context_->currentMapName.c_str());
    if (client_->hasUnsavedChanges() && promptAction_ != PromptAction::OpenDatabase)
        ImGui::Text("The map %s has unsaved entity changes.", context_->currentMapName.c_str());
    const bool uiToo = promptAction_ == PromptAction::OpenGame || promptAction_ == PromptAction::Quit;
    if (uiToo && ui_->hasUnsavedChanges()) ImGui::Text("The UI layout has unsaved changes.");
    ImGui::Spacing();
    auto proceed = [&]() {
        switch (promptAction_) {
            case PromptAction::OpenGame: context_->queueGameRoot(promptTarget_); skipUnsavedCheck_ = true; break;
            case PromptAction::OpenMap: context_->queueMap(promptTarget_); skipUnsavedCheck_ = true; break;
            case PromptAction::OpenDatabase: context_->queueDatabase(promptTarget_); skipUnsavedCheck_ = true; break;
            case PromptAction::Quit: quitApproved_ = true; break;
            default: break;
        }
        promptAction_ = PromptAction::None;
        ImGui::CloseCurrentPopup();
    };
    if (ImGui::Button("Save")) {
        const bool clientToo = promptAction_ != PromptAction::OpenDatabase;
        if (server_->saveChanges() && (!clientToo || client_->saveChanges()) && (!uiToo || ui_->saveChanges())) {
            proceed();
        } else {
            promptAction_ = PromptAction::None;              // keep the changes; the status bar shows why
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard")) proceed();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        promptAction_ = PromptAction::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
