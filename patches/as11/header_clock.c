/* Display local time in the dashboard and therapy-screen title bar. */

#include "stubs.h"
#include "vars.h"

#define GUI_PAGE_DASHBOARD                0u
#define GUI_PAGE_THERAPY_SINGLE_PRESSURE  2u
#define GUI_PAGE_THERAPY_PRESSURE_BAR     3u

#define STATUS_STATE_BLINKING 2u
#define MINUTE_MILLISECONDS 60000u

typedef struct {
    unsigned short home;
    unsigned short empty;
    unsigned short menu;

/* --- insert after the existing #define block, before header_clock_text_ids_t --- */

#define AUTO_DST_BUMP_MINUTES 60
#define AUTO_DST_MIN_STANDARD_OFFSET_MIN (-720)
/* Keep standard+60 inside TZO's own stock range (-720..840). */
#define AUTO_DST_MAX_STANDARD_OFFSET_MIN (840 - AUTO_DST_BUMP_MINUTES)
#define MS_PER_DAY 86400000

/* Reclaimed by the patcher when --patch-auto-dst is requested (field RTF).
 * Stores the STANDARD (winter) UTC offset, minutes. 0xFFFF means the feature
 * was not claimed for this build: auto_dst_tick() becomes a no-op and
 * TimeZoneOffset keeps today's fully manual, clinician-edited behavior. */
volatile const unsigned short auto_dst_base_var_id
    __attribute__((used, section(".rodata.params.var_id"))) = 0xFFFFu;

/* --- pure-integer Gregorian calendar math (Howard Hinnant's algorithm).
 * No libc <time.h> in this freestanding payload. Valid well past 2100. --- */

static void civil_from_days(int z, int *y, unsigned int *m, unsigned int *d)
{
    int era;
    unsigned int doe, yoe, doy, mp;

    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned int)(z - era * 146097);
    yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    mp = (5u * doy + 2u) / 153u;
    *d = doy - (153u * mp + 2u) / 5u + 1u;
    *m = mp < 10u ? mp + 3u : mp - 9u;
    *y = (int)yoe + era * 400 + (*m <= 2u ? 1 : 0);
}

static int days_from_civil(int y, unsigned int m, unsigned int d)
{
    int era;
    unsigned int yoe, doy, doe;

    y -= (m <= 2u) ? 1 : 0;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned int)(y - era * 400);
    doy = (153u * (m + (m > 2u ? (unsigned int)-3 : 9u)) + 2u) / 5u + d - 1u;
    doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int)doe - 719468;
}

/* 0=Sunday .. 6=Saturday. Epoch day 0 (1970-01-01) was a Thursday. */
static unsigned int weekday_from_days(int z)
{
    int w = (z + 4) % 7;
    if (w < 0)
        w += 7;
    return (unsigned int)w;
}

/* Epoch-day of the `nth` (1-based) `weekday` occurring in (year, month). */
static int nth_weekday_of_month(
    int year, unsigned int month, unsigned int nth, unsigned int weekday)
{
    int first_of_month = days_from_civil(year, month, 1u);
    unsigned int first_weekday = weekday_from_days(first_of_month);
    unsigned int delta = (weekday + 7u - first_weekday) % 7u;

    return first_of_month + (int)delta + (int)(nth - 1u) * 7;
}

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

#define AUTO_DST_SUNDAY 0u
#define AUTO_DST_TRANSITION_LOCAL_MS (2u * 3600000u) /* 02:00 local */

/* US/Canada rule: 2nd Sunday of March 02:00 local -> 1st Sunday of
 * November 02:00 local. For an EU build (last Sunday Mar/Oct, 01:00 UTC,
 * not local), replace this function; the UTC-instant test is simpler and
 * does not need the local-time conversion done in auto_dst_tick(). */
