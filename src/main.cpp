#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>
#include "extra/libs/qrcode/lv_qrcode.h"

#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

SPIClass touchSPI(VSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);
TFT_eSPI tft = TFT_eSPI();

static const uint32_t screenWidth  = 320;
static const uint32_t screenHeight = 240;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * 80];

// Global navigation pointers
static lv_obj_t *tv;
static int current_tile = 0;

// Global dot indicators
static lv_obj_t *dots[4];

// ==========================================
// Display & Touch Driver Callbacks
// ==========================================

void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)&color_p->full, w * h, false);
    tft.endWrite();
    lv_disp_flush_ready(disp);
}

void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
    static int consecutive_touches = 0;

    if (touchscreen.touched()) {
        TS_Point p = touchscreen.getPoint();

        int16_t x = map(p.x, 200, 3700, 0, screenWidth);
        int16_t y = map(p.y, 240, 3800, 0, screenHeight);

        if (x >= 0 && x < (int16_t)screenWidth && y >= 0 && y < (int16_t)screenHeight) {
            bool is_button_zone = (y > 165);
            int min_pressure    = is_button_zone ? 80  : 500;
            int req_consecutive = is_button_zone ? 1   : 3;

            if (p.z > min_pressure && p.z < 4000) {
                consecutive_touches++;
                if (consecutive_touches >= req_consecutive) {
                    data->state   = LV_INDEV_STATE_PR;
                    data->point.x = x;
                    data->point.y = y;
                    return;
                }
            } else {
                consecutive_touches = 0;
            }
        } else {
            consecutive_touches = 0;
        }
    } else {
        consecutive_touches = 0;
    }
    data->state = LV_INDEV_STATE_REL;
}

// ==========================================
// Style Helpers
// ==========================================

void apply_minimal_style(lv_obj_t *obj, lv_color_t bg_color, lv_color_t border_color, lv_coord_t border_width, lv_coord_t radius) {
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, bg_color, 0);
    lv_obj_set_style_border_color(obj, border_color, 0);
    lv_obj_set_style_border_width(obj, border_width, 0);
}

// ==========================================
// Page Indicator Dots
// ==========================================

