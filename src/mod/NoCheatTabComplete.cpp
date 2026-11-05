#include "mod/NoCheatTabComplete.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/command/ClientCommandRegisterEvent.h"
#include "ll/api/event/command/ServerCommandRegisterEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"

#include "mc/client/gui/screens/IntellisenseHandler.h"
#include "mc/client/gui/screens/models/ClientInstanceScreenModel.h"
#include "mc/locale/I18n.h"
#include "mc/network/packet/AvailableCommandsPacket.h"
#include "mc/server/commands/AutoCompleteInformation.h"
#include "mc/server/commands/AutoCompleteOption.h"
#include "mc/server/commands/CommandFlag.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandParameterData.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/server/commands/CommandRegistry.h"
#include "mc/server/commands/SemanticConstraint.h"

namespace no_cheat_tab_complete {

namespace {

[[nodiscard]] constexpr CommandFlagValue operator~(CommandFlagValue const val) noexcept {
    return static_cast<CommandFlagValue>(
        ~static_cast<std::underlying_type_t<CommandFlagValue>>(val)
    );
}

void fixSignatures(CommandRegistry* registry) {
    if (!registry) return;
    for (auto& [name, sig] : registry->mSignatures) {
        sig.flags.value = (sig.flags.value | CommandFlagValue::NotCheat) & ~CommandFlagValue::HiddenFromPlayerOrigin;
        sig.permissionLevel = CommandPermissionLevel::Any;
    }
}

void fixSignatures(CommandRegistry& registry) {
    fixSignatures(&registry);
}

std::vector<std::string> getFirstLevelCandidates(
    IntellisenseHandler const& handler,
    std::string const&         typedPrefix
) {
    std::vector<std::string>        candidates;
    std::unordered_set<std::string> seen;

    auto matches = [&](std::string_view name) {
        if (name.empty()) return false;
        if (typedPrefix.empty()) return true;
        if (name.size() < typedPrefix.size()) return false;
        for (size_t i = 0; i < typedPrefix.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(name[i])) !=
                std::tolower(static_cast<unsigned char>(typedPrefix[i]))) {
                return false;
            }
        }
        return true;
    };

    auto addCandidate = [&](std::string name) {
        if (name.starts_with('/')) {
            name = name.substr(1);
        }
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
            name.pop_back();
        }
        if (name.empty()) return;
        if (matches(name)) {
            if (seen.insert(name).second) {
                candidates.push_back(name);
            }
        }
    };

    // 1. From mAutoCompleteMessages
    for (auto const& item : handler.mAutoCompleteMessages.get()) {
        addCandidate(item.text.get());
    }

    // 2. From mIntellisenseMessages (e.g. "gamemode - description...")
    for (auto const& msg : handler.mIntellisenseMessages.get()) {
        auto        spacePos = msg.find(' ');
        std::string name     = (spacePos != std::string::npos) ? msg.substr(0, spacePos) : msg;
        addCandidate(name);
    }

    // 3. From client command registry (holds remote server commands when connected to a server)
    if (auto regRef = ll::service::getCommandRegistry(true); regRef.has_value()) {
        for (auto const& [name, sig] : regRef->mSignatures) {
            if ((sig.flags.value & CommandFlagValue::Removed) == CommandFlagValue::Removed) continue;
            if (!sig.runnable && sig.overloads.empty()) continue;
            addCandidate(name);
        }
    }

    // 4. From server command registry (holds local server commands in singleplayer)
    if (auto regRef = ll::service::getCommandRegistry(false); regRef.has_value()) {
        for (auto const& [name, sig] : regRef->mSignatures) {
            if ((sig.flags.value & CommandFlagValue::Removed) == CommandFlagValue::Removed) continue;
            if (!sig.runnable && sig.overloads.empty()) continue;
            addCandidate(name);
        }
    }

    // Alphabetical sort for predictable Tab-cycling
    std::sort(candidates.begin(), candidates.end());
    return candidates;
}

