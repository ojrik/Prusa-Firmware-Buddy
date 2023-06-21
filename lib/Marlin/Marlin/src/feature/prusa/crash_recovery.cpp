#include "inc/MarlinConfigPre.h"

#if ENABLED(CRASH_RECOVERY)

    #include "../../module/stepper.h"
    #include "crash_recovery.h"
    #include "bsod.h"
    #include "../../module/printcounter.h"
    #include "metric.h"
    #include <configuration_store.hpp>

Crash_s &crash_s = Crash_s::instance();

Crash_s &Crash_s::instance() {
    static Crash_s s;
    return s;
}

Crash_s::Crash_s()
    : home_sensitivity { X_STALL_SENSITIVITY, Y_STALL_SENSITIVITY, Z_STALL_SENSITIVITY }
    , m_axis_is_homing { false, false }
    , m_enable_stealth { false, false }
    , toolchange_in_progress(false) {
    reset();
    enabled = config_store().crash_enabled.get();
    max_period.x = config_store().crash_max_period_x.get();
    max_period.y = config_store().crash_max_period_y.get();
    sensitivity.x = config_store().crash_sens_x.get();
    sensitivity.y = config_store().crash_sens_y.get();
    #if HAS_DRIVER(TMC2130)
    filter = config_store().crash_filter.get();
    #endif
}

// Called from ISR
void Crash_s::stop_and_save() {
    // freeze motion first
    stepper.suspend();
    loop = true;

    // get the current live block
    const block_t *current_block = stepper.block();
    float e_position;

    if (!current_block && planner.movesplanned()) {
        // there's no live block, attempt to fetch the next block from the planner
        current_block = &planner.block_buffer[planner.block_buffer_tail];
    }

    if (current_block) {
        // recover the correct crash block
        const uint8_t crash_index = current_block - planner.block_buffer;
        const Crash_s::crash_block_t &crash_block = crash_s.crash_block[crash_index];

        // save current_block state
        check_and_set_sdpos(crash_block.sdpos);
        segments_finished = crash_block.segment_idx;
        inhibit_flags = crash_block.inhibit_flags;
        fr_mm_s = crash_block.fr_mm_s;
    #if ENABLED(LIN_ADVANCE)
        advance_mm = stepper.get_LA_steps() * Planner::mm_per_step[E_AXIS];
    #endif
        start_current_position = crash_block.start_current_position;

        // recover delta E position
        float d_e_steps = crash_block.e_steps * stepper.segment_progress();
        e_position = crash_block.e_position + d_e_steps * planner.mm_per_step[E_AXIS_N(active_extruder)];
    } else {
        // no block, get state from the queue & planner
        check_and_set_sdpos(queue.get_current_sdpos());
        segments_finished = 0;
        inhibit_flags = gcode_state.inhibit_flags;
        fr_mm_s = feedrate_mm_s;
    #if ENABLED(LIN_ADVANCE)
        advance_mm = 0;
    #endif
        start_current_position = current_position;
        e_position = current_position[E_AXIS];
    }

    // save planner state
    if (toolchange_in_progress) {
        leveling_active = pretoolchange_leveling; // Leveling was active before toolchange
    } else {
        leveling_active = planner.leveling_active;
    }
    crash_axis_known_position = axis_known_position;
    // TODO: this is incomplete, as some of the planner state is ahead of the stepper state
    //       marlin state is also not saved, notably: absolute/relative axis state
    // marlin_server.motion_param.save();

    // stop any movement: this will discard any planner state!
    planner.quick_stop();
    planner.reset_position();
    crash_position = planner.get_machine_position_mm();

    // Due to e_factor not being reflected in the physical positioning (allowing the stepper position
    // to drift), we cannot easily recover an absolute E position that makes sense. We thus work in
    // segment-relative offsets, requiring us to store extra state.
    crash_position[E_AXIS] = e_position;

    // update crash_current_position. WARNING: this is NOT intended to be fully reversible (doing so
    // would require keeping more state), it's only usable to abort or return to the same position.
    planner.get_axis_position_mm(crash_current_position);

    #if HAS_POSITION_MODIFIERS
    planner.unapply_modifiers(crash_current_position
        #if HAS_LEVELING
        ,
        true
        #endif
    );
    #endif
}

