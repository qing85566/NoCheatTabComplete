#include "mod/NoCheatTabComplete.h"

#include <string>
#include <type_traits>

#include "ll/api/event/EventBus.h"
#include "ll/api/event/command/ClientCommandRegisterEvent.h"
#include "ll/api/event/command/ServerCommandRegisterEvent.h"
#include "ll/api/event/world/ClientLevelTickEvent.h"
#include "ll/api/memory/Hook.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/service/Bedrock.h"

#include "mc/network/packet/AvailableCommandsPacket.h"
#include "mc/server/commands/CommandFlag.h"
#include "mc/server/commands/CommandPermissionLevel.h"
#include "mc/server/commands/CommandRegistry.h"

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

} // namespace

// Hook registerCommand so all newly registered commands have NotCheat flag and Any permission
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
    requirement = CommandPermissionLevel::Any;
    origin(name, description, requirement, flag1, flag2);
}

// Hook loadRemoteCommands so remote server commands loaded into client registry have NotCheat flag and Any permission
LL_AUTO_TYPE_INSTANCE_HOOK(
    CommandRegistryLoadRemoteCommandsHook,
    ll::memory::HookPriority::Normal,
    CommandRegistry,
    &CommandRegistry::loadRemoteCommands,
    void,
    ::AvailableCommandsPacket const& packet
) {
    origin(packet);
    fixSignatures(this);
}

// Hook serializeAvailableCommands so packets sent across network mark commands with NotCheat flag and Any permission
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

    // Periodically ensure signatures remain fixed during client level tick
    mClientTickListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::ClientLevelTickEvent>(
        [](ll::event::ClientLevelTickEvent&) {
            if (auto reg = ll::service::getCommandRegistry(true); reg.has_value()) {
                fixSignatures(*reg);
            }
            if (auto reg = ll::service::getCommandRegistry(false); reg.has_value()) {
                fixSignatures(*reg);
            }
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
    if (mClientTickListener) {
        ll::event::EventBus::getInstance().removeListener(mClientTickListener);
        mClientTickListener.reset();
    }
    return true;
}

} // namespace no_cheat_tab_complete

LL_REGISTER_MOD(no_cheat_tab_complete::NoCheatTabComplete, no_cheat_tab_complete::NoCheatTabComplete::getInstance());
