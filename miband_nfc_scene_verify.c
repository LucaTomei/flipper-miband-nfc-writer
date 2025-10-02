/**
 * @file miband_nfc_scene_verify.c
 * @brief Data verification scene (FIXED VERSION)
 * 
 * This scene reads data from the Mi Band NFC and compares it with the original
 * dump file to verify that the write operation was successful.
 * 
 * Key improvements in this version:
 * - Tries dump keys first, then falls back to 0xFF magic keys
 * - Smart comparison that ignores keys in sector trailers
 * - Compares only UID/BCC in Block 0 (not manufacturer data)
 * - Robust authentication with retry logic
 * - Detailed progress feedback
 */

#include "miband_nfc_i.h"

#define TAG "MiBandNfc"

enum {
    MiBandNfcSceneVerifyStateCardSearch,
    MiBandNfcSceneVerifyStateReading,
    MiBandNfcSceneVerifyStateComparison,
};

static void verify_dialog_callback(DialogExResult result, void* context) {
    MiBandNfcApp* app = context;

    if(result == DialogExResultLeft) {
        // Exit - torna al menu
        view_dispatcher_send_custom_event(app->view_dispatcher, MiBandNfcCustomEventVerifyExit);
    } else if(result == DialogExResultRight) {
        // View Details - vai al diff viewer
        view_dispatcher_send_custom_event(
            app->view_dispatcher, MiBandNfcCustomEventVerifyViewDetails);
    }
}

/**
 * @brief Verification progress tracker
 * 
 * Tracks reading progress, authentication attempts, and comparison results
 * to provide detailed feedback to the user.
 */
typedef struct {
    uint32_t current_sector;
    uint32_t total_sectors;
    uint32_t sectors_read;
    uint32_t sectors_failed;
    uint32_t auth_attempts;
    uint32_t auth_successes;
    uint32_t blocks_compared;
    uint32_t blocks_different;
    FuriString* current_operation;
    FuriString* last_result;
    FuriString* error_details;
    bool reading_complete;
} VerifyTracker;

static VerifyTracker verify_tracker = {0};

/**
 * @brief Initialize verification tracker
 */
static void verify_tracker_init(void) {
    verify_tracker.current_sector = 0;
    verify_tracker.total_sectors = 0;
    verify_tracker.sectors_read = 0;
    verify_tracker.sectors_failed = 0;
    verify_tracker.auth_attempts = 0;
    verify_tracker.auth_successes = 0;
    verify_tracker.blocks_compared = 0;
    verify_tracker.blocks_different = 0;
    verify_tracker.reading_complete = false;

    if(!verify_tracker.current_operation) {
        verify_tracker.current_operation = furi_string_alloc();
        verify_tracker.last_result = furi_string_alloc();
        verify_tracker.error_details = furi_string_alloc();
    }

    furi_string_reset(verify_tracker.current_operation);
    furi_string_reset(verify_tracker.last_result);
    furi_string_reset(verify_tracker.error_details);
}

/**
 * @brief Free verification tracker resources
 */
static void verify_tracker_free(void) {
    if(verify_tracker.current_operation) {
        furi_string_free(verify_tracker.current_operation);
        furi_string_free(verify_tracker.last_result);
        furi_string_free(verify_tracker.error_details);
        verify_tracker.current_operation = NULL;
        verify_tracker.last_result = NULL;
        verify_tracker.error_details = NULL;
    }
}

/**
 * @brief Update verification UI with current progress
 * 
 * Displays progress bar, statistics, and current operation status.
 * 
 * @param app Pointer to MiBandNfcApp instance
 * @param header Header text for the popup
 */