void check_loop() {
    if (crash_s.loop) {
        // guard against incomplete buffer flushing: if we reach this point, the state machine is
        // incorrectly being handled recursively (double ungood)
        bsod("reentrant recovery");
    }
}

void Crash_s::resume_movement() {
    // at this point leveling should be off until the print is restored
    planner.leveling_active = false;

    // order is important here! set an approximate current position which is only good enough for
    // re-homing and guarantees no changes with a zero offset from current_position.
    current_position = crash_current_position;
    planner.set_position_mm(current_position);

    check_loop();
    planner.resume_queuing();
}

void Crash_s::restore_state() {
    if (inhibit_flags & INHIBIT_PARTIAL_REPLAY)
        segments_finished = 0;

    if (inhibit_flags & INHIBIT_XYZ_REPOSITIONING) {
        // also reset internal crash locations to current_position
        LOOP_XYZ(i) {
            start_current_position[i] = current_position[i];
            crash_position[i] = current_position[i];
        }
    }

    // order is important here!
    planner.leveling_active = leveling_active;
    current_position = start_current_position;
    planner.set_position_mm(current_position);

    // restore additional queue parameters
    feedrate_mm_s = fr_mm_s;

    check_loop();
    planner.resume_queuing();
}

void Crash_s::set_state(state_t new_state) {
    auto isr_bsod = [](const char *msg) {
        /// @todo bsod cannot be used from ISR with priority above RTOS.
        ///   Use fatal_error instead as it goes through sys_reset().
        ///   When bsod does the same, this could be replaced by just bsod(msg);.
        fatal_error(msg, "Crash_s red BSOD");
    };

    // Prevent race condition here
    static std::atomic_flag reentrant_flag;
    if (reentrant_flag.test_and_set()) {
        isr_bsod("reentrant Crash_s::set_state()");
    }

    if (state == new_state) {
        switch (state) {
        case IDLE:
            isr_bsod("crash idle");
        case RECOVERY:
            isr_bsod("crash recovery");
        case REPLAY:
            isr_bsod("crash replay");
        case TRIGGERED_ISR:
        case TRIGGERED_AC_FAULT:
        case TRIGGERED_TOOLFALL:
        case TRIGGERED_TOOLCRASH:
        case TRIGGERED_HOMEFAIL:
            isr_bsod("crash double trigger");
        case REPEAT_WAIT:
            isr_bsod("toolcrash or homing fail repeat");
        case PRINTING:
            isr_bsod("crash printing");
        }
    }

    switch (new_state) {
    case IDLE:
        reset();
        break;

    case TRIGGERED_ISR:
        if (state != PRINTING || !is_active() || !is_enabled()) {
            isr_bsod("isr, not active");
        }
        // FALLTHRU
    case TRIGGERED_AC_FAULT:
        // transition to AC_FAULT is _always_ possible from any state
        toolchange_event = toolchange_in_progress;
        stop_and_save();
        break;

    case TRIGGERED_TOOLFALL:
        if (state != PRINTING || !is_active()) { // Allow toolfall recovery even if user disables crash recovery
            isr_bsod("toolfall, not active");
        }

        toolchange_event = true;
        stop_and_save();
        break;

    case TRIGGERED_TOOLCRASH:
        if (state != PRINTING || !is_active()) {
            isr_bsod("toolcrash, not active");
        }

        toolchange_event = true;
        crash_axis_known_position = axis_known_position; // Needed for powerpanic
        check_and_set_sdpos(queue.get_current_sdpos());
        break;

    case TRIGGERED_HOMEFAIL:
        if (state != PRINTING || !is_active()) {
            isr_bsod("home, not active");
        }

        toolchange_event = false;
        crash_axis_known_position = axis_known_position; // Needed for powerpanic
        check_and_set_sdpos(queue.get_current_sdpos());
        break;

    case REPEAT_WAIT:
        if (state != PRINTING && state != TRIGGERED_TOOLCRASH && state != TRIGGERED_HOMEFAIL) {
            isr_bsod("invalid wait transition");
        }

        // Just wait for user to pick up tools or to rehome
        break;

    case RECOVERY:
        // TODO: the following checks are too broad (should check for existing state)
        if (state != PRINTING && state != TRIGGERED_ISR && state != TRIGGERED_TOOLFALL && state != TRIGGERED_AC_FAULT)
            isr_bsod("invalid recovery transition");
        resume_movement();
        break;

    case REPLAY:
        if (state != RECOVERY)
            isr_bsod("invalid replay transition");
        activate();
        restore_state();
        break;

    case PRINTING:
        if (state != RECOVERY && state != REPEAT_WAIT && state != IDLE && state != REPLAY)
            isr_bsod("invalid printing transition");
        reset_repeated_crash();
        if (state != REPLAY)
            activate();
        break;
    }

    state = new_state;
    reentrant_flag.clear();
}
/**
 * @brief Update sensitivity and threshold to drivers
 */
