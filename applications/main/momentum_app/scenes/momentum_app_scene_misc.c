#include "../momentum_app.h"
#include "momentum_intro.h"

enum VarItemListIndex {
    VarItemListIndexScreen,
    VarItemListIndexDolphin,
    VarItemListIndexSpoof,
    VarItemListIndexVgm,
    VarItemListIndexShowMomentumIntro,
};

void momentum_app_scene_misc_var_item_list_callback(void* context, uint32_t index) {
    MomentumApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void momentum_app_scene_misc_on_enter(void* context) {
    MomentumApp* app = context;
    VariableItemList* var_item_list = app->var_item_list;
    VariableItem* item;

    item = variable_item_list_add(var_item_list, "Screen", 0, NULL, app);
    variable_item_set_current_value_text(item, ">");

    item = variable_item_list_add(var_item_list, "Dolphin", 0, NULL, app);
    variable_item_set_current_value_text(item, ">");

    item = variable_item_list_add(var_item_list, "Spoofing Options", 0, NULL, app);
    variable_item_set_current_value_text(item, ">");

    item = variable_item_list_add(var_item_list, "VGM Options", 0, NULL, app);
    variable_item_set_current_value_text(item, ">");

    variable_item_list_add(var_item_list, "Show Momentum Intro", 0, NULL, app);

    variable_item_list_set_enter_callback(
        var_item_list, momentum_app_scene_misc_var_item_list_callback, app);

    variable_item_list_set_selected_item(
        var_item_list, scene_manager_get_scene_state(app->scene_manager, MomentumAppSceneMisc));

    view_dispatcher_switch_to_view(app->view_dispatcher, MomentumAppViewVarItemList);
}

bool momentum_app_scene_misc_on_event(void* context, SceneManagerEvent event) {
    MomentumApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_set_scene_state(app->scene_manager, MomentumAppSceneMisc, event.event);
        consumed = true;
        switch(event.event) {
        case VarItemListIndexScreen:
            scene_manager_set_scene_state(app->scene_manager, MomentumAppSceneMiscScreen, 0);
            scene_manager_next_scene(app->scene_manager, MomentumAppSceneMiscScreen);
            break;
        case VarItemListIndexDolphin:
            scene_manager_set_scene_state(app->scene_manager, MomentumAppSceneMiscDolphin, 0);
            scene_manager_next_scene(app->scene_manager, MomentumAppSceneMiscDolphin);
            break;
        case VarItemListIndexSpoof:
            scene_manager_set_scene_state(app->scene_manager, MomentumAppSceneMiscSpoof, 0);
            scene_manager_next_scene(app->scene_manager, MomentumAppSceneMiscSpoof);
            break;
        case VarItemListIndexVgm:
            scene_manager_set_scene_state(app->scene_manager, MomentumAppSceneMiscVgm, 0);
            scene_manager_next_scene(app->scene_manager, MomentumAppSceneMiscVgm);
            break;
        case VarItemListIndexShowMomentumIntro: {
            /* Write the embedded intro slideshow to internal flash */
            File* f = storage_file_alloc(app->storage);
            if(storage_file_open(f, SLIDESHOW_FS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
                if(storage_file_write(f, momentum_intro_bin, momentum_intro_bin_size) ==
                   momentum_intro_bin_size) {
                    app->show_slideshow = true;
                }
            }
            storage_file_close(f);
            storage_file_free(f);
            if(app->show_slideshow) {
                momentum_app_apply(app);
            }
            break;
        }
        default:
            break;
        }
    }

    return consumed;
}

void momentum_app_scene_misc_on_exit(void* context) {
    MomentumApp* app = context;
    variable_item_list_reset(app->var_item_list);
}