bool doFirstLevelTabComplete(
    IntellisenseHandler&                         handler,
    std::weak_ptr<ClientInstanceScreenModel>     minecraftScreenModel,
    std::string&                                 currentMessage,
    int                                          index
) {
    // Only execute command Tab completion if the message starts with '/'
    if (!currentMessage.starts_with('/')) {
        handler.mInitTabComplete = false;
        return false;
    }

    std::string text = currentMessage.substr(1);

    // If there is a space, it's a subcommand/argument, not a first-level command
    if (text.find(' ') != std::string::npos) {
        handler.mInitTabComplete = false;
        return false;
    }

    static std::string              sInitialPrefix;
    static std::vector<std::string> sCandidates;

    std::string const& lastMsg = handler.mLastTabCompleteMessage.get();

    // Check if starting a new tab cycle or continuing an existing one
    if (!handler.mInitTabComplete || currentMessage != lastMsg) {
        sInitialPrefix = text;
        sCandidates    = getFirstLevelCandidates(handler, sInitialPrefix);
        if (sCandidates.empty()) {
            handler.mInitTabComplete = false;
            return false;
        }

        handler.mInitTabComplete      = true;
        handler.mLastTabCompleteIndex = -1;
    }

    if (sCandidates.empty()) {
        handler.mInitTabComplete = false;
        return false;
    }

    int count   = static_cast<int>(sCandidates.size());
    int step    = (index >= 0) ? 1 : -1;
    int nextIdx = (handler.mLastTabCompleteIndex + step + count) % count;

    // Do NOT add a trailing space: command completion leaves the cursor right after the command name
    std::string completed = "/" + sCandidates[nextIdx];
    currentMessage        = completed;

    if (auto model = minecraftScreenModel.lock()) {
        model->setTextBoxBackend(completed);
    }

    handler.mLastTabCompleteIndex   = nextIdx;
    handler.mLastTabCompleteMessage = completed;
    handler.mNeedsLayoutUpdate      = true;
    return true;
}

} // namespace

// Hook registerCommand so all newly registered commands have NotCheat flag and are visible
LL_AUTO_TYPE_INSTANCE_HOOK(
    CommandRegistryRegisterCommandHook,
    ll::memory::HookPriority::Normal,
    CommandRegistry,
    &CommandRegistry::registerCommand,
    void,
    std::string const&       name,
    char const*              description,
    ::CommandPermissionLevel requirement,
    CommandFlag              flag1,
    CommandFlag              flag2
) {
    flag1.value = (flag1.value | CommandFlagValue::NotCheat) & ~CommandFlagValue::HiddenFromPlayerOrigin;
    origin(name, description, requirement, flag1, flag2);
}

// Hook serializeAvailableCommands so that packets sent across network mark commands with NotCheat flag
LL_AUTO_TYPE_INSTANCE_HOOK(
    CommandRegistrySerializeAvailableCommandsHook,
    ll::memory::HookPriority::Normal,
    CommandRegistry,
    &CommandRegistry::serializeAvailableCommands,
    ::AvailableCommandsPacket
) {
    auto& nonConstRegistry = const_cast<CommandRegistry&>(static_cast<CommandRegistry const&>(*this));
    fixSignatures(nonConstRegistry);
    auto packet = origin();
    return packet;
}

// Hook autoCompleteCommand on client to ensure all matching commands are included in AutoCompleteInformation
LL_AUTO_TYPE_INSTANCE_HOOK(
    CommandRegistryAutoCompleteCommandHook,
    ll::memory::HookPriority::Normal,
    CommandRegistry,
    &CommandRegistry::autoCompleteCommand,
    void,
    ::std::string const&       partialMatch,
    ::CommandOrigin const&     cmdOrigin,
    ::AutoCompleteInformation& info
) {
    fixSignatures(const_cast<CommandRegistry&>(static_cast<CommandRegistry const&>(*this)));
    origin(partialMatch, cmdOrigin, info);
}

// Hook canCommandBeUsed on client to ensure intellisense previews are NEVER hidden
LL_AUTO_TYPE_INSTANCE_HOOK(
    CommandRegistryCanCommandBeUsedHook,
    ll::memory::HookPriority::Normal,
    CommandRegistry,
    &CommandRegistry::canCommandBeUsed,
    bool,
    [[maybe_unused]] ::std::string const&            commandLine,
    [[maybe_unused]] ::CommandPermissionLevel const& permissionLevel,
    [[maybe_unused]] bool                            cheatsEnabled,
    [[maybe_unused]] bool                            isEditorWorld
) {
    return true;
}