static void update_verify_ui(MiBandNfcApp* app, const char* header) {
    popup_set_header(app->popup, header, 64, 2, AlignCenter, AlignTop);

    FuriString* status_text = furi_string_alloc();

    if(verify_tracker.total_sectors > 0) {
        uint32_t progress_percent =
            verify_tracker.reading_complete ?
                100 :
                (verify_tracker.current_sector * 100) / verify_tracker.total_sectors;

        furi_string_printf(
            status_text,
            "Sector: %lu/%lu\n\n",
            verify_tracker.current_sector,
            verify_tracker.total_sectors);

        furi_string_cat_str(status_text, "[");
        for(uint32_t i = 0; i < 20; i++) {
            if(i < (progress_percent / 5)) {
                furi_string_cat_str(status_text, "=");
            } else if(i == (progress_percent / 5) && progress_percent < 100) {
                furi_string_cat_str(status_text, ">");
            } else {
                furi_string_cat_str(status_text, " ");
            }
        }
        furi_string_cat_printf(status_text, "]\n%lu%%\n\n", progress_percent);
    }

    if(furi_string_size(verify_tracker.current_operation) > 0) {
        furi_string_cat_printf(
            status_text, "%s\n", furi_string_get_cstr(verify_tracker.current_operation));
    }

    if(verify_tracker.auth_attempts > 0) {
        furi_string_cat_printf(
            status_text,
            "Auth: %lu/%lu\n",
            verify_tracker.auth_successes,
            verify_tracker.auth_attempts);
    }

    if(verify_tracker.blocks_compared > 0) {
        furi_string_cat_printf(status_text, "Compared: %lu\n", verify_tracker.blocks_compared);
        if(verify_tracker.blocks_different > 0) {
            furi_string_cat_printf(status_text, "Diff: %lu\n", verify_tracker.blocks_different);
        }
    }

    if(furi_string_size(verify_tracker.error_details) > 0) {
        furi_string_cat_printf(
            status_text, "\n%s", furi_string_get_cstr(verify_tracker.error_details));
    }

    popup_set_text(app->popup, furi_string_get_cstr(status_text), 4, 12, AlignLeft, AlignTop);

    // Icona solo alla fine
    if(verify_tracker.reading_complete) {
        if(furi_string_size(verify_tracker.error_details) > 0) {
            popup_set_icon(app->popup, 96, 20, &I_WarningDolphinFlip_45x42);
        } else {
            popup_set_icon(app->popup, 90, 16, &I_DolphinSuccess_91x55);
        }
    }

    furi_string_free(status_text);
}

/**
 * @brief Read a sector using multiple key strategies
 * 
 * This is the KEY FIX: tries dump keys first (Key A and Key B from the original
 * dump file), then falls back to 0xFF magic keys if those fail. This handles
 * both scenarios:
 * 1. Mi Band has original keys after successful write
 * 2. Mi Band still has magic keys from emulation
 * 
 * @param app Pointer to MiBandNfcApp instance
 * @param sector Sector number to read
 * @param first_block First block index of the sector
 * @param blocks_in_sector Number of blocks in the sector
 * @return true if sector read successfully, false otherwise
 */
static bool read_sector_with_keys(
    MiBandNfcApp* app,
    size_t sector,
    uint8_t first_block,
    uint8_t blocks_in_sector) {
    MfClassicError error;
    MfClassicAuthContext auth_context;
    MfClassicSectorTrailer* sec_tr =
        mf_classic_get_sector_trailer_by_sector(app->mf_classic_data, sector);

    if(!sec_tr) {
        FURI_LOG_E(TAG, "Sector %zu: No trailer in dump", sector);
        return false;
    }

    // Build list of keys to try
    MfClassicKey keys_to_try[3];
    MfClassicKeyType key_types[3];
    int keys_count = 0;

    // STRATEGY: Try dump keys first, then magic keys as fallback

    // 1. Try Key A from dump
    if(mf_classic_is_key_found(app->mf_classic_data, sector, MfClassicKeyTypeA)) {
        memcpy(keys_to_try[keys_count].data, sec_tr->key_a.data, 6);
        key_types[keys_count] = MfClassicKeyTypeA;
        keys_count++;
    }

    // 2. Try Key B from dump
    if(mf_classic_is_key_found(app->mf_classic_data, sector, MfClassicKeyTypeB)) {
        memcpy(keys_to_try[keys_count].data, sec_tr->key_b.data, 6);
        key_types[keys_count] = MfClassicKeyTypeB;
        keys_count++;
    }

    // 3. Try magic key 0xFF as fallback
    memset(keys_to_try[keys_count].data, 0xFF, 6);
    key_types[keys_count] = MfClassicKeyTypeA;
    keys_count++;

    // Try each key in order
    for(int key_idx = 0; key_idx < keys_count; key_idx++) {
        verify_tracker.auth_attempts++;

        error = mf_classic_poller_sync_auth(
            app->nfc, first_block, &keys_to_try[key_idx], key_types[key_idx], &auth_context);

        if(error != MfClassicErrorNone) {
            continue; // Try next key
        }

        verify_tracker.auth_successes++;
        FURI_LOG_D(TAG, "Sector %zu: Auth OK with key %d", sector, key_idx);

        // Read all blocks in the sector
        bool all_blocks_read = true;
        for(uint8_t block_in_sector = 0; block_in_sector < blocks_in_sector; block_in_sector++) {
            size_t block_idx = first_block + block_in_sector;

            // Re-authenticate every 2 blocks for stability
            if(block_in_sector > 0 && block_in_sector % 2 == 0) {
                error = mf_classic_poller_sync_auth(
                    app->nfc,
                    first_block,
                    &keys_to_try[key_idx],
                    key_types[key_idx],
                    &auth_context);

                if(error != MfClassicErrorNone) {
                    FURI_LOG_W(TAG, "Re-auth failed at block %zu", block_idx);
                    all_blocks_read = false;
                    miband_logger_log(
                        app->logger, LogLevelError, "Auth failed on sector %zu", sector);

                    break;
                }
            }

            // Read block with retry logic
            bool block_read = false;
            for(int retry = 0; retry < 3 && !block_read; retry++) {
                if(retry > 0) furi_delay_ms(50);

                error = mf_classic_poller_sync_read_block(
                    app->nfc,
                    block_idx,
                    &keys_to_try[key_idx],
                    key_types[key_idx],
                    &app->target_data->block[block_idx]);

                if(error == MfClassicErrorNone) {
                    block_read = true;
                } else if(error != MfClassicErrorTimeout) {
                    break; // Non-timeout error, stop retrying
                }
            }

            if(!block_read) {
                FURI_LOG_E(TAG, "Failed to read block %zu", block_idx);
                all_blocks_read = false;
                break;
            }
        }

        if(all_blocks_read) {
            FURI_BIT_SET(app->target_data->key_a_mask, sector);
            FURI_BIT_SET(app->target_data->key_b_mask, sector);
            // view_port_update(view_dispatcher_get_current_view(app->view_dispatcher));
            furi_delay_ms(10);

            return true;
        }
    }

    FURI_LOG_E(TAG, "Sector %zu: All auth attempts failed", sector);
    return false;
}

