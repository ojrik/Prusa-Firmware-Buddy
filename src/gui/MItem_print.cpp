#include "MItem_print.hpp"
#include "marlin_client.hpp"
#include "common/conversions.hpp"
#include "menu_vars.h"
#include "WindowMenuSpin.hpp"
#include "img_resources.hpp"
#if ENABLED(PRUSA_TOOLCHANGER)
    #include "module/prusa/toolchanger.h"
#endif
#if HAS_MMU2()
    #include <feature/prusa/MMU2/mmu2_mk4.h>
#endif

/*****************************************************************************/
// MI_NOZZLE_ABSTRACT
is_hidden_t MI_NOZZLE_ABSTRACT::is_hidden([[maybe_unused]] uint8_t tool_nr) {
#if ENABLED(PRUSA_TOOLCHANGER)
    return prusa_toolchanger.is_tool_enabled(tool_nr) ? is_hidden_t::no : is_hidden_t::yes;
#else
    return is_hidden_t::no;
#endif
}

static constexpr NumericInputConfig nozzle_spin_config = {
    .max_value = HEATER_0_MAXTEMP - HEATER_MAXTEMP_SAFETY_MARGIN,
    .special_value = 0,
    .unit = Unit::celsius,
};

MI_NOZZLE_ABSTRACT::MI_NOZZLE_ABSTRACT(uint8_t tool_nr, [[maybe_unused]] const char *label)
    : WiSpin(uint16_t(marlin_vars()->hotend(tool_nr).target_nozzle), nozzle_spin_config,
#if ENABLED(PRUSA_TOOLCHANGER)
        prusa_toolchanger.is_toolchanger_enabled() ? _(label) : _(generic_label),
#else
        _(generic_label),
#endif
        &img::nozzle_16x16, is_enabled_t::yes, is_hidden(tool_nr))
    , tool_nr(tool_nr) {
}

void MI_NOZZLE_ABSTRACT::OnClick() {
    marlin_client::set_target_nozzle(value(), tool_nr);
    marlin_client::set_display_nozzle(value(), tool_nr);
}

/*****************************************************************************/
// MI_HEATBED
static constexpr NumericInputConfig bed_spin_config = {
    .max_value = (BED_MAXTEMP - BED_MAXTEMP_SAFETY_MARGIN),
    .special_value = 0,
    .unit = Unit::celsius,
};

MI_HEATBED::MI_HEATBED()
    : WiSpin(uint8_t(marlin_vars()->target_bed), bed_spin_config, _(label), &img::heatbed_16x16, is_enabled_t::yes, is_hidden_t::no) {
}
void MI_HEATBED::OnClick() {
    marlin_client::set_target_bed(value());
}

/*****************************************************************************/
// MI_PRINTFAN

static constexpr NumericInputConfig printfan_spin_config = {
    .max_value = 100,
    .special_value = 0,
    .unit = Unit::percent,
};

MI_PRINTFAN::MI_PRINTFAN()
    : WiSpin(val_mapping(false, marlin_vars()->print_fan_speed, 255, 100),
        printfan_spin_config, _(label), &img::fan_16x16, is_enabled_t::yes, is_hidden_t::no) {
}
void MI_PRINTFAN::OnClick() {
    marlin_client::set_fan_speed(val_mapping(true, value(), 100, 255));
}

/*****************************************************************************/
// MI_SPEED

static constexpr NumericInputConfig print_speed_spin_config = {
#if (PRINTER_IS_PRUSA_MK4 || PRINTER_IS_PRUSA_MK3_5 || PRINTER_IS_PRUSA_XL || PRINTER_IS_PRUSA_iX)
    .min_value = 50,
    .max_value = 1000,
#else
    .min_value = 10,
    .max_value = 255,
#endif
    .unit = Unit::percent,
};

MI_SPEED::MI_SPEED()
    : WiSpin(uint16_t(marlin_vars()->print_speed), print_speed_spin_config, _(label), &img::speed_16x16, is_enabled_t::yes, is_hidden_t::no) {
}

void MI_SPEED::OnClick() {
    marlin_client::set_print_speed(value());
}

/*****************************************************************************/
// MI_FLOWFACT_ABSTRACT
is_hidden_t MI_FLOWFACT_ABSTRACT::is_hidden([[maybe_unused]] uint8_t tool_nr) {
#if HAS_TOOLCHANGER()
    return prusa_toolchanger.is_tool_enabled(tool_nr) ? is_hidden_t::no : is_hidden_t::yes;
#elif HAS_MMU2()
    return (tool_nr == 0 || MMU2::mmu2.Enabled()) ? is_hidden_t::no : is_hidden_t::yes;
#else
    return is_hidden_t::no;
#endif
}

static constexpr NumericInputConfig flowfact_spin_config {
    .min_value = 50,
    .max_value = 150,
    .unit = Unit::percent,
};

MI_FLOWFACT_ABSTRACT::MI_FLOWFACT_ABSTRACT(uint8_t tool_nr, [[maybe_unused]] const char *label)
    : WiSpin(uint16_t(marlin_vars()->hotend(tool_nr).flow_factor), flowfact_spin_config,
#if HAS_TOOLCHANGER()
        prusa_toolchanger.is_toolchanger_enabled() ? _(label) : _(generic_label),
#elif HAS_MMU2()
        MMU2::mmu2.Enabled() ? _(label) : _(generic_label),
#else
        _(generic_label),
#endif /*TOOLCHANGER or MMU2*/
        nullptr, is_enabled_t::yes, is_hidden(tool_nr))
    , tool_nr(tool_nr) {
}

void MI_FLOWFACT_ABSTRACT::OnClick() {
    marlin_client::set_flow_factor(value(), tool_nr);
}
