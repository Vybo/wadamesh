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

// Stop LVGL rasterising frames that will never be shown. This does NOT set the
// rate the user sees -- the panel commit is throttled separately by the refresh
// policy in TDeckProDisplay -- it just stops the render work on a loop task that
// also runs the mesh. 500 ms is LilyGo's own figure for this panel.
#ifdef LV_DISP_DEF_REFR_PERIOD
#undef LV_DISP_DEF_REFR_PERIOD
#endif
#define LV_DISP_DEF_REFR_PERIOD 500