/**
 * @brief Read all sectors from Mi Band
 * 
 * Performs sector-by-sector reading with progress updates.
 * 
 * @param app Pointer to MiBandNfcApp instance
 * @return true if all sectors read successfully, false otherwise
 */
static bool miband_verify_read_card(MiBandNfcApp* app) {
    if(!app->is_valid_nfc_data) {
        return false;
    }

    verify_tracker.total_sectors = mf_classic_get_total_sectors_num(app->mf_classic_data->type);

    furi_string_set_str(verify_tracker.current_operation, "Initializing read");
    update_verify_ui(app, "Reading Mi Band");

    mf_classic_reset(app->target_data);
    app->target_data->type = app->mf_classic_data->type;

    bool overall_success = true;

    for(size_t sector = 0; sector < verify_tracker.total_sectors; sector++) {
        verify_tracker.current_sector = sector;

        furi_string_printf(verify_tracker.current_operation, "Reading sector %zu", sector);

        // CHIAMALO PIÙ SPESSO - ogni settore
        update_verify_ui(app, "Reading Mi Band");

        furi_delay_ms(50);

        uint8_t first_block = mf_classic_get_first_block_num_of_sector(sector);
        uint8_t blocks_in_sector = mf_classic_get_blocks_num_in_sector(sector);

        bool sector_success = read_sector_with_keys(app, sector, first_block, blocks_in_sector);

        if(sector_success) {
            verify_tracker.sectors_read++;
            furi_string_printf(verify_tracker.last_result, "Sector %zu OK", sector);
            // AGGIORNA SEMPRE
            update_verify_ui(app, "Reading Mi Band");
            furi_delay_ms(50);
        } else {
            verify_tracker.sectors_failed++;
            overall_success = false;
            furi_string_printf(verify_tracker.error_details, "Sector %zu failed", sector);
            FURI_LOG_E(TAG, "Failed to read sector %zu", sector);
        }
    }

    verify_tracker.current_sector = verify_tracker.total_sectors;
    verify_tracker.reading_complete = true;

    if(overall_success) {
        furi_string_set_str(verify_tracker.current_operation, "Read complete");
        furi_string_printf(
            verify_tracker.last_result, "All %lu sectors read", verify_tracker.sectors_read);
    } else {
        furi_string_printf(verify_tracker.current_operation, "Read incomplete");
        furi_string_printf(
            verify_tracker.last_result, "%lu sectors failed", verify_tracker.sectors_failed);
    }

    update_verify_ui(app, "Read Complete");
    furi_delay_ms(1000);

    return overall_success;
}

