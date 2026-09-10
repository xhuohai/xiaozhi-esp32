// AI Usage 页面：同一屏显示 GPT + Cursor 额度
//
// 400x300 黑白单色 RLCD。文案只用 ASCII，适配 alibaba_puhui 字库范围。

#include "custom_lcd_display.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <esp_log.h>

#include "managers/ai_usage_manager.h"

LV_FONT_DECLARE(alibaba_puhui_16);
LV_FONT_DECLARE(alibaba_puhui_24);

static const char* TAG = "AiUsageUI";

static const char* kMonths[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static void FormatPlanName(const char* raw, char* out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!raw || raw[0] == '\0') {
        return;
    }

    size_t n = 0;
    bool new_word = true;
    for (const char* p = raw; *p && n + 1 < out_size; ++p) {
        char ch = *p;
        if (ch == '_' || ch == '-') {
            if (n + 1 < out_size) {
                out[n++] = ' ';
            }
            new_word = true;
            continue;
        }
        if (new_word && ch >= 'a' && ch <= 'z') {
            ch = static_cast<char>(ch - 'a' + 'A');
        }
        out[n++] = ch;
        new_word = false;
    }
    out[n] = '\0';
}

static void FormatResetTime(time_t reset_at, int32_t window_seconds, char* out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    if (reset_at <= 0) {
        snprintf(out, out_size, "Reset --");
        return;
    }

    struct tm local_tm = {};
    localtime_r(&reset_at, &local_tm);
    const bool short_window = window_seconds > 0 && window_seconds < 86400;
    if (short_window) {
        snprintf(out, out_size, "Reset %02d:%02d", local_tm.tm_hour, local_tm.tm_min);
        return;
    }

    const char* month = (local_tm.tm_mon >= 0 && local_tm.tm_mon < 12)
        ? kMonths[local_tm.tm_mon] : "---";
    snprintf(out, out_size, "Reset %s %d %02d:%02d",
             month, local_tm.tm_mday, local_tm.tm_hour, local_tm.tm_min);
}

static float ClampPercent(float percent, bool limit_reached) {
    if (limit_reached) {
        return 100.0f;
    }
    return std::clamp(percent, 0.0f, 100.0f);
}

static void StyleUsageBar(lv_obj_t* bar) {
    lv_bar_set_range(bar, 0, 1000);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_white(), 0);
    lv_obj_set_style_radius(bar, 4, 0);
    lv_obj_set_style_pad_all(bar, 1, 0);
    lv_obj_set_style_bg_color(bar, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
}

static void SetBarPercent(lv_obj_t* bar, lv_obj_t* pct_label, float percent, bool limit_reached) {
    float clamped = ClampPercent(percent, limit_reached);
    if (bar) {
        lv_bar_set_value(bar, static_cast<int32_t>(clamped * 10.0f), LV_ANIM_OFF);
    }
    if (pct_label) {
        if (limit_reached) {
            lv_label_set_text(pct_label, "FULL");
        } else {
            char buf[12];
            snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(clamped + 0.5f));
            lv_label_set_text(pct_label, buf);
        }
    }
}

static const AiUsageWindow* FindWindow(const GptUsage& gpt, const char* id) {
    for (uint8_t i = 0; i < gpt.window_count; ++i) {
        if (strcmp(gpt.windows[i].id, id) == 0) {
            return &gpt.windows[i];
        }
    }
    return nullptr;
}

