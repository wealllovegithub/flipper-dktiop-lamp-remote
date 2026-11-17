/**
 * @file dktiop_lamp_remote.c
 * @brief Graphical Flipper Zero Sub-GHz remote app for DKTIOP desktop lamp (433.92 MHz, Princeton).
 */

#include <assets_icons.h>
#include <dolphin/dolphin.h>
#include <flipper_format/flipper_format.h>
#include <furi.h>
#include <furi_hal_subghz.h>
#include <gui/elements.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <lib/subghz/blocks/custom_btn.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/preset.h>
#include <lib/subghz/subghz_protocol_registry.h>
#include <lib/subghz/transmitter.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <stdio.h>
#include <string.h>

#define DKTIOP_FREQ_HZ    433920000U
#define DKTIOP_MIN_TX_MS  160U
#define DKTIOP_GUARD_TIME 30U
#define DKTIOP_BIT_LEN    24U
#define DKTIOP_REPEAT_MAX 200U

typedef enum {
    LampCmdPower = 0,
    LampCmdBright,
    LampCmdDim,
    LampCmdWarm,
    LampCmdCold,
    LampCmdCount,
} LampCommandId;

typedef struct {
    uint8_t code;
    uint32_t te;
    const char* tx_label;
} LampCommandDef;

static const LampCommandDef LAMP_COMMANDS[LampCmdCount] = {
    [LampCmdPower] = {.code = 0x45, .te = 403, .tx_label = "TX Power"},
    [LampCmdBright] = {.code = 0x40, .te = 403, .tx_label = "TX Bright"},
    [LampCmdDim] = {.code = 0x15, .te = 403, .tx_label = "TX Dim"},
    [LampCmdWarm] = {.code = 0x44, .te = 404, .tx_label = "TX Warm"},
    [LampCmdCold] = {.code = 0x43, .te = 404, .tx_label = "TX Cold"},
};

typedef enum {
    LampViewMain = 0,
} LampViewId;

typedef struct {
    bool ok_pressed;
    bool up_pressed;
    bool down_pressed;
    bool left_pressed;
    bool right_pressed;
    char status[20];
} LampViewModel;

typedef struct {
    Gui* gui;
    NotificationApp* notifications;
    ViewDispatcher* view_dispatcher;
    View* main_view;
    const SubGhzDevice* radio_device;
    SubGhzEnvironment* subghz_env;
    SubGhzTransmitter* transmitter;
    bool tx_active;
    uint32_t tx_start_tick;
} LampRemoteApp;

static void lamp_set_status(LampRemoteApp* app, const char* text) {
    with_view_model(
        app->main_view,
        LampViewModel * m,
        { snprintf(m->status, sizeof(m->status), "%s", text); },
        true);
}

static void lamp_stop_tx(LampRemoteApp* app) {
    if(!app->tx_active) return;

    uint32_t elapsed = furi_get_tick() - app->tx_start_tick;
    if(elapsed < DKTIOP_MIN_TX_MS) {
        furi_delay_ms(DKTIOP_MIN_TX_MS - elapsed);
    }

    subghz_devices_stop_async_tx(app->radio_device);
    if(app->transmitter) {
        subghz_transmitter_stop(app->transmitter);
        subghz_transmitter_free(app->transmitter);
        app->transmitter = NULL;
    }
    subghz_custom_btns_reset();
    subghz_devices_idle(app->radio_device);
    subghz_devices_sleep(app->radio_device);
    notification_message(app->notifications, &sequence_reset_blue);
    app->tx_active = false;
}