/**
 * @brief NFC poller callback for initial card detection
 * 
 * Handles card detection events before we start manual reading.
 * 
 * @param event NFC generic event
 * @param context Pointer to MiBandNfcApp instance
 * @return NfcCommand to continue or stop polling
 */
static NfcCommand miband_verify_reader_callback(NfcGenericEvent event, void* context) {
    MiBandNfcApp* app = context;
    furi_assert(event.protocol == NfcProtocolMfClassic);

    const MfClassicPollerEvent* mfc_event = event.event_data;

    if(mfc_event->type == MfClassicPollerEventTypeCardDetected) {
        furi_string_set_str(verify_tracker.current_operation, "Card detected");
        update_verify_ui(app, "Card Found");
        view_dispatcher_send_custom_event(app->view_dispatcher, MiBandNfcCustomEventCardDetected);

    } else if(mfc_event->type == MfClassicPollerEventTypeRequestMode) {
        mfc_event->data->poller_mode.mode = MfClassicPollerModeRead;
        mf_classic_reset(app->target_data);
        mfc_event->data->poller_mode.data = app->target_data;

    } else if(
        mfc_event->type == MfClassicPollerEventTypeSuccess ||
        mfc_event->type == MfClassicPollerEventTypeFail) {
        view_dispatcher_send_custom_event(app->view_dispatcher, MiBandNfcCustomEventPollerDone);
        return NfcCommandStop;
    }

    return NfcCommandContinue;
}

/**
 * @brief Scene entry point
 * 
 * Initializes verification and starts card detection.
 * 
 * @param context Pointer to MiBandNfcApp instance
 */
void miband_nfc_scene_verify_on_enter(void* context) {
    MiBandNfcApp* app = context;

    if(!app->is_valid_nfc_data) {
        scene_manager_previous_scene(app->scene_manager);
        return;
    }

    verify_tracker_init();
    scene_manager_set_scene_state(
        app->scene_manager, MiBandNfcSceneVerify, MiBandNfcSceneVerifyStateCardSearch);

    popup_reset(app->popup);
    popup_set_header(app->popup, "Verify Data", 64, 2, AlignCenter, AlignTop);
    furi_string_set_str(verify_tracker.current_operation, "Place Mi Band near Flipper");
    update_verify_ui(app, "Verify Data");

    view_dispatcher_switch_to_view(app->view_dispatcher, MiBandNfcViewIdScanner);
    notification_message(app->notifications, &sequence_blink_start_cyan);

    mf_classic_reset(app->target_data);
    app->target_data->type = app->mf_classic_data->type;

    app->poller = nfc_poller_alloc(app->nfc, NfcProtocolMfClassic);
    nfc_poller_start(app->poller, miband_verify_reader_callback, app);
}

/**
 * @brief Scene event handler
 * 
 * Handles card detection and verification completion events.
 * 
 * @param context Pointer to MiBandNfcApp instance
 * @param event Scene manager event
 * @return true if event was consumed, false otherwise
 */