void update_dots(int active) {
    for (int i = 0; i < 4; i++) {
        if (i == active) {
            // Active: filled white circle
            lv_obj_set_style_bg_color(dots[i], lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(dots[i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(dots[i], lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_border_width(dots[i], 0, 0);
            lv_obj_set_size(dots[i], 8, 8);
        } else {
            // Inactive: dim outline only
            lv_obj_set_style_bg_opa(dots[i], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(dots[i], lv_color_hex(0x52525B), 0);
            lv_obj_set_style_border_width(dots[i], 1, 0);
            lv_obj_set_size(dots[i], 6, 6);
        }
    }
}

// ==========================================
// Navigation
// ==========================================

static void tile_click_event_handler(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        lv_indev_t *indev = lv_indev_get_act();
        if (indev == NULL) return;

        lv_point_t p;
        lv_indev_get_point(indev, &p);

        // Expanded touch zones slightly inward (x < 70, x > 275) for comfortable physical swiping
        bool in_middle_y = (p.y >= 60 && p.y <= 180);

        if (in_middle_y && p.x > 275) {
            current_tile = (current_tile + 1) % 4;
            lv_obj_set_tile_id(tv, current_tile, 0, LV_ANIM_ON);
            update_dots(current_tile);
        } else if (in_middle_y && p.x < 70) {
            current_tile = (current_tile + 3) % 4;
            lv_obj_set_tile_id(tv, current_tile, 0, LV_ANIM_ON);
            update_dots(current_tile);
        }
    }
}

// ==========================================
// QR Modal
// ==========================================

static void close_modal_action(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        lv_obj_t *modal = (lv_obj_t *)lv_event_get_user_data(e);
        lv_obj_del(modal);
    }
}

static void qr_click_action(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        lv_event_stop_bubbling(e);

        const char *qr_url    = (const char *)lv_event_get_user_data(e);
        bool        is_portfolio = (qr_url == NULL);
        if (qr_url == NULL) qr_url = "https://zedizu.site";

        // Fullscreen overlay
        lv_obj_t *modal = lv_obj_create(lv_scr_act());
        lv_obj_set_size(modal, 320, 240);
        lv_obj_set_pos(modal, 0, 0);
        apply_minimal_style(modal, lv_color_hex(0x18181B), lv_color_hex(0x000000), 0, 0);
        lv_obj_set_style_pad_all(modal, 0, 0); // No padding so children align to absolute coordinates
        lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE); // Fix: prevent horizontal/vertical scrolling completely

        // Top accent line
        lv_obj_t *top_line = lv_obj_create(modal);
        lv_obj_set_size(top_line, 320, 2);
        lv_obj_set_pos(top_line, 0, 0);
        apply_minimal_style(top_line, lv_color_hex(0x3B82F6), lv_color_hex(0x3B82F6), 0, 0);
        lv_obj_set_style_pad_all(top_line, 0, 0);

        // Header label
        lv_obj_t *title = lv_label_create(modal);
        lv_label_set_text(title, is_portfolio ? "SCAN TO VIEW PORTFOLIO" : "SCAN TO VIEW PROJECT");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(0x9CA3AF), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

        // QR code — white on black for best contrast, downsized to 110 to avoid overlaps
        lv_obj_t *qr_large = lv_qrcode_create(modal, 110, lv_color_black(), lv_color_white());
        lv_qrcode_update(qr_large, qr_url, strlen(qr_url));
        lv_obj_align(qr_large, LV_ALIGN_CENTER, 0, -8);

        // URL label below QR
        lv_obj_t *url_label = lv_label_create(modal);
        lv_label_set_text(url_label, qr_url);
        lv_obj_set_style_text_font(url_label, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(url_label, lv_color_hex(0x52525B), 0);
        lv_obj_align(url_label, LV_ALIGN_BOTTOM_MID, 0, -50);

        // Close button
        lv_obj_t *close_btn = lv_btn_create(modal);
        lv_obj_set_size(close_btn, 110, 28);
        lv_obj_align(close_btn, LV_ALIGN_BOTTOM_MID, 0, -12);
        apply_minimal_style(close_btn, lv_color_hex(0x27272A), lv_color_hex(0x3B82F6), 1, 14);
        lv_obj_set_ext_click_area(close_btn, 15);

        lv_obj_t *close_label = lv_label_create(close_btn);
        lv_label_set_text(close_label, "CLOSE");
        lv_obj_set_style_text_font(close_label, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(close_label, lv_color_hex(0x3B82F6), 0);
        lv_obj_align(close_label, LV_ALIGN_CENTER, 0, 0);

        lv_obj_add_event_cb(close_btn, close_modal_action, LV_EVENT_CLICKED, modal);
    }
}

// ==========================================
// Animated Underline (name reveal on Tile 0)
// ==========================================

static void underline_anim_cb(void *obj, int32_t val) {
    lv_obj_set_width((lv_obj_t *)obj, val);
}

void start_underline_anim(lv_obj_t *line) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, line);
    lv_anim_set_exec_cb(&a, underline_anim_cb);
    lv_anim_set_values(&a, 0, 272);   // slides from 0 to card's inner content width
    lv_anim_set_time(&a, 600);         // 600ms — snappy but visible
    lv_anim_set_delay(&a, 300);        // slight pause after boot
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

// ==========================================
// Project Card Builder
// ==========================================

void create_project_card(
    lv_obj_t   *tile,
    const char *number_str,       // e.g. "01"
    const char *title_str,        // e.g. "QBike"
    const char *description,
    const char *stack_str,        // e.g. "Next.js + Firebase"
    const char *project_url,
    lv_color_t  accent_color
) {
    // Outer card
    lv_obj_t *card = lv_obj_create(tile);
    lv_obj_set_size(card, 304, 224);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    apply_minimal_style(card, lv_color_hex(0x27272A), lv_color_hex(0x000000), 0, 8);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Top accent bar (3px, per-card shade of blue)
    lv_obj_t *accent_bar = lv_obj_create(card);
    lv_obj_set_size(accent_bar, 272, 3);
    lv_obj_align(accent_bar, LV_ALIGN_TOP_LEFT, 0, 0);
    apply_minimal_style(accent_bar, accent_color, accent_color, 0, 2);
    lv_obj_set_style_pad_all(accent_bar, 0, 0);
    lv_obj_add_flag(accent_bar, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Project number — dim, small
    lv_obj_t *num_label = lv_label_create(card);
    lv_label_set_text(num_label, number_str);
    lv_obj_set_style_text_font(num_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(num_label, lv_color_hex(0x52525B), 0);
    lv_obj_align(num_label, LV_ALIGN_TOP_LEFT, 0, 12);
    lv_obj_add_flag(num_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Project title — large, accent colour
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, title_str);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, accent_color, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 24);
    lv_obj_add_flag(title, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Description
    lv_obj_t *desc = lv_label_create(card);
    lv_label_set_text(desc, description);
    lv_obj_set_style_text_font(desc, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(desc, lv_color_hex(0x9CA3AF), 0);
    lv_obj_set_width(desc, 272);
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(desc, LV_ALIGN_TOP_LEFT, 0, 58);
    lv_obj_add_flag(desc, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Stack badge — dim box, small label
    lv_obj_t *badge = lv_obj_create(card);
    lv_obj_set_height(badge, 20);
    lv_obj_set_width(badge, LV_SIZE_CONTENT);
    lv_obj_align(badge, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    apply_minimal_style(badge, lv_color_hex(0x18181B), accent_color, 1, 4);
    lv_obj_set_style_pad_hor(badge, 8, 0);
    lv_obj_set_style_pad_ver(badge, 3, 0);
    lv_obj_add_flag(badge, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *stack_label = lv_label_create(badge);
    lv_label_set_text(stack_label, stack_str);
    lv_obj_set_style_text_font(stack_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(stack_label, accent_color, 0);
    lv_obj_align(stack_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(stack_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    // View Project button — outline style, right aligned
    lv_obj_t *btn = lv_btn_create(card);
    lv_obj_set_size(btn, 120, 28);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    apply_minimal_style(btn, lv_color_hex(0x27272A), accent_color, 1, 6);
    lv_obj_set_ext_click_area(btn, 15);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "View Project");
    lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(btn_label, accent_color, 0);
    lv_obj_align(btn_label, LV_ALIGN_CENTER, 0, 0);

    lv_obj_add_event_cb(btn, qr_click_action, LV_EVENT_CLICKED, (void *)project_url);
}

// ==========================================
// Setup
// ==========================================

void setup() {
    Serial.begin(115200);

    tft.init();
    tft.setRotation(1);
    tft.invertDisplay(true);
    tft.fillScreen(TFT_BLACK);
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
    touchscreen.begin(touchSPI);
    touchscreen.setRotation(1);

    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * 80);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type      = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb   = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x18181B), 0);

    // ==========================================
    // Tileview
    // ==========================================
    tv = lv_tileview_create(lv_scr_act());
    lv_obj_set_size(tv, 320, 240);
    lv_obj_set_pos(tv, 0, 0);
    apply_minimal_style(tv, lv_color_hex(0x18181B), lv_color_hex(0x000000), 0, 0);
    lv_obj_set_style_pad_all(tv, 0, 0);
    lv_obj_clear_flag(tv, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_anim_time(tv, 200, 0);

    lv_obj_t *tile0 = lv_tileview_add_tile(tv, 0, 0, LV_DIR_NONE);
    lv_obj_t *tile1 = lv_tileview_add_tile(tv, 1, 0, LV_DIR_NONE);
    lv_obj_t *tile2 = lv_tileview_add_tile(tv, 2, 0, LV_DIR_NONE);
    lv_obj_t *tile3 = lv_tileview_add_tile(tv, 3, 0, LV_DIR_NONE);

    lv_obj_clear_flag(tile0, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tile1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tile2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tile3, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(tile0, tile_click_event_handler, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(tile1, tile_click_event_handler, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(tile2, tile_click_event_handler, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(tile3, tile_click_event_handler, LV_EVENT_CLICKED, NULL);

    // ==========================================
    // Page Indicator Dots (rendered on top of tileview)
    // ==========================================
    lv_obj_t *dot_row = lv_obj_create(lv_scr_act());
    lv_obj_set_size(dot_row, 60, 12);
    lv_obj_align(dot_row, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_opa(dot_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(dot_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(dot_row, 0, 0);
    lv_obj_clear_flag(dot_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(dot_row, 0, 0);
    lv_obj_set_flex_flow(dot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dot_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < 4; i++) {
        dots[i] = lv_obj_create(dot_row);
        lv_obj_set_style_radius(dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(dots[i], 0, 0);
        lv_obj_set_style_border_width(dots[i], 0, 0);
    }
    update_dots(0); // initialise with tile 0 active

    // ==========================================
    // Tile 0 — Main Identity Card
    // ==========================================
    lv_obj_t *card0 = lv_obj_create(tile0);
    lv_obj_set_size(card0, 304, 224);
    lv_obj_align(card0, LV_ALIGN_CENTER, 0, 0);
    apply_minimal_style(card0, lv_color_hex(0x27272A), lv_color_hex(0x000000), 0, 8);
    lv_obj_set_style_pad_all(card0, 16, 0);
    lv_obj_add_flag(card0, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(card0, LV_OBJ_FLAG_SCROLLABLE);

    // Top accent bar — animates in with the underline, leaving margins
    lv_obj_t *card0_accent = lv_obj_create(card0);
    lv_obj_set_size(card0_accent, 272, 2);
    lv_obj_align(card0_accent, LV_ALIGN_TOP_LEFT, 0, 0);
    apply_minimal_style(card0_accent, lv_color_hex(0x3B82F6), lv_color_hex(0x3B82F6), 0, 0);
    lv_obj_set_style_pad_all(card0_accent, 0, 0);
    lv_obj_add_flag(card0_accent, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Logo — </> in brighter white, .izu in blue
    lv_obj_t *logo_icon = lv_label_create(card0);
    lv_label_set_text(logo_icon, "</>");
    lv_obj_set_style_text_font(logo_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(logo_icon, lv_color_hex(0xE4E4E7), 0);
    lv_obj_align(logo_icon, LV_ALIGN_TOP_LEFT, 0, 10);
    lv_obj_add_flag(logo_icon, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *logo_text = lv_label_create(card0);
    lv_label_set_text(logo_text, "#3b82f6 zed#.izu");
    lv_label_set_recolor(logo_text, true);
    lv_obj_set_style_text_font(logo_text, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(logo_text, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(logo_text, LV_ALIGN_TOP_LEFT, 30, 10);
    lv_obj_add_flag(logo_text, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Name — large, white
    lv_obj_t *name_label = lv_label_create(card0);
    lv_label_set_text(name_label, "ZAID IZZUDDIN");
    lv_obj_set_style_text_font(name_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(name_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(name_label, LV_ALIGN_TOP_LEFT, 0, 60);
    lv_obj_add_flag(name_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Animated underline beneath name
    lv_obj_t *div_line = lv_obj_create(card0);
    lv_obj_set_size(div_line, 0, 1); // starts at 0, animated to 272
    lv_obj_align(div_line, LV_ALIGN_TOP_LEFT, 0, 89);
    apply_minimal_style(div_line, lv_color_hex(0x3B82F6), lv_color_hex(0x3B82F6), 0, 0);
    lv_obj_set_style_pad_all(div_line, 0, 0);
    lv_obj_add_flag(div_line, LV_OBJ_FLAG_EVENT_BUBBLE);
    start_underline_anim(div_line);

    // Title
    lv_obj_t *sub_label = lv_label_create(card0);
    lv_label_set_text(sub_label, "STUDENT DEVELOPER");
    lv_obj_set_style_text_font(sub_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sub_label, lv_color_hex(0x9CA3AF), 0);
    lv_obj_align(sub_label, LV_ALIGN_TOP_LEFT, 0, 96);
    lv_obj_add_flag(sub_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Phone
    lv_obj_t *phone_label = lv_label_create(card0);
    lv_label_set_text(phone_label, "+60 18-377 0754");
    lv_obj_set_style_text_font(phone_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(phone_label, lv_color_hex(0x9CA3AF), 0);
    lv_obj_align(phone_label, LV_ALIGN_BOTTOM_LEFT, 0, -42);
    lv_obj_add_flag(phone_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Email
    lv_obj_t *email_label = lv_label_create(card0);
    lv_label_set_text(email_label, "zaidizzuddin07@gmail.com");
    lv_obj_set_style_text_font(email_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(email_label, lv_color_hex(0x9CA3AF), 0);
    lv_obj_align(email_label, LV_ALIGN_BOTTOM_LEFT, 0, -22);
    lv_obj_add_flag(email_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Portfolio link row — tappable, opens QR
    lv_obj_t *link_group = lv_obj_create(card0);
    lv_obj_set_size(link_group, 150, 20);
    lv_obj_align(link_group, LV_ALIGN_BOTTOM_LEFT, 0, -2);
    lv_obj_set_style_bg_opa(link_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(link_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(link_group, 0, 0);
    lv_obj_clear_flag(link_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(link_group, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(link_group, 10);

    lv_obj_t *link_label = lv_label_create(link_group);
    lv_label_set_text(link_label, "zedizu.site");
    lv_obj_set_style_text_font(link_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(link_label, lv_color_hex(0x3B82F6), 0);
    lv_obj_set_style_text_decor(link_label, LV_TEXT_DECOR_UNDERLINE, 0);
    lv_obj_align(link_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_flag(link_label, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Custom-drawn "External Link" Vector Icon next to zedizu.site
    lv_obj_t *icon_conn = lv_obj_create(link_group);
    lv_obj_set_size(icon_conn, 14, 14);
    lv_obj_align_to(icon_conn, link_label, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
    apply_minimal_style(icon_conn, lv_color_hex(0x000000), lv_color_hex(0x000000), 0, 0);
    lv_obj_set_style_bg_opa(icon_conn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(icon_conn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(icon_conn, 0, 0);
    lv_obj_clear_flag(icon_conn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(icon_conn, LV_OBJ_FLAG_EVENT_BUBBLE);

    // 1. Box outline
    lv_obj_t *box = lv_obj_create(icon_conn);
    lv_obj_set_size(box, 9, 9);
    lv_obj_set_pos(box, 0, 5);
    apply_minimal_style(box, lv_color_hex(0x000000), lv_color_hex(0x3B82F6), 1, 1);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_add_flag(box, LV_OBJ_FLAG_EVENT_BUBBLE);

    // 2. Diagonal pointing line
    static lv_point_t diag_pts[] = {{3, 11}, {12, 2}};
    lv_obj_t * diag = lv_line_create(icon_conn);
    lv_line_set_points(diag, diag_pts, 2);
    lv_obj_set_style_line_color(diag, lv_color_hex(0x3B82F6), 0);
    lv_obj_set_style_line_width(diag, 1, 0);
    lv_obj_add_flag(diag, LV_OBJ_FLAG_EVENT_BUBBLE);

    // 3. Arrow head horizontal line
    static lv_point_t horiz_pts[] = {{8, 2}, {12, 2}};
    lv_obj_t * horiz = lv_line_create(icon_conn);
    lv_line_set_points(horiz, horiz_pts, 2);
    lv_obj_set_style_line_color(horiz, lv_color_hex(0x3B82F6), 0);
    lv_obj_set_style_line_width(horiz, 1, 0);
    lv_obj_add_flag(horiz, LV_OBJ_FLAG_EVENT_BUBBLE);

    // 4. Arrow head vertical line
    static lv_point_t vert_pts[] = {{12, 6}, {12, 2}};
    lv_obj_t * vert = lv_line_create(icon_conn);
    lv_line_set_points(vert, vert_pts, 2);
    lv_obj_set_style_line_color(vert, lv_color_hex(0x3B82F6), 0);
    lv_obj_set_style_line_width(vert, 1, 0);
    lv_obj_add_flag(vert, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_add_event_cb(link_group, qr_click_action, LV_EVENT_CLICKED, NULL); // NULL = portfolio

    // ==========================================
    // Tile 1 — QBike  (#3B82F6 base blue)
    // ==========================================
    create_project_card(
        tile1,
        "01",
        "QBike",
        "Built for KMJ students to book bicycles digitally - no more paper logbooks or lining up just to borrow a bike. Features real-time availability, QR-validated pickup & return, and a full admin dashboard with usage analytics and maintenance logs.",
        "Next.js + Firebase",
        "https://qbike-kmj.web.app",
        lv_color_hex(0x3B82F6)
    );

    // ==========================================
    // Tile 2 — SwitchOff!  (#60A5FA lighter blue)
    // ==========================================
    create_project_card(
        tile2,
        "02",
        "SwitchOff!",
        "A physical IoT device that flips any wall switch without rewiring. Uses a rack & pinion motor with radar, light, and sound sensors to detect empty rooms and flip the switch automatically. Built as a group FYP to solve KMJ's energy waste problem.",
        "ESP32 + Firebase",
        "https://github.com/urnword/S1G1-SwitchOff",
        lv_color_hex(0x60A5FA)
    );

    // ==========================================
    // Tile 3 — YouFIM  (#1D4ED8 deeper blue)
    // ==========================================
    create_project_card(
        tile3,
        "03",
        "YouFIM",
        "An AI financial advisor built to help students stop overspending. Powered by Google Gemini for conversational budgeting advice and personalized spending insights. Helped a friend's team build it - they walked away with Gold at IIEC 2026 at KMKN.",
        "Next.js + Genkit",
        "https://ai-chatbot-ff7fd.web.app/",
        lv_color_hex(0x1D4ED8)
    );
}

void loop() {
    lv_timer_handler();
    delay(5);
}