static bool lamp_start_tx(LampRemoteApp* app, LampCommandId cmd_id) {
    if(cmd_id >= LampCmdCount || !app->radio_device) return false;
    lamp_stop_tx(app);

    const LampCommandDef* cmd = &LAMP_COMMANDS[cmd_id];
    FlipperFormat* ff = flipper_format_string_alloc();
    uint32_t bit = DKTIOP_BIT_LEN;
    uint32_t te = cmd->te;
    uint32_t guard_time = DKTIOP_GUARD_TIME;
    uint32_t repeat = DKTIOP_REPEAT_MAX;
    uint8_t key[8] = {0, 0, 0, 0, 0, 0, 0xFF, cmd->code};

    bool ok = flipper_format_write_header_cstr(ff, "Flipper SubGhz Key File", 1) &&
              flipper_format_write_uint32(ff, "Bit", &bit, 1) &&
              flipper_format_write_hex(ff, "Key", key, sizeof(key)) &&
              flipper_format_write_uint32(ff, "TE", &te, 1) &&
              flipper_format_write_uint32(ff, "Guard_time", &guard_time, 1) &&
              flipper_format_write_uint32(ff, "Repeat", &repeat, 1) &&
              flipper_format_rewind(ff);

    if(ok) {
        /* Reset Momentum's global Princeton custom-button latch so each button keeps its own nibble. */
        subghz_custom_btns_reset();
        app->transmitter = subghz_transmitter_alloc_init(app->subghz_env, "Princeton");
        ok = (app->transmitter != NULL) &&
             (subghz_transmitter_deserialize(app->transmitter, ff) == SubGhzProtocolStatusOk);
    }
    flipper_format_free(ff);

    if(!ok) {
        if(app->transmitter) {
            subghz_transmitter_free(app->transmitter);
            app->transmitter = NULL;
        }
        lamp_set_status(app, "Encode Err");
        return false;
    }

    subghz_devices_reset(app->radio_device);
    subghz_devices_idle(app->radio_device);
    subghz_devices_load_preset(app->radio_device, FuriHalSubGhzPresetOok650Async, NULL);
    subghz_devices_set_frequency(app->radio_device, DKTIOP_FREQ_HZ);

    if(!subghz_devices_set_tx(app->radio_device)) {
        subghz_devices_idle(app->radio_device);
        subghz_devices_sleep(app->radio_device);
        subghz_transmitter_free(app->transmitter);
        app->transmitter = NULL;
        lamp_set_status(app, "TX Blocked");
        return false;
    }

    app->tx_start_tick = furi_get_tick();
    app->tx_active = subghz_devices_start_async_tx(
        app->radio_device, subghz_transmitter_yield, app->transmitter);
    if(!app->tx_active) {
        subghz_devices_idle(app->radio_device);
        subghz_devices_sleep(app->radio_device);
        subghz_transmitter_free(app->transmitter);
        app->transmitter = NULL;
        lamp_set_status(app, "TX Failed");
        return false;
    }

    notification_message(app->notifications, &sequence_set_only_blue_255);
    dolphin_deed(DolphinDeedSubGhzSend);
    lamp_set_status(app, cmd->tx_label);
    return true;
}

static uint32_t lamp_exit_callback(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static void lamp_draw_pill(
    Canvas* canvas,
    bool pressed,
    uint8_t x,
    uint8_t y,
    uint8_t w,
    uint8_t h,
    const char* label,
    CanvasDirection tri_dir,
    bool tri_after) {
    elements_slightly_rounded_frame(canvas, x, y, w, h);
    if(pressed) {
        elements_slightly_rounded_box(canvas, x + 1, y + 1, w - 2, h - 2);
        canvas_set_color(canvas, ColorWhite);
    }

    canvas_set_font(canvas, FontSecondary);
    uint8_t mid_y = y + (h / 2);
    uint8_t base_y = mid_y + 4;
    if(tri_after) {
        canvas_draw_str(canvas, x + 3, base_y, label);
        canvas_draw_triangle(canvas, x + w - 5, mid_y, 5, 3, tri_dir);
    } else if(w > 34) {
        canvas_draw_triangle(canvas, x + 8, mid_y, 5, 3, tri_dir);
        canvas_draw_str(canvas, x + 13, base_y, label);
    } else {
        canvas_draw_triangle(canvas, x + 4, mid_y, 5, 3, tri_dir);
        canvas_draw_str(canvas, x + 6, base_y, label);
    }
    canvas_set_color(canvas, ColorBlack);
}

static void lamp_draw_callback(Canvas* canvas, void* context) {
    LampViewModel* m = context;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 32, 2, AlignCenter, AlignTop, "DKTIOP");
    canvas_draw_line(canvas, 0, 13, 63, 13);

    /* Power card */
    elements_slightly_rounded_frame(canvas, 1, 16, 62, 26);
    if(m->ok_pressed) {
        elements_slightly_rounded_box(canvas, 2, 17, 60, 24);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_draw_icon(
        canvas, 4, 19, m->ok_pressed ? &I_power_hover_19x20 : &I_power_19x20);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 28, 28, "Power");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 28, 38, "[OK]");
    canvas_set_color(canvas, ColorBlack);

    /* Bright / Warm / Cold / Dim cross */
    lamp_draw_pill(
        canvas, m->up_pressed, 10, 45, 44, 19, "Bright", CanvasDirectionBottomToTop, false);
    lamp_draw_pill(
        canvas, m->left_pressed, 0, 67, 31, 20, "Warm", CanvasDirectionRightToLeft, false);
    lamp_draw_pill(
        canvas, m->right_pressed, 33, 67, 31, 20, "Cold", CanvasDirectionLeftToRight, true);
    lamp_draw_pill(
        canvas, m->down_pressed, 10, 90, 44, 19, "Dim", CanvasDirectionTopToBottom, false);

    /* Status and exit hint */
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 32, 117, AlignCenter, AlignBottom, m->status);
    canvas_draw_icon(canvas, 7, 119, &I_Pin_back_arrow_10x8);
    canvas_draw_str(canvas, 20, 127, "Back: Exit");
}