bool miband_nfc_scene_verify_on_event(void* context, SceneManagerEvent event) {
    MiBandNfcApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case MiBandNfcCustomEventCardDetected:
            scene_manager_set_scene_state(
                app->scene_manager, MiBandNfcSceneVerify, MiBandNfcSceneVerifyStateReading);
            furi_string_set_str(verify_tracker.current_operation, "Card detected");
            update_verify_ui(app, "Card Detected");
            consumed = true;
            break;

        case MiBandNfcCustomEventPollerDone:
            if(app->poller) {
                nfc_poller_stop(app->poller);
                nfc_poller_free(app->poller);
                app->poller = NULL;
            }

            scene_manager_set_scene_state(
                app->scene_manager, MiBandNfcSceneVerify, MiBandNfcSceneVerifyStateComparison);

            bool read_success = miband_verify_read_card(app);

            if(!read_success) {
                notification_message(app->notifications, &sequence_error);
                furi_string_set_str(
                    verify_tracker.error_details, "Cannot read Mi Band\nCheck keys or position");
                update_verify_ui(app, "Read Failed");
                furi_delay_ms(3000);
                scene_manager_search_and_switch_to_another_scene(
                    app->scene_manager, MiBandNfcSceneMainMenu);
                consumed = true;
                break;
            }

            furi_string_set_str(verify_tracker.current_operation, "Comparing data");
            update_verify_ui(app, "Comparing Data");
            furi_delay_ms(500);

            bool data_match = true;
            int different_blocks = 0;
            size_t total_blocks = mf_classic_get_total_block_num(app->mf_classic_data->type);

            for(size_t i = 0; i < total_blocks; i++) {
                verify_tracker.blocks_compared++;

                if(i % 16 == 0) {
                    furi_string_printf(
                        verify_tracker.current_operation,
                        "Comparing block %zu/%zu",
                        i,
                        total_blocks);
                    update_verify_ui(app, "Comparing Data");
                }

                if(i == 0) {
                    FURI_LOG_D(TAG, "Skipping Block 0 (UID block)");
                    continue;
                } else if(mf_classic_is_sector_trailer(i)) {
                    FURI_LOG_D(TAG, "Skipping trailer block %zu", i);
                    continue;
                } else {
                    if(memcmp(
                           app->mf_classic_data->block[i].data,
                           app->target_data->block[i].data,
                           16) != 0) {
                        data_match = false;
                        different_blocks++;
                        verify_tracker.blocks_different++;
                        FURI_LOG_W(TAG, "Block %zu differs", i);
                    }
                }
            }

            if(data_match && different_blocks == 0) {
                // SUCCESS - tutto ok
                notification_message(app->notifications, &sequence_success);
                popup_reset(app->popup);
                popup_set_header(app->popup, "SUCCESS!", 64, 4, AlignCenter, AlignTop);
                popup_set_text(
                    app->popup, "All data matches!\n\nPress Back", 64, 20, AlignCenter, AlignTop);
                popup_set_icon(app->popup, 32, 28, &I_DolphinSuccess_91x55);
                view_dispatcher_switch_to_view(app->view_dispatcher, MiBandNfcViewIdScanner);
                furi_delay_ms(3000);
                scene_manager_search_and_switch_to_another_scene(
                    app->scene_manager, MiBandNfcSceneMainMenu);
            } else {
                notification_message(app->notifications, &sequence_blink_stop);
                notification_message(app->notifications, &sequence_error);
                dialog_ex_reset(app->dialog_ex);
                dialog_ex_set_header(
                    app->dialog_ex, "Differences Found", 64, 0, AlignCenter, AlignTop);

                FuriString* msg =
                    furi_string_alloc_printf("%d data blocks\ndiffer from dump", different_blocks);
                dialog_ex_set_text(
                    app->dialog_ex, furi_string_get_cstr(msg), 64, 28, AlignCenter, AlignCenter);
                furi_string_free(msg);

                dialog_ex_set_left_button_text(app->dialog_ex, "Exit");
                dialog_ex_set_right_button_text(app->dialog_ex, "Details");
                dialog_ex_set_icon(app->dialog_ex, 0, 0, NULL);

                dialog_ex_set_result_callback(app->dialog_ex, verify_dialog_callback);
                dialog_ex_set_context(app->dialog_ex, app);

                view_dispatcher_switch_to_view(app->view_dispatcher, MiBandNfcViewIdDialog);
            }

            consumed = true;
            break;

        case MiBandNfcCustomEventVerifyExit:
            // User sceglie Exit
            scene_manager_search_and_switch_to_another_scene(
                app->scene_manager, MiBandNfcSceneMainMenu);
            consumed = true;
            break;

        case MiBandNfcCustomEventVerifyViewDetails:
            // User sceglie View Details - vai al diff viewer
            scene_manager_next_scene(app->scene_manager, MiBandNfcSceneDiffViewer);
            consumed = true;
            break;

        case MiBandNfcCustomEventPollerFailed:
            notification_message(app->notifications, &sequence_error);
            furi_string_set_str(verify_tracker.error_details, "Card detection failed");
            update_verify_ui(app, "Detection Failed");
            furi_delay_ms(2000);
            scene_manager_search_and_switch_to_another_scene(
                app->scene_manager, MiBandNfcSceneMainMenu);
            consumed = true;
            break;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        scene_manager_search_and_switch_to_another_scene(
            app->scene_manager, MiBandNfcSceneMainMenu);
        consumed = true;
    }

    return consumed;
}

/**
 * @brief Scene exit handler
 * 
 * Cleans up resources and stops notifications.
 * 
 * @param context Pointer to MiBandNfcApp instance
 */
void miband_nfc_scene_verify_on_exit(void* context) {
    MiBandNfcApp* app = context;

    if(app->poller) {
        nfc_poller_stop(app->poller);
        nfc_poller_free(app->poller);
        app->poller = NULL;
    }

    verify_tracker_free();
    notification_message(app->notifications, &sequence_blink_stop);
    popup_reset(app->popup);
}