static int auto_dst_is_active(int local_days, unsigned int local_ms_of_day)
{
    int year;
    unsigned int month, day;
    int dst_start, dst_end;

    civil_from_days(local_days, &year, &month, &day);
    dst_start = nth_weekday_of_month(year, 3u, 2u, AUTO_DST_SUNDAY);
    dst_end = nth_weekday_of_month(year, 11u, 1u, AUTO_DST_SUNDAY);

    if (local_days < dst_start)
        return 0;
    if (local_days == dst_start)
        return local_ms_of_day >= AUTO_DST_TRANSITION_LOCAL_MS;
    if (local_days < dst_end)
        return 1;
    if (local_days == dst_end)
        return local_ms_of_day < AUTO_DST_TRANSITION_LOCAL_MS;
    return 0;
}

static void auto_dst_tick(void)
{
    int utc_days, utc_ms, standard_offset, current_tzo;
    int dst_was_active, effective_offset_before;
    int local_ms_signed, local_days, dst_active, desired_tzo;

    if (auto_dst_base_var_id == 0xFFFFu)
        return; /* feature not claimed for this build */

    utc_days = (int)DataItem_read_value_by_id(VAR_ID_ZDD);
    utc_ms = (int)DataItem_read_value_by_id(VAR_ID_ZDT);
    standard_offset = clamp_int(
        (int)DataItem_read_value_by_id(auto_dst_base_var_id),
        AUTO_DST_MIN_STANDARD_OFFSET_MIN, AUTO_DST_MAX_STANDARD_OFFSET_MIN);
    current_tzo = (int)DataItem_read_value_by_id(VAR_ID_TZO);

    /* Hysteresis: resolve "now, local" using whichever state TZO already
     * encodes, same as any conventional DST-aware clock. */
    dst_was_active = (current_tzo != standard_offset);
    effective_offset_before = dst_was_active
        ? standard_offset + AUTO_DST_BUMP_MINUTES
        : standard_offset;

    /* Offset magnitude is bounded to +/-900 min (+/-54,000,000 ms); no
     * overflow combining with a ms-of-day value bounded to [0, 86399999]. */
    local_ms_signed = utc_ms + effective_offset_before * 60000;
    local_days = utc_days;
    while (local_ms_signed < 0) {
        local_ms_signed += MS_PER_DAY;
        local_days -= 1;
    }
    while (local_ms_signed >= MS_PER_DAY) {
        local_ms_signed -= MS_PER_DAY;
        local_days += 1;
    }

    dst_active = auto_dst_is_active(local_days, (unsigned int)local_ms_signed);
    desired_tzo = dst_active
        ? standard_offset + AUTO_DST_BUMP_MINUTES
        : standard_offset;

    if (desired_tzo != current_tzo)
        DataItem_write_raw_by_id(VAR_ID_TZO, desired_tzo);
}

} header_clock_text_ids_t;

/* Native root-widget prefix; the owned-timer callback receives &gui_object. */
typedef struct {
    unsigned char fields_00_0b[0x0c];
    unsigned char gui_object;
    unsigned char fields_0d_67[0x5b];
    unsigned int status_state;
    unsigned char fields_6c_8f[0x24];
    void *status_timer;
} user_interface_root_widget_layout_t;

volatile const header_clock_text_ids_t header_clock_text_ids
    __attribute__((used, section(".rodata.params"))) = {
        0xFFFFu,
        0xFFFFu,
        0xFFFFu,
    };

/* Replaced with the reclaimed setting ID when custom settings are enabled. */
volatile const unsigned short header_clock_var_id
    __attribute__((used, section(".rodata.params.var_id"))) = 0xFFFFu;

static int clock_is_enabled(void)
{
    return header_clock_var_id == 0xFFFFu ||
        DataItem_read_value_by_id(header_clock_var_id) != 0;
}

static int read_local_milliseconds(unsigned int *result)
{
    /* Applies TimeZoneOffset and normalizes day rollover. */
    *result = datetime_current_local_milliseconds_of_day();
    return *result != 0xFFFFFFFFu;
}