void Crash_s::update_machine() {
    if (!m_axis_is_homing[0] && TERN1(CORE_IS_XY, !m_axis_is_homing[1])) {
        if (enabled) {
            stepperX.stall_max_period(0);
    #if AXIS_DRIVER_TYPE_X(TMC2130)
            stepperX.sfilt(filter);
            stepperX.diag1_stall(true);
    #endif
            stepperX.stall_sensitivity(crash_s.sensitivity.x);
            stepperX.stall_max_period(crash_s.max_period.x);
        } else {
            tmc_disable_stallguard(stepperX, m_enable_stealth[0]);
        }
    }
    if (!m_axis_is_homing[1] && TERN1(CORE_IS_XY, !m_axis_is_homing[0])) {
        if (enabled) {
            stepperY.stall_max_period(0);
    #if AXIS_DRIVER_TYPE_Y(TMC2130)
            stepperY.sfilt(filter);
            stepperY.diag1_stall(true);
    #endif
            stepperY.stall_sensitivity(crash_s.sensitivity.y);
            stepperY.stall_max_period(crash_s.max_period.y);
        } else {
            tmc_disable_stallguard(stepperY, m_enable_stealth[1]);
        }
    }
}

void Crash_s::enable(bool state) {
    if (state == enabled)
        return;
    enabled = state;
    config_store().crash_enabled.set(state);
    update_machine();
}

void Crash_s::set_sensitivity(xy_long_t sens) {
    if (sensitivity != sens) {
        sensitivity = sens;
        config_store().crash_sens_x.set(sensitivity.x);
        config_store().crash_sens_y.set(sensitivity.y);
        update_machine();
    }
}

void Crash_s::reset_crash_counter() {
    counter_crash = xy_uint_t({ 0, 0 });
    counter_power_panic = 0;
}

void Crash_s::send_reports() {
    if (axis_hit != X_AXIS && axis_hit != Y_AXIS)
        return;

    float speed = -1;
    if (axis_hit == X_AXIS) {
        speed = period_to_speed(X_MICROSTEPS, int(stepperX.TSTEP()), get_steps_per_unit_x());
    }
    if (axis_hit == Y_AXIS) {
        speed = period_to_speed(Y_MICROSTEPS, int(stepperY.TSTEP()), get_steps_per_unit_y());
    }

    static metric_t crash_metric = METRIC("crash", METRIC_VALUE_CUSTOM, 0, METRIC_HANDLER_ENABLE_ALL);
    metric_record_custom(&crash_metric, ",axis=%c sens=%ii,period=%ui,speed=%.3f", axis_codes[axis_hit], sensitivity.pos[axis_hit], max_period.pos[axis_hit], (double)speed);
}

