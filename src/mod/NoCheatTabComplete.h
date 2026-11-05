#pragma once

#include "ll/api/event/ListenerBase.h"
#include "ll/api/mod/NativeMod.h"

namespace no_cheat_tab_complete {

class NoCheatTabComplete {
public:
    static NoCheatTabComplete& getInstance();

    NoCheatTabComplete()  = default;
    ~NoCheatTabComplete() = default;

    NoCheatTabComplete(NoCheatTabComplete const&)            = delete;
    NoCheatTabComplete& operator=(NoCheatTabComplete const&) = delete;

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return *ll::mod::NativeMod::current(); }

    bool load();
    bool enable();
    bool disable();

private:
    ll::event::ListenerPtr mServerCommandRegisterListener;
    ll::event::ListenerPtr mClientCommandRegisterListener;
    ll::event::ListenerPtr mClientTickListener;
};

} // namespace no_cheat_tab_complete
