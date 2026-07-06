/**************************************************************************/
/*  steam_stub.h                                                          */
/**************************************************************************/
#ifndef GODOTSTEAM_STUB_STEAM_STUB_H
#define GODOTSTEAM_STUB_STEAM_STUB_H

#include "core/object/class_db.h"
#include "core/object/object.h"

class Steam : public Object {
    GDCLASS(Steam, Object);

protected:
    static void _bind_methods();

public:
    Steam() = default;
    ~Steam() = default;

    Variant stub_call(const Variant **p_args, int p_arg_count, Callable::CallError &r_error);

    Dictionary steamInitEx(int p_app_id = 0, bool p_embed_callbacks = false);
    bool steamInit(bool p_embed_callbacks = false);
    void run_callbacks();
    void runCallbacks();
    bool isSteamRunning();
    bool loggedOn();
    uint64_t getSteamID();
    String getPersonaName();
};

#endif // GODOTSTEAM_STUB_STEAM_STUB_H
