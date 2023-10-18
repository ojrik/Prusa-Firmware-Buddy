// screen_printing.hpp
#pragma once
#include "status_footer.hpp"
#include "window_header.hpp"
#include "window_roll_text.hpp"
#include "window_icon.hpp"
#include "window_term.hpp"
#include "window_print_progress.hpp"
#include "ScreenPrintingModel.hpp"
#include "print_progress.hpp"
#include "print_time_module.hpp"
#include <option/development_items.h>
#include <option/developer_mode.h>
#include <array>

enum class printing_state_t : uint8_t {
    INITIAL,
    PRINTING,
    ABSORBING_HEAT,
    PAUSING,
    PAUSED,
    RESUMING,
    ABORTING,
    REHEATING,
    REHEATING_DONE,
    MBL_FAILED,
    STOPPED,
    PRINTED,
    COUNT // setting this state == forced update
};

constexpr static const size_t POPUP_MSG_DUR_MS = 5000;

class screen_printing_data_t : public AddSuperWindow<ScreenPrintingModel> {
    static constexpr const char *caption = N_("PRINTING ...");

#if defined(USE_ILI9488)
    PrintProgress print_progress;
#endif

    window_roll_text_t w_filename;
    WindowPrintProgress w_progress;
    WindowNumbPrintProgress w_progress_txt;
#if defined(USE_ST7789)
    window_text_t w_time_label;
    window_text_t w_time_value;
#endif // USE_ST7789
    window_text_t w_etime_label;
    window_text_t w_etime_value;

    // std::array<char, 15> label_etime;  // "Remaining Time" or "Print will end" // nope, if you have only 2 static const strings, you can swap pointers
    string_view_utf8 label_etime;      // not sure if we really must keep this in memory
    std::array<char, 5> text_filament; // 999m\0 | 1.2m\0
    uint32_t message_timer;
    bool stop_pressed;
    bool waiting_for_abort; /// flag specific for stop pressed when MBL is performed
    printing_state_t state__readonly__use_change_print_state;

    float last_e_axis_position;
    const Rect16 popup_rect;
    PrintTime print_time;
    PT_t time_end_format;

#if DEVELOPMENT_ITEMS() && !DEVELOPER_MODE()
    bool print_feedback_pending = false;
#endif

public:
    screen_printing_data_t();

protected:
    virtual void windowEvent(EventLock /*has private ctor*/, window_t *sender, GUI_event_t event, void *param) override;

private:
    void invalidate_print_state();
    void updateTimes();

#if defined(USE_ST7789)
    void update_print_duration(time_t rawtime);
#endif // USE_ST7789
    void screen_printing_reprint();
    void set_pause_icon_and_label();
    void set_tune_icon_and_label();
    void set_stop_icon_and_label();
    void change_print_state();

    virtual void stopAction() override;
    virtual void pauseAction() override;
    virtual void tuneAction() override;

public:
    printing_state_t GetState() const;
    virtual Rect16 GetPopUpRect() override { return popup_rect; }
};