void Crash_s::set_max_period(xy_long_t mp) {
    if (max_period != mp) {
        max_period = mp;
        config_store().crash_max_period_x.set(max_period.x);
        config_store().crash_max_period_y.set(max_period.y);
        update_machine();
    }
}

void Crash_s::write_stat_to_eeprom() {
    xy_uint_t total({ config_store().crash_count_x.get(), config_store().crash_count_y.get() });
    uint16_t power_panics = config_store().power_panics_count.get();

    LOOP_XY(axis) {
        if (counter_crash.pos[axis] > 0) {
            total.pos[axis] += counter_crash.pos[axis];
            if (axis == 0) {
                config_store().crash_count_x.set(total.pos[axis]);
            } else if (axis == 1) {
                config_store().crash_count_y.set(total.pos[axis]);
            }
            static metric_t crash_stat = METRIC("crash_stat", METRIC_VALUE_CUSTOM, 0, METRIC_HANDLER_ENABLE_ALL);
            metric_record_custom(&crash_stat, ",axis=%c last=%ui,total=%ui", axis_codes[axis], counter_crash.pos[axis], total.pos[axis]);
        }
    }
    power_panics += counter_power_panic;
    config_store().power_panics_count.set(power_panics);

    reset_crash_counter();
}

uint32_t Crash_s::clean_history() {
    int valid = 0;
    for (auto &ts : crash_timestamps) {
        if (ts.has_value() && (print_job_timer.duration() - ts.value() <= CRASH_TIMER)) {
            ++valid;
        } else {
            ts = std::nullopt;
        }
    }
    return valid;
}

void Crash_s::reset_history() {
    for (auto &t : crash_timestamps)
        t = std::nullopt;
}

void Crash_s::count_crash() {
    if (axis_hit == X_AXIS || axis_hit == Y_AXIS)
        ++counter_crash.pos[axis_hit];

    uint32_t valid = clean_history();
    if (valid == crash_timestamps.size()) {
        repeated_crash = true;
        static metric_t crash_repeated = METRIC("crash_repeated", METRIC_VALUE_EVENT, 0, METRIC_HANDLER_ENABLE_ALL);
        metric_record_event(&crash_repeated);
    }
    crash_timestamps[crash_timestamps_idx++] = print_job_timer.duration();
    crash_timestamps_idx %= crash_timestamps.size();
}

void Crash_s::reset() {
    reset_history();
    repeated_crash = false;
    toolchange_in_progress = false;
    segments_finished = 0;
    inhibit_flags = 0;
    state = IDLE;
    vars_locked = false;
    active = false;
    axis_hit = NO_AXIS_ENUM;
}

void Crash_s::start_sensorless_homing_per_axis(const AxisEnum axis) {
    if (axis < (sizeof(m_axis_is_homing) / sizeof(m_axis_is_homing[0]))) {
        m_axis_is_homing[axis] = true;
    #if ENABLED(CORE_IS_XY)
        if (X_AXIS == axis || Y_AXIS == axis) {
            stepperX.stall_sensitivity(crash_s.home_sensitivity[0]);
            stepperY.stall_sensitivity(crash_s.home_sensitivity[1]);
        }
    #else
        if (X_AXIS == axis) {
            stepperX.stall_sensitivity(crash_s.home_sensitivity[0]);
        } else if (Y_AXIS == axis) {
            stepperY.stall_sensitivity(crash_s.home_sensitivity[1]);
        }
    #endif
    }
}

/**
 */
void Crash_s::end_sensorless_homing_per_axis(const AxisEnum axis, const bool enable_stealth) {
    if (axis < (sizeof(m_axis_is_homing) / sizeof(m_axis_is_homing[0]))) {
        m_axis_is_homing[axis] = false;
        m_enable_stealth[axis] = enable_stealth;
        update_machine();
    }
}

    #if HAS_DRIVER(TMC2130)
void Crash_s::set_filter(bool on) {
    if (filter == on)
        return;
    filter = on;
    config_store().crash_filter.set(on);
    update_machine();
}
    #endif
#endif // ENABLED(CRASH_RECOVERY)
