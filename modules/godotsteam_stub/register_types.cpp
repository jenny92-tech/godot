/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/
#include "register_types.h"

#include "steam_stub.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"

static Steam *steam_singleton = nullptr;

void initialize_godotsteam_stub_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }

    GDREGISTER_CLASS(Steam);
    steam_singleton = memnew(Steam);
    Engine::get_singleton()->register_singleton("Steam", steam_singleton);
}

void uninitialize_godotsteam_stub_module(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }

    Engine::get_singleton()->unregister_singleton("Steam");
    memdelete(steam_singleton);
    steam_singleton = nullptr;
}
