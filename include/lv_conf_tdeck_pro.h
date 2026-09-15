#pragma once

// T-Deck Pro-specific LVGL config wrapper.
//
// Use a unique LV_CONF_PATH so the project copy wins over MeshCore's stale
// include/lv_conf.h. LVGL 8.4 then defines the scroll threshold/throw macros
// unconditionally in lv_hal_indev.h, so remove the shared touch overrides
// after loading the rest of the project configuration to avoid redefinitions.
#ifndef LV_CONF_H
#define LV_CONF_H
#endif

#include "lv_conf.h"

#ifdef LV_INDEV_DEF_SCROLL_LIMIT
#undef LV_INDEV_DEF_SCROLL_LIMIT
#endif

#ifdef LV_INDEV_DEF_SCROLL_THROW
#undef LV_INDEV_DEF_SCROLL_THROW
#endif

// The stock theme animates bg-colour/opacity over 80 ms on press and release of
// every button. On a bistable panel that is two extra panel updates per tap,
// showing a fade one bit cannot represent anyway.
#ifdef LV_THEME_DEFAULT_TRANSITION_TIME
#undef LV_THEME_DEFAULT_TRANSITION_TIME
#endif
#define LV_THEME_DEFAULT_TRANSITION_TIME 0

// LVGL's display refresh timer period -- how often lv_timer_handler is allowed
// to walk the invalidated-area list and call flush_cb. This is dead time on
// EVERY user-visible change: worst case a full period, mean half of it, before
// a tap or keystroke even begins to rasterise.
//
// It was 500 ms on the theory that rasterising faster than the panel can show
// is wasted work. That was wrong about where the throttle lives: the panel
// commit rate is set by the blocking commit itself plus _min_interval_ms in
// TDeckProDisplay::serviceRefresh, and that floor DEFERS rather than drops
// (_refresh_pending survives the early return). So a shorter period does not
// multiply panel updates -- LVGL just rasterises a couple of intermediate
// frames into the same shadow buffer, which are coalesced into one commit. What
// it buys is latency.
//
// Note this is also the global animation-timer period (lv_anim.c), so LVGL
// animations step at this rate too. Theme transitions are already 0 and the
// marquees are disabled, so what remains is scroll-to and the at-a-glance fade.
#define LV_DISP_DEF_REFR_PERIOD 100
