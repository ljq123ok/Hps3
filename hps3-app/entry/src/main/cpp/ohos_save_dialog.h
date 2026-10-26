#pragma once
#include "Emu/Cell/Modules/cellSaveData.h"
#include "Emu/Cell/Modules/cellSysutil.h"
#include "Emu/RSX/Overlays/overlay_manager.h"
#include "Emu/RSX/Overlays/overlay_save_dialog.h"
#include "Emu/localized_string_id.h"
#include "Utilities/StrUtil.h"

// Use the core's pad-operated overlay; a frontend without Qt must still let the
// user choose an existing save or explicitly create a new entry.
class ohos_save_dialog final : public SaveDialogBase
{
public:
    s32 ShowSaveDataList(const std::string& base_dir, std::vector<SaveDataEntry>& entries,
        s32 focused, u32 op, vm::ptr<CellSaveDataListSet> list_set, bool enable_overlay) override
    {
        struct drawing_scope
        {
            bool active = sysutil_send_system_cmd(CELL_SYSUTIL_DRAWING_BEGIN, 0) >= 0;
            ~drawing_scope() { if (active) sysutil_send_system_cmd(CELL_SYSUTIL_DRAWING_END, 0); }
        } drawing;
        s32 selected = -2;
        if (auto manager = g_fxo->try_get<rsx::overlays::display_manager>())
            selected = manager->create<rsx::overlays::save_dialog>()->show(
                base_dir, entries, focused, op, list_set, enable_overlay);
        if (selected == rsx::overlays::user_interface::selection_code::error) selected = -2;
        HPS3_LOG("HPS3_SAVE_DIALOG selected=%{public}d entries=%{public}u", selected,
            static_cast<unsigned>(entries.size()));
        return selected;
    }
};

inline std::string hps3_localized_string(localized_string_id id, const char* args)
{
    using enum localized_string_id;
    std::string text;
    switch (id)
    {
    case RSX_OVERLAYS_MSG_DIALOG_YES: text = "Yes"; break;
    case RSX_OVERLAYS_MSG_DIALOG_NO: text = "No"; break;
    case RSX_OVERLAYS_MSG_DIALOG_CANCEL: text = "Cancel"; break;
    case RSX_OVERLAYS_MSG_DIALOG_OK: text = "OK"; break;
    case RSX_OVERLAYS_SAVE_DIALOG_TITLE: text = "Saved Data"; break;
    case RSX_OVERLAYS_SAVE_DIALOG_SAVE: text = "Save"; break;
    case RSX_OVERLAYS_SAVE_DIALOG_LOAD: text = "Load"; break;
    case RSX_OVERLAYS_SAVE_DIALOG_DELETE: text = "Delete"; break;
    case RSX_OVERLAYS_LIST_SELECT: text = "Select"; break;
    case RSX_OVERLAYS_LIST_CANCEL: text = "Cancel"; break;
    case RSX_OVERLAYS_LIST_DENY: text = "Back"; break;
    case CELL_SAVEDATA_NEW_SAVED_DATA_TITLE: text = "New Saved Data"; break;
    case CELL_SAVEDATA_NEW_SAVED_DATA_SUB_TITLE: text = "Select to create a new entry"; break;
    case CELL_SAVEDATA_NO_DATA: text = "There is no saved data."; break;
    case CELL_SAVEDATA_SAVE_CONFIRMATION: text = "Do you want to save this data?"; break;
    case CELL_SAVEDATA_OVERWRITE: text = "Overwrite the saved data?\n\n%0"; break;
    case CELL_SAVEDATA_LOAD: text = "Load this data?\n\n%0"; break;
    case CELL_SAVEDATA_SAVE: text = "Save this data?\n\n%0"; break;
    case CELL_SAVEDATA_DELETE_CONFIRMATION:
    case CELL_SAVEDATA_DELETE: text = "Delete this data?\n\n%0"; break;
    case CELL_SAVEDATA_DELETE_SUCCESS: text = "Successfully removed data.\n\n%0"; break;
    case CELL_SAVEDATA_AUTOSAVE: text = "Saving..."; break;
    case CELL_SAVEDATA_AUTOLOAD: text = "Loading..."; break;
    case CELL_SAVEDATA_CB_BROKEN: text = "Saved data is corrupted."; break;
    case CELL_SAVEDATA_CB_FAILURE: text = "Saved data operation failed."; break;
    case CELL_SAVEDATA_CB_NO_DATA: text = "Saved data cannot be found."; break;
    case CELL_SAVEDATA_CB_NO_SPACE: text = "Insufficient free space. Required: %0 KB"; break;
    case RSX_OVERLAYS_COMPILING_SHADERS: text = "Compiling shaders..."; break;
    case RSX_OVERLAYS_COMPILING_SHADERS_TITLE: text = "Compiling shaders"; break;
    case RSX_OVERLAYS_COMPILING_SHADERS_VULKAN: text = "Compiling Vulkan pipelines..."; break;
    case PROGRESS_DIALOG_OF: text = "of"; break;
    case PROGRESS_DIALOG_PLEASE_WAIT: text = "Please wait..."; break;
    case PROGRESS_DIALOG_DONE: text = "Done"; break;
    case RSX_OVERLAYS_SPINNER_NO_TEXT: return {};
    default: text = "System message"; break;
    }
    for (auto pos = text.find("%0"); pos != std::string::npos; pos = text.find("%0", pos))
    {
        const std::string value = args ? args : "";
        text.replace(pos, 2, value);
        pos += value.size();
    }
    return text;
}