// Hook _updateAutoCompleteMessages on client to ensure candidates are not stripped when cheats are disabled
LL_AUTO_TYPE_INSTANCE_HOOK(
    IntellisenseHandlerUpdateAutoCompleteMessagesHook,
    ll::memory::HookPriority::Normal,
    IntellisenseHandler,
    &IntellisenseHandler::_updateAutoCompleteMessages,
    void,
    ::AutoCompleteInformation const& autocompleteInfo,
    ::CommandOrigin const&           cmdOrigin,
    ::CommandRegistry const&         registry,
    [[maybe_unused]] bool            isCheatsEnabled,
    bool                             isCommand
) {
    origin(autocompleteInfo, cmdOrigin, registry, true, isCommand);
}

// Hook updateIntellisense on client to guarantee all matching first-level commands and their localized descriptions
// are present in mIntellisenseMessages
LL_AUTO_TYPE_INSTANCE_HOOK(
    IntellisenseHandlerUpdateIntellisenseHook,
    ll::memory::HookPriority::Normal,
    IntellisenseHandler,
    &IntellisenseHandler::updateIntellisense,
    void,
    ::std::weak_ptr<::ClientInstanceScreenModel> minecraftScreenModel,
    ::std::string const&                         currentMessage
) {
    origin(minecraftScreenModel, currentMessage);

    // Only process command preview if the input starts with '/'
    if (!currentMessage.starts_with('/')) {
        this->mInitTabComplete = false;
        return;
    }

    std::string text = currentMessage.substr(1);

    // Only backfill first-level commands when typing the command name (no space yet)
    if (text.find(' ') != std::string::npos) {
        return;
    }

    // First-level commands are displayed exclusively through mIntellisenseMessages (with descriptions).
    // Clear mAutoCompleteMessages so the unannotated suggestion pills panel is not displayed.
    this->mAutoCompleteMessages.get().clear();
    this->mAutoCompleteGridSize = 0;

    auto toLower = [](std::string_view s) {
        std::string res;
        res.reserve(s.size());
        for (char c : s) res.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        return res;
    };

    std::string typedPrefix = toLower(text);

    // Collect all command names currently present in mIntellisenseMessages
    std::unordered_set<std::string> existingInPreview;
    for (auto const& msg : this->mIntellisenseMessages.get()) {
        auto        spacePos = msg.find(' ');
        std::string cmdName  = (spacePos != std::string::npos) ? msg.substr(0, spacePos) : msg;
        if (cmdName.starts_with('/')) cmdName = cmdName.substr(1);
        existingInPreview.insert(toLower(cmdName));
    }

    auto checkRegistry = [&](CommandRegistry& reg) {
        for (auto const& [name, sig] : reg.mSignatures) {
            if ((sig.flags.value & CommandFlagValue::Removed) == CommandFlagValue::Removed) continue;
            if (!sig.runnable && sig.overloads.empty()) continue;

            std::string lowerName = toLower(name);
            if (!lowerName.starts_with(typedPrefix)) continue;

            if (existingInPreview.insert(lowerName).second) {
                std::string desc = sig.description;
                if (!desc.empty()) {
                    try {
                        std::string localized = getI18n().get(desc, nullptr);
                        if (!localized.empty()) {
                            desc = localized;
                        }
                    } catch (...) {}
                }
                std::string line = name;
                if (!desc.empty()) {
                    line += " - " + desc;
                }
                this->mIntellisenseMessages.get().push_back(line);
            }
        }
    };

    if (auto clientReg = ll::service::getCommandRegistry(true); clientReg.has_value()) {
        checkRegistry(*clientReg);
    }
    if (auto serverReg = ll::service::getCommandRegistry(false); serverReg.has_value()) {
        checkRegistry(*serverReg);
    }

    // Sort mIntellisenseMessages so all matching commands appear in alphabetical order
    auto& msgs = this->mIntellisenseMessages.get();
    std::sort(msgs.begin(), msgs.end(), [&](std::string const& a, std::string const& b) {
        auto getCmd = [](std::string const& s) {
            auto pos = s.find(' ');
            return (pos != std::string::npos) ? s.substr(0, pos) : s;
        };
        std::string cmdA = getCmd(a);
        std::string cmdB = getCmd(b);
        if (cmdA.starts_with('/')) cmdA = cmdA.substr(1);
        if (cmdB.starts_with('/')) cmdB = cmdB.substr(1);
        std::string lA = toLower(cmdA);
        std::string lB = toLower(cmdB);
        bool matchA = lA.starts_with(typedPrefix);
        bool matchB = lB.starts_with(typedPrefix);
        if (matchA != matchB) return matchA;
        return lA < lB;
    });
}