static void SetHidden(lv_obj_t* obj, bool hidden) {
    if (!obj) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void CustomLcdDisplay::SetupAiUsageUI() {
    DisplayLockGuard lock(this);

    lv_obj_t* root = lv_screen_active();
    const lv_font_t* font_title = &alibaba_puhui_24;
    const lv_font_t* font_body = &alibaba_puhui_16;

    const int SCR_W = 400;
    const int SCR_H = 300;
    const int PAD = 12;

    ai_usage_page_ = lv_obj_create(root);
    lv_obj_set_size(ai_usage_page_, SCR_W, SCR_H);
    lv_obj_set_pos(ai_usage_page_, 0, 0);
    lv_obj_set_style_bg_color(ai_usage_page_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ai_usage_page_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ai_usage_page_, 0, 0);
    lv_obj_set_style_pad_all(ai_usage_page_, 0, 0);
    lv_obj_set_style_radius(ai_usage_page_, 0, 0);
    lv_obj_remove_flag(ai_usage_page_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ai_usage_page_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* page = ai_usage_page_;

    ai_usage_title_label_ = lv_label_create(page);
    lv_obj_set_style_text_font(ai_usage_title_label_, font_title, 0);
    lv_obj_set_style_text_color(ai_usage_title_label_, lv_color_white(), 0);
    lv_obj_align(ai_usage_title_label_, LV_ALIGN_TOP_LEFT, PAD, 8);
    lv_label_set_text(ai_usage_title_label_, "AI Usage");

    ai_usage_refresh_label_ = lv_label_create(page);
    lv_obj_set_style_text_font(ai_usage_refresh_label_, font_body, 0);
    lv_obj_set_style_text_color(ai_usage_refresh_label_, lv_color_white(), 0);
    lv_obj_align(ai_usage_refresh_label_, LV_ALIGN_TOP_RIGHT, -PAD, 12);
    lv_label_set_text(ai_usage_refresh_label_, "");

    ai_usage_gpt_name_label_ = lv_label_create(page);
    lv_obj_set_style_text_font(ai_usage_gpt_name_label_, font_body, 0);
    lv_obj_set_style_text_color(ai_usage_gpt_name_label_, lv_color_white(), 0);
    lv_obj_align(ai_usage_gpt_name_label_, LV_ALIGN_TOP_LEFT, PAD, 42);
    lv_label_set_text(ai_usage_gpt_name_label_, "GPT");

    ai_usage_gpt_plan_label_ = lv_label_create(page);
    lv_obj_set_style_text_font(ai_usage_gpt_plan_label_, font_body, 0);
    lv_obj_set_style_text_color(ai_usage_gpt_plan_label_, lv_color_white(), 0);
    lv_obj_align(ai_usage_gpt_plan_label_, LV_ALIGN_TOP_RIGHT, -PAD, 42);
    lv_label_set_text(ai_usage_gpt_plan_label_, "");

    const int bar_x = 64;
    const int bar_w = 230;
    const int pct_x = 308;
    const int row0_y = 68;
    const int row1_y = 108;

    const char* gpt_ids[2] = {"5H", "7D"};
    const int gpt_ys[2] = {row0_y, row1_y};
    for (int i = 0; i < 2; ++i) {
        ai_usage_gpt_w_id_[i] = lv_label_create(page);
        lv_obj_set_style_text_font(ai_usage_gpt_w_id_[i], font_body, 0);
        lv_obj_set_style_text_color(ai_usage_gpt_w_id_[i], lv_color_white(), 0);
        lv_obj_set_pos(ai_usage_gpt_w_id_[i], PAD, gpt_ys[i]);
        lv_label_set_text(ai_usage_gpt_w_id_[i], gpt_ids[i]);

        ai_usage_gpt_w_bar_[i] = lv_bar_create(page);
        lv_obj_set_size(ai_usage_gpt_w_bar_[i], bar_w, 14);
        lv_obj_set_pos(ai_usage_gpt_w_bar_[i], bar_x, gpt_ys[i] + 2);
        StyleUsageBar(ai_usage_gpt_w_bar_[i]);

        ai_usage_gpt_w_pct_[i] = lv_label_create(page);
        lv_obj_set_style_text_font(ai_usage_gpt_w_pct_[i], font_body, 0);
        lv_obj_set_style_text_color(ai_usage_gpt_w_pct_[i], lv_color_white(), 0);
        lv_obj_set_pos(ai_usage_gpt_w_pct_[i], pct_x, gpt_ys[i]);
        lv_label_set_text(ai_usage_gpt_w_pct_[i], "--%");

        ai_usage_gpt_w_reset_[i] = lv_label_create(page);
        lv_obj_set_style_text_font(ai_usage_gpt_w_reset_[i], font_body, 0);
        lv_obj_set_style_text_color(ai_usage_gpt_w_reset_[i], lv_color_white(), 0);
        lv_obj_set_style_text_opa(ai_usage_gpt_w_reset_[i], LV_OPA_70, 0);
        lv_obj_set_pos(ai_usage_gpt_w_reset_[i], bar_x, gpt_ys[i] + 18);
        lv_label_set_text(ai_usage_gpt_w_reset_[i], "Reset --");
    }

    lv_obj_t* divider = lv_obj_create(page);
    lv_obj_set_size(divider, SCR_W - PAD * 2, 1);
    lv_obj_set_pos(divider, PAD, 150);
    lv_obj_set_style_bg_color(divider, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_radius(divider, 0, 0);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);

    ai_usage_cursor_name_label_ = lv_label_create(page);
    lv_obj_set_style_text_font(ai_usage_cursor_name_label_, font_body, 0);
    lv_obj_set_style_text_color(ai_usage_cursor_name_label_, lv_color_white(), 0);
    lv_obj_align(ai_usage_cursor_name_label_, LV_ALIGN_TOP_LEFT, PAD, 160);
    lv_label_set_text(ai_usage_cursor_name_label_, "Cursor");

    ai_usage_cursor_plan_label_ = lv_label_create(page);
    lv_obj_set_style_text_font(ai_usage_cursor_plan_label_, font_body, 0);
    lv_obj_set_style_text_color(ai_usage_cursor_plan_label_, lv_color_white(), 0);
    lv_obj_align(ai_usage_cursor_plan_label_, LV_ALIGN_TOP_RIGHT, -PAD, 160);
    lv_label_set_text(ai_usage_cursor_plan_label_, "");

    ai_usage_cursor_total_id_ = lv_label_create(page);
    lv_obj_set_style_text_font(ai_usage_cursor_total_id_, font_body, 0);
    lv_obj_set_style_text_color(ai_usage_cursor_total_id_, lv_color_white(), 0);
    lv_obj_set_pos(ai_usage_cursor_total_id_, PAD, 186);
    lv_label_set_text(ai_usage_cursor_total_id_, "Total");

    ai_usage_cursor_total_bar_ = lv_bar_create(page);
    lv_obj_set_size(ai_usage_cursor_total_bar_, bar_w, 14);
    lv_obj_set_pos(ai_usage_cursor_total_bar_, bar_x, 188);
    StyleUsageBar(ai_usage_cursor_total_bar_);

    ai_usage_cursor_total_pct_ = lv_label_create(page);
    lv_obj_set_style_text_font(ai_usage_cursor_total_pct_, font_body, 0);
    lv_obj_set_style_text_color(ai_usage_cursor_total_pct_, lv_color_white(), 0);
    lv_obj_set_pos(ai_usage_cursor_total_pct_, pct_x, 186);
    lv_label_set_text(ai_usage_cursor_total_pct_, "--%");

    ai_usage_cursor_api_id_ = lv_label_create(page);
    lv_obj_set_style_text_font(ai_usage_cursor_api_id_, font_body, 0);
    lv_obj_set_style_text_color(ai_usage_cursor_api_id_, lv_color_white(), 0);
    lv_obj_set_pos(ai_usage_cursor_api_id_, PAD, 214);
    lv_label_set_text(ai_usage_cursor_api_id_, "API");

    ai_usage_cursor_api_bar_ = lv_bar_create(page);
    lv_obj_set_size(ai_usage_cursor_api_bar_, bar_w, 14);
    lv_obj_set_pos(ai_usage_cursor_api_bar_, bar_x, 216);
    StyleUsageBar(ai_usage_cursor_api_bar_);

    ai_usage_cursor_api_pct_ = lv_label_create(page);
    lv_obj_set_style_text_font(ai_usage_cursor_api_pct_, font_body, 0);
    lv_obj_set_style_text_color(ai_usage_cursor_api_pct_, lv_color_white(), 0);
    lv_obj_set_pos(ai_usage_cursor_api_pct_, pct_x, 214);
    lv_label_set_text(ai_usage_cursor_api_pct_, "--%");

    ai_usage_cursor_reset_label_ = lv_label_create(page);
    lv_obj_set_style_text_font(ai_usage_cursor_reset_label_, font_body, 0);
    lv_obj_set_style_text_color(ai_usage_cursor_reset_label_, lv_color_white(), 0);
    lv_obj_set_style_text_opa(ai_usage_cursor_reset_label_, LV_OPA_70, 0);
    lv_obj_set_pos(ai_usage_cursor_reset_label_, bar_x, 234);
    lv_label_set_text(ai_usage_cursor_reset_label_, "Reset --");

    ai_usage_cursor_ondemand_label_ = lv_label_create(page);
    lv_obj_set_style_text_font(ai_usage_cursor_ondemand_label_, font_body, 0);
    lv_obj_set_style_text_color(ai_usage_cursor_ondemand_label_, lv_color_white(), 0);
    lv_obj_set_style_text_opa(ai_usage_cursor_ondemand_label_, LV_OPA_70, 0);
    lv_obj_set_pos(ai_usage_cursor_ondemand_label_, PAD, 258);
    lv_label_set_text(ai_usage_cursor_ondemand_label_, "");
    lv_obj_add_flag(ai_usage_cursor_ondemand_label_, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "AI Usage page created");
}

void CustomLcdDisplay::UpdateAiUsageDisplay(bool force) {
    auto& manager = AiUsageManager::GetInstance();
    const bool refreshing = manager.IsRefreshing();
    const uint32_t revision = manager.Revision();
    if (!force && revision == last_ai_usage_revision_ && refreshing == last_ai_usage_refreshing_) {
        return;
    }
    last_ai_usage_revision_ = revision;
    last_ai_usage_refreshing_ = refreshing;

    AiUsageSnapshot snap = manager.GetSnapshot();
    DisplayLockGuard lock(this);

    if (ai_usage_refresh_label_) {
        lv_label_set_text(ai_usage_refresh_label_, refreshing ? "*" : "");
    }

    const bool gpt_has_data = (snap.gpt.status == UsageStatus::kOk ||
                               snap.gpt.status == UsageStatus::kStale);
    char gpt_plan[24] = {};
    if (gpt_has_data) {
        FormatPlanName(snap.gpt.plan, gpt_plan, sizeof(gpt_plan));
        if (snap.gpt.status == UsageStatus::kStale) {
            char with_stale[32];
            snprintf(with_stale, sizeof(with_stale), "%s stale", gpt_plan[0] ? gpt_plan : "GPT");
            if (ai_usage_gpt_plan_label_) {
                lv_label_set_text(ai_usage_gpt_plan_label_, with_stale);
            }
        } else if (ai_usage_gpt_plan_label_) {
            lv_label_set_text(ai_usage_gpt_plan_label_, gpt_plan);
        }
    } else if (ai_usage_gpt_plan_label_) {
        lv_label_set_text(ai_usage_gpt_plan_label_, "Unavailable");
    }

    const AiUsageWindow* win5h = FindWindow(snap.gpt, "5h");
    const AiUsageWindow* win7d = FindWindow(snap.gpt, "7d");
    const AiUsageWindow* gpt_rows[2] = {win5h, win7d};
    if (!win5h && !win7d && snap.gpt.window_count > 0) {
        gpt_rows[0] = &snap.gpt.windows[0];
        gpt_rows[1] = snap.gpt.window_count > 1 ? &snap.gpt.windows[1] : nullptr;
    }

    for (int i = 0; i < 2; ++i) {
        const bool show = gpt_has_data && gpt_rows[i] != nullptr;
        SetHidden(ai_usage_gpt_w_id_[i], !show);
        SetHidden(ai_usage_gpt_w_bar_[i], !show);
        SetHidden(ai_usage_gpt_w_pct_[i], !show);
        SetHidden(ai_usage_gpt_w_reset_[i], !show);
        if (!show) {
            continue;
        }
        char id_buf[8];
        snprintf(id_buf, sizeof(id_buf), "%s", gpt_rows[i]->id);
        for (char* p = id_buf; *p; ++p) {
            if (*p >= 'a' && *p <= 'z') {
                *p = static_cast<char>(*p - 'a' + 'A');
            }
        }
        if (ai_usage_gpt_w_id_[i]) {
            lv_label_set_text(ai_usage_gpt_w_id_[i], id_buf);
        }
        SetBarPercent(ai_usage_gpt_w_bar_[i], ai_usage_gpt_w_pct_[i],
                      gpt_rows[i]->used_percent, snap.gpt.limit_reached);
        char reset_buf[32];
        FormatResetTime(gpt_rows[i]->reset_at, gpt_rows[i]->window_seconds,
                        reset_buf, sizeof(reset_buf));
        if (ai_usage_gpt_w_reset_[i]) {
            lv_label_set_text(ai_usage_gpt_w_reset_[i], reset_buf);
        }
    }

    const bool cursor_has_data = (snap.cursor.status == UsageStatus::kOk ||
                                  snap.cursor.status == UsageStatus::kStale);
    char cursor_plan[24] = {};
    if (cursor_has_data) {
        FormatPlanName(snap.cursor.plan, cursor_plan, sizeof(cursor_plan));
        if (snap.cursor.status == UsageStatus::kStale) {
            char with_stale[32];
            snprintf(with_stale, sizeof(with_stale), "%s stale",
                     cursor_plan[0] ? cursor_plan : "Cursor");
            if (ai_usage_cursor_plan_label_) {
                lv_label_set_text(ai_usage_cursor_plan_label_, with_stale);
            }
        } else if (ai_usage_cursor_plan_label_) {
            lv_label_set_text(ai_usage_cursor_plan_label_, cursor_plan);
        }
    } else if (ai_usage_cursor_plan_label_) {
        lv_label_set_text(ai_usage_cursor_plan_label_, "Unavailable");
    }

    SetHidden(ai_usage_cursor_total_id_, !cursor_has_data);
    SetHidden(ai_usage_cursor_total_bar_, !cursor_has_data);
    SetHidden(ai_usage_cursor_total_pct_, !cursor_has_data);
    SetHidden(ai_usage_cursor_api_id_, !cursor_has_data);
    SetHidden(ai_usage_cursor_api_bar_, !cursor_has_data);
    SetHidden(ai_usage_cursor_api_pct_, !cursor_has_data);
    SetHidden(ai_usage_cursor_reset_label_, !cursor_has_data);

    if (cursor_has_data) {
        SetBarPercent(ai_usage_cursor_total_bar_, ai_usage_cursor_total_pct_,
                      snap.cursor.total_used_percent, false);
        SetBarPercent(ai_usage_cursor_api_bar_, ai_usage_cursor_api_pct_,
                      snap.cursor.api_used_percent, false);
        char reset_buf[32];
        FormatResetTime(snap.cursor.cycle_end, 86400, reset_buf, sizeof(reset_buf));
        if (ai_usage_cursor_reset_label_) {
            lv_label_set_text(ai_usage_cursor_reset_label_, reset_buf);
        }
    }

    if (cursor_has_data && snap.cursor.on_demand_enabled) {
        char od_buf[40];
        snprintf(od_buf, sizeof(od_buf), "On-demand %.0f", snap.cursor.on_demand_used);
        if (ai_usage_cursor_ondemand_label_) {
            lv_label_set_text(ai_usage_cursor_ondemand_label_, od_buf);
        }
        SetHidden(ai_usage_cursor_ondemand_label_, false);
    } else {
        SetHidden(ai_usage_cursor_ondemand_label_, true);
    }
}