static bool lamp_input_callback(InputEvent* event, void* context) {
    LampRemoteApp* app = context;
    if(event->key == InputKeyBack) {
        return false;
    }

    LampCommandId cmd_to_start = LampCmdCount;
    bool should_stop = false;

    with_view_model(
        app->main_view,
        LampViewModel * m,
        {
            bool is_press = (event->type == InputTypePress);
            bool is_release = (event->type == InputTypeRelease);
            if(event->key == InputKeyOk) {
                if(is_press) {
                    m->ok_pressed = true;
                    cmd_to_start = LampCmdPower;
                } else if(is_release) {
                    m->ok_pressed = false;
                    should_stop = true;
                }
            } else if(event->key == InputKeyUp) {
                if(is_press) {
                    m->up_pressed = true;
                    cmd_to_start = LampCmdBright;
                } else if(is_release) {
                    m->up_pressed = false;
                    should_stop = true;
                }
            } else if(event->key == InputKeyDown) {
                if(is_press) {
                    m->down_pressed = true;
                    cmd_to_start = LampCmdDim;
                } else if(is_release) {
                    m->down_pressed = false;
                    should_stop = true;
                }
            } else if(event->key == InputKeyLeft) {
                if(is_press) {
                    m->left_pressed = true;
                    cmd_to_start = LampCmdWarm;
                } else if(is_release) {
                    m->left_pressed = false;
                    should_stop = true;
                }
            } else if(event->key == InputKeyRight) {
                if(is_press) {
                    m->right_pressed = true;
                    cmd_to_start = LampCmdCold;
                } else if(is_release) {
                    m->right_pressed = false;
                    should_stop = true;
                }
            }
            if(should_stop) {
                bool any_held = m->ok_pressed || m->up_pressed || m->down_pressed ||
                                m->left_pressed || m->right_pressed;
                if(any_held) should_stop = false;
            }
        },
        true);

    if(cmd_to_start < LampCmdCount) {
        lamp_start_tx(app, cmd_to_start);
    } else if(should_stop) {
        lamp_stop_tx(app);
        lamp_set_status(app, "433.92 MHz");
    }
    return true;
}

static LampRemoteApp* lamp_remote_alloc(void) {
    LampRemoteApp* app = malloc(sizeof(LampRemoteApp));
    memset(app, 0, sizeof(LampRemoteApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    subghz_devices_init();
    app->radio_device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
    if(app->radio_device) {
        subghz_devices_begin(app->radio_device);
        subghz_devices_sleep(app->radio_device);
    }

    app->subghz_env = subghz_environment_alloc();
    subghz_environment_set_protocol_registry(app->subghz_env, (void*)&subghz_protocol_registry);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->main_view = view_alloc();
    view_set_context(app->main_view, app);
    view_set_orientation(app->main_view, ViewOrientationVertical);
    view_allocate_model(app->main_view, ViewModelTypeLocking, sizeof(LampViewModel));
    with_view_model(
        app->main_view,
        LampViewModel * m,
        { snprintf(m->status, sizeof(m->status), "433.92 MHz"); },
        true);
    view_set_draw_callback(app->main_view, lamp_draw_callback);
    view_set_input_callback(app->main_view, lamp_input_callback);
    view_set_previous_callback(app->main_view, lamp_exit_callback);
    view_dispatcher_add_view(app->view_dispatcher, LampViewMain, app->main_view);

    view_dispatcher_switch_to_view(app->view_dispatcher, LampViewMain);
    return app;
}

static void lamp_remote_free(LampRemoteApp* app) {
    lamp_stop_tx(app);

    view_dispatcher_remove_view(app->view_dispatcher, LampViewMain);
    view_free(app->main_view);
    view_dispatcher_free(app->view_dispatcher);

    if(app->radio_device) {
        subghz_devices_end(app->radio_device);
    }
    subghz_devices_deinit();
    subghz_environment_free(app->subghz_env);

    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    free(app);
}

int32_t dktiop_lamp_remote_app(void* p) {
    UNUSED(p);
    LampRemoteApp* app = lamp_remote_alloc();
    view_dispatcher_run(app->view_dispatcher);
    lamp_remote_free(app);
    return 0;
}