static void schedule_next_minute(user_interface_root_widget_layout_t *root)
{
    unsigned int local_ms;
    unsigned int delay = 1000u;

    /* Reuse the root widget's status timer, aligned to the next minute. */
    if (read_local_milliseconds(&local_ms))
        delay = MINUTE_MILLISECONDS - local_ms % MINUTE_MILLISECONDS;
    thunk_gui_timer_handle_reschedule_with_optional_delay(
        root->status_timer, delay);
}

static int clock_replaces_label(unsigned int text_id)
{
    unsigned int page = (unsigned int)DataItem_read_value_by_id(VAR_ID_ZMD);

    if (page == GUI_PAGE_DASHBOARD)
        return text_id == header_clock_text_ids.home;
    if (page == GUI_PAGE_THERAPY_SINGLE_PRESSURE ||
            page == GUI_PAGE_THERAPY_PRESSURE_BAR)
        return text_id == header_clock_text_ids.empty;
    return 0;
}

void __attribute__((section(".text.0.main")))
start(
    unsigned int text_id,
    void *rect,
    unsigned int text_size,
    unsigned int font,
    int clip,
    unsigned int alignment)
{
    unsigned int local_ms;
    unsigned int total_minutes;
    unsigned int hour;
    unsigned int minute;
    char clock[6];

    /* The hook is shared by other title bars; preserve their stock labels. */
    if (!clock_is_enabled() || !clock_replaces_label(text_id) ||
            !read_local_milliseconds(&local_ms)) {
        GuiPaint_DrawLocalizedTextById(
            text_id, rect, text_size, font, clip, alignment);
        return;
    }

    total_minutes = local_ms / MINUTE_MILLISECONDS;
    hour = total_minutes / 60u;
    minute = total_minutes - hour * 60u;
    clock[0] = (char)('0' + hour / 10u);
    clock[1] = (char)('0' + hour % 10u);
    clock[2] = ':';
    clock[3] = (char)('0' + minute / 10u);
    clock[4] = (char)('0' + minute % 10u);
    clock[5] = '\0';

    /* Draw dynamic text with the font slot selected for the replaced label. */
    GuiPaint_DrawStringInRect(
        clock,
        rect,
        text_size,
        font,
        clip,
        alignment,
        gui_localized_text_font_slot_for_id(text_id));
}

void header_clock_menu_label_draw(
    unsigned int text_id,
    void *rect,
    unsigned int text_size,
    unsigned int font,
    int clip,
    unsigned int alignment)
{
    /* The custom row reuses the detached Reminders text ID. */
    if (header_clock_var_id == 0xFFFFu ||
            text_id != header_clock_text_ids.menu) {
        GuiPaint_DrawLocalizedTextById(
            text_id, rect, text_size, font, clip, alignment);
        return;
    }

    GuiPaint_DrawStringInRect(
        "Clock",
        rect,
        text_size,
        font,
        clip,
        alignment,
        gui_localized_text_font_slot_for_id(text_id));
}

void *header_clock_root_widget_ctor(
    void *root,
    unsigned int arg2,
    unsigned int arg3,
    unsigned int arg4)
{
    /* Preserve construction, then arm its existing timer for clock refresh. */
    user_interface_root_widget_layout_t *layout =
        user_interface_root_widget_ctor(root, arg2, arg3, arg4);

    schedule_next_minute(layout);
    return layout;
}

void header_clock_timer_callback(void *callback_self)
{
    user_interface_root_widget_layout_t *root =
        (user_interface_root_widget_layout_t *)(
            (unsigned char *)callback_self -
            __builtin_offsetof(
                user_interface_root_widget_layout_t, gui_object));

    if (root->status_state == STATUS_STATE_BLINKING) {
        user_interface_root_widget_status_blink_timer_callback_adjustor(
            callback_self);
    } else {
        auto_dst_tick();           /* <-- new: keep TimeZoneOffset DST-correct */
        schedule_next_minute(root);
    }

    gui_owned_object_invalidate(&root->gui_object);
}