// Hook handleTabComplete (overload 1) to execute first-level Tab autocomplete
LL_AUTO_TYPE_INSTANCE_HOOK(
    IntellisenseHandlerHandleTabCompleteHook,
    ll::memory::HookPriority::Normal,
    IntellisenseHandler,
    (void (IntellisenseHandler::*)(::std::weak_ptr<::ClientInstanceScreenModel>, ::std::string&, int))
        &IntellisenseHandler::handleTabComplete,
    void,
    ::std::weak_ptr<::ClientInstanceScreenModel> minecraftScreenModel,
    ::std::string&                               currentMessage,
    int                                          index
) {
    if (doFirstLevelTabComplete(*this, minecraftScreenModel, currentMessage, index)) {
        return;
    }
    origin(minecraftScreenModel, currentMessage, index);
}

// Hook handleTabComplete (overload 2) to execute first-level Tab autocomplete
LL_AUTO_TYPE_INSTANCE_HOOK(
    IntellisenseHandlerHandleTabCompleteWithOriginHook,
    ll::memory::HookPriority::Normal,
    IntellisenseHandler,
    (void (IntellisenseHandler::*)(::std::weak_ptr<::ClientInstanceScreenModel>, ::CommandOrigin const&, ::std::string&, int, bool))
        &IntellisenseHandler::handleTabComplete,
    void,
    ::std::weak_ptr<::ClientInstanceScreenModel> minecraftScreenModel,
    ::CommandOrigin const&                       cmdOrigin,
    ::std::string&                               currentMessage,
    int                                          index,
    bool                                         isSlashRequired
) {
    if (doFirstLevelTabComplete(*this, minecraftScreenModel, currentMessage, index)) {
        return;
    }
    origin(minecraftScreenModel, cmdOrigin, currentMessage, index, isSlashRequired);
}

// Hook originCanRunOverloadWithParam on client to permit overload autocomplete
LL_AUTO_TYPE_INSTANCE_HOOK(
    CommandRegistryOriginCanRunOverloadWithParamHook,
    ll::memory::HookPriority::Normal,
    CommandRegistry,
    &CommandRegistry::originCanRunOverloadWithParam,
    bool,
    [[maybe_unused]] ::CommandOrigin const&        cmdOrigin,
    [[maybe_unused]] ::CommandParameterData const& param
) {
    return true;
}

NoCheatTabComplete& NoCheatTabComplete::getInstance() {
    static NoCheatTabComplete instance;
    return instance;
}

bool NoCheatTabComplete::load() {
    getSelf().getLogger().info("Loading NoCheatTabComplete...");
    return true;
}

bool NoCheatTabComplete::enable() {
    getSelf().getLogger().info("Enabling NoCheatTabComplete...");

    // Fix signatures for server command registry (singleplayer) if already created
    if (auto registry = ll::service::getCommandRegistry(false); registry.has_value()) {
        fixSignatures(*registry);
    }

    // Register event listener for server command registration (singleplayer)
    mServerCommandRegisterListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerCommandRegisterEvent>(
        [](ll::event::ServerCommandRegisterEvent& ev) {
            fixSignatures(ev.commandRegistry());
        }
    );

    // Fix signatures for client command registry if already created
    if (auto clientRegistry = ll::service::getCommandRegistry(true); clientRegistry.has_value()) {
        fixSignatures(*clientRegistry);
    }

    // Register event listener for client command registration
    mClientCommandRegisterListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientCommandRegisterEvent>(
        [](ll::event::ClientCommandRegisterEvent& ev) {
            fixSignatures(ev.commandRegistry());
        }
    );

    return true;
}

bool NoCheatTabComplete::disable() {
    getSelf().getLogger().info("Disabling NoCheatTabComplete...");
    if (mServerCommandRegisterListener) {
        ll::event::EventBus::getInstance().removeListener(mServerCommandRegisterListener);
        mServerCommandRegisterListener.reset();
    }
    if (mClientCommandRegisterListener) {
        ll::event::EventBus::getInstance().removeListener(mClientCommandRegisterListener);
        mClientCommandRegisterListener.reset();
    }
    return true;
}

} // namespace no_cheat_tab_complete

LL_REGISTER_MOD(no_cheat_tab_complete::NoCheatTabComplete, no_cheat_tab_complete::NoCheatTabComplete::getInstance());
